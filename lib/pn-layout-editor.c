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

#include "pn-layout-editor.h"
#include "pn-panel-geometry.h"
#include "pn-desktop-geometry.h"
#include "pn-injector-widget.h"
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
#include "pn-inject.h"
#include "pn-label.h"
#include "pn-led.h"
#include "pn-matrix57.h"
#include "pn-numeric.h"
#include "pn-switch.h"

/* ------------------------------------------------------------------ */
/*  PnLayoutEditor                                                     */
/*                                                                     */
/*  GUI layout editor — the visual counterpart to the node-wiring       */
/*  #PnWorksheet, but for laying out the widgets a flow drives rather   */
/*  than the dataflow itself.  One class serves the whole family of      */
/*  surfaces through #PnLayoutEditorKind:                                */
/*                                                                     */
/*    PANEL   — the XFCE panel applet: widgets snap onto a one-widget-   */
/*              tall band standing in for the real desktop panel, and    */
/*              their canvas positions are saved in the flow's panel     */
/*              layout.                                                  */
/*    DESKTOP — the desktop application (still to be written): widgets   */
/*              are arranged freely inside a window frame whose size and */
/*              title the user picks here, and their window-relative     */
/*              positions are saved in the flow's desktop layout.        */
/*                                                                     */
/*  The web and mobile surfaces sketched in the original plan slot in    */
/*  the same way: a kind, a guide to draw, a snapping rule and a pair of */
/*  flow accessors.                                                      */
/*                                                                     */
/*  Role split: the panel widgets (#PnLedDisplay, #PnLedLamp, … from the */
/*  GTK/Cairo-only panel-widgets library) are dumb faces that know       */
/*  nothing about nodes; this editor is the controller that binds them   */
/*  to the model.  It keeps one widget per representable node across the */
/*  whole flow (every sheet) — a seven-segment readout for each          */
/*  #PnCountdown, an indicator lamp for each #PnLed — observing the      */
/*  flow's single node store to create and destroy widgets as those      */
/*  nodes come and go, and mirroring each node's live value through its  */
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
#define PN_LE_CANVAS_WIDTH  1000
#define PN_LE_CANVAS_HEIGHT  700

/* Desktop kind: inset of the window frame from the canvas corner.  Deep
 * enough to leave room for the mock title bar drawn just above the frame,
 * so the framed rectangle is exactly the window's content area and a
 * stored (x, y) needs no correction for chrome. */
#define PN_LE_FRAME_MARGIN   40
#define PN_LE_TITLEBAR_H     24

/* Sticky snapping of a dragged row to a guide (the panel band, or a
 * desktop window edge or centre line): it snaps when the pointer-driven
 * position comes within PN_LE_SNAP_GRAB px of the guide, and stays stuck
 * until the pointer is pulled more than PN_LE_SNAP_BREAK px away — so a
 * widget clings to the guide yet can be torn off deliberately.  BREAK is
 * the larger of the two, which is what gives the snap its grip. */
#define PN_LE_SNAP_GRAB     10
#define PN_LE_SNAP_BREAK    22

struct _PnLayoutEditor
{
    GtkBox parent_instance;

    /* Which surface this editor lays out for; fixed at construction. */
    PnLayoutEditorKind kind;

    /* The shared model.  Owned via a reference; @nodes is borrowed from
     * it (the flow keeps it alive). */
    PnFlow      *flow;
    PnNodeStore *nodes;

    /* Free-positioning canvas the readout rows are placed on, plus the
     * "nothing here yet" hint shown when there are no countdown nodes. */
    GtkWidget *canvas;
    GtkWidget *empty_label;

    /* Desktop kind only (%NULL on the panel): the window-properties bar
     * above the canvas — the title the application will give its window
     * and the size the layout is arranged for.  All three edit the flow;
     * the flow's desktop-layout-changed signal pushes values back here so
     * a load or an undo is reflected without a second code path. */
    GtkWidget *title_entry;
    GtkWidget *width_spin;
    GtkWidget *height_spin;

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

    /* A drag that started inside a plot's corner grip RESIZES it instead
     * of moving it; @drag_start_w/h is the size at grab time, so a
     * resize that ends where it began leaves the document clean the same
     * way an unmoved row does. */
    gboolean   drag_resize;
    gint       drag_start_w;
    gint       drag_start_h;

    /* Which guide the row being dragged is currently stuck to on each
     * axis (an index into that axis's guide list, or -1 for free).
     * Carried across motion events so the snap has hysteresis: it holds
     * until the pointer pulls PN_LE_SNAP_BREAK px clear (see
     * layout_snap_axis).  The panel kind has a single y guide (the band)
     * and no x guide; the desktop kind has three of each. */
    gint       drag_snap_x;
    gint       drag_snap_y;

    /* Running count of rows ever placed, used to cascade the initial
     * position of each new readout so they do not all land on top of
     * one another. */
    guint      cascade;
};

G_DEFINE_TYPE (PnLayoutEditor, pn_layout_editor, GTK_TYPE_BOX)

/* ------------------------------------------------------------------ */
/*  Kind-dependent geometry                                            */
/* ------------------------------------------------------------------ */

/* Pixel height every mirrored widget is drawn at on this surface.  The
 * panel matches a typical XFCE panel icon; a window has room for more. */
static gint
layout_editor_widget_height (PnLayoutEditor *self)
{
    return self->kind == PN_LAYOUT_EDITOR_DESKTOP ? PN_DE_WIDGET_HEIGHT
                                                  : PN_PE_PREVIEW_HEIGHT;
}

/* Canvas coordinates of the layout origin — the point stored placements
 * are relative to.  The panel band spans the whole canvas, so its origin
 * is the canvas corner and a stored coordinate IS a canvas coordinate;
 * the desktop window is an inset rectangle, so its placements are stored
 * relative to the frame's top-left, which is exactly what the desktop
 * application wants to paint with. */
static void
layout_editor_origin (PnLayoutEditor *self, gint *out_x, gint *out_y)
{
    gboolean desktop = (self->kind == PN_LAYOUT_EDITOR_DESKTOP);

    *out_x = desktop ? PN_LE_FRAME_MARGIN : 0;
    *out_y = desktop ? PN_LE_FRAME_MARGIN : 0;
}

/* The desktop window's content rectangle in canvas coordinates.  Only
 * meaningful for the desktop kind. */
static void
layout_editor_frame_rect (PnLayoutEditor *self, GdkRectangle *rect)
{
    gint w, h;

    pn_flow_get_desktop_window (self->flow, &w, &h);

    rect->x      = PN_LE_FRAME_MARGIN;
    rect->y      = PN_LE_FRAME_MARGIN;
    rect->width  = w;
    rect->height = h;
}

/* Read @node's saved placement for this surface, translated into canvas
 * coordinates.  Returns %FALSE when the node has never been placed. */
static gboolean
layout_editor_load_position (PnLayoutEditor *self,
                             PnNode         *node,
                             gint           *out_x,
                             gint           *out_y)
{
    const gchar *uuid = pn_node_get_uuid (node);
    gdouble      sx, sy;
    gint         ox, oy;
    gboolean     found;

    found = (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
            ? pn_flow_get_desktop_position (self->flow, uuid, &sx, &sy)
            : pn_flow_get_panel_position   (self->flow, uuid, &sx, &sy);
    if (!found)
        return FALSE;

    layout_editor_origin (self, &ox, &oy);
    *out_x = ox + (gint) sx;
    *out_y = oy + (gint) sy;
    return TRUE;
}

/* Save @node's canvas placement into this surface's layout map, converted
 * back to origin-relative coordinates. */
static void
layout_editor_store_position (PnLayoutEditor *self,
                              PnNode         *node,
                              gint            x,
                              gint            y)
{
    const gchar *uuid = pn_node_get_uuid (node);
    gint         ox, oy;

    layout_editor_origin (self, &ox, &oy);

    if (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
        pn_flow_set_desktop_position (self->flow, uuid, x - ox, y - oy);
    else
        pn_flow_set_panel_position   (self->flow, uuid, x - ox, y - oy);
}

/* Read @node's saved widget size for this surface, or the plot default
 * when it has none.  Only the desktop kind has resizable widgets, so the
 * panel always gets the default (which it never asks for). */
static void
layout_editor_load_size (PnLayoutEditor *self,
                         PnNode         *node,
                         gint           *out_w,
                         gint           *out_h)
{
    gdouble w = PN_DE_PLOT_DEFAULT_WIDTH;
    gdouble h = PN_DE_PLOT_DEFAULT_HEIGHT;

    if (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
        pn_flow_get_desktop_size (self->flow, pn_node_get_uuid (node), &w, &h);

    *out_w = (gint) w;
    *out_h = (gint) h;
}

/* Save @node's widget size into this surface's layout map. */
static void
layout_editor_store_size (PnLayoutEditor *self,
                          PnNode         *node,
                          gint            w,
                          gint            h)
{
    if (self->kind != PN_LAYOUT_EDITOR_DESKTOP)
        return;

    pn_flow_set_desktop_size (self->flow, pn_node_get_uuid (node), w, h);
}

/* Sticky-snap @v to one of @guides, remembering in @state which guide
 * (index, or -1 for free) it is currently stuck to.  That memory is what
 * gives the snap hysteresis: it grabs within PN_LE_SNAP_GRAB px and only
 * lets go once the pointer has pulled more than PN_LE_SNAP_BREAK px past
 * the guide it holds. */
static gint
layout_snap_axis (gint v, const gint *guides, gint n_guides, gint *state)
{
    gint i;

    if (*state >= 0 && *state < n_guides)
    {
        if (ABS (v - guides[*state]) <= PN_LE_SNAP_BREAK)
            return guides[*state];
        *state = -1;
    }

    for (i = 0; i < n_guides; i++)
    {
        if (ABS (v - guides[i]) <= PN_LE_SNAP_GRAB)
        {
            *state = i;
            return guides[i];
        }
    }
    return v;
}

/* Push @node's current countdown value into its readout, using the same
 * day/hour/minute/second breakdown the node itself paints with. */
static void
layout_editor_sync_countdown (PnNode *node, PnLedDisplay *led)
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
    layout_editor_sync_countdown (node, PN_LED_DISPLAY (user_data));
}

/* Push @node's current digital-clock value into its readout.  The clock is
 * the Countdown without a days field, so it mirrors onto a days-less
 * (day_digits == 0) HH:MM:SS readout. */
static void
layout_editor_sync_digital_clock (PnNode *node, PnLedDisplay *led)
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
    layout_editor_sync_digital_clock (node, PN_LED_DISPLAY (user_data));
}

/* Push @node's current lit state and lit colour into its lamp, reading
 * both through the LED node's GTK-free accessors. */
static void
layout_editor_sync_led (PnNode *node, PnLedLamp *lamp)
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
    layout_editor_sync_led (node, PN_LED_LAMP (user_data));
}

/* Push @node's current text and styling into its readout, reading them
 * through the Label node's GTK-free paint-state accessor. */
static void
layout_editor_sync_label (PnNode *node, PnTextDisplay *text)
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
    layout_editor_sync_label (node, PN_TEXT_DISPLAY (user_data));
}

/* Push @node's current text and styling into its readout, reading them
 * through the Matrix57 node's GTK-free paint-state accessor. */
static void
layout_editor_sync_matrix57 (PnNode *node, PnMatrix57Display *display)
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
    layout_editor_sync_matrix57 (node, PN_MATRIX57_DISPLAY (user_data));
}

/* Push @node's current value and styling into its readout, reading them
 * through the Numeric node's GTK-free paint-state accessor. */
static void
layout_editor_sync_numeric (PnNode *node, PnNumericDisplay *display)
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
    layout_editor_sync_numeric (node, PN_NUMERIC_DISPLAY (user_data));
}

/* Push @node's current latch position into its toggle, reading it through
 * the Switch node's GTK-free accessor. */
static void
layout_editor_sync_switch (PnNode *node, PnSwitchWidget *toggle)
{
    pn_switch_widget_set_on (toggle, pn_switch_get_on (PN_SWITCH (node)));
}

/* repaint-needed on a Switch node → refresh its toggle.  @user_data is the
 * row's #PnSwitchWidget (connected with g_signal_connect_object so it dies
 * with the toggle). */
static void
on_switch_repaint_needed (PnNode *node, gpointer user_data)
{
    layout_editor_sync_switch (node, PN_SWITCH_WIDGET (user_data));
}

/* Push @node's configured fire-button icon into its panel-button mirror. */
static void
layout_editor_sync_injector (PnNode *node, PnInjectorWidget *button)
{
    pn_injector_widget_set_icon (button,
                                 pn_inject_get_button_icon (PN_INJECT (node)));
}

/* repaint-needed on an Inject node → refresh the icon shown on its
 * panel-button mirror.  @user_data is the row's #PnInjectorWidget. */
static void
on_injector_repaint_needed (PnNode *node, gpointer user_data)
{
    layout_editor_sync_injector (node, PN_INJECTOR_WIDGET (user_data));
}

/* ------------------------------------------------------------------ */
/*  Plot widgets                                                       */
/*                                                                     */
/*  The one widget kind that is NOT a panel-widgets face.  Every other  */
/*  row restates its node's look in self-contained cairo code, but a    */
/*  plot's look lives in the node's own PnNodeClass::paint_plot         */
/*  painter — PLplot for the 2D graphs, MathGL for the 3D ones, Pango   */
/*  for the cards — and there is no honest way to say that twice.  The  */
/*  editor lives in the same process as the nodes, so it simply calls   */
/*  the painter into a drawing area: what you arrange here is literally */
/*  the node's own picture, at the size it will have in the window.     */
/*                                                                     */
/*  (The desktop VIEWER cannot do this — it links no pipnode runtime    */
/*  and no plotting stack — so the engine renders the same painter to a */
/*  PNG and ships the pixels to a #PnPlotDisplay.  Same picture, one    */
/*  process removed.)                                                   */
/*                                                                     */
/*  Plots are desktop-only: a panel band is one icon tall, which is no  */
/*  place for a plot, and the panel layout has nowhere to store a size. */
/* ------------------------------------------------------------------ */

/* Set on the drawing area (the node it paints) and, for the row that
 * wraps it, on the handle (the area itself) — the press handler uses the
 * latter to tell a resizable plot row from a fixed-height one. */
#define PN_LE_PLOT_NODE_QDATA  "pn-plot-node"
#define PN_LE_PLOT_AREA_QDATA  "pn-plot-area"

/* Draw the resize grip in the bottom-right corner: three short diagonals,
 * the desktop-wide idiom for "drag me bigger".  Drawn over the plot, in a
 * translucent grey that reads on both a light and a dark plot. */
static void
draw_plot_grip (cairo_t *cr, double w, double h)
{
    gint i;

    cairo_save (cr);
    cairo_set_line_width (cr, 1.5);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);

    for (i = 1; i <= 3; i++)
    {
        double d = i * (PN_DE_PLOT_GRIP / 4.0);

        cairo_move_to (cr, w - d - 1.0, h - 1.0);
        cairo_line_to (cr, w - 1.0,     h - d - 1.0);
    }

    /* Stroked twice — a dark line under a light one — so the grip stays
     * visible whatever the plot happens to be painting underneath. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.35);
    cairo_stroke_preserve (cr);
    cairo_set_line_width (cr, 0.8);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.75);
    cairo_stroke (cr);
    cairo_restore (cr);
}

/* Paint one plot row: the node's own painter across the whole area, then
 * the resize grip on top. */
static gboolean
on_plot_draw (GtkWidget *area, cairo_t *cr, gpointer user_data)
{
    PnNode      *node = g_object_get_data (G_OBJECT (area),
                                           PN_LE_PLOT_NODE_QDATA);
    PnNodeClass *klass;
    double       w = gtk_widget_get_allocated_width  (area);
    double       h = gtk_widget_get_allocated_height (area);

    (void) user_data;

    if (node == NULL)
        return GDK_EVENT_PROPAGATE;

    klass = PN_NODE_GET_CLASS (node);
    if (klass->paint_plot != NULL)
        klass->paint_plot (node, cr, 0.0, 0.0, w, h);

    draw_plot_grip (cr, w, h);
    return GDK_EVENT_PROPAGATE;
}

/* repaint-needed on a plot node -> redraw its area.  @user_data is the
 * row's drawing area (connected with g_signal_connect_object so it dies
 * with the area). */
static void
on_plot_repaint_needed (PnNode *node, gpointer user_data)
{
    (void) node;
    gtk_widget_queue_draw (GTK_WIDGET (user_data));
}

/* Whether @node is shown as a plot area on this surface: it installs a
 * paint_plot painter, has no dedicated widget of its own (the checks
 * above this one in layout_editor_build_widget claim those first), and
 * the surface is the desktop. */
static gboolean
layout_editor_is_plot (PnLayoutEditor *self, PnNode *node)
{
    return self->kind == PN_LAYOUT_EDITOR_DESKTOP
        && PN_NODE_GET_CLASS (node)->paint_plot != NULL;
}

/* Toggle the empty-state hint to match the live readout count. */
static void
layout_editor_update_empty_state (PnLayoutEditor *self)
{
    gboolean empty = (g_hash_table_size (self->rows) == 0);

    gtk_widget_set_visible (self->empty_label, empty);
    gtk_widget_set_visible (self->canvas,     !empty);
}

/* Keep the canvas at least as large as the surface it stands in for, so a
 * desktop window bigger than the default canvas is fully reachable
 * through the host's scrolled window. */
static void
layout_editor_update_canvas_size (PnLayoutEditor *self)
{
    gint w = PN_LE_CANVAS_WIDTH;
    gint h = PN_LE_CANVAS_HEIGHT;

    if (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
    {
        GdkRectangle fr;

        layout_editor_frame_rect (self, &fr);
        w = MAX (w, fr.x + fr.width  + PN_LE_FRAME_MARGIN);
        h = MAX (h, fr.y + fr.height + PN_LE_FRAME_MARGIN);
    }

    gtk_widget_set_size_request (self->canvas, w, h);
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

/* Whether (@x, @y) inside @child — a row handle — lands on the resize
 * grip of a plot it wraps.  %FALSE for every non-plot row, which is what
 * keeps the fixed-height readouts drag-only. */
static gboolean
press_in_plot_grip (GtkWidget *child, gdouble x, gdouble y)
{
    if (g_object_get_data (G_OBJECT (child), PN_LE_PLOT_AREA_QDATA) == NULL)
        return FALSE;

    return x >= gtk_widget_get_allocated_width  (child) - PN_DE_PLOT_GRIP
        && y >= gtk_widget_get_allocated_height (child) - PN_DE_PLOT_GRIP;
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
    PnLayoutEditor *self = PN_LAYOUT_EDITOR (user_data);
    GdkWindow     *win;

    if (event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;

    self->drag_child  = child;
    self->drag_grab_x = event->x;
    self->drag_grab_y = event->y;
    self->drag_start_w = gtk_widget_get_allocated_width  (child);
    self->drag_start_h = gtk_widget_get_allocated_height (child);
    gtk_container_child_get (GTK_CONTAINER (self->canvas), child,
                             "x", &self->drag_start_x,
                             "y", &self->drag_start_y, NULL);

    /* A press inside a plot's corner grip resizes rather than moves.  One
     * handle serves both gestures, so the row needs no extra child widget
     * and the plot keeps the whole area for its picture. */
    self->drag_resize = press_in_plot_grip (child, event->x, event->y);

    /* A row picked up already sitting on a guide starts out snapped to it,
     * so it clings rather than tearing off on the first stray pixel of
     * motion.  Resolved lazily on the first motion event, where the guide
     * lists are built: -1 here just means "not yet stuck". */
    self->drag_snap_x = -1;
    self->drag_snap_y = -1;
    if (self->kind == PN_LAYOUT_EDITOR_PANEL &&
        self->drag_start_y == panel_band_snap_y (child))
        self->drag_snap_y = 0;

    win = gtk_widget_get_window (child);
    if (win != NULL)
        gdk_window_raise (win);

    return GDK_EVENT_STOP;
}

/* Put @name's cursor on @child's own #GdkWindow, doing nothing when it is
 * already showing.  The row handle is a #GtkEventBox, so it has one. */
static void
row_set_cursor (GtkWidget *child, const gchar *name)
{
    GdkWindow  *win = gtk_widget_get_window (child);
    GdkCursor  *cursor;

    if (win == NULL)
        return;

    /* Cheap identity check: re-setting the same cursor every motion event
     * would ask X for a new one dozens of times a second. */
    if (g_strcmp0 (g_object_get_data (G_OBJECT (child), "pn-cursor"),
                   name) == 0)
        return;

    cursor = gdk_cursor_new_from_name (gtk_widget_get_display (child), name);
    if (cursor == NULL)
        cursor = gdk_cursor_new_for_display (
                gtk_widget_get_display (child),
                g_str_equal (name, "se-resize") ? GDK_BOTTOM_RIGHT_CORNER
                                                : GDK_FLEUR);

    gdk_window_set_cursor (win, cursor);
    g_clear_object (&cursor);
    g_object_set_data_full (G_OBJECT (child), "pn-cursor",
                            g_strdup (name), g_free);
}

/* Resize the plot in @child so its bottom-right corner tracks the pointer
 * at canvas position (@px, @py).  The grab offset is preserved, so the
 * corner stays exactly where it was grabbed rather than jumping to the
 * cursor; the result is clamped to the plot minimum and to the window
 * frame, so a plot can never be sized out of the window it lives in. */
static void
resize_plot_row (PnLayoutEditor *self, GtkWidget *child, gint px, gint py)
{
    GtkWidget   *area = g_object_get_data (G_OBJECT (child),
                                           PN_LE_PLOT_AREA_QDATA);
    GdkRectangle fr;
    gint         nw, nh;

    if (area == NULL)
        return;

    nw = (px - self->drag_start_x) + (self->drag_start_w
                                      - (gint) self->drag_grab_x);
    nh = (py - self->drag_start_y) + (self->drag_start_h
                                      - (gint) self->drag_grab_y);

    layout_editor_frame_rect (self, &fr);
    nw = CLAMP (nw, PN_DE_PLOT_MIN_WIDTH,
                MAX (PN_DE_PLOT_MIN_WIDTH,
                     fr.x + fr.width  - self->drag_start_x));
    nh = CLAMP (nh, PN_DE_PLOT_MIN_HEIGHT,
                MAX (PN_DE_PLOT_MIN_HEIGHT,
                     fr.y + fr.height - self->drag_start_y));

    gtk_widget_set_size_request (area, nw, nh);
}

/* Track the pointer: move the grabbed row so the grab point stays under
 * the cursor, clamped so the row cannot be dragged off the canvas. */
static gboolean
on_row_motion (GtkWidget      *child,
               GdkEventMotion *event,
               gpointer        user_data)
{
    PnLayoutEditor *self = PN_LAYOUT_EDITOR (user_data);
    gint           px, py;
    gint           nx, ny;
    gint           cw, ch;

    /* Not dragging: the pointer is only here to pick the cursor, so a plot
     * announces its grip before the user commits to a gesture. */
    if (self->drag_child != child)
    {
        row_set_cursor (child,
                        press_in_plot_grip (child, event->x, event->y)
                        ? "se-resize" : "move");
        return GDK_EVENT_PROPAGATE;
    }

    /* event->x/y are relative to @child; translate to canvas coordinates
     * so the move is expressed in the canvas's own space. */
    if (!gtk_widget_translate_coordinates (child, self->canvas,
                                           (gint) event->x, (gint) event->y,
                                           &px, &py))
        return GDK_EVENT_PROPAGATE;

    if (self->drag_resize)
    {
        resize_plot_row (self, child, px, py);
        return GDK_EVENT_STOP;
    }

    nx = px - (gint) self->drag_grab_x;
    ny = py - (gint) self->drag_grab_y;

    cw = gtk_widget_get_allocated_width  (child);
    ch = gtk_widget_get_allocated_height (child);

    if (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
    {
        /* The window frame is the whole world here: a widget the user
         * cannot see in the running application is not a placement worth
         * storing, so the row is confined to the content area. */
        GdkRectangle fr;
        gint         gx[3], gy[3];

        layout_editor_frame_rect (self, &fr);

        nx = CLAMP (nx, fr.x, MAX (fr.x, fr.x + fr.width  - cw));
        ny = CLAMP (ny, fr.y, MAX (fr.y, fr.y + fr.height - ch));

        /* Three guides per axis — the two window edges and the centre
         * line — so rows line up flush or centred without pixel-hunting. */
        gx[0] = fr.x;
        gx[1] = fr.x + (fr.width  - cw) / 2;
        gx[2] = fr.x + fr.width  - cw;
        gy[0] = fr.y;
        gy[1] = fr.y + (fr.height - ch) / 2;
        gy[2] = fr.y + fr.height - ch;

        nx = layout_snap_axis (nx, gx, G_N_ELEMENTS (gx), &self->drag_snap_x);
        ny = layout_snap_axis (ny, gy, G_N_ELEMENTS (gy), &self->drag_snap_y);

        /* A widget wider or taller than the window puts its far-edge and
         * centre guides outside the frame; re-clamp so the snap can never
         * push the row back out of the window. */
        nx = CLAMP (nx, fr.x, MAX (fr.x, fr.x + fr.width  - cw));
        ny = CLAMP (ny, fr.y, MAX (fr.y, fr.y + fr.height - ch));
    }
    else
    {
        /* Panel: free across the whole canvas, with the band as the one
         * guide.  @ny still tracks the pointer, so its distance from the
         * band measures how far the user has pulled. */
        gint gy[1];

        nx = CLAMP (nx, 0,
                    MAX (0, gtk_widget_get_allocated_width  (self->canvas) - cw));
        ny = CLAMP (ny, 0,
                    MAX (0, gtk_widget_get_allocated_height (self->canvas) - ch));

        gy[0] = panel_band_snap_y (child);
        ny = layout_snap_axis (ny, gy, G_N_ELEMENTS (gy), &self->drag_snap_y);
    }

    gtk_fixed_move (GTK_FIXED (self->canvas), child, nx, ny);
    return GDK_EVENT_STOP;
}

static gboolean
on_row_button_release (GtkWidget      *child,
                       GdkEventButton *event,
                       gpointer        user_data)
{
    PnLayoutEditor *self = PN_LAYOUT_EDITOR (user_data);
    gint           x, y;
    PnNode        *node;

    if (event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;
    if (self->drag_child != child)
        return GDK_EVENT_PROPAGATE;

    self->drag_child = NULL;
    node = g_object_get_data (G_OBJECT (child), "pn-node");

    if (self->drag_resize)
    {
        GtkWidget *area = g_object_get_data (G_OBJECT (child),
                                             PN_LE_PLOT_AREA_QDATA);
        gint       w = 0, h = 0;

        self->drag_resize = FALSE;

        /* The request, not the allocation: the canvas may not have
         * re-laid-out yet, and the request is what the user just set. */
        if (area != NULL)
            gtk_widget_get_size_request (area, &w, &h);
        if (node != NULL && w > 0 && h > 0
            && (w != self->drag_start_w || h != self->drag_start_h))
            layout_editor_store_size (self, node, w, h);

        return GDK_EVENT_STOP;
    }

    /* Persist the placement only when the row actually moved, so a plain
     * click never dirties the document. */
    gtk_container_child_get (GTK_CONTAINER (self->canvas), child,
                             "x", &x, "y", &y, NULL);
    if (node != NULL && (x != self->drag_start_x || y != self->drag_start_y))
        layout_editor_store_position (self, node, x, y);

    return GDK_EVENT_STOP;
}

/* Give a row a move ("fleur") cursor so it reads as draggable.  Deferred
 * to realize because the cursor is set on the row's #GdkWindow.  A plot
 * row swaps this for "se-resize" while the pointer is over its grip; see
 * on_row_motion. */
static void
on_row_realize (GtkWidget *child, gpointer user_data)
{
    (void) user_data;
    row_set_cursor (child, "move");
}

/* Initial canvas position for the next readout: cascade down a column,
 * wrapping to the next column once one fills, so a fresh editor lays the
 * existing nodes out tidily before the user rearranges them.  The cascade
 * starts at the surface's origin and stays inside it, so desktop widgets
 * land in the window rather than beside it. */
static void
layout_editor_next_position (PnLayoutEditor *self, gint *out_x, gint *out_y)
{
    const gint step_y  = layout_editor_widget_height (self) + 28;
    const gint step_x  = 170;
    gint       ox, oy;
    gint       height  = PN_LE_CANVAS_HEIGHT;
    gint       per_col;

    layout_editor_origin (self, &ox, &oy);

    if (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
    {
        GdkRectangle fr;
        layout_editor_frame_rect (self, &fr);
        height = fr.height;
    }
    else
    {
        /* The panel cascade historically began at the band's own top. */
        ox = oy = 24;
        height -= 24;
    }

    per_col = MAX (1, height / step_y);

    *out_x = ox + (self->cascade / per_col) * step_x;
    *out_y = oy + (self->cascade % per_col) * step_y;
}

/* Build the widget that mirrors @node, or %NULL when @node is not a kind
 * this surface represents.  Each branch seeds the widget's
 * current value and wires it to follow the node's repaint-needed signal
 * live; tying that handler to the widget (g_signal_connect_object) means
 * destroying the row when the node goes away also severs the connection —
 * no manual handler bookkeeping, no dangling callbacks. */
static GtkWidget *
layout_editor_build_widget (PnLayoutEditor *self, PnNode *node)
{
    const gint height = layout_editor_widget_height (self);

    if (PN_IS_COUNTDOWN (node))
    {
        /* The readout draws on a transparent background (no frame), so the
         * canvas shows through behind it just as the panel does. */
        GtkWidget *led = pn_led_display_new ();

        pn_led_display_set_height (PN_LED_DISPLAY (led), height);

        layout_editor_sync_countdown (node, PN_LED_DISPLAY (led));
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

        pn_led_display_set_height (PN_LED_DISPLAY (led), height);

        layout_editor_sync_digital_clock (node, PN_LED_DISPLAY (led));
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

        pn_text_display_set_height (PN_TEXT_DISPLAY (text), height);

        layout_editor_sync_label (node, PN_TEXT_DISPLAY (text));
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

        pn_matrix57_display_set_height (PN_MATRIX57_DISPLAY (display), height);

        layout_editor_sync_matrix57 (node, PN_MATRIX57_DISPLAY (display));
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

        pn_numeric_display_set_height (PN_NUMERIC_DISPLAY (display), height);

        layout_editor_sync_numeric (node, PN_NUMERIC_DISPLAY (display));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_numeric_repaint_needed),
                                 display, 0);
        return display;
    }

    if (PN_IS_LED (node))
    {
        GtkWidget *lamp = pn_led_lamp_new ();

        pn_led_lamp_set_size (PN_LED_LAMP (lamp), height);

        layout_editor_sync_led (node, PN_LED_LAMP (lamp));
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

        pn_switch_widget_set_height (PN_SWITCH_WIDGET (toggle), height);

        layout_editor_sync_switch (node, PN_SWITCH_WIDGET (toggle));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_switch_repaint_needed),
                                 toggle, 0);
        return toggle;
    }

    if (PN_IS_INJECT (node))
    {
        /* Drag-only here, same as the switch: the fire button is left
         * non-interactive so a click on the canvas drags the row instead
         * of accidentally firing the inject.  The applet is where the
         * button becomes clickable. */
        GtkWidget *button = pn_injector_widget_new ();

        pn_injector_widget_set_height (PN_INJECTOR_WIDGET (button), height);

        layout_editor_sync_injector (node, PN_INJECTOR_WIDGET (button));
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_injector_repaint_needed),
                                 button, 0);
        return button;
    }

    /* Last: every node that paints a plot of its own and has no dedicated
     * widget above.  Deliberately after the specific branches, because
     * Countdown and Label install a paint_plot painter too and their
     * seven-segment / text faces are the better mirror. */
    if (layout_editor_is_plot (self, node))
    {
        GtkWidget *area = gtk_drawing_area_new ();
        gint       w, h;

        layout_editor_load_size (self, node, &w, &h);
        gtk_widget_set_size_request (area, w, h);

        /* Borrowed: the store owns the node and destroys this row before
         * it, exactly like the "pn-node" pointer on the handle. */
        g_object_set_data (G_OBJECT (area), PN_LE_PLOT_NODE_QDATA, node);
        g_signal_connect (area, "draw", G_CALLBACK (on_plot_draw), NULL);
        g_signal_connect_object (node, "repaint-needed",
                                 G_CALLBACK (on_plot_repaint_needed),
                                 area, 0);
        return area;
    }

    return NULL;
}

/* Create the panel widget for @node when it is a kind we represent and are
 * not already tracking. */
static void
layout_editor_add_node (PnLayoutEditor *self, PnNode *node)
{
    GtkWidget *handle;
    GtkWidget *widget;
    gint       x, y;

    if (g_hash_table_contains (self->rows, node))
        return;

    widget = layout_editor_build_widget (self, node);
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
     * outlives it (layout_editor_remove_node destroys the handle), so a
     * borrowed pointer is safe. */
    g_object_set_data (G_OBJECT (handle), "pn-node", node);

    /* Mark a plot row so the press handler knows it has a resize grip; a
     * row without this data is drag-only. */
    if (g_object_get_data (G_OBJECT (widget), PN_LE_PLOT_NODE_QDATA) != NULL)
        g_object_set_data (G_OBJECT (handle), PN_LE_PLOT_AREA_QDATA, widget);

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
    if (!layout_editor_load_position (self, node, &x, &y))
        layout_editor_next_position (self, &x, &y);
    gtk_fixed_put (GTK_FIXED (self->canvas), handle, x, y);
    gtk_widget_show_all (handle);
    self->cascade++;

    g_hash_table_insert (self->rows, node, handle);
    layout_editor_update_empty_state (self);
}

/* Destroy the readout for @node, if we have one. */
static void
layout_editor_remove_node (PnLayoutEditor *self, PnNode *node)
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
    layout_editor_update_empty_state (self);
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
    layout_editor_add_node (PN_LAYOUT_EDITOR (user_data), node);
}

static void
on_store_node_removed (PnNodeStore *store,
                       PnNode      *node,
                       guint        index,
                       gpointer     user_data)
{
    (void) store; (void) index;
    layout_editor_remove_node (PN_LAYOUT_EDITOR (user_data), node);
}

/* Subscribe to the flow's node store and build a widget for every
 * representable node already present, so a freshly-created editor
 * immediately mirrors the whole flow.  The flow reference itself is taken
 * earlier, in pn_layout_editor_new: the widget tree built before this
 * already reads the document's desktop-window settings. */
static void
layout_editor_attach_flow (PnLayoutEditor *self)
{
    guint n;
    guint i;

    g_signal_connect (self->nodes, "node-added",
                      G_CALLBACK (on_store_node_added), self);
    g_signal_connect (self->nodes, "node-removed",
                      G_CALLBACK (on_store_node_removed), self);

    n = pn_node_store_get_length (self->nodes);
    for (i = 0; i < n; i++)
        layout_editor_add_node (self, pn_node_store_get_node (self->nodes, i));
}

static void
pn_layout_editor_dispose (GObject *object)
{
    PnLayoutEditor *self = PN_LAYOUT_EDITOR (object);

    /* Drop the store subscriptions before the flow reference goes; the
     * per-node repaint handlers fall away on their own as the readouts
     * are destroyed with the widget tree. */
    if (self->nodes != NULL)
        g_signal_handlers_disconnect_by_data (self->nodes, self);
    self->nodes = NULL;

    if (self->flow != NULL)
        g_signal_handlers_disconnect_by_data (self->flow, self);
    g_clear_object (&self->flow);
    g_clear_pointer (&self->rows, g_hash_table_unref);

    G_OBJECT_CLASS (pn_layout_editor_parent_class)->dispose (object);
}

/* Paint the panel band behind the readouts: two thin dashed horizontal
 * lines spanning the canvas, marking the top and bottom edges of the strip
 * that stands in for the real XFCE panel. */
static void
draw_panel_band (GtkWidget *canvas, cairo_t *cr)
{
    static const double dashes[] = { 6.0, 4.0 };
    int    width  = gtk_widget_get_allocated_width (canvas);
    /* +0.5 lands the 1px stroke on the pixel grid for a crisp line. */
    double top    = PN_PE_PANEL_TOP + 0.5;
    double bottom = PN_PE_PANEL_TOP + PN_PE_PANEL_HEIGHT + 0.5;

    cairo_set_line_width (cr, 1.0);
    cairo_set_dash (cr, dashes, G_N_ELEMENTS (dashes), 0.0);
    cairo_set_source_rgba (cr, 0.8, 0.0, 0.0, 0.8);

    cairo_move_to (cr, 0.0,    top);
    cairo_line_to (cr, width,  top);
    cairo_move_to (cr, 0.0,    bottom);
    cairo_line_to (cr, width,  bottom);
    cairo_stroke (cr);
}

/* Paint the desktop window the widgets will be shown in: the content
 * rectangle the layout is arranged inside, with a mock title bar drawn
 * just above it (outside the rectangle, so the framed area stays exactly
 * the content area a stored placement is relative to). */
static void
draw_desktop_frame (PnLayoutEditor *self, cairo_t *cr)
{
    GdkRectangle fr;
    const gchar *title;
    double       bar_y;

    layout_editor_frame_rect (self, &fr);
    bar_y = fr.y - PN_LE_TITLEBAR_H;

    /* Content area: a faint wash so the window reads as a surface without
     * fighting the widgets drawn on top of it in either theme. */
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.10);
    cairo_rectangle (cr, fr.x, fr.y, fr.width, fr.height);
    cairo_fill (cr);

    /* Title bar. */
    cairo_set_source_rgba (cr, 0.5, 0.5, 0.5, 0.28);
    cairo_rectangle (cr, fr.x, bar_y, fr.width, PN_LE_TITLEBAR_H);
    cairo_fill (cr);

    title = pn_flow_get_desktop_title (self->flow);
    if (title == NULL || *title == '\0')
        title = "Untitled window";

    cairo_save (cr);
    cairo_rectangle (cr, fr.x, bar_y, fr.width, PN_LE_TITLEBAR_H);
    cairo_clip (cr);
    cairo_set_source_rgba (cr, 0.35, 0.35, 0.35, 0.95);
    cairo_select_font_face (cr, "Sans",
                            CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, 12.0);
    cairo_move_to (cr, fr.x + 8, bar_y + PN_LE_TITLEBAR_H - 7);
    cairo_show_text (cr, title);
    cairo_restore (cr);

    /* Outline last, so it sits crisply over both fills.  +0.5 puts the
     * 1px stroke on the pixel grid. */
    cairo_set_line_width (cr, 1.0);
    cairo_set_source_rgba (cr, 0.45, 0.45, 0.45, 0.9);
    cairo_rectangle (cr, fr.x + 0.5, bar_y + 0.5,
                     fr.width - 1.0, PN_LE_TITLEBAR_H + fr.height - 1.0);
    cairo_move_to (cr, fr.x + 0.5,            fr.y + 0.5);
    cairo_line_to (cr, fr.x + fr.width - 0.5, fr.y + 0.5);
    cairo_stroke (cr);
}

/* Draw the guide the widgets are arranged against.  Connected without
 * _after so it runs before GtkFixed propagates the draw to its children,
 * leaving the widgets on top of the guide. */
static gboolean
on_canvas_draw (GtkWidget *canvas, cairo_t *cr, gpointer user_data)
{
    PnLayoutEditor *self = PN_LAYOUT_EDITOR (user_data);

    cairo_save (cr);
    if (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
        draw_desktop_frame (self, cr);
    else
        draw_panel_band (canvas, cr);
    cairo_restore (cr);

    return GDK_EVENT_PROPAGATE; /* let GtkFixed paint the readouts */
}

static void
pn_layout_editor_class_init (PnLayoutEditorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = pn_layout_editor_dispose;
}

/* The desktop window's title entry changed — push it onto the document.
 * The canvas repaints because the flow's desktop-layout-changed handler
 * runs in turn, so the mock title bar follows every keystroke. */
static void
on_title_entry_changed (GtkEditable *entry, gpointer user_data)
{
    PnLayoutEditor *self = PN_LAYOUT_EDITOR (user_data);

    pn_flow_set_desktop_title (self->flow,
                               gtk_entry_get_text (GTK_ENTRY (entry)));
}

/* Either window-size spin changed — push both onto the document. */
static void
on_size_spin_changed (GtkSpinButton *spin, gpointer user_data)
{
    PnLayoutEditor *self = PN_LAYOUT_EDITOR (user_data);

    (void) spin;
    pn_flow_set_desktop_window (
            self->flow,
            gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->width_spin)),
            gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->height_spin)));
}

/* The document's desktop layout changed — a placement moved, or the window
 * was resized or retitled, from here or from a load or an undo.  Bring the
 * controls and the canvas back into line.  Each control is only written
 * when it actually differs, so echoing our own edit back cannot fight the
 * user's cursor or start a signal loop. */
static void
on_flow_desktop_layout_changed (PnFlow *flow, gpointer user_data)
{
    PnLayoutEditor *self = PN_LAYOUT_EDITOR (user_data);
    const gchar    *title;
    gint            w, h;

    (void) flow;

    title = pn_flow_get_desktop_title (self->flow);
    if (g_strcmp0 (gtk_entry_get_text (GTK_ENTRY (self->title_entry)),
                   title) != 0)
        gtk_entry_set_text (GTK_ENTRY (self->title_entry), title);

    pn_flow_get_desktop_window (self->flow, &w, &h);
    if (gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->width_spin)) != w)
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->width_spin), w);
    if (gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (self->height_spin)) != h)
        gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->height_spin), h);

    layout_editor_update_canvas_size (self);
    gtk_widget_queue_draw (self->canvas);
}

/* Build the desktop kind's window-properties bar: the title the
 * application will give its window, and the size the layout is arranged
 * for.  Packed above the canvas; the panel kind has no equivalent (its
 * geometry is the real panel's, not the user's to choose). */
static GtkWidget *
layout_editor_build_window_bar (PnLayoutEditor *self)
{
    GtkWidget *bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label;
    gint       w, h;

    pn_flow_get_desktop_window (self->flow, &w, &h);

    gtk_widget_set_margin_start  (bar, 8);
    gtk_widget_set_margin_end    (bar, 8);
    gtk_widget_set_margin_top    (bar, 6);
    gtk_widget_set_margin_bottom (bar, 6);

    label = gtk_label_new ("Window title");
    gtk_box_pack_start (GTK_BOX (bar), label, FALSE, FALSE, 0);

    self->title_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (self->title_entry),
                                    "(document name)");
    gtk_entry_set_text (GTK_ENTRY (self->title_entry),
                        pn_flow_get_desktop_title (self->flow));
    gtk_widget_set_tooltip_text (
            self->title_entry,
            "Title the desktop application gives its window; "
            "left empty it uses the document's own name");
    g_signal_connect (self->title_entry, "changed",
                      G_CALLBACK (on_title_entry_changed), self);
    gtk_box_pack_start (GTK_BOX (bar), self->title_entry, TRUE, TRUE, 0);

    label = gtk_label_new ("Size");
    gtk_box_pack_start (GTK_BOX (bar), label, FALSE, FALSE, 6);

    self->width_spin = gtk_spin_button_new_with_range (PN_DE_WINDOW_MIN_WIDTH,
                                                       PN_DE_WINDOW_MAX_WIDTH,
                                                       10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->width_spin), w);
    gtk_widget_set_tooltip_text (self->width_spin, "Window width in pixels");
    g_signal_connect (self->width_spin, "value-changed",
                      G_CALLBACK (on_size_spin_changed), self);
    gtk_box_pack_start (GTK_BOX (bar), self->width_spin, FALSE, FALSE, 0);

    label = gtk_label_new ("\303\227");     /* multiplication sign */
    gtk_box_pack_start (GTK_BOX (bar), label, FALSE, FALSE, 0);

    self->height_spin = gtk_spin_button_new_with_range (PN_DE_WINDOW_MIN_HEIGHT,
                                                        PN_DE_WINDOW_MAX_HEIGHT,
                                                        10);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (self->height_spin), h);
    gtk_widget_set_tooltip_text (self->height_spin, "Window height in pixels");
    g_signal_connect (self->height_spin, "value-changed",
                      G_CALLBACK (on_size_spin_changed), self);
    gtk_box_pack_start (GTK_BOX (bar), self->height_spin, FALSE, FALSE, 0);

    return bar;
}

/* Assemble the editor's widget tree.  Deferred out of init() because it
 * needs both the kind and the flow, neither of which exists until
 * pn_layout_editor_new() has set them. */
static void
layout_editor_build_ui (PnLayoutEditor *self)
{
    if (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
    {
        GtkWidget *bar = layout_editor_build_window_bar (self);

        gtk_box_pack_start (GTK_BOX (self), bar, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (self),
                            gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                            FALSE, FALSE, 0);
    }

    /* The free-positioning canvas the readouts are dragged around on.  A
     * size request gives the host's scrolled window a generous area to
     * scroll, so widgets can be spread well beyond one viewport.  Both the
     * canvas and the empty hint opt out of show_all so the host's
     * gtk_widget_show_all on the tab cannot override the explicit
     * visibility we toggle between them in
     * layout_editor_update_empty_state. */
    self->canvas = gtk_fixed_new ();
    gtk_widget_set_no_show_all (self->canvas, TRUE);
    g_signal_connect (self->canvas, "draw",
                      G_CALLBACK (on_canvas_draw), self);
    gtk_box_pack_start (GTK_BOX (self), self->canvas, TRUE, TRUE, 0);
    layout_editor_update_canvas_size (self);

    self->empty_label = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (self->empty_label),
            self->kind == PN_LAYOUT_EDITOR_DESKTOP
                ? "<span foreground='#888888'>"
                  "No widgets yet — add a Countdown, Digital Clock, Label, "
                  "Numeric, LED or Switch node to a worksheet, or a Graph "
                  "or any other node that draws a picture</span>"
                : "<span foreground='#888888'>"
                  "No panel widgets yet — add a Countdown, Digital Clock or "
                  "LED node to a worksheet</span>");
    gtk_widget_set_no_show_all (self->empty_label, TRUE);
    gtk_box_pack_start (GTK_BOX (self), self->empty_label, TRUE, FALSE, 0);

    /* Start in the empty state; attach_flow flips it as rows appear. */
    layout_editor_update_empty_state (self);

    if (self->kind == PN_LAYOUT_EDITOR_DESKTOP)
        g_signal_connect (self->flow, "desktop-layout-changed",
                          G_CALLBACK (on_flow_desktop_layout_changed), self);
}

static void
pn_layout_editor_init (PnLayoutEditor *self)
{
    self->rows = g_hash_table_new (g_direct_hash, g_direct_equal);

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

GtkWidget *
pn_layout_editor_new (PnFlow *flow, PnLayoutEditorKind kind)
{
    PnLayoutEditor *self;

    g_return_val_if_fail (PN_IS_FLOW (flow), NULL);

    self = g_object_new (PN_TYPE_LAYOUT_EDITOR, NULL);
    self->kind = kind;

    /* Order matters: the flow reference and the widget tree must both
     * exist before attach_flow starts creating rows for existing nodes. */
    self->flow  = g_object_ref (flow);
    self->nodes = pn_flow_get_nodes (flow);
    layout_editor_build_ui (self);
    layout_editor_attach_flow (self);

    return GTK_WIDGET (self);
}
