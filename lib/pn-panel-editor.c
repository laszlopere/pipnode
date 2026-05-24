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

/* Size of the free-positioning canvas.  It is deliberately larger than a
 * typical viewport so there is room to spread widgets out; the host wraps
 * the editor in a scrolled window, which scrolls to reach the rest. */
#define PN_PE_CANVAS_WIDTH  1000
#define PN_PE_CANVAS_HEIGHT  700

struct _PnPanelEditor
{
    GtkBox parent_instance;

    /* The shared model.  Owned via a reference; @nodes is borrowed from
     * it (the flow keeps it alive). */
    PnFlow      *flow;
    PnNodeStore *nodes;

    /* Free-positioning canvas the readout rows are placed on, plus the
     * "nothing here yet" hint shown when there are no countdown nodes. */
    GtkWidget *canvas;
    GtkWidget *empty_label;

    /* PnCountdown node (borrowed; the store owns the ref) → its row
     * widget (a #GtkEventBox drag handle wrapping the framed
     * #PnLedDisplay, owned by @canvas).  The map is the source of truth
     * for the one-readout-per-node invariant. */
    GHashTable *rows;

    /* Drag state.  @drag_child is the row currently being dragged (a
     * canvas child), or %NULL when idle; @drag_grab_x/y is the pointer's
     * offset within that child at the moment of grab, so the row tracks
     * the pointer without jumping; @drag_start_x/y is where the row sat
     * at grab time, so a release that did not actually move it leaves the
     * stored layout (and the document's modified flag) untouched. */
    GtkWidget *drag_child;
    gdouble    drag_grab_x;
    gdouble    drag_grab_y;
    gint       drag_start_x;
    gint       drag_start_y;

    /* Running count of rows ever placed, used to cascade the initial
     * position of each new readout so they do not all land on top of
     * one another. */
    guint      cascade;
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
    gtk_widget_set_visible (self->canvas,     !empty);
}

/* Begin dragging the row under the pointer.  We remember where inside the
 * row the pointer grabbed so the row follows it without snapping its
 * corner to the cursor, and raise the row above its siblings so it stays
 * visible while it is moved over them. */
static gboolean
on_row_button_press (GtkWidget      *child,
                     GdkEventButton *event,
                     gpointer        user_data)
{
    PnPanelEditor *self = PN_PANEL_EDITOR (user_data);
    GdkWindow     *win;

    if (event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;

    self->drag_child  = child;
    self->drag_grab_x = event->x;
    self->drag_grab_y = event->y;
    gtk_container_child_get (GTK_CONTAINER (self->canvas), child,
                             "x", &self->drag_start_x,
                             "y", &self->drag_start_y, NULL);

    win = gtk_widget_get_window (child);
    if (win != NULL)
        gdk_window_raise (win);

    return GDK_EVENT_STOP;
}

/* Track the pointer: move the grabbed row so the grab point stays under
 * the cursor, clamped so the row cannot be dragged off the canvas. */
static gboolean
on_row_motion (GtkWidget      *child,
               GdkEventMotion *event,
               gpointer        user_data)
{
    PnPanelEditor *self = PN_PANEL_EDITOR (user_data);
    gint           px, py;
    gint           nx, ny;

    if (self->drag_child != child)
        return GDK_EVENT_PROPAGATE;

    /* event->x/y are relative to @child; translate to canvas coordinates
     * so the move is expressed in the canvas's own space. */
    if (!gtk_widget_translate_coordinates (child, self->canvas,
                                           (gint) event->x, (gint) event->y,
                                           &px, &py))
        return GDK_EVENT_PROPAGATE;

    nx = px - (gint) self->drag_grab_x;
    ny = py - (gint) self->drag_grab_y;

    nx = CLAMP (nx, 0,
                MAX (0, gtk_widget_get_allocated_width  (self->canvas)
                            - gtk_widget_get_allocated_width  (child)));
    ny = CLAMP (ny, 0,
                MAX (0, gtk_widget_get_allocated_height (self->canvas)
                            - gtk_widget_get_allocated_height (child)));

    gtk_fixed_move (GTK_FIXED (self->canvas), child, nx, ny);
    return GDK_EVENT_STOP;
}

static gboolean
on_row_button_release (GtkWidget      *child,
                       GdkEventButton *event,
                       gpointer        user_data)
{
    PnPanelEditor *self = PN_PANEL_EDITOR (user_data);
    gint           x, y;
    PnNode        *node;

    if (event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;
    if (self->drag_child != child)
        return GDK_EVENT_PROPAGATE;

    self->drag_child = NULL;

    /* Persist the placement only when the row actually moved, so a plain
     * click never dirties the document. */
    gtk_container_child_get (GTK_CONTAINER (self->canvas), child,
                             "x", &x, "y", &y, NULL);
    node = g_object_get_data (G_OBJECT (child), "pn-node");
    if (node != NULL && (x != self->drag_start_x || y != self->drag_start_y))
        pn_flow_set_panel_position (self->flow, pn_node_get_uuid (node), x, y);

    return GDK_EVENT_STOP;
}

/* Give a row a move ("fleur") cursor so it reads as draggable.  Deferred
 * to realize because the cursor is set on the row's #GdkWindow. */
static void
on_row_realize (GtkWidget *child, gpointer user_data)
{
    GdkWindow  *win = gtk_widget_get_window (child);
    GdkDisplay *display;
    GdkCursor  *cursor;

    (void) user_data;
    if (win == NULL)
        return;

    display = gtk_widget_get_display (child);
    cursor  = gdk_cursor_new_from_name (display, "move");
    if (cursor == NULL)
        cursor = gdk_cursor_new_for_display (display, GDK_FLEUR);

    gdk_window_set_cursor (win, cursor);
    g_clear_object (&cursor);
}

/* Initial canvas position for the next readout: cascade down a column,
 * wrapping to the next column once one fills, so a fresh editor lays the
 * existing nodes out tidily before the user rearranges them. */
static void
panel_editor_next_position (PnPanelEditor *self, gint *out_x, gint *out_y)
{
    const gint margin  = 24;
    const gint step_y  = PN_PE_PREVIEW_HEIGHT + 28;
    const gint step_x  = 170;
    gint       per_col = MAX (1, (PN_PE_CANVAS_HEIGHT - margin) / step_y);

    *out_x = margin + (self->cascade / per_col) * step_x;
    *out_y = margin + (self->cascade % per_col) * step_y;
}

/* Create the readout for @node when it is a countdown node we are not
 * already tracking. */
static void
panel_editor_add_node (PnPanelEditor *self, PnNode *node)
{
    GtkWidget *handle;
    GtkWidget *frame;
    GtkWidget *led;
    gint       x, y;

    if (!PN_IS_COUNTDOWN (node))
        return;
    if (g_hash_table_contains (self->rows, node))
        return;

    /* Framed readout, no caption — the bezel reads as a panel-mounted
     * display. */
    frame = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_IN);

    led = pn_led_display_new ();
    pn_led_display_set_height (PN_LED_DISPLAY (led), PN_PE_PREVIEW_HEIGHT);
    gtk_widget_set_margin_top    (led, 4);
    gtk_widget_set_margin_bottom (led, 4);
    gtk_widget_set_margin_start  (led, 6);
    gtk_widget_set_margin_end    (led, 6);
    gtk_container_add (GTK_CONTAINER (frame), led);

    /* An event box wraps the frame as the drag handle: the LED display is
     * a plain drawing area with no event mask, so button/motion events
     * fall through to this parent, which we subscribe to motion as well
     * as the default button events. */
    handle = gtk_event_box_new ();
    gtk_widget_add_events (handle, GDK_POINTER_MOTION_MASK);
    gtk_container_add (GTK_CONTAINER (handle), frame);

    /* The drag/release handlers recover the node from the handle to key
     * its saved position; the store owns the node and the handle never
     * outlives it (panel_editor_remove_node destroys the handle), so a
     * borrowed pointer is safe. */
    g_object_set_data (G_OBJECT (handle), "pn-node", node);

    g_signal_connect (handle, "button-press-event",
                      G_CALLBACK (on_row_button_press), self);
    g_signal_connect (handle, "motion-notify-event",
                      G_CALLBACK (on_row_motion), self);
    g_signal_connect (handle, "button-release-event",
                      G_CALLBACK (on_row_button_release), self);
    g_signal_connect (handle, "realize",
                      G_CALLBACK (on_row_realize), NULL);

    /* Seed the current value, then track it live.  Tying the handler to
     * the readout (g_signal_connect_object) means destroying the row when
     * the node goes away also severs the connection — no manual handler
     * bookkeeping, no dangling callbacks. */
    panel_editor_sync_value (node, PN_LED_DISPLAY (led));
    g_signal_connect_object (node, "repaint-needed",
                             G_CALLBACK (on_node_repaint_needed), led, 0);

    /* Prefer a position saved in the document; otherwise fall back to the
     * cascade so a never-arranged node still lands somewhere sensible. */
    {
        gdouble sx, sy;
        if (pn_flow_get_panel_position (self->flow,
                                        pn_node_get_uuid (node), &sx, &sy))
        {
            x = (gint) sx;
            y = (gint) sy;
        }
        else
        {
            panel_editor_next_position (self, &x, &y);
        }
    }
    gtk_fixed_put (GTK_FIXED (self->canvas), handle, x, y);
    gtk_widget_show_all (handle);
    self->cascade++;

    g_hash_table_insert (self->rows, node, handle);
    panel_editor_update_empty_state (self);
}

/* Destroy the readout for @node, if we have one. */
static void
panel_editor_remove_node (PnPanelEditor *self, PnNode *node)
{
    GtkWidget *handle = g_hash_table_lookup (self->rows, node);

    if (handle == NULL)
        return;

    /* If the row was mid-drag, forget it before it is destroyed. */
    if (self->drag_child == handle)
        self->drag_child = NULL;

    /* Destroying the handle destroys its frame and #PnLedDisplay, which
     * auto-disconnects the node's repaint-needed handler. */
    gtk_widget_destroy (handle);
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
    GtkWidget *title;

    self->rows = g_hash_table_new (g_direct_hash, g_direct_equal);

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

    title = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (title),
            "<span size='large' weight='bold'>Panel applet GUI editor</span>");
    gtk_widget_set_margin_top    (title, 8);
    gtk_widget_set_margin_bottom (title, 8);
    gtk_box_pack_start (GTK_BOX (self), title, FALSE, FALSE, 0);

    /* The free-positioning canvas the readouts are dragged around on.  A
     * size request gives the host's scrolled window a generous area to
     * scroll, so widgets can be spread well beyond one viewport.  Both the
     * canvas and the empty hint opt out of show_all so the host's
     * gtk_widget_show_all on the tab cannot override the explicit
     * visibility we toggle between them in
     * panel_editor_update_empty_state. */
    self->canvas = gtk_fixed_new ();
    gtk_widget_set_size_request (self->canvas,
                                 PN_PE_CANVAS_WIDTH, PN_PE_CANVAS_HEIGHT);
    gtk_widget_set_no_show_all (self->canvas, TRUE);
    gtk_box_pack_start (GTK_BOX (self), self->canvas, TRUE, TRUE, 0);

    self->empty_label = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (self->empty_label),
            "<span foreground='#888888'>"
            "No Countdown nodes yet — add one to a worksheet</span>");
    gtk_widget_set_no_show_all (self->empty_label, TRUE);
    gtk_box_pack_start (GTK_BOX (self), self->empty_label, TRUE, FALSE, 0);

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
