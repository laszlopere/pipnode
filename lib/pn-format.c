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

#include "pn-format.h"
#include "pn-json-path.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <string.h>

struct _PnFormat
{
    PnNode parent_instance;

    gchar *success_text;
    gchar *failure_text;
};

G_DEFINE_TYPE (PnFormat, pn_format, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_SUCCESS_TEXT,
    PROP_FAILURE_TEXT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Read a boolean member from @data.  Returns %TRUE only when the
 *  member is present and holds a boolean value; @out_value is left
 *  untouched otherwise. */
static gboolean
read_boolean_member (
        JsonObject  *data,
        const gchar *name,
        gboolean    *out_value)
{
    JsonNode *n;

    if (data == NULL || !json_object_has_member (data, name))
        return FALSE;

    n = json_object_get_member (data, name);
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_BOOLEAN)
        return FALSE;

    *out_value = json_node_get_boolean (n);
    return TRUE;
}

/** Render a #JsonNode as a freshly-allocated string suitable for
 *  inlining into the output template.  Scalars use their natural
 *  textual form; objects and arrays fall back to compact JSON so a
 *  template author still gets something readable when they reference
 *  a non-leaf path. */
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
            return g_strdup (json_node_get_boolean (node) ? "true" : "false");
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


/** Replace every "${path}" occurrence in @tmpl with the string form
 *  of the JSON value at that path.  Unknown paths render as the empty
 *  string; an unterminated "${" is emitted verbatim. */
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
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_format_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnFormat    *self    = PN_FORMAT (node);
    JsonObject  *data    = pn_message_get_data (message);
    gboolean     success = FALSE;
    const gchar *tmpl;
    JsonObject  *root;
    gchar       *expanded;

    read_boolean_member (data, "success", &success);

    tmpl = success ? self->success_text : self->failure_text;
    if (tmpl == NULL)
        tmpl = "";

    root     = pn_json_lookup_root_for_message (message);
    expanded = expand_placeholders (tmpl, root);
    json_object_unref (root);

    pn_message_set_string (message, "output", expanded);
    g_free (expanded);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_format_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnFormat *self = PN_FORMAT (object);

    switch (prop_id)
    {
    case PROP_SUCCESS_TEXT:
        g_value_set_string (value, self->success_text);
        break;
    case PROP_FAILURE_TEXT:
        g_value_set_string (value, self->failure_text);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_format_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnFormat *self = PN_FORMAT (object);

    switch (prop_id)
    {
    case PROP_SUCCESS_TEXT:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->success_text, s) != 0)
            {
                g_free (self->success_text);
                self->success_text = g_strdup (s ? s : "");
                g_object_notify_by_pspec (object, props[PROP_SUCCESS_TEXT]);
            }
        }
        break;
    case PROP_FAILURE_TEXT:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->failure_text, s) != 0)
            {
                g_free (self->failure_text);
                self->failure_text = g_strdup (s ? s : "");
                g_object_notify_by_pspec (object, props[PROP_FAILURE_TEXT]);
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
pn_format_finalize (GObject *object)
{
    PnFormat *self = PN_FORMAT (object);

    g_clear_pointer (&self->success_text, g_free);
    g_clear_pointer (&self->failure_text, g_free);

    G_OBJECT_CLASS (pn_format_parent_class)->finalize (object);
}

static void
pn_format_class_init (PnFormatClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_format_get_property;
    object_class->set_property = pn_format_set_property;
    object_class->finalize     = pn_format_finalize;
    node_class->receive        = pn_format_receive;

    node_class->class_name     = "Format";
    node_class->icon           = "\xef\x84\xa1";  /* fa-code U+F121 */
    node_class->color          = (GdkRGBA){ 0.40, 0.66, 0.78, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_SUCCESS_TEXT] = g_param_spec_string (
            "success-text", "Success text",
            "Template written to data.output when data.success is true; "
            "${path/to/field} expands to the corresponding JSON value",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FAILURE_TEXT] = g_param_spec_string (
            "failure-text", "Failure text",
            "Template written to data.output when data.success is false "
            "or absent; ${path/to/field} expands to JSON values",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_format_init (PnFormat *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  blue = { 0.40, 0.66, 0.78, 1.0 };

    self->success_text = g_strdup ("");
    self->failure_text = g_strdup ("");

    pn_node_set_class_name (node, "Format");
    pn_node_set_icon       (node, "\xef\x84\xa1");  /* fa-code U+F121 */
    pn_node_set_color      (node, &blue);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnFormat *
pn_format_new (void)
{
    return g_object_new (PN_TYPE_FORMAT, NULL);
}
