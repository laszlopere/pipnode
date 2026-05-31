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
/*  Device-dialog form helpers.                                        */
/*                                                                     */
/*  Extracted verbatim from the Meshtastic dialog pages               */
/*  (pn-mesh-page-*.c), where each page had grown its own private      */
/*  copy of these row/section idioms.  Behaviour is unchanged; the     */
/*  only difference is the symbols are now public and shared.          */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-device-form.h"
#include "pn-device-combo.h"
#include "pn-foldable.h"

/* ------------------------------------------------------------------ */
/*  Section scaffolding                                                 */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_device_form_key_label (const gchar *text)
{
    GtkWidget     *key   = gtk_label_new (text);
    PangoAttrList *attrs = pango_attr_list_new ();

    gtk_label_set_xalign (GTK_LABEL (key), 0.0);
    pango_attr_list_insert (attrs,
                            pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (key), attrs);
    pango_attr_list_unref (attrs);
    gtk_widget_set_margin_end (key, 16);
    return key;
}

GtkWidget *
pn_device_form_new_tab (GtkWidget **inner_box_out)
{
    GtkWidget *scrolled;
    GtkWidget *box;

    g_return_val_if_fail (inner_box_out != NULL, NULL);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (box, 12);
    gtk_widget_set_margin_end    (box, 12);
    gtk_widget_set_margin_top    (box, 12);
    gtk_widget_set_margin_bottom (box, 12);
    gtk_container_add (GTK_CONTAINER (scrolled), box);

    *inner_box_out = box;
    return scrolled;
}

void
pn_device_form_add_section (GtkWidget   *parent,
                            const gchar *title,
                            const gchar *description,
                            GtkWidget   *child)
{
    GtkWidget *foldable;

    g_return_if_fail (GTK_IS_BOX (parent));
    g_return_if_fail (description != NULL);
    g_return_if_fail (GTK_IS_WIDGET (child));

    /* PnFoldable carries the bold heading, the mandatory description line
     * and the shared foldable styling; gtk_container_add() packs @child
     * into the body after the description. */
    foldable = pn_foldable_new (title, description);
    gtk_container_add (GTK_CONTAINER (foldable), child);
    gtk_box_pack_start (GTK_BOX (parent), foldable, FALSE, FALSE, 0);
}

GtkWidget *
pn_device_form_add_action_bar (GtkWidget *parent)
{
    GtkWidget *bar;

    g_return_val_if_fail (GTK_IS_BOX (parent), NULL);

    bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign     (bar, GTK_ALIGN_END);
    gtk_widget_set_margin_top (bar, 8);
    gtk_box_pack_start (GTK_BOX (parent), bar, FALSE, FALSE, 0);
    return bar;
}

/* ------------------------------------------------------------------ */
/*  Rows                                                                */
/* ------------------------------------------------------------------ */

GtkLabel *
pn_device_form_attach_label_row (GtkGrid     *grid,
                                 gint         row,
                                 const gchar *key_text)
{
    GtkWidget *val;

    g_return_val_if_fail (GTK_IS_GRID (grid), NULL);

    gtk_grid_attach (grid, pn_device_form_key_label (key_text), 0, row, 1, 1);

    val = gtk_label_new ("—");
    gtk_label_set_xalign     (GTK_LABEL (val), 0.0);
    gtk_label_set_selectable (GTK_LABEL (val), TRUE);
    gtk_label_set_ellipsize  (GTK_LABEL (val), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand   (val, TRUE);
    gtk_grid_attach (grid, val, 1, row, 1, 1);
    return GTK_LABEL (val);
}

void
pn_device_form_set_value (GtkLabel    *label,
                          const gchar *text)
{
    g_return_if_fail (GTK_IS_LABEL (label));
    gtk_label_set_text (label,
                        (text != NULL && *text != '\0') ? text : "—");
}

/* The value column lines up by adding every (non-switch) control to a
 * per-grid horizontal size group keyed on the grid via qdata; the
 * widths can only be requested once the control is realized, hence the
 * one-shot realize handler. */
#define PN_DEVICE_FORM_VALUE_MIN_WIDTH 260

static GtkSizeGroup *
get_value_size_group (GtkGrid *grid)
{
    GtkSizeGroup *sg = g_object_get_data (G_OBJECT (grid),
                                          "pn-device-form-value-sg");
    if (sg == NULL)
    {
        sg = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
        g_object_set_data_full (G_OBJECT (grid), "pn-device-form-value-sg",
                                sg, g_object_unref);
    }
    return sg;
}

static void
on_cell_realize (GtkWidget *cell, gpointer user_data)
{
    GtkSizeGroup *sg       = user_data;
    GList        *children = gtk_container_get_children (GTK_CONTAINER (cell));

    /* Combos, spin buttons and entries share a common width via the
     * size group so the value column lines up.  A switch is naturally
     * narrow and already left-aligned (halign START), so leave it out
     * -- otherwise the size group stretches it to the widest control. */
    if (children != NULL && !GTK_IS_SWITCH (children->data))
    {
        GtkWidget *first = children->data;
        gtk_widget_set_size_request (first,
                                     PN_DEVICE_FORM_VALUE_MIN_WIDTH, -1);
        gtk_size_group_add_widget (sg, first);
    }
    g_list_free (children);
    g_signal_handlers_disconnect_by_func (cell, on_cell_realize, sg);
}

GtkWidget *
pn_device_form_attach_control_row (GtkGrid     *grid,
                                   gint         row,
                                   const gchar *key_text)
{
    GtkWidget *cell;

    g_return_val_if_fail (GTK_IS_GRID (grid), NULL);

    gtk_grid_attach (grid, pn_device_form_key_label (key_text), 0, row, 1, 1);

    /* Holder ensures the control sits flush-left rather than
     * stretching to fill an over-wide column. */
    cell = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (cell, TRUE);
    gtk_grid_attach (grid, cell, 1, row, 1, 1);
    g_signal_connect (cell, "realize",
                      G_CALLBACK (on_cell_realize),
                      get_value_size_group (grid));
    return cell;
}

GtkEntry *
pn_device_form_attach_entry_row (GtkGrid     *grid,
                                 gint         row,
                                 const gchar *key_text)
{
    GtkWidget *cell;
    GtkWidget *entry;

    g_return_val_if_fail (GTK_IS_GRID (grid), NULL);

    cell  = pn_device_form_attach_control_row (grid, row, key_text);
    entry = gtk_entry_new ();
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_box_pack_start (GTK_BOX (cell), entry, TRUE, TRUE, 0);
    return GTK_ENTRY (entry);
}

GtkWidget *
pn_device_form_reload_button_new (const gchar *tooltip,
                                  GCallback    cb,
                                  gpointer     user_data)
{
    GtkWidget *btn = gtk_button_new_from_icon_name ("view-refresh",
                                                    GTK_ICON_SIZE_BUTTON);

    /* Flat and vertically centred so it reads as a quiet affordance beside
     * the value, not a primary button. */
    gtk_button_set_relief (GTK_BUTTON (btn), GTK_RELIEF_NONE);
    gtk_widget_set_valign  (btn, GTK_ALIGN_CENTER);
    if (tooltip != NULL)
        gtk_widget_set_tooltip_text (btn, tooltip);
    if (cb != NULL)
        g_signal_connect (btn, "clicked", cb, user_data);
    return btn;
}

GtkComboBoxText *
pn_device_form_attach_reload_combo (GtkGrid     *grid,
                                    gint         row,
                                    const gchar *key_text,
                                    const gchar *reload_tooltip,
                                    GCallback    reload_cb,
                                    gpointer     user_data)
{
    GtkWidget *cell;
    GtkWidget *combo;
    GtkWidget *reload;

    g_return_val_if_fail (GTK_IS_GRID (grid), NULL);

    cell  = pn_device_form_attach_control_row (grid, row, key_text);
    combo = pn_device_combo_new ();   /* wheel-safe GtkComboBoxText */
    gtk_widget_set_hexpand (combo, TRUE);
    gtk_box_pack_start (GTK_BOX (cell), combo, TRUE, TRUE, 0);

    /* Reload lives in its own column 2, flush right of the value cell --
     * the same placement the per-value refresh buttons use, so a column of
     * reload affordances stays aligned however wide the controls grow. */
    reload = pn_device_form_reload_button_new (reload_tooltip, reload_cb,
                                               user_data);
    gtk_grid_attach (grid, reload, 2, row, 1, 1);

    return GTK_COMBO_BOX_TEXT (combo);
}

/* ------------------------------------------------------------------ */
/*  Enum combos                                                         */
/* ------------------------------------------------------------------ */

void
pn_device_form_combo_fill (GtkComboBoxText         *combo,
                           const PnDeviceEnumEntry *table,
                           gsize                    n)
{
    gsize i;

    g_return_if_fail (GTK_IS_COMBO_BOX_TEXT (combo));
    g_return_if_fail (table != NULL);

    for (i = 0; i < n; i++)
    {
        gchar id[12];
        g_snprintf (id, sizeof id, "%u", table[i].id);
        gtk_combo_box_text_append (combo, id, table[i].name);
    }
}

void
pn_device_form_combo_select (GtkComboBoxText         *combo,
                             const PnDeviceEnumEntry *table,
                             gsize                    n,
                             guint32                  value)
{
    gchar id[12];
    gsize i;

    g_return_if_fail (GTK_IS_COMBO_BOX_TEXT (combo));
    g_return_if_fail (table != NULL);

    for (i = 0; i < n; i++)
        if (table[i].id == value)
        {
            g_snprintf (id, sizeof id, "%u", value);
            gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), id);
            return;
        }

    /* Unknown id from a newer firmware: surface it rather than silently
     * snapping to a different value the user didn't choose. */
    g_snprintf (id, sizeof id, "%u", value);
    {
        gchar *label = g_strdup_printf ("(unknown #%u)", value);
        gtk_combo_box_text_append (combo, id, label);
        g_free (label);
    }
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), id);
}

guint32
pn_device_form_combo_get_id (GtkComboBoxText *combo,
                             guint32          fallback)
{
    const gchar *id;

    g_return_val_if_fail (GTK_IS_COMBO_BOX_TEXT (combo), fallback);

    id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo));
    if (id == NULL)
        return fallback;
    return (guint32) g_ascii_strtoull (id, NULL, 10);
}
