/*
 * Copyright (C) 2024-2026 Laszlo Pere
 *
 * This file is part of Pipnode.  Pipnode is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License version 3, with the additional permission described in
 * LICENSE.PLUGIN-EXCEPTION, as published by the Free Software Foundation.
 *
 * Pipnode is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; see the GNU General Public License for more details.  You
 * should have received a copy of the license in the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-notify.h"
#include "pn-json-path.h"
#include "pn-message.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#define PN_NOTIFY_ICON  "\xef\x83\xb3"   /* fa-bell U+F0F3 */

/* The freedesktop notification service.  Same triple every
 * libnotify-backed program (notify-send, GNotification) uses. */
#define PN_NOTIFY_BUS_NAME      "org.freedesktop.Notifications"
#define PN_NOTIFY_OBJECT_PATH   "/org/freedesktop/Notifications"
#define PN_NOTIFY_INTERFACE     "org.freedesktop.Notifications"

#define PN_NOTIFY_TIMEOUT_MIN     -1
#define PN_NOTIFY_TIMEOUT_MAX     86400000  /* 24h cap; -1 = server default,
                                              0 = never expires */
#define PN_NOTIFY_TIMEOUT_DEFAULT -1

#define PN_NOTIFY_DEFAULT_SUMMARY "${topic}"
#define PN_NOTIFY_DEFAULT_BODY    "${data/output}"
#define PN_NOTIFY_DEFAULT_APPNAME "pipnode"
#define PN_NOTIFY_DEFAULT_ICON    "dialog-information"

struct _PnNotify
{
    PnNode parent_instance;

    /* Templated summary / body; ${path/to/field} is expanded against
     * the incoming message at receive time. */
    gchar *summary;
    gchar *body;

    /* Freedesktop icon name (e.g. "dialog-information") or absolute
     * path to an image file; passed through to the notification
     * daemon's `app_icon` argument verbatim.  Empty string suppresses
     * the icon. */
    gchar *icon;

    /* Application name reported to the notification daemon.  Some
     * notification UIs surface it; empty falls back to "pipnode". */
    gchar *app_name;

    PnNotifyUrgency urgency;

    /* Expire timeout in milliseconds.  -1 = "let the server decide"
     * (the freedesktop convention), 0 = never expires (the daemon
     * leaves the bubble up until the user dismisses it). */
    gint timeout_ms;

    /* When TRUE, store the daemon-returned notification id and pass
     * it as replaces_id on the next call so a fast-firing upstream
     * does not pile up dozens of bubbles. */
    gboolean replace;

    /* Cached session-bus connection.  Acquired lazily on the first
     * receive and reused for every subsequent call. */
    GDBusConnection *bus;

    /* Last notification id the daemon handed back, used as
     * replaces_id on the next call when @replace is TRUE.  0 means
     * "no previous notification" — the freedesktop spec reserves 0
     * as the sentinel for that case. */
    guint32 last_id;
};

G_DEFINE_TYPE (PnNotify, pn_notify, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_SUMMARY,
    PROP_BODY,
    PROP_ICON,
    PROP_APP_NAME,
    PROP_URGENCY,
    PROP_TIMEOUT_MS,
    PROP_REPLACE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Urgency enum                                                       */
/* ------------------------------------------------------------------ */

GType
pn_notify_urgency_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_NOTIFY_URGENCY_LOW,      "PN_NOTIFY_URGENCY_LOW",      "low"      },
            { PN_NOTIFY_URGENCY_NORMAL,   "PN_NOTIFY_URGENCY_NORMAL",   "normal"   },
            { PN_NOTIFY_URGENCY_CRITICAL, "PN_NOTIFY_URGENCY_CRITICAL", "critical" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnNotifyUrgency", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Template expansion                                                 */
/*                                                                     */
/*  Same shape as PnFormat: scalar nodes render naturally, structured  */
/*  nodes fall back to compact JSON, unknown paths render empty.  An   */
/*  unterminated "${" is emitted verbatim.                             */
/* ------------------------------------------------------------------ */

static gchar *
node_to_string (JsonNode *node)
{
    if (node == NULL || JSON_NODE_HOLDS_NULL (node))
        return g_strdup ("");

    if (JSON_NODE_HOLDS_VALUE (node))
    {
        GType vtype = json_node_get_value_type (node);

        if (vtype == G_TYPE_STRING)
            return g_strdup (json_node_get_string (node));
        if (vtype == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT,
                                    json_node_get_int (node));
        if (vtype == G_TYPE_DOUBLE)
            return g_strdup_printf ("%g", json_node_get_double (node));
        if (vtype == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (node)
                                 ? "true" : "false");
    }

    {
        JsonGenerator *gen = json_generator_new ();
        gchar         *out;

        json_generator_set_root (gen, node);
        out = json_generator_to_data (gen, NULL);
        g_object_unref (gen);
        return out ? out : g_strdup ("");
    }
}

static gchar *
expand_placeholders (
        const gchar *tmpl,
        JsonObject  *root)
{
    GString     *out;
    const gchar *p;

    if (tmpl == NULL)
        return g_strdup ("");

    out = g_string_new (NULL);
    p   = tmpl;

    while (*p != '\0')
    {
        if (p[0] == '$' && p[1] == '{')
        {
            const gchar *end = strchr (p + 2, '}');

            if (end != NULL)
            {
                gchar    *path  = g_strndup (p + 2, end - (p + 2));
                JsonNode *node  = pn_json_resolve_path (root, path);
                gchar    *value = node_to_string (node);

                g_string_append (out, value);

                g_free (value);
                g_free (path);
                p = end + 1;
                continue;
            }
        }

        g_string_append_c (out, *p);
        p++;
    }

    return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  D-Bus call                                                         */
/* ------------------------------------------------------------------ */

/** Reap the async Notify() call and stash the returned notification
 *  id so the next receive can pass it as replaces_id.  A failure is
 *  logged but otherwise ignored — the next message just starts a new
 *  notification chain. */
static void
on_notify_done (
        GObject      *source,
        GAsyncResult *result,
        gpointer      user_data)
{
    GDBusConnection *bus   = G_DBUS_CONNECTION (source);
    PnNotify        *self  = PN_NOTIFY (user_data);
    GError          *error = NULL;
    GVariant        *reply;

    reply = g_dbus_connection_call_finish (bus, result, &error);
    if (reply == NULL)
    {
        g_warning ("pn-notify: Notify() failed: %s",
                   error ? error->message : "(unknown)");
        g_clear_error (&error);
        g_object_unref (self);
        return;
    }

    {
        guint32 id = 0;
        g_variant_get (reply, "(u)", &id);
        self->last_id = id;
    }

    g_variant_unref (reply);
    g_object_unref (self);
}

/** Build the hint dictionary.  Currently carries just the urgency
 *  byte, which every freedesktop-compliant notification server
 *  honours. */
static GVariant *
build_hints (PnNotifyUrgency urgency)
{
    GVariantBuilder b;

    g_variant_builder_init (&b, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&b, "{sv}",
                           "urgency",
                           g_variant_new_byte ((guchar) urgency));
    return g_variant_builder_end (&b);
}

static void
pn_notify_send (
        PnNotify    *self,
        const gchar *summary,
        const gchar *body)
{
    GVariant *params;
    GVariant *actions;
    GVariant *hints;
    guint32   replaces;

    if (self->bus == NULL)
    {
        GError *error = NULL;

        self->bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &error);
        if (self->bus == NULL)
        {
            g_warning ("pn-notify: cannot reach session bus: %s",
                       error ? error->message : "(unknown)");
            g_clear_error (&error);
            return;
        }
    }

    /* Empty array of strings for `actions` — we do not yet expose
     * action buttons to the user. */
    actions  = g_variant_new_strv (NULL, 0);
    hints    = build_hints (self->urgency);
    replaces = self->replace ? self->last_id : 0;

    params = g_variant_new ("(susss@as@a{sv}i)",
                            self->app_name && *self->app_name
                                ? self->app_name
                                : PN_NOTIFY_DEFAULT_APPNAME,
                            replaces,
                            self->icon ? self->icon : "",
                            summary ? summary : "",
                            body    ? body    : "",
                            actions,
                            hints,
                            (gint32) self->timeout_ms);

    g_dbus_connection_call (self->bus,
                            PN_NOTIFY_BUS_NAME,
                            PN_NOTIFY_OBJECT_PATH,
                            PN_NOTIFY_INTERFACE,
                            "Notify",
                            params,
                            G_VARIANT_TYPE ("(u)"),
                            G_DBUS_CALL_FLAGS_NONE,
                            -1, NULL,
                            on_notify_done,
                            g_object_ref (self));
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_notify_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnNotify   *self = PN_NOTIFY (node);
    JsonObject *root;
    gchar      *summary;
    gchar      *body;

    root    = pn_json_lookup_root_for_message (message);
    summary = expand_placeholders (self->summary, root);
    body    = expand_placeholders (self->body,    root);
    json_object_unref (root);

    pn_notify_send (self, summary, body);

    g_free (summary);
    g_free (body);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_notify_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnNotify *self = PN_NOTIFY (object);

    switch (prop_id)
    {
    case PROP_SUMMARY:    g_value_set_string  (value, self->summary);    break;
    case PROP_BODY:       g_value_set_string  (value, self->body);       break;
    case PROP_ICON:       g_value_set_string  (value, self->icon);       break;
    case PROP_APP_NAME:   g_value_set_string  (value, self->app_name);   break;
    case PROP_URGENCY:    g_value_set_enum    (value, self->urgency);    break;
    case PROP_TIMEOUT_MS: g_value_set_int     (value, self->timeout_ms); break;
    case PROP_REPLACE:    g_value_set_boolean (value, self->replace);    break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_string (gchar **slot, const gchar *value)
{
    g_free (*slot);
    *slot = g_strdup (value ? value : "");
}

static void
pn_notify_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnNotify *self = PN_NOTIFY (object);

    switch (prop_id)
    {
    case PROP_SUMMARY:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->summary, s) != 0)
            {
                set_string (&self->summary, s);
                g_object_notify_by_pspec (object, props[PROP_SUMMARY]);
            }
        }
        break;
    case PROP_BODY:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->body, s) != 0)
            {
                set_string (&self->body, s);
                g_object_notify_by_pspec (object, props[PROP_BODY]);
            }
        }
        break;
    case PROP_ICON:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->icon, s) != 0)
            {
                set_string (&self->icon, s);
                g_object_notify_by_pspec (object, props[PROP_ICON]);
            }
        }
        break;
    case PROP_APP_NAME:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->app_name, s) != 0)
            {
                set_string (&self->app_name, s);
                g_object_notify_by_pspec (object, props[PROP_APP_NAME]);
            }
        }
        break;
    case PROP_URGENCY:
        {
            PnNotifyUrgency u = g_value_get_enum (value);
            if (self->urgency != u)
            {
                self->urgency = u;
                g_object_notify_by_pspec (object, props[PROP_URGENCY]);
            }
        }
        break;
    case PROP_TIMEOUT_MS:
        {
            gint v = g_value_get_int (value);
            if (self->timeout_ms != v)
            {
                self->timeout_ms = v;
                g_object_notify_by_pspec (object, props[PROP_TIMEOUT_MS]);
            }
        }
        break;
    case PROP_REPLACE:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->replace != v)
            {
                self->replace = v;
                /* Drop the cached id so a freshly-toggled `replace`
                 * does not accidentally overwrite an unrelated
                 * notification the daemon may still own. */
                self->last_id = 0;
                g_object_notify_by_pspec (object, props[PROP_REPLACE]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_notify_finalize (GObject *object)
{
    PnNotify *self = PN_NOTIFY (object);

    g_clear_pointer (&self->summary,  g_free);
    g_clear_pointer (&self->body,     g_free);
    g_clear_pointer (&self->icon,     g_free);
    g_clear_pointer (&self->app_name, g_free);
    g_clear_object  (&self->bus);

    G_OBJECT_CLASS (pn_notify_parent_class)->finalize (object);
}

static void
pn_notify_class_init (PnNotifyClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_notify_get_property;
    object_class->set_property = pn_notify_set_property;
    object_class->finalize     = pn_notify_finalize;
    node_class->receive        = pn_notify_receive;

    node_class->palette_icon = PN_NOTIFY_ICON;
    node_class->class_name   = "Notify";
    node_class->icon         = PN_NOTIFY_ICON;
    node_class->color        = (GdkRGBA){ 0.95, 0.55, 0.30, 1.0 };
    node_class->category     = "Sinks";
    node_class->has_input    = TRUE;
    node_class->has_output   = FALSE;

    props[PROP_SUMMARY] = g_param_spec_string (
            "summary", "Summary",
            "Notification title template; ${path/to/field} expands "
            "to the corresponding JSON value from the incoming message",
            PN_NOTIFY_DEFAULT_SUMMARY,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BODY] = g_param_spec_string (
            "body", "Body",
            "Notification body template; ${path/to/field} expands to "
            "the corresponding JSON value from the incoming message",
            PN_NOTIFY_DEFAULT_BODY,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ICON] = g_param_spec_string (
            "icon", "Icon",
            "Freedesktop icon name (e.g. \"dialog-information\") or "
            "absolute path to an image file; empty hides the icon",
            PN_NOTIFY_DEFAULT_ICON,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_APP_NAME] = g_param_spec_string (
            "app-name", "App name",
            "Application name reported to the notification daemon; "
            "shown in the bubble header on some desktops",
            PN_NOTIFY_DEFAULT_APPNAME,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_URGENCY] = g_param_spec_enum (
            "urgency", "Urgency",
            "Freedesktop urgency hint: low / normal / critical",
            PN_TYPE_NOTIFY_URGENCY,
            PN_NOTIFY_URGENCY_NORMAL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TIMEOUT_MS] = g_param_spec_int (
            "timeout-ms", "Timeout (ms)",
            "Notification expire timeout in milliseconds; -1 = server "
            "default, 0 = never expires (user must dismiss)",
            PN_NOTIFY_TIMEOUT_MIN, PN_NOTIFY_TIMEOUT_MAX,
            PN_NOTIFY_TIMEOUT_DEFAULT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_REPLACE] = g_param_spec_boolean (
            "replace", "Replace previous",
            "Update the existing notification in place rather than "
            "stacking a new bubble for every incoming message",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_notify_init (PnNotify *self)
{
    PnNode  *node   = PN_NODE (self);
    GdkRGBA  orange = { 0.95, 0.55, 0.30, 1.0 };

    self->summary    = g_strdup (PN_NOTIFY_DEFAULT_SUMMARY);
    self->body       = g_strdup (PN_NOTIFY_DEFAULT_BODY);
    self->icon       = g_strdup (PN_NOTIFY_DEFAULT_ICON);
    self->app_name   = g_strdup (PN_NOTIFY_DEFAULT_APPNAME);
    self->urgency    = PN_NOTIFY_URGENCY_NORMAL;
    self->timeout_ms = PN_NOTIFY_TIMEOUT_DEFAULT;
    self->replace    = TRUE;
    self->bus        = NULL;
    self->last_id    = 0;

    /* The class-level icon/color are used by the palette tile; the
     * worksheet renders the per-instance values, so the instance needs
     * its own pn_node_set_icon / pn_node_set_color to avoid the
     * default grey rectangle without a glyph. */
    pn_node_set_class_name (node, "Notify");
    pn_node_set_icon       (node, PN_NOTIFY_ICON);
    pn_node_set_color      (node, &orange);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnNotify *
pn_notify_new (void)
{
    return g_object_new (PN_TYPE_NOTIFY, NULL);
}
