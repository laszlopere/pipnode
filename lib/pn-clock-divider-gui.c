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
/*  PnClockDivider — gui tier.                                         */
/*                                                                     */
/*  Replaces the auto-generated tab (an "outputs" spin and a raw JSON  */
/*  "divisors" entry) with the output-count spin followed by one       */
/*  divisor spin per output.  The spins are rebuilt when the count     */
/*  changes, so there are always exactly as many as the node has       */
/*  outputs, and refreshed in place when "divisors" changes from       */
/*  elsewhere (undo, D-Bus).  Every change goes straight to the node   */
/*  through pn_clock_divider_set_divisor(); the dialog edits the live  */
/*  node.                                                              */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-clock-divider-gui.h"
#include "pn-clock-divider.h"
#include "pn-node-dialog-helpers.h"

#include <gtk/gtk.h>

typedef struct
{
    PnClockDivider *node;          /* borrowed; outlives the dialog      */
    GtkWidget      *spins_box;     /* refilled with a fresh grid         */
    GPtrArray      *spins;         /* borrowed GtkSpinButton per output  */
    gulong          outputs_handler;
    gulong          divisors_handler;
    gboolean        updating;      /* our own writes; skip our handlers  */
} ClockDividerTab;

static void
tab_free (gpointer data)
{
    ClockDividerTab *tab = data;

    g_signal_handler_disconnect (tab->node, tab->outputs_handler);
    g_signal_handler_disconnect (tab->node, tab->divisors_handler);
    g_ptr_array_unref (tab->spins);
    g_free (tab);
}

static void
on_spin_changed (GtkSpinButton *spin, gpointer user_data)
{
    ClockDividerTab *tab   = user_data;
    gint             index = GPOINTER_TO_INT (
            g_object_get_data (G_OBJECT (spin), "pn-divisor-index"));

    if (tab->updating)
        return;

    tab->updating = TRUE;
    pn_clock_divider_set_divisor (tab->node, index,
                                  (guint) gtk_spin_button_get_value_as_int (spin));
    tab->updating = FALSE;
}

static void
rebuild_spins (ClockDividerTab *tab)
{
    GtkWidget *grid = pn_node_dialog_new_property_grid ();
    gint       n    = pn_node_get_n_outputs (PN_NODE (tab->node));
    gint       i;

    gtk_container_foreach (GTK_CONTAINER (tab->spins_box),
                           (GtkCallback) gtk_widget_destroy, NULL);
    g_ptr_array_set_size (tab->spins, 0);

    /* The grid's own margins would double the tab's; the rows line up
     * with the spin row above without them. */
    g_object_set (grid, "margin", 0, NULL);

    for (i = 0; i < n; i++)
    {
        GtkWidget *spin  = gtk_spin_button_new_with_range (
                PN_CLOCK_DIVIDER_MIN_DIVISOR, PN_CLOCK_DIVIDER_MAX_DIVISOR, 1);
        gchar     *label = g_strdup_printf ("Output %d divisor", i + 1);

        gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin),
                                   pn_clock_divider_get_divisor (tab->node, i));
        gtk_widget_set_hexpand (spin, TRUE);
        g_object_set_data (G_OBJECT (spin), "pn-divisor-index",
                           GINT_TO_POINTER (i));
        g_signal_connect (spin, "value-changed",
                          G_CALLBACK (on_spin_changed), tab);

        pn_node_dialog_attach_row (GTK_GRID (grid), i, label, spin);
        g_ptr_array_add (tab->spins, spin);
        g_free (label);
    }

    gtk_container_add (GTK_CONTAINER (tab->spins_box), grid);
    gtk_widget_show_all (tab->spins_box);
}

static void
on_outputs_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) object;
    (void) pspec;
    rebuild_spins (user_data);
}

static void
on_divisors_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClockDividerTab *tab = user_data;
    guint            i;

    (void) object;
    (void) pspec;

    if (tab->updating)
        return;

    tab->updating = TRUE;
    for (i = 0; i < tab->spins->len; i++)
    {
        GtkSpinButton *spin = g_ptr_array_index (tab->spins, i);
        guint          want = pn_clock_divider_get_divisor (tab->node, (gint) i);

        if ((guint) gtk_spin_button_get_value_as_int (spin) != want)
            gtk_spin_button_set_value (spin, want);
    }
    tab->updating = FALSE;
}

static GtkWidget *
pn_clock_divider_build_class_tab (
        PnNode    *self,
        GtkWindow *parent G_GNUC_UNUSED)
{
    GtkWidget       *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget       *top   = pn_node_dialog_new_property_grid ();
    GtkWidget       *hint;
    GtkWidget       *scrolled;
    ClockDividerTab *tab   = g_new0 (ClockDividerTab, 1);
    GParamSpec      *pspec;

    tab->node  = PN_CLOCK_DIVIDER (self);
    tab->spins = g_ptr_array_new ();

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (self), "outputs");
    pn_node_dialog_attach_row (GTK_GRID (top), 0, "Outputs",
                               pn_node_dialog_default_editor (G_OBJECT (self),
                                                              pspec));
    gtk_box_pack_start (GTK_BOX (outer), top, FALSE, FALSE, 0);

    hint = gtk_label_new (
            "Each output forwards every tick whose count is a multiple of "
            "its divisor: a divisor of 4 fires on the 4th, 8th, 12th tick. "
            "A message on the reset input starts the count again.");
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
    gtk_widget_set_sensitive (hint, FALSE);
    g_object_set (hint, "margin-start", 12, "margin-end", 12, NULL);
    gtk_box_pack_start (GTK_BOX (outer), hint, FALSE, FALSE, 0);

    tab->spins_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    g_object_set (tab->spins_box,
                  "margin-start",  12,
                  "margin-end",    12,
                  "margin-bottom", 12,
                  NULL);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (
            GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), tab->spins_box);
    gtk_box_pack_start (GTK_BOX (outer), scrolled, TRUE, TRUE, 0);

    tab->outputs_handler = g_signal_connect (
            self, "notify::outputs", G_CALLBACK (on_outputs_notify), tab);
    tab->divisors_handler = g_signal_connect (
            self, "notify::divisors", G_CALLBACK (on_divisors_notify), tab);

    g_object_set_data_full (G_OBJECT (outer), "pn-clock-divider-tab",
                            tab, tab_free);

    rebuild_spins (tab);
    return outer;
}

void
pn_clock_divider_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_CLOCK_DIVIDER));

    node_class->build_class_tab = pn_clock_divider_build_class_tab;

    /* Class ref held for the process lifetime, like the factory's. */
}
