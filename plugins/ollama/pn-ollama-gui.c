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
/*  Pipnode Ollama plugin — companion GUI module.                      */
/*                                                                     */
/*  The settings-dialog customisation for the Ollama node.  The node's */
/*  GType, properties and the /api/generate receive() path live in the */
/*  logic module pn-ollama.c; the editor loads this companion next to  */
/*  it and calls pn_plugin_gui_init(), which installs the              */
/*  build_property_editor vfunc onto the already-registered class.     */
/*  The headless runtime never loads this half, so the node's logic    */
/*  runs without GTK.                                                  */
/*                                                                     */
/*  The logic .so is opened G_MODULE_BIND_LOCAL, so its symbols (the   */
/*  pn_ollama_get_type() getter, the type-cast macro) are invisible    */
/*  here: the class is resolved by NAME via g_type_from_name, and the  */
/*  node is driven purely through its GObject properties.  The GTK-free */
/*  /api/tags model enumeration is only ever used by this dialog, so it */
/*  lives here in full rather than reaching across the module boundary. */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gmodule.h>
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "pn-node.h"
#include "pn-node-factory.h"
#include "pn-plugin.h"

/* The GType name the logic half registered.  Resolved with
 * g_type_from_name because the logic .so is BIND_LOCAL. */
#define PN_OLLAMA_TYPE_NAME            "PnOllama"

/* Empty hostname resolves to the loopback name when building URLs, so
 * libsoup reaches a local Ollama regardless of mDNS / DNS setup.  Kept
 * in sync with the same constant in the logic half (pn-ollama.c). */
#define PN_OLLAMA_HTTP_LOCAL_FALLBACK  "localhost"

/* ------------------------------------------------------------------ */
/*  Model combo: query /api/tags                                       */
/* ------------------------------------------------------------------ */

static int
ptr_array_strcmp (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const gchar * const *) a,
                      *(const gchar * const *) b);
}

/** Synchronously enumerate the model names installed on
 *  @hostname:@port via GET /api/tags.  Returns a NULL-terminated
 *  string array the caller owns (free with g_strfreev), or an empty
 *  array when the host is unreachable / not running Ollama.  GTK-free
 *  (libsoup + json-glib); used only to populate the model combo. */
static gchar **
ollama_list_models (
        const gchar *hostname,
        gint         port)
{
    SoupSession *session;
    SoupMessage *msg;
    GError      *error = NULL;
    GBytes      *bytes;
    GPtrArray   *names;
    JsonParser  *parser;
    gchar       *url;
    const gchar *host;

    names = g_ptr_array_new_with_free_func (g_free);
    host  = (hostname && *hostname) ? hostname : PN_OLLAMA_HTTP_LOCAL_FALLBACK;
    url   = g_strdup_printf ("http://%s:%d/api/tags", host, port);

    /* Dedicated synchronous session with a short timeout so the
     * settings dialog does not hang when the host is unreachable. */
    session = soup_session_new ();
    g_object_set (session, "timeout", (guint) 2, NULL);

    msg = soup_message_new (SOUP_METHOD_GET, url);
    g_free (url);

    if (msg != NULL)
        bytes = soup_session_send_and_read (session, msg, NULL, &error);
    else
        bytes = NULL;

    if (bytes != NULL
        && msg != NULL
        && soup_message_get_status (msg) >= 200
        && soup_message_get_status (msg) <  300)
    {
        gsize        len = 0;
        const gchar *body = g_bytes_get_data (bytes, &len);

        parser = json_parser_new ();
        if (json_parser_load_from_data (parser, body, (gssize) len, NULL))
        {
            JsonNode   *root = json_parser_get_root (parser);
            JsonObject *obj  = root ? json_node_get_object (root) : NULL;
            JsonArray  *arr  = NULL;

            if (obj != NULL && json_object_has_member (obj, "models"))
                arr = json_object_get_array_member (obj, "models");

            if (arr != NULL)
            {
                guint n = json_array_get_length (arr);
                guint i;
                for (i = 0; i < n; i++)
                {
                    JsonObject *m = json_array_get_object_element (arr, i);
                    const gchar *name = m && json_object_has_member (m, "name")
                                            ? json_object_get_string_member (m, "name")
                                            : NULL;
                    if (name && *name)
                        g_ptr_array_add (names, g_strdup (name));
                }
            }
        }
        g_object_unref (parser);
    }

    g_clear_pointer (&bytes, g_bytes_unref);
    g_clear_object  (&msg);
    g_clear_object  (&session);
    g_clear_error   (&error);

    g_ptr_array_sort (names, ptr_array_strcmp);
    g_ptr_array_add (names, NULL);

    return (gchar **) g_ptr_array_free (names, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: custom editor for the `model` row                 */
/*                                                                     */
/*  Combo populated from /api/tags on the configured host:port, plus   */
/*  a Refresh button to re-query (the model list changes as the user   */
/*  pulls or removes models on the server).  Repopulates automatically */
/*  on `notify::hostname` / `notify::port` so editing those rows above */
/*  immediately reflects in the combo below.  The node is addressed    */
/*  purely as a GObject (g_object_get / g_object_set), so the companion */
/*  needs neither the node's type macro nor its header.                */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject         *target;        /* borrowed PnOllama, driven via props */
    GtkComboBoxText *combo;
    gulong           notify_model;
    gulong           notify_host;
    gulong           notify_port;
    gboolean         updating;
} ModelBinding;

static void
model_binding_repopulate (ModelBinding *bind)
{
    gchar       **ids;
    gchar       **p;
    gchar        *current  = NULL;
    gchar        *hostname = NULL;
    gint          port     = 0;

    g_object_get (bind->target,
                  "model",    &current,
                  "hostname", &hostname,
                  "port",     &port,
                  NULL);

    bind->updating = TRUE;
    gtk_combo_box_text_remove_all (bind->combo);

    ids = ollama_list_models (hostname, port);

    /* Make sure the currently-saved model id is always selectable, even
     * if the server's list does not include it yet (slow host, model
     * being pulled, hostname not reachable). */
    if (current && *current)
    {
        gboolean present = FALSE;
        for (p = ids; *p; p++)
        {
            if (g_strcmp0 (*p, current) == 0) { present = TRUE; break; }
        }
        if (!present)
            gtk_combo_box_text_append (bind->combo, current, current);
    }

    for (p = ids; *p; p++)
        gtk_combo_box_text_append (bind->combo, *p, *p);
    g_strfreev (ids);

    if (current && *current)
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (bind->combo), current);

    bind->updating = FALSE;
    g_free (current);
    g_free (hostname);
}

static void
on_model_combo_changed (GtkComboBoxText *combo, gpointer user_data)
{
    ModelBinding *bind = user_data;
    const gchar  *id;

    if (bind->updating)
        return;

    id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo));
    if (id == NULL)
        return;

    g_object_set (bind->target, "model", id, NULL);
}

static void
on_model_refresh_clicked (GtkButton *btn G_GNUC_UNUSED, gpointer user_data)
{
    model_binding_repopulate (user_data);
}

static void
on_target_notify_model (GObject *o G_GNUC_UNUSED,
                        GParamSpec *p G_GNUC_UNUSED,
                        gpointer user_data)
{
    ModelBinding *bind = user_data;
    gchar        *value = NULL;

    if (bind->updating)
        return;

    g_object_get (bind->target, "model", &value, NULL);
    bind->updating = TRUE;
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (bind->combo),
                                 value && *value ? value : NULL);
    bind->updating = FALSE;
    g_free (value);
}

static void
on_target_notify_host_or_port (GObject *o G_GNUC_UNUSED,
                               GParamSpec *p G_GNUC_UNUSED,
                               gpointer user_data)
{
    model_binding_repopulate (user_data);
}

static void
model_binding_free (gpointer data)
{
    ModelBinding *bind = data;

    if (bind->target)
    {
        if (bind->notify_model)
            g_signal_handler_disconnect (bind->target, bind->notify_model);
        if (bind->notify_host)
            g_signal_handler_disconnect (bind->target, bind->notify_host);
        if (bind->notify_port)
            g_signal_handler_disconnect (bind->target, bind->notify_port);
    }
    g_free (bind);
}

static GtkWidget *
build_model_editor (GObject *target)
{
    GtkWidget    *box     = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget    *combo   = gtk_combo_box_text_new ();
    GtkWidget    *refresh = gtk_button_new_from_icon_name (
            "view-refresh-symbolic", GTK_ICON_SIZE_BUTTON);
    ModelBinding *bind;

    gtk_widget_set_tooltip_text (refresh,
            "Re-query the server's installed models");

    gtk_widget_set_hexpand (combo, TRUE);
    gtk_box_pack_start (GTK_BOX (box), combo,   TRUE,  TRUE,  0);
    gtk_box_pack_start (GTK_BOX (box), refresh, FALSE, FALSE, 0);

    bind = g_new0 (ModelBinding, 1);
    bind->target = target;
    bind->combo  = GTK_COMBO_BOX_TEXT (combo);

    model_binding_repopulate (bind);

    g_signal_connect (combo,   "changed",
                      G_CALLBACK (on_model_combo_changed), bind);
    g_signal_connect (refresh, "clicked",
                      G_CALLBACK (on_model_refresh_clicked), bind);
    bind->notify_model = g_signal_connect (
            target, "notify::model",
            G_CALLBACK (on_target_notify_model), bind);
    bind->notify_host = g_signal_connect (
            target, "notify::hostname",
            G_CALLBACK (on_target_notify_host_or_port), bind);
    bind->notify_port = g_signal_connect (
            target, "notify::port",
            G_CALLBACK (on_target_notify_host_or_port), bind);

    g_object_set_data_full (G_OBJECT (box),
                            "pn-ollama-model-binding",
                            bind, model_binding_free);
    return box;
}

static GtkWidget *
pn_ollama_build_property_editor (PnNode      *self,
                                 GParamSpec  *pspec,
                                 GObject     *target G_GNUC_UNUSED,
                                 GtkWindow   *parent G_GNUC_UNUSED)
{
    if (g_strcmp0 (pspec->name, "model") == 0)
        return build_model_editor (G_OBJECT (self));
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Companion entry point                                              */
/* ------------------------------------------------------------------ */

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_gui_init (PnNodeFactory *factory G_GNUC_UNUSED)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-ollama (GUI)",
        .version     = "1.0.0",
        .description = "Companion GUI module: the Ollama node's settings "
                       "dialog (model picker populated from /api/tags "
                       "with a Refresh button).",
    };
    GType        type = g_type_from_name (PN_OLLAMA_TYPE_NAME);
    PnNodeClass *node_class;

    if (type == 0)
    {
        /* The logic half did not register the type — nothing to dress
         * up.  Returning the descriptor still counts as a clean load. */
        g_warning ("pipnode-ollama (GUI): %s is not registered; "
                   "is the logic plugin loaded?",
                   PN_OLLAMA_TYPE_NAME);
        return &info;
    }

    /* Install the dialog vfunc slot onto the class the logic .so already
     * registered.  The class ref is intentionally held for the process
     * lifetime so the slot stays valid — the same one-leaked-ref-on-a-
     * singleton-class pattern the in-tree pn_<node>_gui_install() used. */
    node_class = PN_NODE_CLASS (g_type_class_ref (type));

    node_class->build_property_editor = pn_ollama_build_property_editor;

    return &info;
}
