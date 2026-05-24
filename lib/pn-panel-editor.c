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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-panel-editor.h"
#include "pn-led-display.h"
#include "pn-node.h"
#include "pn-node-store.h"
#include "pn-countdown.h"

/* ------------------------------------------------------------------ */
/*  PnPanelEditor                                                      */
/*                                                                     */
/*  Panel-applet GUI layout editor — the visual counterpart to the     */
/*  node-wiring #PnWorksheet, but for laying out the widgets a flow     */
/*  drives rather than the dataflow itself.  Other GUI-layout editors   */
/*  (desktop, web, mobile) are planned to follow the same shape.        */
/*                                                                     */
/*  Role split: #PnLedDisplay (from the GTK/Cairo-only panel-widgets    */
/*  library) is a dumb readout that knows nothing about nodes; this     */
/*  editor is the controller that binds it to the model.  It keeps one  */
/*  readout per #PnCountdown node across the whole flow (every sheet),  */
/*  observing the flow's single node store to create and destroy        */
/*  readouts as countdown nodes come and go, and mirroring each node's  */
/*  live value through its repaint-needed signal.                       */
/*                                                                     */
/*  This editor lives in libpipnode-gui, which links the node runtime,  */
/*  so it may reference PnCountdown / PnFlow / PnNodeStore freely; only  */
/*  the shared panel-widgets library must stay node-free.               */
/* ------------------------------------------------------------------ */

/* Pixel height of each readout — a typical XFCE panel icon size, so the
 * preview matches what lands on a real panel. */
#define PN_PE_PREVIEW_HEIGHT 36

struct _PnPanelEditor
{
    GtkBox parent_instance;

    /* The shared model.  Owned via a reference; @nodes is borrowed from
     * it (the flow keeps it alive). */
    PnFlow      *flow;
    PnNodeStore *nodes;

    /* Vertical stack of readout rows and the "nothing here yet" hint
     * shown when there are no countdown nodes. */
    GtkWidget *list_box;
    GtkWidget *empty_label;

    /* PnCountdown node (borrowed; the store owns the ref) → its row
     * widget (the framed #PnLedDisplay, owned by @list_box).  The map is
     * the source of truth for the one-readout-per-node invariant. */
    GHashTable *rows;
};

G_DEFINE_TYPE (PnPanelEditor, pn_panel_editor, GTK_TYPE_BOX)

/* Push @node's current countdown value into its readout, using the same
 * day/hour/minute/second breakdown the node itself paints with. */
static void
panel_editor_sync_value (PnNode *node, PnLedDisplay *led)
{
    PnCountdownPaintState st;
    gint64                seconds;

    pn_countdown_get_paint_state (PN_COUNTDOWN (node), &st);
    seconds = st.days * 86400 + st.hours * 3600 + st.minutes * 60 + st.seconds;

    pn_led_display_set_day_digits (led, st.day_digits);
    pn_led_display_set_seconds    (led, seconds);
}

/* repaint-needed on a countdown node → refresh its readout.  @user_data
 * is the row's #PnLedDisplay (this handler is connected with
 * g_signal_connect_object so it dies with the readout). */
static void
on_node_repaint_needed (PnNode *node, gpointer user_data)
{
    panel_editor_sync_value (node, PN_LED_DISPLAY (user_data));
}

/* Toggle the empty-state hint to match the live readout count. */
static void
panel_editor_update_empty_state (PnPanelEditor *self)
{
    gboolean empty = (g_hash_table_size (self->rows) == 0);

    gtk_widget_set_visible (self->empty_label, empty);
    gtk_widget_set_visible (self->list_box,   !empty);
}

/* Create the readout for @node when it is a countdown node we are not
 * already tracking. */
static void
panel_editor_add_node (PnPanelEditor *self, PnNode *node)
{
    GtkWidget *frame;
    GtkWidget *led;

    if (!PN_IS_COUNTDOWN (node))
        return;
    if (g_hash_table_contains (self->rows, node))
        return;

    /* Framed readout, no caption — the bezel reads as a panel-mounted
     * display. */
    frame = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_IN);
    gtk_widget_set_halign (frame, GTK_ALIGN_CENTER);

    led = pn_led_display_new ();
    pn_led_display_set_height (PN_LED_DISPLAY (led), PN_PE_PREVIEW_HEIGHT);
    gtk_widget_set_margin_top    (led, 4);
    gtk_widget_set_margin_bottom (led, 4);
    gtk_widget_set_margin_start  (led, 6);
    gtk_widget_set_margin_end    (led, 6);
    gtk_container_add (GTK_CONTAINER (frame), led);

    /* Seed the current value, then track it live.  Tying the handler to
     * the readout (g_signal_connect_object) means destroying the row when
     * the node goes away also severs the connection — no manual handler
     * bookkeeping, no dangling callbacks. */
    panel_editor_sync_value (node, PN_LED_DISPLAY (led));
    g_signal_connect_object (node, "repaint-needed",
                             G_CALLBACK (on_node_repaint_needed), led, 0);

    gtk_box_pack_start (GTK_BOX (self->list_box), frame, FALSE, FALSE, 0);
    gtk_widget_show_all (frame);

    g_hash_table_insert (self->rows, node, frame);
    panel_editor_update_empty_state (self);
}

/* Destroy the readout for @node, if we have one. */
static void
panel_editor_remove_node (PnPanelEditor *self, PnNode *node)
{
    GtkWidget *frame = g_hash_table_lookup (self->rows, node);

    if (frame == NULL)
        return;

    /* Destroying the frame destroys its #PnLedDisplay, which auto-
     * disconnects the node's repaint-needed handler. */
    gtk_widget_destroy (frame);
    g_hash_table_remove (self->rows, node);
    panel_editor_update_empty_state (self);
}

/* The store's node-added / node-removed both carry (node, index); we use
 * only the node. */
static void
on_store_node_added (PnNodeStore *store,
                     PnNode      *node,
                     guint        index,
                     gpointer     user_data)
{
    (void) store; (void) index;
    panel_editor_add_node (PN_PANEL_EDITOR (user_data), node);
}

static void
on_store_node_removed (PnNodeStore *store,
                       PnNode      *node,
                       guint        index,
                       gpointer     user_data)
{
    (void) store; (void) index;
    panel_editor_remove_node (PN_PANEL_EDITOR (user_data), node);
}

/* Bind the editor to @flow: keep a reference, subscribe to the node
 * store, and build a readout for every countdown node already present so
 * a freshly-created editor immediately mirrors the whole flow. */
static void
panel_editor_attach_flow (PnPanelEditor *self, PnFlow *flow)
{
    guint n;
    guint i;

    self->flow  = g_object_ref (flow);
    self->nodes = pn_flow_get_nodes (flow);

    g_signal_connect (self->nodes, "node-added",
                      G_CALLBACK (on_store_node_added), self);
    g_signal_connect (self->nodes, "node-removed",
                      G_CALLBACK (on_store_node_removed), self);

    n = pn_node_store_get_length (self->nodes);
    for (i = 0; i < n; i++)
        panel_editor_add_node (self, pn_node_store_get_node (self->nodes, i));
}

static void
pn_panel_editor_dispose (GObject *object)
{
    PnPanelEditor *self = PN_PANEL_EDITOR (object);

    /* Drop the store subscriptions before the flow reference goes; the
     * per-node repaint handlers fall away on their own as the readouts
     * are destroyed with the widget tree. */
    if (self->nodes != NULL)
        g_signal_handlers_disconnect_by_data (self->nodes, self);
    self->nodes = NULL;

    g_clear_object (&self->flow);
    g_clear_pointer (&self->rows, g_hash_table_unref);

    G_OBJECT_CLASS (pn_panel_editor_parent_class)->dispose (object);
}

static void
pn_panel_editor_class_init (PnPanelEditorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = pn_panel_editor_dispose;
}

static void
pn_panel_editor_init (PnPanelEditor *self)
{
    GtkWidget *content;
    GtkWidget *title;

    self->rows = g_hash_table_new (g_direct_hash, g_direct_equal);

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

    /* A centred column floats in the middle of the canvas (expand=TRUE,
     * fill=FALSE centres it vertically; halign centres it across). */
    content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign (content, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (self), content, TRUE, FALSE, 0);

    title = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (title),
            "<span size='large' weight='bold'>Panel applet GUI editor</span>");
    gtk_box_pack_start (GTK_BOX (content), title, FALSE, FALSE, 0);

    /* One readout per countdown node stacks here.  Both the list and the
     * empty hint opt out of show_all so the host's gtk_widget_show_all on
     * the tab cannot override the explicit visibility we toggle between
     * them in panel_editor_update_empty_state. */
    self->list_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_no_show_all (self->list_box, TRUE);
    gtk_box_pack_start (GTK_BOX (content), self->list_box, FALSE, FALSE, 0);

    self->empty_label = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (self->empty_label),
            "<span foreground='#888888'>"
            "No Countdown nodes yet — add one to a worksheet</span>");
    gtk_widget_set_no_show_all (self->empty_label, TRUE);
    gtk_box_pack_start (GTK_BOX (content), self->empty_label, FALSE, FALSE, 0);

    gtk_widget_show_all (content);

    /* Start in the empty state; attach_flow flips it as rows appear. */
    panel_editor_update_empty_state (self);
}

GtkWidget *
pn_panel_editor_new (PnFlow *flow)
{
    PnPanelEditor *self;

    g_return_val_if_fail (PN_IS_FLOW (flow), NULL);

    self = g_object_new (PN_TYPE_PANEL_EDITOR, NULL);
    panel_editor_attach_flow (self, flow);
    return GTK_WIDGET (self);
}
