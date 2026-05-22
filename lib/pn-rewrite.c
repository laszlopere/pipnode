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

#include "pn-rewrite.h"
#include "pn-json-path.h"
#include "pn-message.h"
#include "pn-subst.h"

#include <gtksourceview/gtksource.h>
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
/*  as its escaped contents.  Unknown paths render as the JSON literal */
/*  `null`; an unterminated "${" is emitted verbatim.                  */
/* ------------------------------------------------------------------ */

static gchar *
expand_placeholders (
        const gchar *tmpl,
        JsonObject  *root)
{
    PnSubstResolver  resolver;
    PnSubstResolver *chain[2];
    PnSubstContext   ctx;

    pn_subst_resolver_json (&resolver, root);
    chain[0] = &resolver;
    chain[1] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = PN_SUBST_JSON;
    ctx.miss      = PN_SUBST_MISS_NULL;

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
    expanded = expand_placeholders (self->template_text, root);
    json_object_unref (root);

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, expanded, -1, &error))
    {
        g_warning ("pn-rewrite: template is not valid JSON after "
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
/*  Settings dialog: GtkSourceView editor with JSON syntax             */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject       *target;          /* borrowed PnRewrite */
    GtkTextBuffer *buffer;
    gulong         notify_handler;
    gboolean       updating;        /* re-entrancy guard */
} PnRewriteBinding;

static void
rewrite_binding_free (gpointer data)
{
    PnRewriteBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);
    g_free (bind);
}

static void
rewrite_binding_pull (PnRewriteBinding *bind)
{
    gchar *value = NULL;

    g_object_get (bind->target, "template", &value, NULL);

    {
        GtkTextIter start, end;
        gchar      *current;

        gtk_text_buffer_get_bounds (bind->buffer, &start, &end);
        current = gtk_text_buffer_get_text (bind->buffer, &start, &end, FALSE);

        if (g_strcmp0 (current, value != NULL ? value : "") != 0)
        {
            bind->updating = TRUE;
            gtk_text_buffer_set_text (bind->buffer,
                                      value != NULL ? value : "", -1);
            bind->updating = FALSE;
        }

        g_free (current);
    }

    g_free (value);
}

static void
rewrite_binding_push (PnRewriteBinding *bind)
{
    GtkTextIter start, end;
    gchar      *text;

    if (bind->updating)
        return;

    gtk_text_buffer_get_bounds (bind->buffer, &start, &end);
    text = gtk_text_buffer_get_text (bind->buffer, &start, &end, FALSE);

    bind->updating = TRUE;
    g_object_set (bind->target, "template", text, NULL);
    bind->updating = FALSE;

    g_free (text);
}

static void
on_rewrite_target_notify (GObject    *object G_GNUC_UNUSED,
                          GParamSpec *pspec  G_GNUC_UNUSED,
                          gpointer    user_data)
{
    rewrite_binding_pull (user_data);
}

static void
on_rewrite_buffer_changed (GtkTextBuffer *buffer G_GNUC_UNUSED,
                           gpointer       user_data)
{
    rewrite_binding_push (user_data);
}

static GtkWidget *
pn_rewrite_build_class_tab (PnNode    *self,
                            GtkWindow *parent G_GNUC_UNUSED)
{
    GObject                  *target = G_OBJECT (self);
    GtkSourceLanguageManager *langs;
    GtkSourceLanguage        *json_lang;
    GtkSourceBuffer          *buffer;
    GtkWidget                *view;
    GtkWidget                *scrolled;
    PnRewriteBinding         *bind;

    langs     = gtk_source_language_manager_get_default ();
    json_lang = gtk_source_language_manager_get_language (langs, "json");

    buffer = gtk_source_buffer_new_with_language (json_lang);
    gtk_source_buffer_set_highlight_syntax    (buffer, TRUE);
    gtk_source_buffer_set_highlight_matching_brackets (buffer, TRUE);

    view = gtk_source_view_new_with_buffer (buffer);
    gtk_source_view_set_show_line_numbers   (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_auto_indent         (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_indent_on_tab       (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_tab_width           (GTK_SOURCE_VIEW (view), 2);
    gtk_source_view_set_insert_spaces_instead_of_tabs (
            GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_highlight_current_line (GTK_SOURCE_VIEW (view), TRUE);
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
    g_object_unref (buffer);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    /* Give the editor a comfortable canvas for a multi-line JSON
     * object — twelve lines is roughly the size of the default
     * template plus a couple of extra fields the user typically adds
     * before the dialog feels cramped. */
    gtk_widget_set_size_request (scrolled, -1, 240);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), view);

    bind = g_new0 (PnRewriteBinding, 1);
    bind->target = target;
    bind->buffer = GTK_TEXT_BUFFER (gtk_text_view_get_buffer (
            GTK_TEXT_VIEW (view)));

    rewrite_binding_pull (bind);

    bind->notify_handler = g_signal_connect (
            target, "notify::template",
            G_CALLBACK (on_rewrite_target_notify), bind);
    g_signal_connect (bind->buffer, "changed",
                      G_CALLBACK (on_rewrite_buffer_changed), bind);

    g_object_set_data_full (G_OBJECT (scrolled),
                            "pn-rewrite-binding",
                            bind, rewrite_binding_free);

    return scrolled;
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
    node_class->build_class_tab = pn_rewrite_build_class_tab;

    node_class->class_name = "Rewrite";
    node_class->icon       = "\xef\x81\x84";  /* fa-edit U+F044 */
    node_class->color      = (GdkRGBA){ 0.55, 0.50, 0.80, 1.0 };
    node_class->category   = "Filters";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_TEMPLATE] = g_param_spec_string (
            "template", "Template",
            "JSON template the outgoing message is built from; "
            "${path/to/field} placeholders expand to JSON values "
            "from the incoming message",
            PN_REWRITE_DEFAULT_TEMPLATE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_rewrite_init (PnRewrite *self)
{
    PnNode  *node   = PN_NODE (self);
    GdkRGBA  violet = { 0.55, 0.50, 0.80, 1.0 };

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
