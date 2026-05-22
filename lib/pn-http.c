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

#include "pn-http.h"

#include <string.h>

/* Visual states.  The icon panel renders in white, so the body colour
 * carries the alert for the warning state. */
#define PN_HTTP_NORMAL_ICON  "\xef\x82\xac"      /* fa-globe U+F0AC */
#define PN_HTTP_WARNING_ICON "\xe2\x9d\x97"      /* ❗ U+2757  */

#define PN_HTTP_DEFAULT_PERIOD 5u

/* Sentinel curl writes between the response body and the HTTP status
 * code via -w.  Picked to be unlikely to appear in real payloads; we
 * still split on the *last* occurrence to be safe. */
#define PN_HTTP_STATUS_SENTINEL "\n--PN-HTTP-STATUS--"

/* Default body colour applied when a subclass leaves
 * PnHttpClass.normal_color zero-initialised. */
static const GdkRGBA PN_HTTP_DEFAULT_COLOR = { 0.40, 0.70, 0.45, 1.0 };

typedef struct
{
    /* The worker thread reads @url while the main thread may
     * overwrite it via the property setter, so accesses are
     * serialised through @mutex.  The string itself is owned by the
     * node and freed in finalize. */
    GMutex  mutex;
    gchar  *url;
} PnHttpPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PnHttp, pn_http, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_URL,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* Forward declarations of the default vfunc implementations so the
 * trigger and class_init can both reach them. */
static gboolean default_is_configured (PnHttp *self);
static gchar   *default_build_command (PnHttp *self, guint timeout);
static void     default_emit_message  (PnHttp      *self,
                                       gboolean     spawned,
                                       gint         exit_status,
                                       const gchar *stdout_text,
                                       const gchar *stderr_text);

/* ------------------------------------------------------------------ */
/*  URL accessor (thread-safe)                                         */
/* ------------------------------------------------------------------ */

gchar *
pn_http_dup_url (PnHttp *self)
{
    PnHttpPrivate *priv;
    gchar         *copy;

    g_return_val_if_fail (PN_IS_HTTP (self), NULL);

    priv = pn_http_get_instance_private (self);

    g_mutex_lock (&priv->mutex);
    copy = g_strdup (priv->url);
    g_mutex_unlock (&priv->mutex);

    return copy;
}

static void
http_set_url (
        PnHttp      *self,
        const gchar *url)
{
    PnHttpPrivate *priv = pn_http_get_instance_private (self);
    PnHttpClass   *klass = PN_HTTP_GET_CLASS (self);
    gchar         *old;
    gchar         *replacement;

    /* Treat NULL and "" as the same unconfigured state but still
     * round-trip whatever the caller passed so `notify::url` reports
     * a stable value. */
    replacement = (url != NULL) ? g_strdup (url) : NULL;

    g_mutex_lock (&priv->mutex);
    old = priv->url;
    priv->url = replacement;
    g_mutex_unlock (&priv->mutex);

    g_free (old);

    /* Subclasses with multi-field configuration override is_configured
     * to take their own state into account; calling through the vfunc
     * keeps the visual flip correct in either case. */
    pn_http_apply_visual_state (self, klass->is_configured (self));
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_URL]);
}

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

void
pn_http_apply_visual_state (
        PnHttp  *self,
        gboolean configured)
{
    PnHttpClass *klass;
    PnNode      *node;

    g_return_if_fail (PN_IS_HTTP (self));

    klass = PN_HTTP_GET_CLASS (self);
    node  = PN_NODE (self);

    if (configured)
    {
        const gchar *icon = klass->normal_icon
                ? klass->normal_icon
                : PN_HTTP_NORMAL_ICON;

        /* Subclasses that leave normal_color zero-initialised get the
         * base class's green; an explicit alpha of 0 is treated as
         * "not set" since a subclass picking a transparent body would
         * be surprising. */
        const GdkRGBA *color = (klass->normal_color.alpha > 0.0)
                ? &klass->normal_color
                : &PN_HTTP_DEFAULT_COLOR;

        pn_node_set_color (node, color);
        pn_node_set_icon  (node, icon);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_HTTP_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Output parsing                                                     */
/* ------------------------------------------------------------------ */

void
pn_http_split_body_and_status (
        const gchar  *stdout_text,
        gchar       **out_body,
        gint         *out_status)
{
    const gchar *sep;
    const gchar *status_text;

    g_return_if_fail (out_body   != NULL);
    g_return_if_fail (out_status != NULL);

    *out_body   = NULL;
    *out_status = 0;

    if (stdout_text == NULL)
    {
        *out_body = g_strdup ("");
        return;
    }

    sep = g_strrstr (stdout_text, PN_HTTP_STATUS_SENTINEL);
    if (sep == NULL)
    {
        *out_body = g_strdup (stdout_text);
        return;
    }

    *out_body   = g_strndup (stdout_text, (gsize) (sep - stdout_text));
    status_text = sep + strlen (PN_HTTP_STATUS_SENTINEL);
    *out_status = (gint) g_ascii_strtoll (status_text, NULL, 10);
}

/* ------------------------------------------------------------------ */
/*  Default vfunc implementations                                      */
/* ------------------------------------------------------------------ */

static gboolean
default_is_configured (PnHttp *self)
{
    gchar    *url = pn_http_dup_url (self);
    gboolean  ok  = (url != NULL && *url != '\0');

    g_free (url);
    return ok;
}

static gchar *
default_build_command (
        PnHttp *self,
        guint   timeout)
{
    gchar *url        = pn_http_dup_url (self);
    /* Interpolate ${nodeclass} / ${nodename} / ${hostname} into the URL;
     * any other ${...} is left verbatim. */
    gchar *expanded   = pn_node_expand_vars (PN_NODE (self), url);
    gchar *quoted_url = g_shell_quote (expanded);
    gchar *cmd;

    cmd = g_strdup_printf (
            "curl --silent --show-error --location "
            "--max-time %u "
            "--write-out '%s%%{http_code}' "
            "%s",
            timeout, PN_HTTP_STATUS_SENTINEL, quoted_url);

    g_free (quoted_url);
    g_free (expanded);
    g_free (url);
    return cmd;
}

static void
default_emit_message (
        PnHttp      *self,
        gboolean     spawned,
        gint         exit_status,
        const gchar *stdout_text,
        const gchar *stderr_text)
{
    PnAutoTrigger *trigger     = PN_AUTO_TRIGGER (self);
    PnNode        *node        = PN_NODE (self);
    gchar         *url         = pn_http_dup_url (self);
    gchar         *body        = NULL;
    gint           http_status = 0;
    gboolean       success     = FALSE;
    PnMessage     *msg;

    (void) stderr_text;

    if (spawned)
    {
        gboolean exited_ok = g_spawn_check_wait_status (exit_status, NULL);

        pn_http_split_body_and_status (stdout_text, &body, &http_status);

        success = exited_ok
                  && http_status >= 200
                  && http_status < 300;
    }
    else
    {
        body = g_strdup ("");
    }

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "url",     url ? url : "");
    pn_message_set_boolean (msg, "success", success);
    if (http_status > 0)
        pn_message_set_int (msg, "status", http_status);
    pn_message_set_string (msg, "output", body ? body : "");

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_free (body);
    g_free (url);
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger.  The     */
/*  base class drives the curl spawn loop and dispatches to the      */
/*  vfuncs so subclasses customise per-request behaviour without     */
/*  re-implementing the spawn boilerplate.                            */
/* ------------------------------------------------------------------ */

static void
pn_http_trigger (PnAutoTrigger *trigger)
{
    PnHttp      *self  = PN_HTTP (trigger);
    PnHttpClass *klass = PN_HTTP_GET_CLASS (self);
    gchar       *cmd;
    gchar       *stdout_text = NULL;
    gchar       *stderr_text = NULL;
    gint         exit_status = 0;
    GError      *error       = NULL;
    gboolean     spawned;
    guint        period;
    guint        timeout;

    /* Skip work entirely while the node is in its "configuration
     * required" state.  The visual marker on the canvas already tells
     * the user; emitting nothing keeps downstream nodes idle. */
    if (!klass->is_configured (self))
        return;

    /* Cap curl --max-time one below the period so a dead host never
     * delays the next tick. */
    period  = pn_auto_trigger_get_period (trigger);
    timeout = (period > 1u) ? period - 1u : 1u;

    cmd = klass->build_command (self, timeout);

    spawned = g_spawn_command_line_sync (cmd,
                                         &stdout_text,
                                         &stderr_text,
                                         &exit_status,
                                         &error);

    if (!spawned)
    {
        g_warning ("pn-http: failed to spawn '%s': %s",
                   cmd, error ? error->message : "(unknown)");
        g_clear_error (&error);
    }

    klass->emit_message (self, spawned, exit_status, stdout_text, stderr_text);

    g_free (cmd);
    g_free (stdout_text);
    g_free (stderr_text);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_http_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnHttp *self = PN_HTTP (object);

    switch (prop_id)
    {
    case PROP_URL:
        g_value_take_string (value, pn_http_dup_url (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_http_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnHttp *self = PN_HTTP (object);

    switch (prop_id)
    {
    case PROP_URL:
        http_set_url (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_http_finalize (GObject *object)
{
    PnHttp        *self = PN_HTTP (object);
    PnHttpPrivate *priv = pn_http_get_instance_private (self);

    g_clear_pointer (&priv->url, g_free);
    g_mutex_clear (&priv->mutex);

    G_OBJECT_CLASS (pn_http_parent_class)->finalize (object);
}

static void
pn_http_class_init (PnHttpClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_http_get_property;
    object_class->set_property = pn_http_set_property;
    object_class->finalize     = pn_http_finalize;
    trigger_class->trigger     = pn_http_trigger;

    /* Class-level visual identity, picked up by
     * pn_http_apply_visual_state().  Subclasses replace these in
     * their own class_init. */
    klass->normal_icon  = PN_HTTP_NORMAL_ICON;
    klass->normal_color = PN_HTTP_DEFAULT_COLOR;

    /* Default vfuncs.  Subclasses override these to customise the
     * request / response handling. */
    klass->is_configured = default_is_configured;
    klass->build_command = default_build_command;
    klass->emit_message  = default_emit_message;

    /* The instance icon flips between the normal glyph and ❗
     * depending on configuration.  The palette wants a stable glyph
     * regardless of state, so pin the normal one here. */
    node_class->palette_icon = PN_HTTP_NORMAL_ICON;
    node_class->class_name   = "Http Client";
    node_class->icon         = PN_HTTP_NORMAL_ICON;
    node_class->color        = PN_HTTP_DEFAULT_COLOR;
    node_class->category     = "Network";
    node_class->has_input    = FALSE;
    node_class->has_output   = TRUE;

    props[PROP_URL] = g_param_spec_string (
            "url", "URL",
            "URL to fetch with HTTP GET; while empty the node is "
            "marked as needing configuration and emits nothing",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_http_init (PnHttp *self)
{
    PnHttpPrivate *priv = pn_http_get_instance_private (self);
    PnNode        *node = PN_NODE (self);

    g_mutex_init (&priv->mutex);
    priv->url = NULL;

    pn_node_set_class_name (node, "Http Client");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* Default period is more relaxed than the base-class 1s so a
     * fresh http node does not immediately hammer the target. */
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_HTTP_DEFAULT_PERIOD);

    /* Start in the unconfigured (red ❗) state; setting the URL (or,
     * for subclasses, completing their configuration) flips it to the
     * normal appearance via pn_http_apply_visual_state(). */
    pn_http_apply_visual_state (self, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnHttp *
pn_http_new (void)
{
    return g_object_new (PN_TYPE_HTTP, NULL);
}
