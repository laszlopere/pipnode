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
/*  Known Nodes page.                                                  */
/*                                                                     */
/*  Sortable GtkTreeView over PnMeshState->nodes.  The owner row is    */
/*  pinned to the top via a primary sort key on "is_us", with a        */
/*  secondary sort on whichever column the user clicks.  No writes,    */
/*  no live monitor -- the page snapshots the state passed to          */
/*  set_state and re-paints from scratch.  Re-painting on every state  */
/*  change is fine: the list is at most a few hundred rows.            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-known-nodes.h"
#include "pn-mesh-formats.h"

#include <time.h>

#define PN_MESH_KNOWN_NODES_CTX_QDATA "pn-mesh-page-known-nodes-ctx"

/* Liststore column order.  IS_US is invisible -- it drives the star
 * column's pixbuf and the owner-row bolding via the cell-data-func. */
enum
{
    COL_IS_US,        /* gboolean, hidden */
    COL_STAR,         /* gchar*, "★" for owner, "" otherwise */
    COL_ID,           /* gchar*  */
    COL_SHORT,        /* gchar*  */
    COL_LONG,         /* gchar*  */
    COL_HW,           /* gchar*  */
    COL_ROLE,         /* gchar*  */
    COL_HOPS,         /* gint, -1 = unset */
    COL_SNR,          /* gchar*  */
    COL_HEARD,        /* gchar*  */
    COL_BATTERY,      /* gchar*  */

    /* Sort keys: numeric mirrors of the display strings so the column
     * sorts in value-order rather than lexicographically. */
    COL_SORT_HEARD,   /* gint, last_heard epoch (0 = never) */
    COL_SORT_SNR,     /* gdouble, 0.0 = unset (also lowest) */
    COL_SORT_BATTERY, /* gint, -1 = unset (sorts below any real value) */

    N_COLS
};

typedef struct
{
    GtkWidget    *page;
    GtkTreeView  *view;
    GtkListStore *store;
    GtkLabel     *summary;
} KnownNodesCtx;

static void
known_nodes_ctx_free (gpointer data)
{
    g_slice_free (KnownNodesCtx, data);
}

/* ------------------------------------------------------------------ */
/*  Formatting                                                          */
/* ------------------------------------------------------------------ */

/* Relative time pretty-printer: ages match the Meshtastic apps' UX
 * (seconds for the last minute, then minutes / hours / days).  An
 * absolute timestamp would be more precise but reading "2m ago"
 * across a row is much faster than parsing "2026-05-27 18:42:13". */
static gchar *
format_relative_time (guint32 epoch)
{
    gint64 now;
    gint64 delta;

    if (epoch == 0)
        return g_strdup ("—");

    now = (gint64) time (NULL);
    delta = now - (gint64) epoch;

    if (delta < 0)
        /* Device clock ahead of host -- show absolute "in the future"
         * cue rather than negative numbers. */
        return g_strdup ("in future");
    if (delta < 60)
        return g_strdup_printf ("%lds ago", (long) delta);
    if (delta < 3600)
        return g_strdup_printf ("%ldm ago", (long) (delta / 60));
    if (delta < 86400)
        return g_strdup_printf ("%ldh ago", (long) (delta / 3600));
    return g_strdup_printf ("%ldd ago", (long) (delta / 86400));
}

static gchar *
format_snr (gfloat snr)
{
    /* The wire format leaves snr=0 to mean both "we never received a
     * direct packet" (owner row of a brand-new device) and "we
     * received a packet at exactly 0 dB" (rare in practice).  We side
     * with the common case and show "—" for 0.0. */
    if (snr == 0.0f)
        return g_strdup ("—");
    return g_strdup_printf ("%.1f dB", (double) snr);
}

static gchar *
format_battery (guint32 level)
{
    /* Per the Meshtastic proto: 0 = unknown, 1..100 = percent, 101 =
     * powered (no battery).  Pretty-print accordingly so the column
     * communicates the difference. */
    if (level == 0)
        return g_strdup ("—");
    if (level >= 101)
        return g_strdup ("powered");
    return g_strdup_printf ("%u%%", level);
}

/* ------------------------------------------------------------------ */
/*  Painters                                                            */
/* ------------------------------------------------------------------ */

/* Bold the owner row across every column.  Hooked via
 * gtk_tree_view_column_set_cell_data_func so the markup applies even
 * when sort order changes -- no need to track row indices. */
static void
bold_if_owner (GtkTreeViewColumn *col,
               GtkCellRenderer   *cell,
               GtkTreeModel      *model,
               GtkTreeIter       *iter,
               gpointer           user_data)
{
    gboolean is_us = FALSE;

    (void) col;
    (void) user_data;

    gtk_tree_model_get (model, iter, COL_IS_US, &is_us, -1);
    g_object_set (cell,
                  "weight", is_us ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                  NULL);
}

static void
add_text_column (GtkTreeView *view, const char *title, int text_col)
{
    GtkCellRenderer   *cell = gtk_cell_renderer_text_new ();
    GtkTreeViewColumn *col  = gtk_tree_view_column_new_with_attributes (
            title, cell, "text", text_col, NULL);

    gtk_tree_view_column_set_resizable (col, TRUE);
    gtk_tree_view_column_set_sort_column_id (col, text_col);
    gtk_tree_view_column_set_cell_data_func (col, cell,
                                             bold_if_owner, NULL, NULL);
    gtk_tree_view_append_column (view, col);
}

/* Same as add_text_column but with a separate model column that
 * drives the sort -- used for Last heard / SNR / Battery where the
 * displayed text is a human label ("3m ago") but we want value-order
 * sorting (epoch). */
static void
add_sorted_text_column (GtkTreeView *view,
                        const char  *title,
                        int          text_col,
                        int          sort_col)
{
    GtkCellRenderer   *cell = gtk_cell_renderer_text_new ();
    GtkTreeViewColumn *col  = gtk_tree_view_column_new_with_attributes (
            title, cell, "text", text_col, NULL);

    gtk_tree_view_column_set_resizable (col, TRUE);
    gtk_tree_view_column_set_sort_column_id (col, sort_col);
    gtk_tree_view_column_set_cell_data_func (col, cell,
                                             bold_if_owner, NULL, NULL);
    gtk_tree_view_append_column (view, col);
}

/* Integer column (hops).  -1 sentinel renders as em-dash. */
static void
hops_cell_data (GtkTreeViewColumn *col,
                GtkCellRenderer   *cell,
                GtkTreeModel      *model,
                GtkTreeIter       *iter,
                gpointer           user_data)
{
    gboolean is_us = FALSE;
    gint     hops  = -1;
    gchar    buf[16];

    (void) col;
    (void) user_data;

    gtk_tree_model_get (model, iter,
                        COL_IS_US, &is_us,
                        COL_HOPS,  &hops,
                        -1);

    if (hops < 0)
        g_strlcpy (buf, "—", sizeof buf);
    else
        g_snprintf (buf, sizeof buf, "%d", hops);

    g_object_set (cell,
                  "text",   buf,
                  "weight", is_us ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                  NULL);
}

static void
add_hops_column (GtkTreeView *view)
{
    GtkCellRenderer   *cell = gtk_cell_renderer_text_new ();
    GtkTreeViewColumn *col  = gtk_tree_view_column_new_with_attributes (
            "Hops", cell, NULL);

    gtk_tree_view_column_set_resizable (col, TRUE);
    gtk_tree_view_column_set_sort_column_id (col, COL_HOPS);
    gtk_tree_view_column_set_cell_data_func (col, cell,
                                             hops_cell_data, NULL, NULL);
    gtk_tree_view_append_column (view, col);
}

/* ------------------------------------------------------------------ */
/*  Population                                                          */
/* ------------------------------------------------------------------ */

static void
populate (KnownNodesCtx *ctx, const PnMeshState *state)
{
    guint i;
    guint visible;

    gtk_list_store_clear (ctx->store);

    if (state == NULL || state->nodes == NULL || state->nodes->len == 0)
    {
        gtk_label_set_text (
                ctx->summary,
                "No nodes reported by the device yet. A device that has "
                "never heard a peer still shows its own row once the "
                "handshake completes — check the Identity tab first.");
        return;
    }

    visible = 0;
    for (i = 0; i < state->nodes->len; i++)
    {
        PnMeshNode *n = g_ptr_array_index (state->nodes, i);
        gchar      *hw;
        const char *role;
        gchar      *heard;
        gchar      *snr;
        gchar      *batt;
        GtkTreeIter iter;

        hw    = pn_mesh_format_hw_model (n->hw_model);
        role  = pn_mesh_format_role (n->role);
        heard = format_relative_time (n->last_heard);
        snr   = format_snr (n->snr);
        batt  = format_battery (n->battery_level);

        gtk_list_store_append (ctx->store, &iter);
        gtk_list_store_set (ctx->store, &iter,
                COL_IS_US,        n->is_us,
                COL_STAR,         n->is_us ? "★" : "",
                COL_ID,           n->id        != NULL ? n->id        : "",
                COL_SHORT,        n->short_name != NULL ? n->short_name : "",
                COL_LONG,         n->long_name != NULL ? n->long_name : "",
                COL_HW,           hw,
                COL_ROLE,         role != NULL ? role : "",
                /* hops_away=0 has two meanings: a direct neighbour
                 * (legitimate) and "field not set" (device never
                 * heard this node directly).  For the owner row 0
                 * is meaningless (the device is itself), so paint
                 * an em-dash instead -- otherwise show the value. */
                COL_HOPS,         n->is_us ? -1 : (gint) n->hops_away,
                COL_SNR,          snr,
                COL_HEARD,        heard,
                COL_BATTERY,      batt,
                COL_SORT_HEARD,   (gint) n->last_heard,
                COL_SORT_SNR,     (gdouble) n->snr,
                COL_SORT_BATTERY, n->battery_level == 0
                                  ? -1 : (gint) n->battery_level,
                -1);

        g_free (hw);
        g_free (heard);
        g_free (snr);
        g_free (batt);
        visible++;
    }

    {
        gchar *msg = g_strdup_printf (
                "%u node%s in the device's database.",
                visible, visible == 1 ? "" : "s");
        gtk_label_set_text (ctx->summary, msg);
        g_free (msg);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_mesh_page_known_nodes_new (void)
{
    KnownNodesCtx *ctx;
    GtkWidget     *box;
    GtkWidget     *scrolled;
    GtkWidget     *view;
    GtkWidget     *summary;
    GtkListStore  *store;

    ctx = g_slice_new0 (KnownNodesCtx);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start  (box, 12);
    gtk_widget_set_margin_end    (box, 12);
    gtk_widget_set_margin_top    (box, 12);
    gtk_widget_set_margin_bottom (box, 12);

    summary = gtk_label_new (
            "Connect to a device to see its known-nodes database.");
    gtk_label_set_xalign (GTK_LABEL (summary), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (summary), TRUE);
    gtk_box_pack_start (GTK_BOX (box), summary, FALSE, FALSE, 0);
    ctx->summary = GTK_LABEL (summary);

    store = gtk_list_store_new (N_COLS,
                                G_TYPE_BOOLEAN,  /* IS_US        */
                                G_TYPE_STRING,   /* STAR         */
                                G_TYPE_STRING,   /* ID           */
                                G_TYPE_STRING,   /* SHORT        */
                                G_TYPE_STRING,   /* LONG         */
                                G_TYPE_STRING,   /* HW           */
                                G_TYPE_STRING,   /* ROLE         */
                                G_TYPE_INT,      /* HOPS         */
                                G_TYPE_STRING,   /* SNR          */
                                G_TYPE_STRING,   /* HEARD        */
                                G_TYPE_STRING,   /* BATTERY      */
                                G_TYPE_INT,      /* SORT_HEARD   */
                                G_TYPE_DOUBLE,   /* SORT_SNR     */
                                G_TYPE_INT);     /* SORT_BATTERY */
    ctx->store = store;

    view = gtk_tree_view_new_with_model (GTK_TREE_MODEL (store));
    /* The store keeps one ref, we drop ours; the view borrows. */
    g_object_unref (store);
    ctx->view = GTK_TREE_VIEW (view);
    gtk_tree_view_set_headers_clickable (ctx->view, TRUE);
    gtk_tree_view_set_grid_lines (ctx->view, GTK_TREE_VIEW_GRID_LINES_HORIZONTAL);

    /* Sort owner row to the top by default: descending on IS_US
     * (TRUE > FALSE), then ascending on hops so close neighbours
     * follow.  The user can override by clicking any header. */
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE (store),
                                          COL_IS_US, GTK_SORT_DESCENDING);

    add_text_column (ctx->view, "",      COL_STAR);
    add_text_column (ctx->view, "ID",    COL_ID);
    add_text_column (ctx->view, "Short", COL_SHORT);
    add_text_column (ctx->view, "Long",  COL_LONG);
    add_text_column (ctx->view, "HW",    COL_HW);
    add_text_column (ctx->view, "Role",  COL_ROLE);
    add_hops_column (ctx->view);
    add_sorted_text_column (ctx->view, "SNR",        COL_SNR,     COL_SORT_SNR);
    add_sorted_text_column (ctx->view, "Last heard", COL_HEARD,   COL_SORT_HEARD);
    add_sorted_text_column (ctx->view, "Battery",    COL_BATTERY, COL_SORT_BATTERY);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    /* Phase 6 gotcha (iv): inside a GtkExpander packed FALSE/FALSE
     * in a tab box, set_min_content_height alone collapses the tree
     * to one or two rows.  The reliable recipe is min_content_height
     * + propagate_natural_height + an explicit set_size_request on
     * the tree itself.  220 px shows ~7 rows; the user can still
     * scroll if the mesh is busier than that. */
    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scrolled),
                                                220);
    gtk_scrolled_window_set_propagate_natural_height (
            GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_widget_set_size_request (view, -1, 200);
    gtk_container_add (GTK_CONTAINER (scrolled), view);
    gtk_box_pack_start (GTK_BOX (box), scrolled, TRUE, TRUE, 0);

    ctx->page = box;
    g_object_set_data_full (G_OBJECT (box),
                            PN_MESH_KNOWN_NODES_CTX_QDATA,
                            ctx, known_nodes_ctx_free);

    return box;
}

void
pn_mesh_page_known_nodes_set_state (GtkWidget         *page,
                                    const PnMeshState *state)
{
    KnownNodesCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_KNOWN_NODES_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    populate (ctx, state);
}
