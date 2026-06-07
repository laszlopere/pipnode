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

/* ------------------------------------------------------------------ */
/*  PnRewrite — logic tier (headless core).                            */
/*                                                                     */
/*  This file holds the GTK-free half of the Rewrite node: the GType,  */
/*  the `template` property, the JSON placeholder expansion and the     */
/*  receive() rewrite (render the template, parse it, apply the         */
/*  resulting envelope / data bag onto the message).  The settings-     */
/*  dialog editor — a GtkSourceView with JSON syntax highlighting —     */
/*  lives in the companion gui-tier file pn-rewrite-gui.c, which        */
/*  installs that vfunc slot onto this class at editor startup (see     */
/*  pn_rewrite_gui_install).  The headless runtime registers and runs   */
/*  this node without ever pulling GTK / GtkSourceView.                 */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-rewrite.h"
#include "pn-json-path.h"
#include "pn-message.h"
#include "pn-subst.h"
#include "pn-flow.h"
#include "pn-settings-schema.h"

#include <json-glib/json-glib.h>

static const gchar PN_REWRITE_DEFAULT_TEMPLATE[] =
    "{\n"
    "  \"topic\": \"${topic}\",\n"
    "  \"data\": {\n"
    "    \"value\":  ${data/value},\n"
    "    \"output\": \"${data/output}\",\n"
    "    \"source\": \"${topic}\",\n"
    "    \"id\":     \"${id}\"\n"
    "  }\n"
    "}\n";

struct _PnRewrite
{
    PnNode parent_instance;

    gchar *template_text;
};

G_DEFINE_TYPE (PnRewrite, pn_rewrite, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_TEMPLATE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Placeholder expansion                                              */
/*                                                                     */
/*  PnRewrite authors its output in JSON, so it expands placeholders   */
/*  in PN_SUBST_JSON mode: the engine tracks whether the cursor sits   */
/*  inside a JSON string literal and renders accordingly — a numeric / */
/*  boolean / null scalar dropped in a value slot                      */
/*  (e.g. `"value": ${data/value}`) emerges as its bare JSON token,    */
/*  while a placeholder inside a string ("rewritten/${topic}") emerges */
/*  as its escaped contents.  An unknown path is left verbatim as      */
/*  "${path}": inside a string literal it survives as typed, while in  */
/*  a value slot it leaves the rendered template invalid JSON, so the  */
/*  node logs a warning and forwards the message unchanged rather than */
/*  silently nulling the field.  An unterminated "${" is also emitted  */
/*  verbatim.                                                          */
/* ------------------------------------------------------------------ */

static gchar *
expand_placeholders (
        const gchar *tmpl,
        JsonObject  *root,
        PnFlow      *flow)
{
    PnSubstResolver  resolver;
    PnSubstResolver  globals;
    PnSubstResolver *chain[3];
    PnSubstContext   ctx;

    pn_subst_resolver_json (&resolver, root);
    pn_flow_subst_resolver_globals (&globals, flow);
    chain[0] = &resolver;   /* message fields win */
    chain[1] = &globals;    /* then document globals */
    chain[2] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = PN_SUBST_JSON;
    ctx.miss      = PN_SUBST_MISS_VERBATIM;

    return pn_subst_expand (tmpl, &ctx);
}

/* ------------------------------------------------------------------ */
/*  Apply rendered JSON onto the message                               */
/*                                                                     */
/*  The user authors the outgoing message in JSON.  After placeholder  */
/*  expansion the template is parsed; if it is an object we look for   */
/*  envelope members (`topic`, `data`) and, failing that, treat the    */
/*  whole object as the new data bag.  This keeps the simple case     */
/*  (just reshape the data bag) free of envelope ceremony while still  */
/*  letting a full-envelope template through unchanged.                */
/* ------------------------------------------------------------------ */

static void
apply_rendered (PnMessage *message, JsonNode *root)
{
    JsonObject *obj;
    JsonObject *data    = pn_message_get_data (message);
    gboolean    has_envelope;

    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
        return;

    obj = json_node_get_object (root);
    has_envelope = json_object_has_member (obj, "topic")
                || json_object_has_member (obj, "data");

    if (has_envelope)
    {
        if (json_object_has_member (obj, "topic"))
        {
            JsonNode *t = json_object_get_member (obj, "topic");
            if (t != NULL && JSON_NODE_HOLDS_VALUE (t) &&
                json_node_get_value_type (t) == G_TYPE_STRING)
                pn_message_set_topic (message, json_node_get_string (t));
        }

        if (json_object_has_member (obj, "data"))
        {
            JsonNode   *d = json_object_get_member (obj, "data");
            JsonObject *src;
            GList      *names, *l;

            if (d == NULL || !JSON_NODE_HOLDS_OBJECT (d))
                return;

            src = json_node_get_object (d);

            /* Replace the data bag in place: drop every existing
             * member, then graft a deep-copy of every member of the
             * rendered "data" object on top. */
            names = json_object_get_members (data);
            for (l = names; l != NULL; l = l->next)
                json_object_remove_member (data, l->data);
            g_list_free (names);

            names = json_object_get_members (src);
            for (l = names; l != NULL; l = l->next)
            {
                const gchar *name = l->data;
                JsonNode    *val  = json_object_get_member (src, name);
                json_object_set_member (data, name, json_node_copy (val));
            }
            g_list_free (names);
        }
    }
    else
    {
        /* No envelope members — the whole object is the new data bag. */
        GList *names, *l;

        names = json_object_get_members (data);
        for (l = names; l != NULL; l = l->next)
            json_object_remove_member (data, l->data);
        g_list_free (names);

        names = json_object_get_members (obj);
        for (l = names; l != NULL; l = l->next)
        {
            const gchar *name = l->data;
            JsonNode    *val  = json_object_get_member (obj, name);
            json_object_set_member (data, name, json_node_copy (val));
        }
        g_list_free (names);
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_rewrite_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnRewrite  *self = PN_REWRITE (node);
    JsonObject *root;
    gchar      *expanded;
    JsonParser *parser;
    GError     *error = NULL;

    if (self->template_text == NULL || *self->template_text == '\0')
    {
        pn_node_emit_message (node, message);
        return;
    }

    root     = pn_json_lookup_root_for_message (message);
    expanded = expand_placeholders (self->template_text, root,
                                    pn_node_get_flow (node));
    json_object_unref (root);

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, expanded, -1, &error))
    {
        pn_node_log_error (node, "Template is not valid JSON after "
                           "placeholder expansion: %s", error->message);
        g_error_free (error);
    }
    else
    {
        apply_rendered (message, json_parser_get_root (parser));
    }

    g_object_unref (parser);
    g_free (expanded);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_rewrite_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnRewrite *self = PN_REWRITE (object);

    switch (prop_id)
    {
    case PROP_TEMPLATE:
        g_value_set_string (value, self->template_text);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_rewrite_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnRewrite *self = PN_REWRITE (object);

    switch (prop_id)
    {
    case PROP_TEMPLATE:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->template_text, s) != 0)
            {
                g_free (self->template_text);
                self->template_text = g_strdup (s != NULL ? s : "");
                g_object_notify_by_pspec (object, props[PROP_TEMPLATE]);
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
pn_rewrite_finalize (GObject *object)
{
    PnRewrite *self = PN_REWRITE (object);

    g_clear_pointer (&self->template_text, g_free);

    G_OBJECT_CLASS (pn_rewrite_parent_class)->finalize (object);
}

static void
pn_rewrite_class_init (PnRewriteClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_rewrite_get_property;
    object_class->set_property = pn_rewrite_set_property;
    object_class->finalize     = pn_rewrite_finalize;

    node_class->receive         = pn_rewrite_receive;

    node_class->class_name = "Rewrite";
    node_class->icon       = "\xef\x81\x84";  /* fa-edit U+F044 */
    node_class->color      = (PnColor){ 0.55, 0.50, 0.80, 1.0 };
    node_class->category   = "Filters/Reshape";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_TEMPLATE] = g_param_spec_string (
            "template", "Template",
            "JSON template the outgoing message is built from; "
            "${path/to/field} placeholders expand to JSON values "
            "from the incoming message, and an unknown path is left "
            "intact as ${path/to/field}",
            PN_REWRITE_DEFAULT_TEMPLATE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings schema (Phase 7.5): the JSON `template` body
     * gets a full-width GtkSourceView code editor (syntax highlighting,
     * line numbers, bracket matching) on a tab named "Rewrite".  The
     * editor itself lives in the GUI tier's renderer (PN_EDITOR_CODE);
     * this is just the GTK-free description, so the deleted
     * pn-rewrite-gui.c is no longer needed and the rewrite logic stays
     * headless. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Rewrite");
        pn_settings_schema_row (schema, "template", PN_EDITOR_CODE);
        pn_settings_schema_row_flags (schema, "template",
                                      PN_ROW_FLAG_FULL_WIDTH);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_rewrite_init (PnRewrite *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.55, 0.50, 0.80, 1.0 };

    self->template_text = g_strdup (PN_REWRITE_DEFAULT_TEMPLATE);

    pn_node_set_class_name (node, "Rewrite");
    pn_node_set_icon       (node, "\xef\x81\x84");  /* fa-edit U+F044 */
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnRewrite *
pn_rewrite_new (void)
{
    return g_object_new (PN_TYPE_REWRITE, NULL);
}
