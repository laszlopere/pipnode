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

/* Visual states.  The icon panel renders in white, so the body colour
 * carries the alert for the warning state. */
#define PN_HTTP_NORMAL_ICON  "\xef\x82\xac"      /* fa-globe U+F0AC */

#define PN_HTTP_DEFAULT_PERIOD 5u

/* Default body colour applied when a subclass leaves
 * PnHttpClass.normal_color zero-initialised. */
static const PnColor PN_HTTP_DEFAULT_COLOR = { 0.40, 0.70, 0.45, 1.0 };

typedef struct
{
    /* The worker thread reads @url while the main thread may
     * overwrite it via the property setter, so accesses are
     * serialised through @mutex.  The string itself is owned by the
     * node and freed in finalize. */
    GMutex  mutex;
    gchar  *url;

    /* Lazily created on the worker thread and reused across ticks for
     * connection keep-alive.  Only ever touched on the worker thread,
     * which #PnAutoTrigger joins in dispose before this node's
     * finalize runs, so no locking is needed and finalize can unref
     * it safely. */
    SoupSession *session;
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
static gboolean     default_is_configured (PnHttp *self);
static SoupMessage *default_build_request (PnHttp *self);
static void         default_emit_message  (PnHttp      *self,
                                           gboolean     ok,
                                           gint         http_status,
                                           const gchar *body,
                                           const gchar *error);

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

    {
        const gchar *icon = klass->normal_icon
                ? klass->normal_icon
                : PN_HTTP_NORMAL_ICON;

        /* Subclasses that leave normal_color zero-initialised get the
         * base class's green; an explicit alpha of 0 is treated as
         * "not set" since a subclass picking a transparent body would
         * be surprising. */
        const PnColor *color = (klass->normal_color.alpha > 0.0)
                ? &klass->normal_color
                : &PN_HTTP_DEFAULT_COLOR;

        /* Keep the healthy identity at all times; the red body + ❗
         * overlay for the unconfigured state is painted centrally by
         * the worksheet whenever has-error is set. */
        pn_node_set_color     (node, color);
        pn_node_set_icon      (node, icon);
        pn_node_set_has_error (node, !configured);
    }
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

static SoupMessage *
default_build_request (PnHttp *self)
{
    gchar       *url      = pn_http_dup_url (self);
    /* Interpolate ${nodeclass} / ${nodename} / ${hostname} into the URL;
     * any other ${...} is left verbatim. */
    gchar       *expanded = pn_node_expand_vars (PN_NODE (self), url);
    SoupMessage *msg      = soup_message_new (SOUP_METHOD_GET, expanded);

    g_free (expanded);
    g_free (url);
    return msg;   /* NULL when @expanded is not a valid URL */
}

static void
default_emit_message (
        PnHttp      *self,
        gboolean     ok,
        gint         http_status,
        const gchar *body,
        const gchar *error)
{
    PnAutoTrigger *trigger = PN_AUTO_TRIGGER (self);
    PnNode        *node    = PN_NODE (self);
    gchar         *url     = pn_http_dup_url (self);
    gboolean       success = ok && http_status >= 200 && http_status < 300;
    PnMessage     *msg;

    (void) error;

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "url",     url ? url : "");
    pn_message_set_boolean (msg, "success", success);
    if (http_status > 0)
        pn_message_set_int (msg, "status", http_status);
    pn_message_set_string (msg, "output", body ? body : "");

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_free (url);
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger.  The     */
/*  base class drives the blocking libsoup request and dispatches to  */
/*  the vfuncs so subclasses customise per-request behaviour without  */
/*  re-implementing the transport boilerplate.                        */
/* ------------------------------------------------------------------ */

static void
pn_http_trigger (PnAutoTrigger *trigger)
{
    PnHttp        *self  = PN_HTTP (trigger);
    PnHttpClass   *klass = PN_HTTP_GET_CLASS (self);
    PnHttpPrivate *priv  = pn_http_get_instance_private (self);
    SoupMessage   *msg;
    GBytes        *bytes;
    GError        *error = NULL;
    guint          period;
    guint          timeout;

    /* Skip work entirely while the node is in its "configuration
     * required" state.  The visual marker on the canvas already tells
     * the user; emitting nothing keeps downstream nodes idle. */
    if (!klass->is_configured (self))
        return;

    msg = klass->build_request (self);
    if (msg == NULL)
    {
        /* soup_message_new() rejected the URL as malformed. */
        klass->emit_message (self, FALSE, 0, "", "invalid URL");
        return;
    }

    if (priv->session == NULL)
        priv->session = soup_session_new ();

    /* Bound the request so a dead or slow host never delays the next
     * tick: cap socket I/O one second below the period.  Unlike curl's
     * --max-time this is a per-operation (connect / stalled-read)
     * timeout rather than a wall-clock cap on the whole transfer, which
     * still meets the goal and will not abort a large but steady
     * download. */
    period  = pn_auto_trigger_get_period (trigger);
    timeout = (period > 1u) ? period - 1u : 1u;
    soup_session_set_timeout (priv->session, timeout);

    /* Synchronous send on the worker thread; libsoup follows redirects
     * by default.  A NULL return is a transport failure (DNS, connect,
     * timeout); an HTTP 4xx/5xx still returns the body with the status
     * recorded on @msg. */
    bytes = soup_session_send_and_read (priv->session, msg, NULL, &error);

    if (bytes == NULL)
    {
        const gchar *reason = error ? error->message : "(unknown)";

        pn_auto_trigger_log_on_main (
                PN_AUTO_TRIGGER (self), PN_LOG_LEVEL_ERROR,
                "Request failed: %s", reason);
        klass->emit_message (self, FALSE, 0, "", reason);
    }
    else
    {
        gsize        len  = 0;
        const gchar *data = g_bytes_get_data (bytes, &len);
        gchar       *body = g_strndup (data ? data : "", len);
        gint         http_status = (gint) soup_message_get_status (msg);

        klass->emit_message (self, TRUE, http_status, body, NULL);

        g_free (body);
        g_bytes_unref (bytes);
    }

    g_clear_error (&error);
    g_object_unref (msg);
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

    /* The worker thread (the only user of @session) was joined by
     * #PnAutoTrigger's dispose, so unreffing here is race-free. */
    g_clear_object (&priv->session);
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
    klass->build_request = default_build_request;
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

    /* Class label is pinned on PnNodeClass.class_name in class_init and
     * resolved through pn_node_get_class_name()'s class fallback; do NOT
     * seed it per-instance here.  PnHttp is a base class (PnRate,
     * PnWeather subclass it), and a per-instance "Http Client" would
     * shadow the label those subclasses pin in their own class_init. */
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
