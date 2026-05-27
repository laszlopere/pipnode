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
#include "pn-panel-geometry.h"
#include "pn-led-display.h"
#include "pn-led-lamp.h"
#include "pn-matrix57-display.h"
#include "pn-numeric-display.h"
#include "pn-switch-widget.h"
#include "pn-text-display.h"
#include "pn-node.h"
#include "pn-node-store.h"
#include "pn-countdown.h"
#include "pn-digital-clock.h"
#include "pn-label.h"
#include "pn-led.h"
#include "pn-matrix57.h"
#include "pn-numeric.h"
#include "pn-switch.h"

/* ------------------------------------------------------------------ */
/*  PnPanelEditor                                                      */
/*                                                                     */
/*  Panel-applet GUI layout editor — the visual counterpart to the     */
/*  node-wiring #PnWorksheet, but for laying out the widgets a flow     */
/*  drives rather than the dataflow itself.  Other GUI-layout editors   */
/*  (desktop, web, mobile) are planned to follow the same shape.        */
/*                                                                     */
/*  Role split: the panel widgets (#PnLedDisplay, #PnLedLamp, from the  */
/*  GTK/Cairo-only panel-widgets library) are dumb faces that know       */
/*  nothing about nodes; this editor is the controller that binds them   */
/*  to the model.  It keeps one widget per representable node across the  */
/*  whole flow (every sheet) — a seven-segment readout for each          */
/*  #PnCountdown, an indicator lamp for each #PnLed — observing the       */
/*  flow's single node store to create and destroy widgets as those      */
/*  nodes come and go, and mirroring each node's live value through its   */
/*  repaint-needed signal.                                              */
/*                                                                     */
/*  This editor lives in libpipnode-gui, which links the node runtime,  */
/*  so it may reference PnCountdown / PnLed / PnFlow / PnNodeStore       */
/*  freely; only the shared panel-widgets library must stay node-free.  */
/* ------------------------------------------------------------------ */

/* PN_PE_PREVIEW_HEIGHT and the panel-band geometry (PN_PE_PANEL_TOP /
 * PN_PE_PANEL_HEIGHT) live in the shared pn-panel-geometry.h so the
 * headless engine's band-membership test stays in lock-step with the snap
 * target used here.  The band is drawn as two thin dashed edges across the
 * canvas one widget apart, so a snapped widget fills it edge to edge. */

/* Size of the free-positioning canvas.  It is deliberately larger than a
 * typical viewport so there is room to spread widgets out; the host wraps
 * the editor in a scrolled window, which scrolls to reach the rest. */
#define PN_PE_CANVAS_WIDTH  1000
#define PN_PE_CANVAS_HEIGHT  700

/* Sticky snapping of a dragged row to the panel band: it snaps when the
 * pointer-driven position comes within PN_PE_SNAP_GRAB px of the band, and
 * stays stuck until the pointer is pulled more than PN_PE_SNAP_BREAK px
 * away — so a widget clings to the panel yet can be torn off deliberately.
 * BREAK is the larger of the two, which is what gives the snap its grip. */
#define PN_PE_SNAP_GRAB     10
#define PN_PE_SNAP_BREAK    22

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

    /* Represented node (borrowed; the store owns the ref) → its row
     * widget (a #GtkEventBox drag handle wrapping the node's panel
     * widget — a framed #PnLedDisplay or a #PnLedLamp — owned by
     * @canvas).  The map is the source of truth for the
     * one-widget-per-node invariant. */
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

    /* Whether the row being dragged is currently stuck to the panel band.
     * Carried across motion events so the snap has hysteresis: it holds
     * until the pointer pulls PN_PE_SNAP_BREAK px clear (see on_row_motion). */
    gboolean   drag_snapped;

    /* Running count of rows ever placed, used to cascade the initial
     * position of each new readout so they do not all land on top of
     * one another. */
    guint      cascade;
};

G_DEFINE_TYPE (PnPanelEditor, pn_panel_editor, GTK_TYPE_BOX)

/* Push @node's current countdown value into its readout, using the same
 * day/hour/minute/second breakdown the node itself paints with. */
static void
panel_editor_sync_countdown (PnNode *node, PnLedDisplay *led)
{
    PnCountdownPaintState st;
    gint64                seconds;

    pn_countdown_get_paint_state (PN_COUNTDOWN (node), &st);
    seconds = st.days * 86400 + st.hours * 3600 + st.minutes * 60 + st.seconds;

    pn_led_display_set_day_digits   (led, st.day_digits);
    pn_led_display_set_seconds      (led, seconds);
    pn_led_display_set_segment_color (led, st.segment_color.red,
                                     st.segment_color.green,
                                     st.segment_color.blue,
                                     st.segment_color.alpha);
    pn_led_display_set_unlit_color  (led, st.unlit_segment_color.red,
                                     st.unlit_segment_color.green,
                                     st.unlit_segment_color.blue,
                                     st.unlit_segment_color.alpha);
}

/* repaint-needed on a countdown node → refresh its readout.  @user_data
 * is the row's #PnLedDisplay (this handler is connected with
 * g_signal_connect_object so it dies with the readout). */
static void
on_countdown_repaint_needed (PnNode *node, gpointer user_data)
{
    panel_editor_sync_countdown (node, PN_LED_DISPLAY (user_data));
}

/* Push @node's current digital-clock value into its readout.  The clock is
 * the Countdown without a days field, so it mirrors onto a days-less
 * (day_digits == 0) HH:MM:SS readout. */
static void
panel_editor_sync_digital_clock (PnNode *node, PnLedDisplay *led)
{
    PnDigitalClockPaintState st;
    gint64                   seconds;

    pn_digital_clock_get_paint_state (PN_DIGITAL_CLOCK (node), &st);
    seconds = st.hours * 3600 + st.minutes * 60 + st.seconds;

    pn_led_display_set_day_digits   (led, 0);
    pn_led_display_set_seconds      (led, seconds);
    pn_led_display_set_segment_color (led, st.segment_color.red,
                                     st.segment_color.green,
                                     st.segment_color.blue,
                                     st.segment_color.alpha);
    pn_led_display_set_unlit_color  (led, st.unlit_segment_color.red,
                                     st.unlit_segment_color.green,
                                     st.unlit_segment_color.blue,
                                     st.unlit_segment_color.alpha);
}

/* repaint-needed on a digital-clock node → refresh its readout.
 * @user_data is the row's #PnLedDisplay (connected with
 * g_signal_connect_object so it dies with the readout). */
static void
on_digital_clock_repaint_needed (PnNode *node, gpointer user_data)
{
    panel_editor_sync_digital_clock (node, PN_LED_DISPLAY (user_data));
}

/* Push @node's current lit state and lit colour into its lamp, reading
 * both through the LED node's GTK-free accessors. */
static void
panel_editor_sync_led (PnNode *node, PnLedLamp *lamp)
{
    PnColor color;

    pn_led_peek_color (PN_LED (node), &color);
    pn_led_lamp_set_color (lamp, color.red, color.green, color.blue);
    pn_led_lamp_set_lit   (lamp, pn_led_get_lit (PN_LED (node)));
}

/* repaint-needed on an LED node → refresh its lamp.  @user_data is the
 * row's #PnLedLamp (connected with g_signal_connect_object so it dies
 * with the lamp). */
static void
on_led_repaint_needed (PnNode *node, gpointer user_data)
{
    panel_editor_sync_led (node, PN_LED_LAMP (user_data));
}

/* Push @node's current text and styling into its readout, reading them
 * through the Label node's GTK-free paint-state accessor. */
static void
panel_editor_sync_label (PnNode *node, PnTextDisplay *text)
{
    PnLabelPaintState st;

    pn_label_get_paint_state (PN_LABEL (node), &st);

    pn_text_display_set_text  (text, st.text);
    pn_text_display_set_lines (text, st.lines);
    pn_text_display_set_font  (text, st.font_family, st.font_scale,
                               st.weight, st.italic);
    pn_text_display_set_align (text, (gint) st.align);
    pn_text_display_set_color (text, st.text_color.red,
                               st.text_color.green, st.text_color.blue);
    pn_text_display_set_background (text, st.background_color.red,
                                   st.background_color.green,
                                   st.background_color.blue,
                                   st.background_color.alpha);
}

/* repaint-needed on a Label node → refresh its readout.  @user_data is the
 * row's #PnTextDisplay (connected with g_signal_connect_object so it dies
 * with the readout). */
static void
on_label_repaint_needed (PnNode *node, gpointer user_data)
{
    panel_editor_sync_label (node, PN_TEXT_DISPLAY (user_data));
}

/* Push @node's current text and styling into its readout, reading them
 * through the Matrix57 node's GTK-free paint-state accessor. */
static void
panel_editor_sync_matrix57 (PnNode *node, PnMatrix57Display *display)
{
    PnMatrix57PaintState st;

    pn_matrix57_get_paint_state (PN_MATRIX57 (node), &st);

    pn_matrix57_display_set_text  (display, st.text);
    pn_matrix57_display_set_cells (display, st.cells);
    pn_matrix57_display_set_lines (display, st.lines);
    pn_matrix57_display_set_frame_color (display,
                                         st.frame_color.red,
                                         st.frame_color.green,
                                         st.frame_color.blue);
    pn_matrix57_display_set_background_color (display,
                                              st.background_color.red,
                                              st.background_color.green,
                                              st.background_color.blue);
    pn_matrix57_display_set_pixel_color (display,
                                         st.pixel_color.red,
                                         st.pixel_color.green,
                                         st.pixel_color.blue);
    pn_matrix57_display_set_unlit_pixel_color (display,
                                               st.unlit_pixel_color.red,
                                               st.unlit_pixel_color.green,
                                               st.unlit_pixel_color.blue);
}

/* repaint-needed on a Matrix57 node → refresh its readout.  @user_data is
 * the row's #PnMatrix57Display (connected with g_signal_connect_object so
 * it dies with the readout). */
static void
on_matrix57_repaint_needed (PnNode *node, gpointer user_data)
{
    panel_editor_sync_matrix57 (node, PN_MATRIX57_DISPLAY (user_data));
}

/* Push @node's current value and styling into its readout, reading them
 * through the Numeric node's GTK-free paint-state accessor. */
static void
panel_editor_sync_numeric (PnNode *node, PnNumericDisplay *display)
{
    PnNumericPaintState st;

    pn_numeric_get_paint_state (PN_NUMERIC (node), &st);

    pn_numeric_display_set_digits         (display, st.digits);
    pn_numeric_display_set_decimal_places (display, st.decimal_places);
    pn_numeric_display_set_has_value      (display, st.has_value);
    if (st.has_value)
        pn_numeric_display_set_value      (display, st.value);
    pn_numeric_display_set_segment_color  (display,
                                           st.segment_color.red,
                                           st.segment_color.green,
                                           st.segment_color.blue,
                                           st.segment_color.alpha);
    pn_numeric_display_set_unlit_color    (display,
                                           st.unlit_segment_color.red,
                                           st.unlit_segment_color.green,
                                           st.unlit_segment_color.blue,
                                           st.unlit_segment_color.alpha);
}

/* repaint-needed on a Numeric node → refresh its readout.  @user_data is
 * the row's #PnNumericDisplay (connected with g_signal_connect_object so
 * it dies with the readout). */
static void
on_numeric_repaint_needed (PnNode *node, gpointer user_data)
{
    panel_editor_sync_numeric (node, PN_NUMERIC_DISPLAY (user_data));
}

/* Push @node's current latch position into its toggle, reading it through
 * the Switch node's GTK-free accessor. */
static void
panel_editor_sync_switch (PnNode *node, PnSwitchWidget *toggle)
{
    pn_switch_widget_set_on (toggle, pn_switch_get_on (PN_SWITCH (node)));
}

/* repaint-needed on a Switch node → refresh its toggle.  @user_data is the
 * row's #PnSwitchWidget (connected with g_signal_connect_object so it dies
 * with the toggle). */
static void
on_switch_repaint_needed (PnNode *node, gpointer user_data)
{
    panel_editor_sync_switch (node, PN_SWITCH_WIDGET (user_data));
}

/* Toggle the empty-state hint to match the live readout count. */
static void
panel_editor_update_empty_state (PnPanelEditor *self)
{
    gboolean empty = (g_hash_table_size (self->rows) == 0);

    gtk_widget_set_visible (self->empty_label, empty);
    gtk_widget_set_visible (self->canvas,     !empty);
}

/* The y at which @child sits centred in the panel band — the position it
 * snaps to.  With the band exactly one widget tall this aligns the row's
 * edges with the two guide lines. */
static gint
panel_band_snap_y (GtkWidget *child)
{
    gint h = gtk_widget_get_allocated_height (child);

    return PN_PE_PANEL_TOP + (PN_PE_PANEL_HEIGHT - h) / 2;
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

    /* A row picked up already sitting in the band starts out snapped, so it
     * clings rather than tearing off on the first stray pixel of motion. */
    self->drag_snapped = (self->drag_start_y == panel_band_snap_y (child));

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

    /* Sticky-snap the row's vertical position to the panel band.  @ny still
     * tracks the pointer, so its distance from the snap line measures how
     * far the user has pulled: once snapped we hold the row on the line
     * until that pull exceeds the (larger) break distance, then let go. */
    {
        gint snap_y = panel_band_snap_y (child);
        gint dist   = ABS (ny - snap_y);

        if (self->drag_snapped)
        {
            if (dist > PN_PE_SNAP_BREAK)
                self->drag_snapped = FALSE;
            else
                ny = snap_y;
        }
        else if (dist <= PN_PE_SNAP_GRAB)
        {
            self->drag_snapped = TRUE;
            ny = snap_y;
        }
    }

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

/* Build the panel widget that mirrors @node, or %NULL when @node is not a
 * kind the panel editor represents.  Each branch seeds the widget's
 * current value and wires it to follow the node's repaint-needed signal
 * live; tying that handler to the widget (g_signal_connect_object) means
 * destroying the row when the node goes away also severs the connection —
 * no manual handler bookkeeping, no dangling callbacks. */
static GtkWidget *
panel_editor_build_widget (PnNode *node)
{
    if (PN_IS_COUNTDOWN (node))
    {
        /* The readout draws on a transparent background (no frame), so the
         * canvas shows through behind it just as the panel does. */
        GtkWidget *led = pn_led_display_new ();

        pn_led_display_set_height (PN_LED_DISPLAY (led), PN_PE_PREVIEW_HEIGHT);

        panel_editor_sync_countdown (node, PN_LED_DISPLAY (led));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_countdown_repaint_needed),
                                 led, 0);
        return led;
    }

    if (PN_IS_DIGITAL_CLOCK (node))
    {
        /* The readout draws on a transparent background (no frame), so the
         * canvas shows through behind it just as the panel does. */
        GtkWidget *led = pn_led_display_new ();

        pn_led_display_set_height (PN_LED_DISPLAY (led), PN_PE_PREVIEW_HEIGHT);

        panel_editor_sync_digital_clock (node, PN_LED_DISPLAY (led));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_digital_clock_repaint_needed),
                                 led, 0);
        return led;
    }

    if (PN_IS_LABEL (node))
    {
        /* The readout draws on a transparent background (no frame), so the
         * canvas shows through behind it just as the panel does. */
        GtkWidget *text = pn_text_display_new ();

        pn_text_display_set_height (PN_TEXT_DISPLAY (text),
                                    PN_PE_PREVIEW_HEIGHT);

        panel_editor_sync_label (node, PN_TEXT_DISPLAY (text));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_label_repaint_needed),
                                 text, 0);
        return text;
    }

    if (PN_IS_MATRIX57 (node))
    {
        /* The readout draws on a transparent background (no frame), so the
         * canvas shows through behind it just as the panel does. */
        GtkWidget *display = pn_matrix57_display_new ();

        pn_matrix57_display_set_height (PN_MATRIX57_DISPLAY (display),
                                        PN_PE_PREVIEW_HEIGHT);

        panel_editor_sync_matrix57 (node, PN_MATRIX57_DISPLAY (display));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_matrix57_repaint_needed),
                                 display, 0);
        return display;
    }

    if (PN_IS_NUMERIC (node))
    {
        /* Transparent background: the digits sit straight on the canvas
         * (and on the panel applet) so the bezel does not double-frame the
         * row. */
        GtkWidget *display = pn_numeric_display_new ();

        pn_numeric_display_set_height (PN_NUMERIC_DISPLAY (display),
                                       PN_PE_PREVIEW_HEIGHT);

        panel_editor_sync_numeric (node, PN_NUMERIC_DISPLAY (display));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_numeric_repaint_needed),
                                 display, 0);
        return display;
    }

    if (PN_IS_LED (node))
    {
        GtkWidget *lamp = pn_led_lamp_new ();

        pn_led_lamp_set_size (PN_LED_LAMP (lamp), PN_PE_PREVIEW_HEIGHT);

        panel_editor_sync_led (node, PN_LED_LAMP (lamp));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_led_repaint_needed),
                                 lamp, 0);
        return lamp;
    }

    if (PN_IS_SWITCH (node))
    {
        /* Drag-only here: the toggle is left non-interactive (the default)
         * so clicks fall through to the drag handle and arranging the
         * layout never flips the switch.  It still mirrors the live latch
         * state.  The applet is where the toggle becomes clickable. */
        GtkWidget *toggle = pn_switch_widget_new ();

        pn_switch_widget_set_height (PN_SWITCH_WIDGET (toggle),
                                     PN_PE_PREVIEW_HEIGHT);

        panel_editor_sync_switch (node, PN_SWITCH_WIDGET (toggle));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_switch_repaint_needed),
                                 toggle, 0);
        return toggle;
    }

    return NULL;
}

/* Create the panel widget for @node when it is a kind we represent and are
 * not already tracking. */
static void
panel_editor_add_node (PnPanelEditor *self, PnNode *node)
{
    GtkWidget *handle;
    GtkWidget *widget;
    gint       x, y;

    if (g_hash_table_contains (self->rows, node))
        return;

    widget = panel_editor_build_widget (node);
    if (widget == NULL)
        return;

    /* An event box wraps the widget as the drag handle: the panel widgets
     * are plain drawing areas with no event mask, so button/motion events
     * fall through to this parent, which we subscribe to motion as well as
     * the default button events. */
    handle = gtk_event_box_new ();
    gtk_widget_add_events (handle, GDK_POINTER_MOTION_MASK);
    gtk_container_add (GTK_CONTAINER (handle), widget);

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

/* Paint the panel band behind the readouts: two thin dashed horizontal
 * lines spanning the canvas, marking the top and bottom edges of the strip
 * that stands in for the real XFCE panel.  Connected without _after so it
 * runs before GtkFixed propagates the draw to its children, leaving the
 * readouts on top of the guide. */
static gboolean
on_canvas_draw (GtkWidget *canvas, cairo_t *cr, gpointer user_data)
{
    static const double dashes[] = { 6.0, 4.0 };
    int    width  = gtk_widget_get_allocated_width (canvas);
    /* +0.5 lands the 1px stroke on the pixel grid for a crisp line. */
    double top    = PN_PE_PANEL_TOP + 0.5;
    double bottom = PN_PE_PANEL_TOP + PN_PE_PANEL_HEIGHT + 0.5;

    (void) user_data;

    cairo_save (cr);
    cairo_set_line_width (cr, 1.0);
    cairo_set_dash (cr, dashes, G_N_ELEMENTS (dashes), 0.0);
    cairo_set_source_rgba (cr, 0.8, 0.0, 0.0, 0.8);

    cairo_move_to (cr, 0.0,    top);
    cairo_line_to (cr, width,  top);
    cairo_move_to (cr, 0.0,    bottom);
    cairo_line_to (cr, width,  bottom);
    cairo_stroke (cr);
    cairo_restore (cr);

    return GDK_EVENT_PROPAGATE; /* let GtkFixed paint the readouts */
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
    self->rows = g_hash_table_new (g_direct_hash, g_direct_equal);

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

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
    g_signal_connect (self->canvas, "draw",
                      G_CALLBACK (on_canvas_draw), NULL);
    gtk_box_pack_start (GTK_BOX (self), self->canvas, TRUE, TRUE, 0);

    self->empty_label = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (self->empty_label),
            "<span foreground='#888888'>"
            "No panel widgets yet — add a Countdown, Digital Clock or "
            "LED node to a worksheet</span>");
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
