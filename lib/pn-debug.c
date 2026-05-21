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

#include "pn-debug.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>

struct _PnDebug
{
    PnNode parent_instance;

    PnDebugTarget target;
    PnDebugFormat format;
};

G_DEFINE_TYPE (PnDebug, pn_debug, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_TARGET,
    PROP_FORMAT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

enum {
    SIG_STATUS_MESSAGE,
    SIG_DEBUG_MESSAGE,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------ */
/*  Enum boxed types                                                   */
/* ------------------------------------------------------------------ */

GType
pn_debug_target_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        /* Listed in the order the target combo should present them;
           the numeric values stay fixed so saved projects keep working. */
        static const GEnumValue values[] = {
            { PN_DEBUG_TARGET_DEBUG_VIEW,
              "PN_DEBUG_TARGET_DEBUG_VIEW", "Debug View"      },
            { PN_DEBUG_TARGET_STATUS_BAR,
              "PN_DEBUG_TARGET_STATUS_BAR", "Status Bar"      },
            { PN_DEBUG_TARGET_STDOUT,
              "PN_DEBUG_TARGET_STDOUT",     "Standard Output" },
            { PN_DEBUG_TARGET_STDERR,
              "PN_DEBUG_TARGET_STDERR",     "Standard Error"  },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnDebugTarget", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

GType
pn_debug_format_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_DEBUG_FORMAT_TEXT,
              "PN_DEBUG_FORMAT_TEXT",     "text"     },
            { PN_DEBUG_FORMAT_ONELINER,
              "PN_DEBUG_FORMAT_ONELINER", "oneliner" },
            { PN_DEBUG_FORMAT_JSON,
              "PN_DEBUG_FORMAT_JSON",     "JSON"     },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnDebugFormat", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Pick the user-facing label for a node: prefer the assigned
 *  #PnNode:name and fall back to its class name when unset. */
static const gchar *
display_name (PnNode *node)
{
    const gchar *n;

    if (node == NULL)
        return NULL;

    n = pn_node_get_name (node);
    if (n != NULL && *n != '\0')
        return n;

    return pn_node_get_class_name (node);
}

/** Build a #JsonObject describing @message: source/topic/id metadata
 *  alongside the message's data bag (recursively, via JSON-GLib's
 *  reference semantics).  Returned object is owned by the caller. */
static JsonObject *
message_to_json_object (
        PnNode    *self,
        PnMessage *message)
{
    PnNode      *source    = pn_message_get_source  (message);
    const gchar *src_name  = display_name (source);
    const gchar *src_uuid  = source != NULL
                             ? pn_node_get_uuid (source) : NULL;
    const gchar *id        = pn_message_get_id      (message);
    const gchar *topic     = pn_message_get_topic   (message);
    const gchar *created   = pn_message_get_created (message);
    JsonObject  *data      = pn_message_get_data    (message);
    JsonObject  *obj       = json_object_new ();
    JsonNode    *data_node = json_node_new (JSON_NODE_OBJECT);

    (void) self;

    /* Concrete class of the message object (PnMessage, PnImageMessage,
     * …) so the debug pane can title each entry with its type. */
    json_object_set_string_member (obj, "type",
                                   G_OBJECT_TYPE_NAME (message));

    json_object_set_string_member (obj, "from",
                                   src_name  ? src_name  : "");
    /* Stable counterpart to "from": the source node's persistent
     * UUID, suitable for unambiguous lookup (the debug pane uses it
     * to focus the originating node on click).  Falls through as the
     * empty string when there is no source — message_to_json_object
     * is called from synthesised contexts too. */
    json_object_set_string_member (obj, "from_id",
                                   src_uuid  ? src_uuid  : "");
    json_object_set_string_member (obj, "topic",
                                   topic     ? topic     : "");
    json_object_set_string_member (obj, "id",
                                   id        ? id        : "");
    json_object_set_string_member (obj, "created",
                                   created   ? created   : "");

    /* Reference, do not copy: callers consuming the returned object
     * via JsonGenerator only read it. */
    json_node_set_object   (data_node, data);
    json_object_set_member (obj, "data", data_node);

    return obj;
}

/** Render @object as compact (single-line) JSON. */
static gchar *
json_object_to_compact_string (JsonObject *object)
{
    JsonNode      *root = json_node_new (JSON_NODE_OBJECT);
    JsonGenerator *gen  = json_generator_new ();
    gchar         *out;

    json_node_set_object    (root, object);
    json_generator_set_root (gen, root);
    out = json_generator_to_data (gen, NULL);

    json_node_free (root);
    g_object_unref (gen);

    return out;
}

/** Render @object as multi-line, indented JSON suitable for a debug
 *  log.  The trailing newline that the generator does not emit is
 *  added by the caller when writing to its target. */
static gchar *
json_object_to_pretty_string (JsonObject *object)
{
    JsonNode      *root = json_node_new (JSON_NODE_OBJECT);
    JsonGenerator *gen  = json_generator_new ();
    gchar         *out;

    json_node_set_object    (root, object);
    json_generator_set_root (gen, root);
    json_generator_set_pretty (gen, TRUE);
    json_generator_set_indent (gen, 2);
    out = json_generator_to_data (gen, NULL);

    json_node_free (root);
    g_object_unref (gen);

    return out;
}

static gchar *
format_oneliner (
        PnNode    *self,
        PnMessage *message)
{
    PnNode      *source       = pn_message_get_source  (message);
    const gchar *self_name    = display_name (self);
    const gchar *source_name  = display_name (source);
    const gchar *id           = pn_message_get_id      (message);
    const gchar *topic        = pn_message_get_topic   (message);
    const gchar *created      = pn_message_get_created (message);
    JsonObject  *data         = pn_message_get_data    (message);
    gchar       *data_text    = json_object_to_compact_string (data);
    gchar       *result;

    result = g_strdup_printf (
            "[%s] from=%s topic=%s id=%s created=%s data=%s",
            self_name    ? self_name    : "debug",
            source_name  ? source_name  : "(none)",
            topic        ? topic        : "(none)",
            id           ? id           : "(none)",
            created      ? created      : "(none)",
            data_text    ? data_text    : "{}");

    g_free (data_text);
    return result;
}

static gchar *
format_json (
        PnNode    *self,
        PnMessage *message)
{
    JsonObject *obj    = message_to_json_object (self, message);
    gchar      *result = json_object_to_pretty_string (obj);

    json_object_unref (obj);
    return result;
}

/** Render just the message's "output" string from its data bag.
 *  Designed for sinks that want a clean, human-readable line per
 *  message; returns an empty string when "output" is absent or not a
 *  string so the surrounding line break still marks the event. */
static gchar *
format_text (
        PnNode    *self,
        PnMessage *message)
{
    JsonObject *data = pn_message_get_data (message);
    JsonNode   *node;

    (void) self;

    if (data == NULL || !json_object_has_member (data, "output"))
        return g_strdup ("");

    node = json_object_get_member (data, "output");
    if (!JSON_NODE_HOLDS_VALUE (node) ||
        json_node_get_value_type (node) != G_TYPE_STRING)
        return g_strdup ("");

    return g_strdup (json_node_get_string (node));
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_debug_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnDebug *self = PN_DEBUG (node);
    gchar   *text;

    switch (self->format)
    {
    case PN_DEBUG_FORMAT_JSON:
        text = format_json     (node, message);
        break;
    case PN_DEBUG_FORMAT_ONELINER:
        text = format_oneliner (node, message);
        break;
    case PN_DEBUG_FORMAT_TEXT:
    default:
        text = format_text     (node, message);
        break;
    }

    switch (self->target)
    {
    case PN_DEBUG_TARGET_STDOUT:
        g_print    ("%s\n", text);
        break;
    case PN_DEBUG_TARGET_STATUS_BAR:
        g_signal_emit (self, signals[SIG_STATUS_MESSAGE], 0, text);
        break;
    case PN_DEBUG_TARGET_DEBUG_VIEW:
        g_signal_emit (self, signals[SIG_DEBUG_MESSAGE], 0, text);
        break;
    case PN_DEBUG_TARGET_STDERR:
    default:
        g_printerr ("%s\n", text);
        break;
    }

    g_free (text);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_debug_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnDebug *self = PN_DEBUG (object);

    switch (prop_id)
    {
    case PROP_TARGET:
        g_value_set_enum (value, self->target);
        break;
    case PROP_FORMAT:
        g_value_set_enum (value, self->format);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_debug_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnDebug *self = PN_DEBUG (object);

    switch (prop_id)
    {
    case PROP_TARGET:
        {
            PnDebugTarget t = g_value_get_enum (value);
            if (self->target != t)
            {
                self->target = t;
                g_object_notify_by_pspec (object, props[PROP_TARGET]);
            }
        }
        break;
    case PROP_FORMAT:
        {
            PnDebugFormat f = g_value_get_enum (value);
            if (self->format != f)
            {
                self->format = f;
                g_object_notify_by_pspec (object, props[PROP_FORMAT]);
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
pn_debug_class_init (PnDebugClass *klass)
{
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    node_class->receive        = pn_debug_receive;
    object_class->get_property = pn_debug_get_property;
    object_class->set_property = pn_debug_set_property;

    node_class->class_name     = "Debug Print";
    node_class->icon           = "\xef\x86\x88";  /* fa-bug U+F188 */
    node_class->color          = (GdkRGBA){ 0.34, 0.66, 0.49, 1.0 };
    node_class->category       = "Sinks";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    props[PROP_TARGET] = g_param_spec_enum (
            "target", "Target",
            "Where rendered messages are written",
            PN_TYPE_DEBUG_TARGET,
            PN_DEBUG_TARGET_STDERR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FORMAT] = g_param_spec_enum (
            "format", "Format",
            "How each message is rendered",
            PN_TYPE_DEBUG_FORMAT,
            PN_DEBUG_FORMAT_JSON,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Emitted when target == status bar.  The worksheet listens and
     * forwards the text up to the main window. */
    signals[SIG_STATUS_MESSAGE] = g_signal_new (
            "status-message",
            PN_TYPE_DEBUG,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);

    /* Emitted when target == debug view.  Routed up to the main
     * window's collapsible debug pane. */
    signals[SIG_DEBUG_MESSAGE] = g_signal_new (
            "debug-message",
            PN_TYPE_DEBUG,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);
}

static void
pn_debug_init (PnDebug *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  green = { 0.34, 0.66, 0.49, 1.0 };

    self->target = PN_DEBUG_TARGET_STDERR;
    self->format = PN_DEBUG_FORMAT_JSON;

    pn_node_set_class_name (node, "Debug Print");
    pn_node_set_icon       (node, "\xef\x86\x88");      /* fa-bug U+F188 */
    pn_node_set_color      (node, &green);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDebug *
pn_debug_new (void)
{
    return g_object_new (PN_TYPE_DEBUG, NULL);
}
