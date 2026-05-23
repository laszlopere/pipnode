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
/*  PnOllama — gui tier.                                               */
/*                                                                     */
/*  The settings-dialog customisation for the Ollama node.  The node's */
/*  GType, properties, the /api/generate receive() path and the        */
/*  GTK-free /api/tags model enumeration (pn_ollama_list_models) live   */
/*  in the core file pn-ollama.c; this file installs the                */
/*  build_property_editor vfunc onto that class at editor startup       */
/*  (pn_ollama_gui_install).  The model combo reads hostname / port /   */
/*  model through their GObject properties and calls the public         */
/*  pn_ollama_list_models accessor, so it needs no extra core seam.     */
/*  The headless runtime never loads this half, so the node's logic     */
/*  runs without GTK.                                                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-ollama-gui.h"
#include "pn-ollama.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog: custom editor for the `model` row                 */
/*                                                                     */
/*  Combo populated from /api/tags on the configured host:port, plus   */
/*  a Refresh button to re-query (the model list changes as the user   */
/*  pulls or removes models on the server).  Repopulates automatically */
/*  on `notify::hostname` / `notify::port` so editing those rows above */
/*  immediately reflects in the combo below.                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnOllama        *target;        /* borrowed */
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

    ids = pn_ollama_list_models (hostname, port);

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
build_model_editor (PnOllama *self)
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
    bind->target = self;
    bind->combo  = GTK_COMBO_BOX_TEXT (combo);

    model_binding_repopulate (bind);

    g_signal_connect (combo,   "changed",
                      G_CALLBACK (on_model_combo_changed), bind);
    g_signal_connect (refresh, "clicked",
                      G_CALLBACK (on_model_refresh_clicked), bind);
    bind->notify_model = g_signal_connect (
            self, "notify::model",
            G_CALLBACK (on_target_notify_model), bind);
    bind->notify_host = g_signal_connect (
            self, "notify::hostname",
            G_CALLBACK (on_target_notify_host_or_port), bind);
    bind->notify_port = g_signal_connect (
            self, "notify::port",
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
        return build_model_editor (PN_OLLAMA (self));
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_ollama_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_OLLAMA));

    node_class->build_property_editor = pn_ollama_build_property_editor;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid. */
}
