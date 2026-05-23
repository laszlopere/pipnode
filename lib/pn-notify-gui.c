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
/*  PnNotify — gui tier.                                               */
/*                                                                     */
/*  The settings-dialog customisation for the Notify node.  The node's */
/*  GType, properties and the desktop-notification receive() logic     */
/*  live in the GTK-free core file pn-notify.c; this file installs the  */
/*  build_property_editor vfunc onto that class at editor startup       */
/*  (pn_notify_gui_install).  The icon-name combo binds the node's      */
/*  "icon" property directly, so it needs no core seam.  The headless   */
/*  runtime never loads this half, so the notification logic runs       */
/*  without GTK.                                                        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-notify-gui.h"
#include "pn-notify.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog: icon combo                                        */
/*                                                                     */
/*  The `icon` row gets an editable combo pre-loaded with the common   */
/*  freedesktop "standard" icon names so the user can pick one from a  */
/*  list — each shown with its actual glyph — instead of having to     */
/*  recall the exact name.  The combo keeps its entry editable, so a   */
/*  custom theme icon name or an absolute image path can still be      */
/*  typed by hand: exactly the two forms the daemon's `app_icon`       */
/*  argument accepts.                                                  */
/* ------------------------------------------------------------------ */

/* Curated subset of the freedesktop Icon Naming Specification's
 * "standard" names that make sense on a notification bubble; every one
 * ships with the common icon themes (Adwaita, Yaru, Breeze).  Not
 * exhaustive on purpose — the entry stays editable for anything else. */
static const gchar *const PN_NOTIFY_ICON_NAMES[] = {
    "dialog-information",
    "dialog-warning",
    "dialog-error",
    "dialog-question",
    "dialog-password",
    "emblem-important",
    "emblem-default",
    "emblem-favorite",
    "mail-message-new",
    "appointment-soon",
    "software-update-available",
    "weather-clear",
    "audio-volume-high",
    "network-wireless",
    "system-run",
};

static GtkWidget *
build_icon_editor (PnNotify *self)
{
    GtkListStore    *store;
    GtkWidget       *combo;
    GtkWidget       *entry;
    GtkCellRenderer *pixbuf;
    GtkCellRenderer *text;
    guint            i;

    store = gtk_list_store_new (1, G_TYPE_STRING);
    for (i = 0; i < G_N_ELEMENTS (PN_NOTIFY_ICON_NAMES); i++)
        gtk_list_store_insert_with_values (store, NULL, -1,
                                           0, PN_NOTIFY_ICON_NAMES[i], -1);

    combo = gtk_combo_box_new_with_model_and_entry (GTK_TREE_MODEL (store));
    g_object_unref (store);

    /* The entry — and so the bound property — carries the raw icon
     * name selected from (or typed into) the combo. */
    gtk_combo_box_set_entry_text_column (GTK_COMBO_BOX (combo), 0);

    /* gtk_combo_box_new_with_model_and_entry() auto-packs a text
     * renderer for the entry-text-column into the dropdown, so wipe the
     * cell layout before laying out our own — otherwise the name shows
     * twice (once from that renderer, once from ours).  The entry's text
     * comes from the entry-text-column directly, not from any renderer,
     * so clearing the popup cells leaves selection behaviour intact. */
    gtk_cell_layout_clear (GTK_CELL_LAYOUT (combo));

    /* Our dropdown rows: the icon's glyph followed by its name. */
    pixbuf = gtk_cell_renderer_pixbuf_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), pixbuf, FALSE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), pixbuf,
                                    "icon-name", 0, NULL);

    text = gtk_cell_renderer_text_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), text, TRUE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), text,
                                    "text", 0, NULL);

    entry = gtk_bin_get_child (GTK_BIN (combo));
    g_object_bind_property (self, "icon", entry, "text",
                            G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);

    gtk_widget_set_hexpand (combo, TRUE);
    return combo;
}

static GtkWidget *
pn_notify_build_property_editor (
        PnNode     *self,
        GParamSpec *pspec,
        GObject    *target G_GNUC_UNUSED,
        GtkWindow  *parent G_GNUC_UNUSED)
{
    if (g_strcmp0 (pspec->name, "icon") == 0)
        return build_icon_editor (PN_NOTIFY (self));

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_notify_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_NOTIFY));

    node_class->build_property_editor = pn_notify_build_property_editor;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid. */
}
