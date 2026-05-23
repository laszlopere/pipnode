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

#include "pn-set.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Compiled assignment                                                */
/*                                                                     */
/*  An assignment's literal is held as the JsonNode parsed straight    */
/*  from the user's "props" string.  The literal's value type drives   */
/*  how the corresponding data-bag member is written at receive time,  */
/*  so the on-disk representation does not need a separate type tag.   */
/* ------------------------------------------------------------------ */

typedef struct
{
    gchar    *path;
    JsonNode *literal;   /* owned */
} CompiledSet;

static void
compiled_set_clear (CompiledSet *entry)
{
    g_clear_pointer (&entry->path,    g_free);
    g_clear_pointer (&entry->literal, json_node_unref);
}

/* ------------------------------------------------------------------ */
/*  PnSet                                                              */
/* ------------------------------------------------------------------ */

struct _PnSet
{
    PnNode parent_instance;

    gchar  *props_json;     /* canonical string property */
    GArray *compiled;       /* CompiledSet, parsed cache */
};

G_DEFINE_TYPE (PnSet, pn_set, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_PROPS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Compilation                                                        */
/* ------------------------------------------------------------------ */

static void
free_compiled (GArray *arr)
{
    if (arr == NULL)
        return;

    for (guint i = 0; i < arr->len; i++)
        compiled_set_clear (&g_array_index (arr, CompiledSet, i));

    g_array_set_size (arr, 0);
}

static GArray *
compile_props (const gchar *json_string)
{
    GArray       *out    = g_array_new (FALSE, FALSE, sizeof (CompiledSet));
    JsonParser   *parser = json_parser_new ();
    JsonNode     *root;
    JsonArray    *array;
    GError       *error  = NULL;
    guint         i, n;

    if (json_string == NULL || *json_string == '\0')
        json_string = "[]";

    if (!json_parser_load_from_data (parser, json_string, -1, &error))
    {
        g_warning ("pn-set: invalid props JSON: %s", error->message);
        g_error_free (error);
        g_object_unref (parser);
        return out;
    }

    root  = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
        g_object_unref (parser);
        return out;
    }

    array = json_node_get_array (root);
    n     = json_array_get_length (array);
    for (i = 0; i < n; i++)
    {
        JsonNode    *item = json_array_get_element (array, i);
        JsonObject  *obj;
        const gchar *path;
        JsonNode    *lit;
        CompiledSet  entry = { 0 };

        if (item == NULL || !JSON_NODE_HOLDS_OBJECT (item))
            continue;

        obj = json_node_get_object (item);
        if (!json_object_has_member (obj, "path") ||
            !json_object_has_member (obj, "literal"))
            continue;

        path = json_object_get_string_member (obj, "path");
        lit  = json_object_get_member        (obj, "literal");

        if (path == NULL || *path == '\0' || lit == NULL)
            continue;

        entry.path    = g_strdup (path);
        entry.literal = json_node_copy (lit);
        g_array_append_val (out, entry);
    }

    g_object_unref (parser);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Path assignment                                                    */
/*                                                                     */
/*  A path is normally a single member name like "value" or "tcp4",    */
/*  but accepting a dotted form ("foo.bar") for free lets the user     */
/*  reach nested data-bag objects without needing a Query node         */
/*  upstream.  Intermediate segments that are missing — or hold a      */
/*  non-object value — are replaced with a fresh JsonObject so the     */
/*  full path becomes addressable; this matches what a user typing     */
/*  "foo.bar" expects ("create the nested shape on demand") and        */
/*  mirrors how an object-literal assignment in JavaScript or a        */
/*  jq `.foo.bar = …` behaves.                                         */
/* ------------------------------------------------------------------ */

static void
assign_path (JsonObject *data, const gchar *path, JsonNode *literal)
{
    gchar    **parts;
    guint      i;

    if (data == NULL || path == NULL || *path == '\0' || literal == NULL)
        return;

    /* Bare member name fast path. */
    if (strchr (path, '.') == NULL)
    {
        json_object_set_member (data, path, json_node_copy (literal));
        return;
    }

    parts = g_strsplit (path, ".", -1);
    {
        JsonObject *cur = data;
        for (i = 0; parts[i] != NULL; i++)
        {
            if (parts[i + 1] == NULL)
            {
                json_object_set_member (cur, parts[i],
                                        json_node_copy (literal));
                break;
            }

            /* Intermediate segment: descend into an existing nested
             * object, or replace whatever is there with a fresh one. */
            if (json_object_has_member (cur, parts[i]))
            {
                JsonNode *child = json_object_get_member (cur, parts[i]);
                if (child != NULL && JSON_NODE_HOLDS_OBJECT (child))
                {
                    cur = json_node_get_object (child);
                    continue;
                }
            }

            {
                JsonObject *fresh = json_object_new ();
                JsonNode   *node  = json_node_new (JSON_NODE_OBJECT);
                json_node_take_object (node, fresh);
                json_object_set_member (cur, parts[i], node);
                cur = fresh;
            }
        }
    }
    g_strfreev (parts);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_set_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnSet      *self = PN_SET (node);
    JsonObject *data;
    guint       i;

    if (self->compiled == NULL || self->compiled->len == 0)
    {
        pn_node_emit_message (node, message);
        return;
    }

    data = pn_message_get_data (message);

    for (i = 0; i < self->compiled->len; i++)
    {
        const CompiledSet *entry = &g_array_index (self->compiled,
                                                   CompiledSet, i);
        assign_path (data, entry->path, entry->literal);
    }

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_set_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnSet *self = PN_SET (object);

    switch (prop_id)
    {
    case PROP_PROPS:
        g_value_set_string (value,
                            self->props_json != NULL ? self->props_json
                                                     : "[]");
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_set_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnSet *self = PN_SET (object);

    switch (prop_id)
    {
    case PROP_PROPS:
        {
            const gchar *new_str = g_value_get_string (value);
            if (new_str == NULL || *new_str == '\0')
                new_str = "[]";

            if (g_strcmp0 (self->props_json, new_str) == 0)
                break;

            g_free (self->props_json);
            self->props_json = g_strdup (new_str);

            free_compiled (self->compiled);
            g_array_unref (self->compiled);
            self->compiled = compile_props (self->props_json);

            g_object_notify_by_pspec (object, props[PROP_PROPS]);
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
pn_set_finalize (GObject *object)
{
    PnSet *self = PN_SET (object);

    if (self->compiled != NULL)
    {
        free_compiled (self->compiled);
        g_array_unref (self->compiled);
        self->compiled = NULL;
    }
    g_clear_pointer (&self->props_json, g_free);

    G_OBJECT_CLASS (pn_set_parent_class)->finalize (object);
}

static void
pn_set_class_init (PnSetClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_set_get_property;
    object_class->set_property = pn_set_set_property;
    object_class->finalize     = pn_set_finalize;
    node_class->receive        = pn_set_receive;

    /* build_class_tab installed by the gui tier (pn_set_gui_install). */

    node_class->class_name     = "Set";
    node_class->icon           = "\xef\x80\xac";  /* fa-tags U+F02C */
    node_class->color          = (PnColor){ 0.62, 0.45, 0.78, 1.0 };
    node_class->category       = "Filters/Reshape";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_PROPS] = g_param_spec_string (
            "props", "Props",
            "JSON array of {path, literal} assignments applied to every "
            "passing message",
            "[]",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_set_init (PnSet *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.62, 0.45, 0.78, 1.0 };

    self->props_json = g_strdup ("[]");
    self->compiled   = g_array_new (FALSE, FALSE, sizeof (CompiledSet));

    pn_node_set_class_name (node, "Set");
    pn_node_set_icon       (node, "\xef\x80\xac");  /* fa-tags U+F02C */
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnSet *
pn_set_new (void)
{
    return g_object_new (PN_TYPE_SET, NULL);
}
