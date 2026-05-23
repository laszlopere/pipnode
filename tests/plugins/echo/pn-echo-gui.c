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
/*  Sample pipnode plugin: PnEcho — companion GUI module.              */
/*                                                                     */
/*  The GTK-touching half of the sample plugin (TODO #23, Phase 6).    */
/*  Built as pn_echo-gui.so, the sibling of the logic module           */
/*  pn_echo.so.  The editor loads the logic .so first (registering the */
/*  PnEcho #GType), then finds this companion next to it and calls its */
/*  pn_plugin_gui_init(), which installs the settings-dialog vfunc      */
/*  slots onto the already-registered class — exactly the way the      */
/*  in-tree split nodes' pn_<node>_gui_install() functions do.         */
/*                                                                     */
/*  pipnode-run never loads a companion, so the logic half above runs  */
/*  on a server with no GTK while the editor still shows the custom    */
/*  dialog this file builds.  The two overrides exercise the ABI       */
/*  dialog hooks from out-of-tree-shaped code so a regression in the   */
/*  vfunc dispatch / helper surface — or in the companion loader       */
/*  itself — lights up in CI (tests/test_plugin_gui_companion.py).     */
/* ------------------------------------------------------------------ */

#include <gmodule.h>
#include <gtk/gtk.h>

#include "pn-node.h"
#include "pn-node-factory.h"
#include "pn-node-dialog-helpers.h"
#include "pn-plugin.h"

/* The companion resolves the node #GType by its registered name rather
 * than calling pn_echo_get_type().  The host loads the logic plugin with
 * G_MODULE_BIND_LOCAL, so its symbols (the type getter, any read seams)
 * are NOT visible to a separately-dlopened companion — linking the logic
 * header here would only produce an "undefined symbol: pn_echo_get_type"
 * at companion-load time.  g_type_from_name() needs no such symbol: the
 * type is already in the global GType registry once the logic .so ran.
 * A companion therefore drives the node through public API only (GObject
 * properties, the PnNode vfunc table), which is exactly what the dialog
 * hooks below do. */
#define PN_ECHO_TYPE_NAME "PnEcho"

/* ------------------------------------------------------------------ */
/*  Dialog extension demo                                              */
/* ------------------------------------------------------------------ */

static GtkWidget *
pn_echo_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                               GParamSpec  *pspec,
                               GObject     *target,
                               GtkWindow   *parent    G_GNUC_UNUSED)
{
    if (g_strcmp0 (pspec->name, "device") == 0)
    {
        gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
        GBindingFlags flags    = G_BINDING_SYNC_CREATE
                                 | (writable ? G_BINDING_BIDIRECTIONAL : 0);
        GtkWidget    *combo    = gtk_combo_box_text_new ();

        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   "loop",   "Loopback");
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   "tee",    "Tee");

        gtk_widget_set_hexpand   (combo, TRUE);
        gtk_widget_set_sensitive (combo, writable);
        g_object_bind_property (target, "device", combo, "active-id", flags);
        return combo;
    }

    /* Other properties (none right now, but kept open for future) fall
     * back to the host default. */
    return NULL;
}

static void
pn_echo_build_extra_pages (PnNode      *self      G_GNUC_UNUSED,
                           GtkNotebook *notebook,
                           GtkWindow   *parent    G_GNUC_UNUSED)
{
    /* A trivial extra tab so the dialog has a "Stats" page beyond the
     * auto-generated property tab.  Real plugins would put live
     * counters / a logs view / a "scan again" button here.  Its
     * presence is the clean positive signal the companion-loader test
     * asserts on: no companion, no "Stats" tab. */
    GtkWidget *box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *label = gtk_label_new (
            "Echo plugin extra page — wired through "
            "PnNodeClass.build_extra_pages installed by the companion "
            "GUI module.");

    g_object_set (box,
                  "margin-start",  12,
                  "margin-end",    12,
                  "margin-top",    12,
                  "margin-bottom", 12,
                  NULL);
    gtk_widget_set_halign (label, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

    pn_node_dialog_append_page (notebook, box, "Stats");
}

/* ------------------------------------------------------------------ */
/*  Companion entry point                                              */
/* ------------------------------------------------------------------ */

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_gui_init (PnNodeFactory *factory G_GNUC_UNUSED)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "EchoPlugin (GUI)",
        .version     = "1.0.0",
        .description = "Companion GUI module: the Echo node's settings-"
                       "dialog customisations (combo editor + Stats tab).",
    };
    GType        type = g_type_from_name (PN_ECHO_TYPE_NAME);
    PnNodeClass *node_class;

    if (type == 0)
    {
        /* The logic half did not register the type — nothing to dress
         * up.  Returning the descriptor still counts as a clean load. */
        g_warning ("EchoPlugin (GUI): %s is not registered; is the logic "
                   "plugin loaded?", PN_ECHO_TYPE_NAME);
        return &info;
    }

    /* Install the dialog vfunc slots onto the class the logic .so
     * already registered.  The class ref is intentionally held for the
     * process lifetime so the slots stay valid — the same one-leaked-
     * ref-on-a-singleton-class pattern pn_<node>_gui_install() uses. */
    node_class = PN_NODE_CLASS (g_type_class_ref (type));

    node_class->build_property_editor = pn_echo_build_property_editor;
    node_class->build_extra_pages     = pn_echo_build_extra_pages;

    return &info;
}
