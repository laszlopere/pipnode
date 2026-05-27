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
/*  Identity page — Phase 2e read-only summary.                        */
/*                                                                     */
/*  Two-column grid: a bold key label on the left, a value label on    */
/*  the right.  The dialog hands us the device kind (already known     */
/*  from the discover row) plus the state captured by the              */
/*  want_config_id handshake; we render every field in one place so    */
/*  the user can see at a glance what the device thinks it is.        */
/*                                                                     */
/*  Each value label is held by qdata under its key string so          */
/*  set_state() can refresh them all without re-walking the grid.      */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-identity.h"

/* qdata keys for the individual value labels. */
#define VL_KIND        "vl-kind"
#define VL_TTY         "vl-tty"
#define VL_NODE_NUM    "vl-node-num"
#define VL_LONG_NAME   "vl-long-name"
#define VL_SHORT_NAME  "vl-short-name"
#define VL_HW_MODEL    "vl-hw-model"
#define VL_CHANNELS    "vl-channels"

static void
attach_field (GtkGrid     *grid,
              GtkWidget   *page,
              gint         row,
              const gchar *label_text,
              const gchar *qdata_key)
{
    GtkWidget *key;
    GtkWidget *val;

    key = gtk_label_new (label_text);
    gtk_label_set_xalign (GTK_LABEL (key), 0.0);
    {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs,
                                pango_attr_weight_new (PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes (GTK_LABEL (key), attrs);
        pango_attr_list_unref (attrs);
    }
    gtk_widget_set_margin_end (key, 16);

    val = gtk_label_new ("—");
    gtk_label_set_xalign (GTK_LABEL (val), 0.0);
    gtk_label_set_selectable (GTK_LABEL (val), TRUE);
    /* Long owner names / tty paths get an ellipsis rather than
     * pushing the column wider. */
    gtk_label_set_ellipsize (GTK_LABEL (val), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (val, TRUE);

    gtk_grid_attach (grid, key, 0, row, 1, 1);
    gtk_grid_attach (grid, val, 1, row, 1, 1);

    /* Borrowed pointer stashed on the page, looked up by set_state. */
    g_object_set_data (G_OBJECT (page), qdata_key, val);
}

GtkWidget *
pn_mesh_page_identity_new (void)
{
    GtkWidget *page;
    GtkWidget *title;
    GtkWidget *subtitle;
    GtkWidget *grid;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start  (page, 24);
    gtk_widget_set_margin_end    (page, 24);
    gtk_widget_set_margin_top    (page, 18);
    gtk_widget_set_margin_bottom (page, 18);

    title = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (title),
                          "<span size='large' weight='bold'>Identity</span>");
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_box_pack_start (GTK_BOX (page), title, FALSE, FALSE, 0);

    subtitle = gtk_label_new (
            "What the device reported during the configuration handshake.");
    gtk_label_set_xalign (GTK_LABEL (subtitle), 0.0);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 12);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);

    attach_field (GTK_GRID (grid), page, 0, "Device",          VL_KIND);
    attach_field (GTK_GRID (grid), page, 1, "Serial port",     VL_TTY);
    attach_field (GTK_GRID (grid), page, 2, "Mesh node #",     VL_NODE_NUM);
    attach_field (GTK_GRID (grid), page, 3, "Long name",       VL_LONG_NAME);
    attach_field (GTK_GRID (grid), page, 4, "Short name",      VL_SHORT_NAME);
    attach_field (GTK_GRID (grid), page, 5, "Hardware model",  VL_HW_MODEL);
    attach_field (GTK_GRID (grid), page, 6, "Channels",        VL_CHANNELS);

    return page;
}

static void
set_label (GtkWidget *page, const gchar *key, const gchar *text)
{
    GtkLabel *label = g_object_get_data (G_OBJECT (page), key);
    /* An em-dash for an empty value keeps the column visually
     * populated and signals "nothing here yet" rather than looking
     * like a layout bug. */
    gtk_label_set_text (label, (text != NULL && *text != '\0') ? text : "—");
}

/* Count channels whose role != 0 (DISABLED).  pip-mesh follows the
 * same convention: a disabled slot exists but does not count as a
 * usable channel for the user. */
static guint
count_active_channels (const PnMeshState *state)
{
    guint i;
    guint n = 0;

    if (state == NULL || state->channels == NULL)
        return 0;

    for (i = 0; i < state->channels->len; i++)
    {
        const PnMeshChannel *ch = g_ptr_array_index (state->channels, i);
        if (ch->role != 0)
            n++;
    }
    return n;
}

void
pn_mesh_page_identity_set_state (GtkWidget         *page,
                                 const gchar       *device_kind,
                                 const gchar       *tty_path,
                                 const PnMeshState *state)
{
    g_return_if_fail (GTK_IS_WIDGET (page));

    set_label (page, VL_KIND, device_kind);
    set_label (page, VL_TTY,  tty_path);

    if (state == NULL)
    {
        set_label (page, VL_NODE_NUM,   NULL);
        set_label (page, VL_LONG_NAME,  NULL);
        set_label (page, VL_SHORT_NAME, NULL);
        set_label (page, VL_HW_MODEL,   NULL);
        set_label (page, VL_CHANNELS,   NULL);
        return;
    }

    {
        /* Render as "!abcd1234" hex when an id was learned, else the
         * decimal node number on its own.  pip-mesh's --list-nodes
         * uses the same convention. */
        gchar *node_text = state->owner_id != NULL && *state->owner_id != '\0'
                ? g_strdup_printf ("%u (%s)",
                                   state->my_node_num, state->owner_id)
                : g_strdup_printf ("%u", state->my_node_num);
        set_label (page, VL_NODE_NUM, node_text);
        g_free (node_text);
    }

    set_label (page, VL_LONG_NAME,  state->owner_long_name);
    set_label (page, VL_SHORT_NAME, state->owner_short_name);

    {
        /* Raw enum int for Phase 2e; Phase 3 will swap this for the
         * human-readable name (TLORA_V2, HELTEC_V3, …) using the
         * mapping pip-mesh's format_hw_model already does. */
        gchar *hw = g_strdup_printf ("%u", state->owner_hw_model);
        set_label (page, VL_HW_MODEL, hw);
        g_free (hw);
    }

    {
        guint active = count_active_channels (state);
        gchar *txt;
        if (state->channels != NULL && state->channels->len > active)
            txt = g_strdup_printf ("%u active (%u total)",
                                   active, state->channels->len);
        else
            txt = g_strdup_printf ("%u", active);
        set_label (page, VL_CHANNELS, txt);
        g_free (txt);
    }
}
