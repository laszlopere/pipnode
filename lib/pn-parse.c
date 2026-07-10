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

#include <json-glib/json-glib.h>

#include "pn-parse.h"
#include "pn-json-path.h"
#include "pn-message.h"

struct _PnParse
{
    PnNode parent_instance;

    gchar *source_field;   /* path to the string to parse    */
    gchar *result_field;   /* "" or "data" → replace the bag */
};

G_DEFINE_TYPE (PnParse, pn_parse, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_SOURCE_FIELD,
    PROP_RESULT_FIELD,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

#define PN_PARSE_ICON  "\xef\x84\xa1"   /* fa-code U+F121 */

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** Copy the string addressed by source-field out of @message.
 *
 *  The caller gets its own copy on purpose: the lookup root holds a
 *  reference to the *live* data bag, and a replace-the-bag result will
 *  clear that bag out from under us.  Taking the text first means the
 *  parse never reads freed memory.
 *
 *  Returns %NULL and sets @error_out (static string, do not free) when
 *  the path is missing or does not address a string. */
static gchar *
take_source_text (PnParse *self, PnMessage *message, const gchar **error_out)
{
    JsonObject *root;
    JsonNode   *node;
    gchar      *text = NULL;

    if (self->source_field == NULL || *self->source_field == '\0')
    {
        *error_out = "source-field is empty";
        return NULL;
    }

    root = pn_json_lookup_root_for_message (message);
    node = pn_json_resolve_path (root, self->source_field);

    if (node == NULL)
        *error_out = "no such field";
    else if (!JSON_NODE_HOLDS_VALUE (node) ||
             json_node_get_value_type (node) != G_TYPE_STRING)
        *error_out = "field is not a string";
    else
        text = json_node_dup_string (node);

    json_object_unref (root);
    return text;
}

/** Parse @text into a fresh JsonNode, or %NULL with @error set. */
static JsonNode *
parse_text (const gchar *text, GError **error)
{
    JsonParser *parser = json_parser_new ();
    JsonNode   *root;
    JsonNode   *result = NULL;

    if (json_parser_load_from_data (parser, text, -1, error))
    {
        root = json_parser_get_root (parser);
        /* An input of only whitespace parses "successfully" into no
         * root at all; that is not a JSON document. */
        if (root != NULL)
            result = json_node_copy (root);
        else
            g_set_error_literal (error, JSON_PARSER_ERROR,
                                 JSON_PARSER_ERROR_PARSE,
                                 "empty document");
    }

    g_object_unref (parser);
    return result;
}

/** Empty every member of @bag in place. */
static void
clear_data_bag (JsonObject *bag)
{
    GList *members = json_object_get_members (bag);
    GList *l;
    for (l = members; l != NULL; l = l->next)
        json_object_remove_member (bag, l->data);
    g_list_free (members);
}

/** Copy every member of @src into @dst, replacing same-named members. */
static void
copy_object_into (JsonObject *dst, JsonObject *src)
{
    GList *members = json_object_get_members (src);
    GList *l;
    for (l = members; l != NULL; l = l->next)
        json_object_set_member (dst, l->data,
                json_node_copy (json_object_get_member (src, l->data)));
    g_list_free (members);
}

/** Forward @message unchanged apart from data.error, and light the
 *  node's error lamp.  Deliberately non-destructive: the text we could
 *  not parse is still in the bag, which is the first thing anyone
 *  debugging a bad response body wants to look at. */
static void
fail (PnNode *node, PnMessage *message, const gchar *reason)
{
    pn_message_set_string (message, "error", reason);
    pn_node_set_has_error (node, TRUE);
    pn_node_emit_message (node, message);
}

static void
pn_parse_receive (PnNode *node, PnMessage *message)
{
    PnParse     *self  = PN_PARSE (node);
    const gchar *why   = NULL;
    GError      *error = NULL;
    gchar       *text;
    JsonNode    *result;
    JsonObject  *bag;
    const gchar *field;

    text = take_source_text (self, message, &why);
    if (text == NULL)
    {
        fail (node, message, why);
        return;
    }

    result = parse_text (text, &error);
    g_free (text);

    if (result == NULL)
    {
        fail (node, message, error ? error->message : "parse failed");
        g_clear_error (&error);
        return;
    }

    field = self->result_field ? self->result_field : "";
    bag   = pn_message_get_data (message);

    /* An empty result-field, or the literal "data", both mean the parsed
     * document *is* the new data bag — otherwise choosing "data" would
     * write data.data and leave the user with two nested bags. */
    if (*field == '\0' || g_strcmp0 (field, "data") == 0)
    {
        clear_data_bag (bag);

        if (JSON_NODE_HOLDS_OBJECT (result))
        {
            copy_object_into (bag, json_node_get_object (result));
            json_node_unref (result);
        }
        else
        {
            /* A bare scalar or array is a valid JSON document but not a
             * valid data bag; wrap it so the bag stays an object. */
            json_object_set_member (bag, "value", result);
        }
    }
    else
    {
        pn_message_set_member (message, field, result);
    }

    if (json_object_has_member (bag, "error"))
        json_object_remove_member (bag, "error");
    pn_node_set_has_error (node, FALSE);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_parse_get_property (GObject *object, guint prop_id,
                       GValue *value, GParamSpec *pspec)
{
    PnParse *self = PN_PARSE (object);

    switch (prop_id)
    {
    case PROP_SOURCE_FIELD:
        g_value_set_string (value, self->source_field);
        break;
    case PROP_RESULT_FIELD:
        g_value_set_string (value, self->result_field);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_string_prop (gchar **slot, const GValue *value, GObject *object,
                 GParamSpec *pspec)
{
    const gchar *s = g_value_get_string (value);
    if (g_strcmp0 (*slot, s) == 0) return;
    g_free (*slot);
    *slot = g_strdup (s ? s : "");
    g_object_notify_by_pspec (object, pspec);
}

static void
pn_parse_set_property (GObject *object, guint prop_id,
                       const GValue *value, GParamSpec *pspec)
{
    PnParse *self = PN_PARSE (object);

    switch (prop_id)
    {
    case PROP_SOURCE_FIELD:
        set_string_prop (&self->source_field, value, object,
                         props[PROP_SOURCE_FIELD]);
        break;
    case PROP_RESULT_FIELD:
        set_string_prop (&self->result_field, value, object,
                         props[PROP_RESULT_FIELD]);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_parse_finalize (GObject *object)
{
    PnParse *self = PN_PARSE (object);

    g_clear_pointer (&self->source_field, g_free);
    g_clear_pointer (&self->result_field, g_free);

    G_OBJECT_CLASS (pn_parse_parent_class)->finalize (object);
}

static void
pn_parse_class_init (PnParseClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_parse_get_property;
    object_class->set_property = pn_parse_set_property;
    object_class->finalize     = pn_parse_finalize;
    node_class->receive        = pn_parse_receive;

    node_class->class_name     = "Parse JSON";
    node_class->icon           = PN_PARSE_ICON;
    node_class->color          = (PnColor){ 0.33, 0.58, 0.75, 1.0 };
    node_class->category       = "Filters/Expressions";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_SOURCE_FIELD] = g_param_spec_string (
            "source-field", "Source field",
            "JSON pointer (slash-separated path) to the string that "
            "should be parsed. The default \"data/output\" is where "
            "Http Client leaves a response body.",
            "data/output",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RESULT_FIELD] = g_param_spec_string (
            "result-field", "Result field",
            "Where to put the parsed document. A non-empty name "
            "(default \"result\") writes to that member of the data "
            "bag, leaving the surrounding members in place. \"\" or "
            "\"data\" replace the entire data bag with the document; "
            "a non-object document is wrapped under a \"value\" key.",
            "result",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_parse_init (PnParse *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.33, 0.58, 0.75, 1.0 };

    self->source_field = g_strdup ("data/output");
    self->result_field = g_strdup ("result");

    pn_node_set_class_name (node, "Parse JSON");
    pn_node_set_icon       (node, PN_PARSE_ICON);
    pn_node_set_color      (node, &blue);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnParse *
pn_parse_new (void)
{
    return g_object_new (PN_TYPE_PARSE, NULL);
}
