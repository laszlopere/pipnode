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
/*  PnInject — gui tier.                                               */
/*                                                                     */
/*  Installs the settings-dialog customisation for the Injector node.  */
/*  The node's GType, properties and fire() logic live in the GTK-free */
/*  core file pn-inject.c; this file installs the build_property_editor*/
/*  vfunc onto that class at editor startup (pn_inject_gui_install).   */
/*  Only the "button-icon" picker is custom — every other property     */
/*  keeps the auto-built default editor.                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-inject-gui.h"
#include "pn-inject.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog: icon combo                                        */
/*                                                                     */
/*  Curated catalog of freedesktop Icon Naming Specification names     */
/*  drawn primarily from the Actions context — the icons that read     */
/*  well as a click-to-fire affordance on a worksheet node.  Each name */
/*  here is shipped by the common icon themes (Adwaita, Yaru, Breeze,  */
/*  elementary, hicolor), so the dropdown's icon-name cell renderer    */
/*  shows the actual glyph in every theme the editor is likely to run  */
/*  under.  The entry stays editable, so a theme-specific name can     */
/*  still be typed by hand.  The first row carries an empty icon-name  */
/*  so picking it returns the node to the compact tab + play-triangle. */
/* ------------------------------------------------------------------ */

/* Either the entry's icon_name and label match (the common case — the
 * label is just the icon name so the user sees exactly what they will
 * be storing into the property), or the entry is the synthetic "no
 * icon" row whose icon_name is empty and label is human prose. */
static const gchar *const PN_INJECT_BUTTON_ICONS[] = {
    /* Media playback — natural "trigger" affordances. */
    "media-playback-start",
    "media-playback-pause",
    "media-playback-stop",
    "media-record",
    "media-skip-forward",
    "media-skip-backward",
    "media-seek-forward",
    "media-seek-backward",
    "media-eject",

    /* System actions. */
    "system-run",
    "system-search",
    "system-shutdown",
    "system-reboot",
    "system-log-out",
    "system-lock-screen",

    /* Edit actions. */
    "edit-clear",
    "edit-copy",
    "edit-cut",
    "edit-paste",
    "edit-delete",
    "edit-find",
    "edit-find-replace",
    "edit-redo",
    "edit-undo",
    "edit-select-all",

    /* Document actions. */
    "document-new",
    "document-open",
    "document-save",
    "document-save-as",
    "document-send",
    "document-print",
    "document-edit",
    "document-revert",

    /* View / navigation. */
    "view-refresh",
    "view-fullscreen",
    "view-restore",
    "go-next",
    "go-previous",
    "go-up",
    "go-down",
    "go-first",
    "go-last",
    "go-top",
    "go-bottom",
    "go-home",
    "go-jump",

    /* Mail. */
    "mail-send",
    "mail-message-new",
    "mail-forward",
    "mail-reply-sender",
    "mail-reply-all",
    "mail-mark-important",

    /* Insert / create. */
    "list-add",
    "list-remove",
    "insert-link",
    "insert-text",
    "insert-image",
    "tab-new",
    "window-new",
    "window-close",
    "folder-new",
    "bookmark-new",
    "contact-new",
    "appointment-new",

    /* Zoom. */
    "zoom-in",
    "zoom-out",
    "zoom-fit-best",
    "zoom-original",

    /* Process / help. */
    "process-stop",
    "help-about",
    "help-contents",
    "help-faq",

    /* Object transforms (less "action", more "operation"). */
    "object-flip-horizontal",
    "object-flip-vertical",
    "object-rotate-left",
    "object-rotate-right",

    /* Emblems & status — sometimes a better fit than an action icon. */
    "emblem-important",
    "emblem-default",
    "emblem-favorite",
    "starred",
    "non-starred",
    "dialog-information",
    "dialog-warning",
    "dialog-error",
    "dialog-question",

    /* Common categories. */
    "weather-clear",
    "audio-volume-high",
    "network-wireless",
    "appointment-soon",
    "applications-system",
};

/* Synthetic "no icon" entry shown at the top of the dropdown.  An empty
 * icon name returns the node to the compact tab + play-triangle look. */
#define PN_INJECT_NO_ICON_LABEL  "(none — default)"

enum {
    COL_ICON_NAME,   /* the bound property value */
    COL_LABEL,       /* user-visible row text    */
    N_ICON_COLS
};

static GtkWidget *
build_button_icon_editor (PnInject *self)
{
    GtkListStore    *store;
    GtkWidget       *combo;
    GtkWidget       *entry;
    GtkCellRenderer *pixbuf;
    GtkCellRenderer *text;
    guint            i;

    store = gtk_list_store_new (N_ICON_COLS,
                                G_TYPE_STRING,   /* COL_ICON_NAME */
                                G_TYPE_STRING);  /* COL_LABEL     */

    /* Synthetic top entry: empty icon name + human label, so the user
     * can explicitly pick "no icon" without having to clear the entry
     * by hand. */
    gtk_list_store_insert_with_values (
            store, NULL, -1,
            COL_ICON_NAME, "",
            COL_LABEL,     PN_INJECT_NO_ICON_LABEL,
            -1);

    /* Icon rows.  For these the label intentionally matches the icon
     * name verbatim — the icon name is the value the property stores,
     * and showing it as-is lets the user see (and recognise) exactly
     * what they will be picking. */
    for (i = 0; i < G_N_ELEMENTS (PN_INJECT_BUTTON_ICONS); i++)
        gtk_list_store_insert_with_values (
                store, NULL, -1,
                COL_ICON_NAME, PN_INJECT_BUTTON_ICONS[i],
                COL_LABEL,     PN_INJECT_BUTTON_ICONS[i],
                -1);

    combo = gtk_combo_box_new_with_model_and_entry (GTK_TREE_MODEL (store));
    g_object_unref (store);

    /* The entry — and so the bound "button-icon" property — carries the
     * raw icon name selected from (or typed into) the combo. */
    gtk_combo_box_set_entry_text_column (GTK_COMBO_BOX (combo), COL_ICON_NAME);

    /* gtk_combo_box_new_with_model_and_entry() auto-packs a text
     * renderer for the entry-text column into the dropdown, so wipe the
     * cell layout before laying out our own — otherwise the icon name
     * shows twice (once from that renderer, once from ours). */
    gtk_cell_layout_clear (GTK_CELL_LAYOUT (combo));

    /* Dropdown rows: the icon's glyph followed by the human label. */
    pixbuf = gtk_cell_renderer_pixbuf_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), pixbuf, FALSE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), pixbuf,
                                    "icon-name", COL_ICON_NAME, NULL);

    text = gtk_cell_renderer_text_new ();
    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), text, TRUE);
    gtk_cell_layout_set_attributes (GTK_CELL_LAYOUT (combo), text,
                                    "text", COL_LABEL, NULL);

    entry = gtk_bin_get_child (GTK_BIN (combo));
    g_object_bind_property (self, "button-icon", entry, "text",
                            G_BINDING_SYNC_CREATE | G_BINDING_BIDIRECTIONAL);

    gtk_widget_set_hexpand (combo, TRUE);
    return combo;
}

static GtkWidget *
pn_inject_build_property_editor (
        PnNode     *self,
        GParamSpec *pspec,
        GObject    *target G_GNUC_UNUSED,
        GtkWindow  *parent G_GNUC_UNUSED)
{
    if (g_strcmp0 (pspec->name, "button-icon") == 0)
        return build_button_icon_editor (PN_INJECT (self));

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_inject_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_INJECT));

    node_class->build_property_editor = pn_inject_build_property_editor;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid. */
}
