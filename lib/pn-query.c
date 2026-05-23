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

#include "pn-query.h"
#include "pn-jmespath.h"
#include "pn-json-path.h"
#include "pn-message.h"

struct _PnQuery
{
    PnNode parent_instance;

    gchar *expression;
    gchar *result_field;
    gchar *source_field;   /* "" or NULL → query the whole message root  */
};

G_DEFINE_TYPE (PnQuery, pn_query, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_EXPRESSION,
    PROP_RESULT_FIELD,
    PROP_SOURCE_FIELD,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** Pick the JsonNode the query should run against.  When source-field
 *  is empty we query the whole message root (topic / id / created /
 *  data); otherwise we resolve the path through the same /-separated
 *  pointer the rest of the codebase uses, and fall back to a JSON
 *  null when the path doesn't resolve. */
static JsonNode *
build_input (PnQuery *self, PnMessage *message, JsonObject **out_root)
{
    JsonObject *root = pn_json_lookup_root_for_message (message);
    JsonNode   *node;

    *out_root = root;

    if (self->source_field == NULL || *self->source_field == '\0')
    {
        node = json_node_alloc ();
        json_node_init_object (node, root);
        return node;
    }

    {
        JsonNode *resolved = pn_json_resolve_path (root, self->source_field);
        if (resolved != NULL)
            return json_node_copy (resolved);

        node = json_node_alloc ();
        json_node_init_null (node);
        return node;
    }
}

/** Empty every member of @bag in place — used when the result is
 *  about to take over the data bag wholesale, so we don't carry
 *  stale fields the user did not ask to keep. */
static void
clear_data_bag (JsonObject *bag)
{
    GList *members = json_object_get_members (bag);
    GList *l;
    for (l = members; l != NULL; l = l->next)
        json_object_remove_member (bag, l->data);
    g_list_free (members);
}

/** Copy every member of @src into @dst, replacing any member of the
 *  same name. */
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

static void
pn_query_receive (PnNode *node, PnMessage *message)
{
    PnQuery     *self  = PN_QUERY (node);
    JsonObject  *root  = NULL;
    JsonObject  *bag   = pn_message_get_data (message);
    JsonNode    *input;
    JsonNode    *result;
    GError      *error = NULL;
    const gchar *field;
    gboolean     replace_bag;

    field = self->result_field ? self->result_field : "";
    /* An empty result-field, or the literal "data", both mean the
     * query result *is* the new data bag.  Without this, picking
     * "data" would just write data.data = result and the user ends
     * up with two nested data fields. */
    replace_bag = (*field == '\0') || g_strcmp0 (field, "data") == 0;

    input  = build_input (self, message, &root);
    result = pn_jmespath_search (
            self->expression ? self->expression : "",
            input, &error);
    json_node_unref (input);
    json_object_unref (root);

    if (replace_bag)
    {
        clear_data_bag (bag);

        if (result != NULL && JSON_NODE_HOLDS_OBJECT (result))
        {
            copy_object_into (bag, json_node_get_object (result));
            json_node_unref (result);
        }
        else
        {
            /* Non-object result (scalar/array/null, or an error
             * which we represent as null): wrap under "value" so
             * the bag remains a valid JSON object the rest of the
             * pipeline can speak to. */
            JsonNode *v;
            if (result != NULL)
                v = result;
            else
            {
                v = json_node_alloc ();
                json_node_init_null (v);
            }
            json_object_set_member (bag, "value", v);
        }
        if (result == NULL)
            pn_message_set_string (message, "error",
                    error ? error->message : "query failed");
    }
    else if (result == NULL)
    {
        JsonNode *null_node = json_node_alloc ();
        json_node_init_null (null_node);
        pn_message_set_member (message, field, null_node);
        pn_message_set_string (message, "error",
                error ? error->message : "query failed");
    }
    else
    {
        pn_message_set_member (message, field, result);
        if (json_object_has_member (bag, "error"))
            json_object_remove_member (bag, "error");
    }

    g_clear_error (&error);
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_query_get_property (GObject *object, guint prop_id,
                       GValue *value, GParamSpec *pspec)
{
    PnQuery *self = PN_QUERY (object);

    switch (prop_id)
    {
    case PROP_EXPRESSION:
        g_value_set_string (value, self->expression);
        break;
    case PROP_RESULT_FIELD:
        g_value_set_string (value, self->result_field);
        break;
    case PROP_SOURCE_FIELD:
        g_value_set_string (value, self->source_field);
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
pn_query_set_property (GObject *object, guint prop_id,
                       const GValue *value, GParamSpec *pspec)
{
    PnQuery *self = PN_QUERY (object);

    switch (prop_id)
    {
    case PROP_EXPRESSION:
        set_string_prop (&self->expression, value, object,
                         props[PROP_EXPRESSION]);
        break;
    case PROP_RESULT_FIELD:
        set_string_prop (&self->result_field, value, object,
                         props[PROP_RESULT_FIELD]);
        break;
    case PROP_SOURCE_FIELD:
        set_string_prop (&self->source_field, value, object,
                         props[PROP_SOURCE_FIELD]);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_query_finalize (GObject *object)
{
    PnQuery *self = PN_QUERY (object);

    g_clear_pointer (&self->expression,   g_free);
    g_clear_pointer (&self->result_field, g_free);
    g_clear_pointer (&self->source_field, g_free);

    G_OBJECT_CLASS (pn_query_parent_class)->finalize (object);
}

static void
pn_query_class_init (PnQueryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_query_get_property;
    object_class->set_property = pn_query_set_property;
    object_class->finalize     = pn_query_finalize;
    node_class->receive        = pn_query_receive;

    node_class->class_name     = "JMESPath";
    node_class->icon           = "\xef\x81\x9b";  /* fa-crosshairs U+F05B */
    node_class->color          = (PnColor){ 0.30, 0.66, 0.62, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_EXPRESSION] = g_param_spec_string (
            "expression", "Expression",
            "JMESPath expression evaluated against every received "
            "message; empty means the result is JSON null",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_RESULT_FIELD] = g_param_spec_string (
            "result-field", "Result field",
            "Where to put the result. A non-empty name (default "
            "\"result\") writes to that member of the data bag, "
            "leaving the surrounding members in place. \"\" or "
            "\"data\" replace the entire data bag with the result; "
            "non-object results are wrapped under a \"value\" key.",
            "result",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SOURCE_FIELD] = g_param_spec_string (
            "source-field", "Source field",
            "JSON pointer (slash-separated path) into the incoming "
            "message that the query runs against. Empty (default) "
            "queries the whole message root: topic, id, created, data.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_query_init (PnQuery *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.30, 0.66, 0.62, 1.0 };

    self->expression   = g_strdup ("");
    self->result_field = g_strdup ("result");
    self->source_field = g_strdup ("");

    pn_node_set_class_name (node, "JMESPath");
    pn_node_set_icon       (node, "\xef\x81\x9b");  /* fa-crosshairs U+F05B */
    pn_node_set_color      (node, &teal);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnQuery *
pn_query_new (void)
{
    return g_object_new (PN_TYPE_QUERY, NULL);
}
