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
/*  PnValueRouter — gui tier.                                          */
/*                                                                     */
/*  Replaces the auto-generated tab (an "outputs" spin, a "path" entry */
/*  and a raw JSON "rules" entry) with the output-count spin, the path */
/*  entry, and one rule row per output: an operator combo followed by  */
/*  one number entry, or two for a range.  The rows are rebuilt when   */
/*  the count changes and refreshed in place when "rules" changes from */
/*  elsewhere (undo, D-Bus).  Every edit goes straight to the node     */
/*  through pn_value_router_set_rule(); the dialog edits the live node.*/
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-value-router-gui.h"
#include "pn-value-router.h"
#include "pn-node-dialog-helpers.h"

#include <gtk/gtk.h>
#include <stdlib.h>

/* Combo entries, in display order: id = the rule's JSON op spelling. */
static const struct
{
    PnValueRouterOp op;
    const gchar    *label;
} op_choices[] = {
    { PN_VALUE_ROUTER_OP_UNUSED, "unused" },
    { PN_VALUE_ROUTER_OP_LT,     "<" },
    { PN_VALUE_ROUTER_OP_LE,     "\xe2\x89\xa4" },          /* ≤ */
    { PN_VALUE_ROUTER_OP_EQ,     "=" },
    { PN_VALUE_ROUTER_OP_NE,     "\xe2\x89\xa0" },          /* ≠ */
    { PN_VALUE_ROUTER_OP_GE,     "\xe2\x89\xa5" },          /* ≥ */
    { PN_VALUE_ROUTER_OP_GT,     ">" },
    { PN_VALUE_ROUTER_OP_RANGE,  "between" },
    { PN_VALUE_ROUTER_OP_ELSE,   "otherwise" },
};

typedef struct
{
    GtkWidget *combo;
    GtkWidget *a;      /* value, or the range's low end   */
    GtkWidget *and;    /* "and" label between range ends  */
    GtkWidget *b;      /* the range's high end            */
} RuleRow;

typedef struct
{
    PnValueRouter *node;          /* borrowed; outlives the dialog        */
    GtkWidget     *rows_box;      /* refilled with a fresh grid           */
    GArray        *rows;          /* RuleRow per output                   */
    gulong         outputs_handler;
    gulong         rules_handler;
    gboolean       updating;      /* our own writes; skip our handlers    */
} ValueRouterTab;

static void
tab_free (gpointer data)
{
    ValueRouterTab *tab = data;

    g_signal_handler_disconnect (tab->node, tab->outputs_handler);
    g_signal_handler_disconnect (tab->node, tab->rules_handler);
    g_array_unref (tab->rows);
    g_free (tab);
}

static gchar *
format_number (gdouble v)
{
    gchar buf[G_ASCII_DTOSTR_BUF_SIZE];

    return g_strdup (g_ascii_formatd (buf, sizeof buf, "%.15g", v));
}

/* The number typed into @entry; empty or unparsable text reads as 0. */
static gdouble
entry_number (GtkWidget *entry)
{
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (entry));

    return g_ascii_strtod (text, NULL);
}

/* Show only the fields the row's operator uses. */
static void
sync_visibility (RuleRow *row, PnValueRouterOp op)
{
    gboolean has_a = op != PN_VALUE_ROUTER_OP_UNUSED &&
                     op != PN_VALUE_ROUTER_OP_ELSE;
    gboolean range = op == PN_VALUE_ROUTER_OP_RANGE;

    gtk_widget_set_visible (row->a,   has_a);
    gtk_widget_set_visible (row->and, range);
    gtk_widget_set_visible (row->b,   range);
}

static PnValueRouterOp
combo_op (GtkWidget *combo)
{
    gint i = gtk_combo_box_get_active (GTK_COMBO_BOX (combo));

    return (i >= 0 && i < (gint) G_N_ELEMENTS (op_choices))
           ? op_choices[i].op : PN_VALUE_ROUTER_OP_UNUSED;
}

static gint
op_choice_index (PnValueRouterOp op)
{
    guint i;

    for (i = 0; i < G_N_ELEMENTS (op_choices); i++)
        if (op_choices[i].op == op)
            return (gint) i;
    return 0;
}

/* Put the node's rule for output @index into @row. */
static void
load_row (ValueRouterTab *tab, RuleRow *row, gint index)
{
    gdouble          a, b;
    PnValueRouterOp  op = pn_value_router_get_rule (tab->node, index, &a, &b);
    gchar           *text;

    gtk_combo_box_set_active (GTK_COMBO_BOX (row->combo), op_choice_index (op));

    /* Leave a half-typed number alone when it already means the same. */
    if (entry_number (row->a) != a || *gtk_entry_get_text (GTK_ENTRY (row->a)) == '\0')
    {
        text = format_number (a);
        gtk_entry_set_text (GTK_ENTRY (row->a), text);
        g_free (text);
    }
    if (entry_number (row->b) != b || *gtk_entry_get_text (GTK_ENTRY (row->b)) == '\0')
    {
        text = format_number (b);
        gtk_entry_set_text (GTK_ENTRY (row->b), text);
        g_free (text);
    }

    sync_visibility (row, op);
}

static void
on_row_changed (GtkWidget *widget, gpointer user_data)
{
    ValueRouterTab *tab   = user_data;
    gint            index = GPOINTER_TO_INT (
            g_object_get_data (G_OBJECT (widget), "pn-rule-index"));
    RuleRow        *row;
    PnValueRouterOp op;

    if (tab->updating || index < 0 || (guint) index >= tab->rows->len)
        return;

    row = &g_array_index (tab->rows, RuleRow, index);
    op  = combo_op (row->combo);
    sync_visibility (row, op);

    tab->updating = TRUE;
    pn_value_router_set_rule (tab->node, index, op,
                              entry_number (row->a), entry_number (row->b));
    tab->updating = FALSE;
}

static GtkWidget *
new_number_entry (ValueRouterTab *tab, gint index)
{
    GtkWidget *entry = gtk_entry_new ();

    gtk_entry_set_width_chars (GTK_ENTRY (entry), 8);
    gtk_entry_set_input_purpose (GTK_ENTRY (entry), GTK_INPUT_PURPOSE_NUMBER);
    gtk_widget_set_hexpand (entry, TRUE);
    g_object_set_data (G_OBJECT (entry), "pn-rule-index", GINT_TO_POINTER (index));
    g_signal_connect (entry, "changed", G_CALLBACK (on_row_changed), tab);
    gtk_widget_set_no_show_all (entry, TRUE);
    return entry;
}

static void
rebuild_rows (ValueRouterTab *tab)
{
    GtkWidget *grid = pn_node_dialog_new_property_grid ();
    gint       n    = pn_node_get_n_outputs (PN_NODE (tab->node));
    gint       i;

    gtk_container_foreach (GTK_CONTAINER (tab->rows_box),
                           (GtkCallback) gtk_widget_destroy, NULL);
    g_array_set_size (tab->rows, 0);
    g_object_set (grid, "margin", 0, NULL);

    tab->updating = TRUE;
    for (i = 0; i < n; i++)
    {
        RuleRow    row   = { 0 };
        GtkWidget *hbox  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gchar     *label = g_strdup_printf ("Output %d", i + 1);
        guint      k;

        row.combo = gtk_combo_box_text_new ();
        for (k = 0; k < G_N_ELEMENTS (op_choices); k++)
            gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (row.combo),
                                            op_choices[k].label);
        g_object_set_data (G_OBJECT (row.combo), "pn-rule-index",
                           GINT_TO_POINTER (i));
        g_signal_connect (row.combo, "changed",
                          G_CALLBACK (on_row_changed), tab);

        row.a   = new_number_entry (tab, i);
        row.and = gtk_label_new ("and");
        gtk_widget_set_no_show_all (row.and, TRUE);
        row.b   = new_number_entry (tab, i);

        gtk_box_pack_start (GTK_BOX (hbox), row.combo, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (hbox), row.a,     TRUE,  TRUE,  0);
        gtk_box_pack_start (GTK_BOX (hbox), row.and,   FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (hbox), row.b,     TRUE,  TRUE,  0);

        pn_node_dialog_attach_row (GTK_GRID (grid), i, label, hbox);
        g_array_append_val (tab->rows, row);
        load_row (tab, &g_array_index (tab->rows, RuleRow, i), i);
        g_free (label);
    }
    tab->updating = FALSE;

    gtk_container_add (GTK_CONTAINER (tab->rows_box), grid);
    gtk_widget_show_all (tab->rows_box);
}

static void
on_outputs_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) object;
    (void) pspec;
    rebuild_rows (user_data);
}

static void
on_rules_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ValueRouterTab *tab = user_data;
    guint           i;

    (void) object;
    (void) pspec;

    if (tab->updating)
        return;

    tab->updating = TRUE;
    for (i = 0; i < tab->rows->len; i++)
        load_row (tab, &g_array_index (tab->rows, RuleRow, i), (gint) i);
    tab->updating = FALSE;
}

static GtkWidget *
pn_value_router_build_class_tab (
        PnNode    *self,
        GtkWindow *parent G_GNUC_UNUSED)
{
    GtkWidget      *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget      *top   = pn_node_dialog_new_property_grid ();
    GtkWidget      *hint;
    GtkWidget      *scrolled;
    ValueRouterTab *tab   = g_new0 (ValueRouterTab, 1);
    GObjectClass   *klass = G_OBJECT_GET_CLASS (self);

    tab->node = PN_VALUE_ROUTER (self);
    tab->rows = g_array_new (FALSE, TRUE, sizeof (RuleRow));

    pn_node_dialog_attach_row (GTK_GRID (top), 0, "Outputs",
            pn_node_dialog_default_editor (G_OBJECT (self),
                    g_object_class_find_property (klass, "outputs")));
    pn_node_dialog_attach_row (GTK_GRID (top), 1, "Path",
            pn_node_dialog_default_editor (G_OBJECT (self),
                    g_object_class_find_property (klass, "path")));
    gtk_box_pack_start (GTK_BOX (outer), top, FALSE, FALSE, 0);

    hint = gtk_label_new (
            "A message leaves by the first output whose rule matches the "
            "number at Path; unmatched messages are dropped. \"between\" "
            "includes both ends. \"otherwise\" matches everything, so put "
            "it last.");
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
    gtk_widget_set_sensitive (hint, FALSE);
    g_object_set (hint, "margin-start", 12, "margin-end", 12, NULL);
    gtk_box_pack_start (GTK_BOX (outer), hint, FALSE, FALSE, 0);

    tab->rows_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    g_object_set (tab->rows_box,
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
    gtk_container_add (GTK_CONTAINER (scrolled), tab->rows_box);
    gtk_box_pack_start (GTK_BOX (outer), scrolled, TRUE, TRUE, 0);

    tab->outputs_handler = g_signal_connect (
            self, "notify::outputs", G_CALLBACK (on_outputs_notify), tab);
    tab->rules_handler = g_signal_connect (
            self, "notify::rules", G_CALLBACK (on_rules_notify), tab);

    g_object_set_data_full (G_OBJECT (outer), "pn-value-router-tab",
                            tab, tab_free);

    rebuild_rows (tab);
    return outer;
}

void
pn_value_router_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_VALUE_ROUTER));

    node_class->build_class_tab = pn_value_router_build_class_tab;

    /* Class ref held for the process lifetime, like the factory's. */
}
