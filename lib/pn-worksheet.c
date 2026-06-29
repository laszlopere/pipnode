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

#include "pn-worksheet.h"
#include "pn-flow.h"
#include "pn-node.h"
#include "pn-node-dialog.h"
#include "pn-log-dialog.h"
#include "pn-node-factory.h"
#include "pn-node-store.h"
#include "pn-wire.h"
#include "pn-wire-store.h"
#include "pn-message.h"
#include "pn-inject.h"
#include "pn-auto-trigger.h"
#include "pn-switch.h"
#include "pn-knob.h"
#include "pn-chat.h"
#include "pn-comment.h"
#include "pn-sun-path.h"
#include "pn-oscilloscope.h"
#include "pn-filedrop.h"
#include "pn-palette.h"
#include "pn-preferences.h"

#include <math.h>
#include <pango/pangocairo.h>
#include <json-glib/json-glib.h>

/* Info codes distinguishing the drop payloads the worksheet accepts.
 * Stamped into each #GtkTargetEntry and handed back to
 * on_drag_data_received() so it can tell a palette node-type drop from
 * a desktop file-URI drop. */
enum {
    PN_WS_DND_INFO_PALETTE = 0,
    PN_WS_DND_INFO_URI     = 1,
};

/* Depth range over which the message-flow animation is staggered hop by
 * hop; matches the node dispatch-loop guard.  Deeper hops are clamped
 * (a cosmetic detail only). */
#define PN_WORKSHEET_WIRE_PULSE_DEPTH 256

/* ------------------------------------------------------------------ */
/*  PnWorksheet                                                        */
/*                                                                     */
/*  Custom GtkDrawingArea-derived widget that will eventually host     */
/*  the visual flow editor.  At this stage it owns an (empty) model    */
/*  of nodes and connections, draws a background grid, and exposes     */
/*  load/save helpers backed by JSON-GLib so worksheets can be         */
/*  persisted.  Painting of nodes and edit interactions are stubs.     */
/* ------------------------------------------------------------------ */

struct _PnWorksheet
{
    GtkDrawingArea parent_instance;

    /** Headless model the widget renders.  pn_worksheet_new() builds
     *  its own and owns the reference; pn_worksheet_new_for_flow()
     *  borrows one supplied by the caller (a multi-sheet host that
     *  shares one model across many widgets).  Either way the widget
     *  holds a strong ref via @flow and releases it in finalize, so
     *  ownership is uniform from the widget's point of view; @owns_flow
     *  is informational only. */
    PnFlow      *flow;
    gboolean     owns_flow;

    /** Borrowed pointer to flow's node store. */
    PnNodeStore *nodes;

    /** Borrowed pointer to flow's wire store. */
    PnWireStore *wires;

    /** Spreadsheet-style sheet tag this widget renders.  Filters all
     *  iterators below: nodes whose pn_node_get_worksheet() differs
     *  are skipped during draw, hit-test, marquee selection,
     *  drag-and-drop, copy, paste, content-extent measurement, and
     *  wire creation.  Always non-empty.  Set at construction time
     *  and not mutated afterwards (the host adds new widgets when the
     *  sheet list grows; an existing widget keeps its assignment for
     *  the duration of its lifetime). */
    gchar       *sheet_name;

    /** Canvas zoom factor.  Pan is handled by the enclosing
     *  #GtkScrolledWindow's adjustments; the worksheet does not
     *  carry its own pan offset because that would let content drift
     *  outside the drawing-area extent and become unreachable by
     *  the scrollbars (the scrollbars only span [0, upper] of the
     *  drawing-area coordinate system, not whatever Cairo translate
     *  the painter applies on top). */
    double zoom;

    /** Deferred zoom-to-cursor anchor.  Setting the scroll position
     *  inline with the wheel event clamps it against the *old*
     *  adjustment upper, since #GtkAdjustment.value is only
     *  re-clamped after the queue_resize-induced size negotiation
     *  bumps `upper` to the new canvas extent.  on_scroll captures
     *  the desired adjustment values here and an idle handler
     *  applies them after layout has settled. */
    gboolean zoom_anchor_pending;
    double   zoom_anchor_hadj;
    double   zoom_anchor_vadj;
    guint    zoom_anchor_idle_id;

    /** Set of currently selected nodes.  Hashed by pointer; each key
     *  is a strong reference owned by the table.  Selected nodes are
     *  drawn with a thin red highlight and move together when one of
     *  them is dragged. */
    GHashTable *selection;

    /** Group-drag state.  drag_pivot is %NULL when no drag is in
     *  progress; otherwise it owns a reference to the node under the
     *  cursor at button-press, drag_offset_{x,y} hold the press
     *  position relative to that node's anchor, and drag_anchors
     *  records the position each selected node had when the drag
     *  began so motion can apply a uniform delta. */
    PnNode *drag_pivot;
    double  drag_offset_x;
    double  drag_offset_y;
    GArray *drag_anchors;

    /** Comment-box resize state.  %resize_comment is %NULL when no resize
     *  is in progress; otherwise it owns a reference to the #PnComment
     *  whose corner is being dragged.  %resize_edge records which
     *  handle was grabbed; %resize_press_{x,y} is the world-space press
     *  point and %resize_anchor_{x,y,w,h} the box geometry at press, so
     *  motion can apply a uniform delta.  The undo history is frozen for
     *  the duration so the whole resize coalesces into one step. */
    PnNode  *resize_comment;
    int      resize_edge;
    double   resize_press_x, resize_press_y;
    double   resize_anchor_x, resize_anchor_y;
    double   resize_anchor_w, resize_anchor_h;

    /** Palette drag-preview state.  While a palette item is dragged
     *  over this worksheet, the painter traces a thin red wireframe of
     *  the node that a drop would create — its outline, icon, and name,
     *  all in red with no fill — so the user sees where and what they
     *  are about to drop.  @drag_preview_node is a throwaway instance
     *  built from the dragged #GType (owned here), measured by the
     *  painter for its size/icon/name; %NULL when no palette drag is
     *  hovering.  The dragged type is read straight off the source
     *  #PnPalette during drag-motion (no selection round-trip — that
     *  would risk freezing the display while the DnD grab is held).
     *  @drag_preview_x/y are the last pointer position in widget
     *  space. */
    PnNode  *drag_preview_node;
    gint     drag_preview_x;
    gint     drag_preview_y;

    /** Rubber-band selection state.  marquee_active is %TRUE while
     *  the user is sweeping out a selection rectangle with the
     *  primary button held down; the corners run from (x0, y0) at
     *  the press point to (x1, y1) at the current cursor position. */
    gboolean marquee_active;
    double   marquee_x0;
    double   marquee_y0;
    double   marquee_x1;
    double   marquee_y1;

    /** Wire-creation state.  A drag can be pulled from either end of a
     *  prospective wire, so exactly one of #wire_source / #wire_dest is
     *  non-%NULL while a drag is in progress (both %NULL when idle):
     *
     *    - #wire_source owns a ref to the node whose *output* port the
     *      user grabbed; the drag runs toward an input port (the
     *      original direction).
     *    - #wire_dest owns a ref to the node whose *input* port the user
     *      grabbed, and #wire_dest_input is which of its inputs; the drag
     *      runs toward an output port (the reverse direction).
     *
     *  Either way #wire_cursor_{x,y} hold the current pointer position so
     *  the painter can draw a tentative line to the loose end. */
    PnNode *wire_source;
    PnNode *wire_dest;
    gint    wire_dest_input;
    double  wire_cursor_x;
    double  wire_cursor_y;

    /** The #PnInject whose fire button is currently held down by the
     *  primary mouse button, or %NULL if no fire button is pressed.
     *  Drives the inset visual the painter draws while the click is
     *  in flight; the inject itself fires on press, so this only
     *  governs feedback. */
    PnInject *pressed_inject;

    /** Plot-zoom overlay state.  Clicking the plot extension of a
     *  node that exposes a #PnNodeClass.paint_plot vfunc (graph,
     *  table, …) lifts the plot off the canvas and animates it to a
     *  large rectangle centred in the widget; clicking on the
     *  enlarged plot animates it back into place.  zoomed_node is
     *  borrowed (the node store still owns the strong ref) and is
     *  cleared from on_node_removed_from_store so a delete during
     *  the zoom can never leave a dangling pointer.  zoom_t runs
     *  from 0 (sitting on the canvas at the original size) to 1
     *  (fully enlarged); zoom_dir is +1 while animating in, -1 while
     *  animating out, and 0 once the animation has settled.
     *  zoom_tick_id is the GtkTickCallback handle that drives the
     *  per-frame interpolation, or 0 when no tick is registered. */
    PnNode  *zoomed_node;
    double   zoom_t;
    int      zoom_dir;
    gint64   zoom_start_us;
    guint    zoom_tick_id;

    /** One-shot paste target.  Set by the worksheet-background popup's
     *  Paste handler before it kicks off the async clipboard fetch:
     *  on_clipboard_text consumes the flag and aligns the bounding-box
     *  top-left of the pasted group with #paste_target_{x,y} (worksheet
     *  coords) instead of applying the default `2 * PN_GRID_STEP`
     *  offset.  Cleared back to %FALSE the moment the paste callback
     *  runs so subsequent toolbar/menu pastes fall back to the offset
     *  shape. */
    gboolean paste_at_cursor;
    double   paste_target_x;
    double   paste_target_y;

    /** Focus-pulse state.  Driven by
     *  #pn_worksheet_focus_node_by_uuid: the painter renders a
     *  fading outline around #pulse_node so the user can spot which
     *  node a debug message originated from when its `from`
     *  shortcut was clicked.  #pulse_node is borrowed (the store
     *  owns the strong ref) and is cleared from
     *  on_node_removed_from_store so a delete during the pulse can
     *  never leave a dangling pointer.  #pulse_tick_id is the
     *  GtkTickCallback handle, or 0 when no pulse is in flight. */
    PnNode  *pulse_node;
    gint64   pulse_start_us;
    guint    pulse_tick_id;

    /** Borrowed pointer to the chat node currently taking keyboard
     *  input (or %NULL).  Set by the entry-strip primary press in
     *  on_button_press; cleared on any other primary press, on
     *  Escape, and on submit.  The store owns the strong ref —
     *  cleared from on_node_removed_from_store so a delete during a
     *  typing session can never leave a dangling pointer. */
    PnChat  *focused_chat;

    /** Drag-to-rotate state for a lifted #PnSunPath card.  While the
     *  scene is in the zoom overlay a primary press on it begins an
     *  orbit drag instead of collapsing the overlay: #sun_path_drag is
     *  then %TRUE, #sun_path_press_{x,y} hold the press point (to tell a
     *  click from a drag on release) and #sun_path_last_{x,y} the
     *  previous motion sample (to feed per-frame deltas to
     *  pn_sun_path_rotate).  #sun_path_dragged latches once the pointer
     *  has moved past the click threshold so a release that barely moved
     *  still dismisses the overlay.  The rotated node is always
     *  #zoomed_node, so no separate borrowed pointer is needed. */
    gboolean sun_path_drag;
    gboolean sun_path_dragged;
    double   sun_path_press_x, sun_path_press_y;
    double   sun_path_last_x,  sun_path_last_y;

    /** Knob-drag state for a lifted #PnOscilloscope card.  Mirrors the
     *  sun-path drag above: while the scope is maximized a primary press
     *  on one of its on-screen knobs begins a value drag instead of
     *  collapsing the overlay.  #osc_knob is the grabbed #PnOscKnob,
     *  #osc_press_{x,y} the press point (click vs. drag), #osc_last_{x,y}
     *  the previous motion sample (per-frame delta).  #osc_fine is TRUE
     *  when the grab landed on a concentric range knob's inner (fine)
     *  disc, so the whole drag stays on the fine vernier.  The node is
     *  always #zoomed_node. */
    gboolean osc_drag;
    int      osc_knob;
    gboolean osc_fine;
    double   osc_press_x, osc_press_y;
    double   osc_last_x,  osc_last_y;

    /** Cursor-drag state for a lifted #PnOscilloscope card: while maximized a
     *  primary press on a shown measurement-cursor LINE begins dragging that
     *  line (#osc_cursor is the grabbed #PnOscCursor) instead of collapsing
     *  the overlay.  The node is always #zoomed_node. */
    gboolean osc_cursor_drag;
    int      osc_cursor;

    /** Cursor-position-knob drag state for a lifted #PnOscilloscope card:
     *  a primary press on one of the four coarse/fine cursor knobs begins a
     *  value drag (#osc_cknob is the grabbed #PnOscCursor, #osc_cknob_fine
     *  TRUE when the grab landed on its inner fine disc, #osc_cknob_last_y the
     *  previous motion sample for the per-frame delta).  The node is always
     *  #zoomed_node. */
    gboolean osc_cknob_drag;
    int      osc_cknob;
    gboolean osc_cknob_fine;
    double   osc_cknob_last_y;

    /** Transient "magnification" readout shown while the user spins
     *  Ctrl+wheel.  #zoom_label_shown_us is the frame-clock timestamp
     *  of the most recent zoom step — every subsequent step re-stamps
     *  it so the label stays at full opacity for as long as the wheel
     *  is moving.  The painter holds the label at alpha 1.0 for
     *  #PN_WORKSHEET_ZOOM_LABEL_HOLD_US after the last step, then
     *  fades to 0 across the following #PN_WORKSHEET_ZOOM_LABEL_FADE_US
     *  before the per-frame tick tears itself down.  Borrowed nothing
     *  — purely visual state, so dispose/finalize just removes the
     *  tick. */
    gint64   zoom_label_shown_us;
    guint    zoom_label_tick_id;

    /** Per-widget handler IDs on the #PnPreferences singleton, kept so
     *  dispose can cleanly disconnect them.  All three drive a single
     *  queue_draw — the painter reads the current values straight off
     *  the singleton on every frame, so there is no cached state on
     *  the widget that would need invalidating beyond the redraw. */
    gulong   prefs_grid_type_handler;
    gulong   prefs_grid_color_handler;
    gulong   prefs_bg_handler;
    gulong   prefs_anim_handler;
    gulong   prefs_anim_interval_handler;
    gulong   prefs_visualize_handler;

    /** Wire message-flow animation.  When the
     *  "animate-messages-on-wires" preference is on, each message that
     *  crosses a wire spawns a #PnWirePulse: a light that runs along the
     *  wire's curve.  @wire_pulses holds the live pulses (each owning a
     *  strong ref to its #PnWire, released by the array's clear func);
     *  @wire_pulse_timer_id is the GLib timeout that advances them at a
     *  coarse rate (see PN_WORKSHEET_WIRE_PULSE_STEP_MS), or 0 when none
     *  are in flight. */
    GArray  *wire_pulses;
    guint    wire_pulse_timer_id;

    /** Node processing-activity glow (TODO #42).  When the
     *  "visualize-node-processing" preference is on, a node doing work
     *  glows a neon halo behind its card.  @processing_timer_id is the
     *  self-terminating poll that maintains the glow and tears itself
     *  down once no node is busy (0 when idle).  The per-node "is it lit"
     *  state rides on the node as object-data (PN_WORKSHEET_GLOW_KEY), so
     *  there are no node pointers here to dangle on removal. */
    guint    processing_timer_id;

    /** Widget-space bounding box invalidated for the lights on the last
     *  repaint; unioned with the current box each frame so the previous
     *  position is erased without repainting the whole canvas.  Valid
     *  only while #wire_pulse_dirty_valid is set. */
    double   wire_pulse_dirty_x0, wire_pulse_dirty_y0;
    double   wire_pulse_dirty_x1, wire_pulse_dirty_y1;
    gboolean wire_pulse_dirty_valid;

    /** Per-hop accumulated start offsets (microseconds), indexed by the
     *  message-dispatch depth, used to stagger a single message's
     *  animation along its path: each hop's light starts only when the
     *  previous hop's light has arrived.  Because delivery is a
     *  synchronous depth-first cascade, a parent hop always writes its
     *  slot before its child hop reads it, so no per-cascade reset is
     *  needed.  See on_message_passed / wire_pulse_spawn. */
    gint64   wire_hop_end[PN_WORKSHEET_WIRE_PULSE_DEPTH];

};

typedef struct {
    PnNode *node;   /* borrowed; selection holds the strong ref */
    double  ax;
    double  ay;
} PnDragAnchor;

/* One in-flight light running along a wire as a message crosses it. */
typedef struct {
    PnWire *wire;        /* strong ref; released by the array clear func */
    gint64  start_us;    /* frame-clock stamp at spawn */
    gint64  duration_us; /* constant-speed travel time */
    double  hue;         /* 0..1 from the message id; < 0 = neutral */
} PnWirePulse;

G_DEFINE_TYPE (PnWorksheet, pn_worksheet, GTK_TYPE_DRAWING_AREA)

enum {
    SIG_STATUS_MESSAGE,
    SIG_DEBUG_MESSAGE,
    SIG_SELECTION_CHANGED,
    SIG_MODIFIED_CHANGED,
    SIG_HELP_REQUESTED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/** Sheet-name filter used by every iterator below.  Returns %TRUE
 *  iff @node belongs to the spreadsheet-style sheet this widget
 *  represents.  When @self->sheet_name is %NULL (unusual; only
 *  during construction) the filter is permissive so the widget
 *  behaves like the pre-multi-sheet design. */
static inline gboolean
node_on_sheet (PnWorksheet *self, PnNode *node)
{
    if (self->sheet_name == NULL)
        return TRUE;
    return g_strcmp0 (pn_node_get_worksheet (node), self->sheet_name) == 0;
}

static inline gboolean
wire_on_sheet (PnWorksheet *self, PnWire *wire)
{
    PnNode *src = pn_wire_get_source (wire);
    PnNode *dst = pn_wire_get_target (wire);
    if (src == NULL || dst == NULL)
        return FALSE;
    return node_on_sheet (self, src) && node_on_sheet (self, dst);
}

/* Axis-aligned rectangle overlap test (inclusive).  Used by the painter
 * to skip wires and nodes that fall outside the current cairo clip, so a
 * small partial redraw (e.g. just around a moving message-flow light)
 * does not pay to lay out and paint the whole sheet. */
static inline gboolean
rects_overlap (double ax0, double ay0, double ax1, double ay1,
               double bx0, double by0, double bx1, double by1)
{
    return ax0 <= bx1 && ax1 >= bx0 && ay0 <= by1 && ay1 >= by0;
}

/* Forward declarations for helpers used by the painter. */
static void marquee_normalise (PnWorksheet *self,
                               double *out_x, double *out_y,
                               double *out_w, double *out_h);
static void node_plot_rect    (PnNode *node,
                               double *out_x, double *out_y,
                               double *out_w, double *out_h);
static void zoom_target_rect  (PnWorksheet *self,
                               double *out_x, double *out_y,
                               double *out_w, double *out_h);
static void zoom_current_rect (PnWorksheet *self,
                               double *out_x, double *out_y,
                               double *out_w, double *out_h);

/* ------------------------------------------------------------------ */
/*  Drawing                                                            */
/* ------------------------------------------------------------------ */

/* Worksheet grid step (in worksheet coordinates).  The reference
 * grid is drawn at this pitch, and node geometry is laid out in
 * whole multiples so rectangles align with grid lines. */
#define PN_GRID_STEP        20.0

/* View-zoom limits and per-tick multiplier.  Ctrl+wheel multiplies
 * #self->zoom by PN_ZOOM_STEP per scroll unit (or divides on the
 * other direction), clamped into [PN_ZOOM_MIN, PN_ZOOM_MAX].  The
 * range is symmetric in log space around 1.0 so both zoom-in and
 * zoom-out from 100% feel equivalent — 25% is two doublings out and
 * 400% is two doublings in. */
#define PN_ZOOM_MIN        0.25
#define PN_ZOOM_MAX        4.00
#define PN_ZOOM_STEP       1.10

/* One mouse-wheel tick over a maximized oscilloscope knob stands in for
 * this many pixels of vertical drag (the sign carries the direction), so
 * the wheel nudges the knob the same way a drag does without the precise
 * grab.  Sized for a noticeable-but-gentle step per detent. */
#define PN_OSC_WHEEL_STEP_PX  14.0

/* Floor for the worksheet's reported size, in widget pixels.  An
 * empty worksheet still asks for at least this much so the enclosing
 * GtkScrolledWindow has a sensible viewport even before the user
 * drops the first node. */
#define PN_CANVAS_MIN_WIDTH    800.0
#define PN_CANVAS_MIN_HEIGHT   600.0

/* Slack added around the content bounding box (in worksheet
 * coordinates) so the user can drop a new node a bit past the
 * right/bottom edge of the existing content without immediately
 * fighting the scrollbar. */
#define PN_CANVAS_PADDING      200.0

/** Convert a widget-space x coordinate (e.g. from a #GdkEvent) into
 *  the worksheet-space coordinate that the canvas uses for node
 *  positions.  Inverse of the cairo transform applied during
 *  drawing: world = widget / zoom.  Pan is handled by the enclosing
 *  scrolled window (events arrive in drawing-area coordinates that
 *  already include the scrollbar offset), so no offset term is
 *  needed here. */
static inline double
widget_to_world_x (PnWorksheet *self, double x)
{
    return x / self->zoom;
}

static inline double
widget_to_world_y (PnWorksheet *self, double y)
{
    return y / self->zoom;
}

/** Walk up from the worksheet to find the enclosing
 *  #GtkScrolledWindow whose adjustments drive its pan, or %NULL if
 *  the worksheet has somehow been parented elsewhere.  Used by the
 *  Ctrl+wheel zoom-to-cursor path to scroll the viewport rather
 *  than offsetting the painter. */
static GtkScrolledWindow *
find_scrolled_window (PnWorksheet *self)
{
    GtkWidget *p = gtk_widget_get_parent (GTK_WIDGET (self));

    while (p != NULL && !GTK_IS_SCROLLED_WINDOW (p))
        p = gtk_widget_get_parent (p);

    return p != NULL ? GTK_SCROLLED_WINDOW (p) : NULL;
}

/** Idle callback that applies a pending zoom-to-cursor anchor.
 *  Runs after the post-zoom queue_resize has updated the canvas
 *  extent (and therefore each adjustment's `upper`), so setting the
 *  values here lands them in the freshly-grown valid range instead
 *  of getting clamped against the pre-zoom upper. */
static gboolean
apply_zoom_anchor (gpointer data)
{
    PnWorksheet       *self = PN_WORKSHEET (data);
    GtkScrolledWindow *sw   = find_scrolled_window (self);

    self->zoom_anchor_idle_id = 0;

    if (sw != NULL && self->zoom_anchor_pending)
    {
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (sw);
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (sw);

        if (hadj) gtk_adjustment_set_value (hadj, self->zoom_anchor_hadj);
        if (vadj) gtk_adjustment_set_value (vadj, self->zoom_anchor_vadj);
    }
    self->zoom_anchor_pending = FALSE;

    return G_SOURCE_REMOVE;
}

/* Geometry of the demo nodes.  Node body dimensions are exact
 * multiples of PN_GRID_STEP so the rectangles fit the grid. */
#define PN_NODE_WIDTH      (PN_GRID_STEP * 7)   /* 140 */
#define PN_NODE_HEIGHT     (PN_GRID_STEP * 2)   /*  40 */
#define PN_NODE_ICON_WIDTH (PN_GRID_STEP * 2)   /*  40 */
#define PN_NODE_RADIUS       4.0
/* Error-state decoration.  Any node whose pn_node_get_has_error() is set
 * is painted with this body colour and warning glyph (overriding the
 * node's own colour/icon) so every node type gets a uniform "I am
 * broken" indication without painting its own.  ❗ U+2757. */
#define PN_NODE_ERROR_ICON   "\xe2\x9d\x97"
/* Horizontal breathing room inside the label area: keeps the text
 * (or its trailing ellipsis) from butting up against the icon-panel
 * separator on the left and the rounded outer edge on the right.
 * Pango's `set_width` budget shrinks by twice this value so the
 * ellipsization decision sees the same area the painter does. */
#define PN_NODE_LABEL_PADDING 4.0
#define PN_PORT_WIDTH        7.0
#define PN_PORT_HEIGHT      10.0
#define PN_PORT_RADIUS       2.0

/* World-space y of the centre of input port @index (0-based) of @node.
 *
 * Single-input nodes (N == 1) keep the historical position — the centre
 * of the header edge (header_h/2) — bit-for-bit, so existing nodes
 * (including tall ones like graphs that anchor on the header centreline)
 * do not shift.
 *
 * Multi-input nodes (N >= 2) move their ports off the header into the
 * stacked lower section: port i sits at the centre of row i, header_h +
 * (i + 0.5)·row.  Used by the body painter, the port hit-test, and the
 * wire renderer so all three agree on where a tab is. */
static inline double
input_port_y (PnNode *node, gint index)
{
    const gint    n  = pn_node_get_n_inputs       (node);
    const double  hh = pn_node_get_header_height   (node);
    const PnPoint *p = pn_node_get_position        (node);

    if (n <= 1)
        return p->y + hh * (double) (index + 1) / (double) (n + 1);

    return p->y + hh
         + ((double) index + 0.5) * PN_NODE_INPUT_ROW_HEIGHT;
}

/* Height of a node's solid body — its header plus, for a multi-input
 * node, the stacked input-row section fenced off below it.  This is the
 * filled/outlined rectangle and the node's selection target; it
 * deliberately excludes any paint_plot extension (a passive readout that
 * is not a hit target).  Single-input and input-less nodes report just
 * the header, so their footprint is unchanged. */
static inline double
node_body_height (PnNode *node)
{
    return pn_node_get_header_height (node)
         + pn_node_get_input_section_height (node);
}

/* The "fire" button drawn on the left edge of #PnInject nodes.  Sits
 * outside the node body, the body's left edge being free because
 * inject nodes have no input port.  When the node has no configured
 * button icon the tab keeps the historical compact width with a small
 * cairo-drawn play triangle; once an icon is picked the tab widens to
 * match the header height (i.e. a square) so the chosen icon has room
 * to read and the click target grows with it.
 *
 * PN_INJECT_BUTTON_GAP is the empty space we leave between the tab's
 * right edge and the node body's left edge.  Wide enough to keep the
 * button's own drop shadow (paint_drop_shadow's deepest offset is
 * ~5 px) — and the press-time 2 px down-right translation — clear of
 * the node's outline, so the tab never visually crowds or covers the
 * body. */
#define PN_INJECT_BUTTON_WIDTH         16.0
#define PN_INJECT_BUTTON_ICON_PIXELS   22
#define PN_INJECT_BUTTON_GAP            3.0

/** Width of the fire-button tab for @inject — narrow when no icon is
 *  configured (legacy play-triangle look) and as wide as the header is
 *  tall when an icon name is set, giving a square hit area with room
 *  for the rendered theme icon. */
static double
inject_button_width (PnInject *inject)
{
    const gchar *icon = pn_inject_get_button_icon (inject);
    if (icon == NULL || *icon == '\0')
        return PN_INJECT_BUTTON_WIDTH;
    return pn_node_get_header_height (PN_NODE (inject));
}

/** Trace a rounded-rectangle subpath.  Leaves the path open for the
 *  caller to fill, stroke, or clip as needed. */
static void
rounded_rect_path (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h,
        double   r)
{
    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r,         r, -G_PI_2, 0);
    cairo_arc (cr, x + w - r, y + h - r,     r,  0,       G_PI_2);
    cairo_arc (cr, x + r,     y + h - r,     r,  G_PI_2,  G_PI);
    cairo_arc (cr, x + r,     y + r,         r,  G_PI,    3 * G_PI_2);
    cairo_close_path (cr);
}

/* Node processing-activity glow (TODO #42).  The neon halo's outward
 * bleed (model units) and the number of stacked layers that feather it,
 * the poll cadence that maintains it, and the object-data key under which
 * each node remembers whether it is currently lit. */
#define PN_WORKSHEET_HALO_SPREAD        7.0
#define PN_WORKSHEET_HALO_LAYERS        6
#define PN_WORKSHEET_PROCESSING_POLL_MS 250
#define PN_WORKSHEET_GLOW_KEY           "pn-ws-processing-glow"

/** Paint a neon "busy" halo bleeding out from behind a rounded rectangle
 *  (TODO #42).  Same stacked-translucent-copies trick as
 *  paint_drop_shadow, but the copies grow concentrically OUTWARD in a
 *  neon colour instead of being offset down-right in black, so the node
 *  reads as glowing from behind rather than casting a shadow.  Layers
 *  closer to the edge overlap more, so the alpha builds up bright against
 *  the body and feathers out into the canvas.  Drawn before the body
 *  fill, so the body occludes the inner glow and only the outer ring
 *  shows.  V1: one fixed neon (vivid red) for every node, steady. */
static void
paint_processing_halo (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h,
        double   radius)
{
    int i;
    for (i = 0; i < PN_WORKSHEET_HALO_LAYERS; i++)
    {
        const double s = PN_WORKSHEET_HALO_SPREAD * (i + 1)
                       / (double) PN_WORKSHEET_HALO_LAYERS;
        rounded_rect_path (cr, x - s, y - s, w + 2 * s, h + 2 * s, radius + s);
        cairo_set_source_rgba (cr, 1.00, 0.18, 0.20, 0.13);
        cairo_fill (cr);
    }
}

/** Paint a soft drop shadow under a rounded rectangle.  Cairo has no
 *  blur primitive, so we fake one by stacking several semi-transparent
 *  copies of the silhouette at incrementally larger down-right offsets.
 *  The resulting alpha builds up under the body (and is hidden by it)
 *  while feathering out along the visible bottom-right crescent. */
static void
paint_drop_shadow (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h,
        double   radius)
{
    int i;
    for (i = 0; i < 6; i++)
    {
        const double off = 1.2 + i * 0.72;
        rounded_rect_path (cr, x + off, y + off, w, h, radius);
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.088);
        cairo_fill (cr);
    }
}

/* ------------------------------------------------------------------ */
/*  Comment box (PnComment) — chromeless free-text annotation.         */
/* ------------------------------------------------------------------ */

/* Resize-edge bitmask: which box edges a grabbed handle moves. */
enum
{
    PN_RESIZE_NONE   = 0,
    PN_RESIZE_LEFT   = 1 << 0,
    PN_RESIZE_RIGHT  = 1 << 1,
    PN_RESIZE_TOP    = 1 << 2,
    PN_RESIZE_BOTTOM = 1 << 3,
};

static void     selection_clear    (PnWorksheet *self);
static void     selection_add      (PnWorksheet *self, PnNode *node);
static gboolean selection_contains (PnWorksheet *self, PnNode *node);

/* Fill @cx/@cy (length 8) with the world-space centres of a comment
 * box's resize handles and @mask with the edge bitmask each handle
 * drives, for a box at top-left (@x,@y) with size @w×@h.  Order:
 * NW, N, NE, E, SE, S, SW, W. */
static void
comment_handle_geometry (double x, double y, double w, double h,
                         double cx[8], double cy[8], int mask[8])
{
    const double mx = x + w * 0.5, my = y + h * 0.5;
    const double rx = x + w,       by = y + h;

    cx[0] = x;  cy[0] = y;  mask[0] = PN_RESIZE_TOP    | PN_RESIZE_LEFT;
    cx[1] = mx; cy[1] = y;  mask[1] = PN_RESIZE_TOP;
    cx[2] = rx; cy[2] = y;  mask[2] = PN_RESIZE_TOP    | PN_RESIZE_RIGHT;
    cx[3] = rx; cy[3] = my; mask[3] = PN_RESIZE_RIGHT;
    cx[4] = rx; cy[4] = by; mask[4] = PN_RESIZE_BOTTOM | PN_RESIZE_RIGHT;
    cx[5] = mx; cy[5] = by; mask[5] = PN_RESIZE_BOTTOM;
    cx[6] = x;  cy[6] = by; mask[6] = PN_RESIZE_BOTTOM | PN_RESIZE_LEFT;
    cx[7] = x;  cy[7] = my; mask[7] = PN_RESIZE_LEFT;
}

/* Return the resize-edge bitmask of the comment handle at (@wx,@wy), or
 * %PN_RESIZE_NONE.  Only meaningful while @comment is selected. */
static int
hit_test_comment_handle (PnComment *comment, double wx, double wy)
{
    const PnPoint        *p = pn_node_get_position (PN_NODE (comment));
    PnCommentPaintState   st;
    double                cx[8], cy[8];
    int                   mask[8], i;
    const double          hs = PN_COMMENT_HANDLE_SIZE;

    pn_comment_get_paint_state (comment, &st);
    comment_handle_geometry (p->x, p->y, st.width, st.height, cx, cy, mask);
    for (i = 0; i < 8; i++)
        if (fabs (wx - cx[i]) <= hs * 0.5 && fabs (wy - cy[i]) <= hs * 0.5)
            return mask[i];
    return PN_RESIZE_NONE;
}

/** Paint a comment box: a filled, framed rounded rectangle with wrapped
 *  multi-line text clipped inside, plus eight resize handles when it is
 *  selected.  Reads the GTK-free snapshot pn_comment_get_paint_state. */
static void
draw_comment_box (cairo_t *cr, PnNode *node, PnWorksheet *self)
{
    const PnPoint        *pos = pn_node_get_position (node);
    PnCommentPaintState   st;
    const double          x = pos->x, y = pos->y;
    double                ix, iy, iw, ih;

    pn_comment_get_paint_state (PN_COMMENT (node), &st);

    /* Drop shadow, then the white (configured) fill and grey (configured)
     * frame — matching how node bodies sit above the canvas. */
    paint_drop_shadow (cr, x, y, st.width, st.height,
                       PN_COMMENT_CORNER_RADIUS);

    rounded_rect_path (cr, x, y, st.width, st.height,
                       PN_COMMENT_CORNER_RADIUS);
    cairo_set_source_rgba (cr, st.background_color.red,
                           st.background_color.green,
                           st.background_color.blue,
                           st.background_color.alpha);
    cairo_fill_preserve (cr);
    cairo_set_source_rgba (cr, st.frame_color.red, st.frame_color.green,
                           st.frame_color.blue, st.frame_color.alpha);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    /* Wrapped text inside the padded inner rect, top-left anchored and
     * clipped so overflow is cropped rather than spilling out. */
    ix = x + PN_COMMENT_PADDING;
    iy = y + PN_COMMENT_PADDING;
    iw = st.width  - 2.0 * PN_COMMENT_PADDING;
    ih = st.height - 2.0 * PN_COMMENT_PADDING;

    if (iw >= 1.0 && ih >= 1.0 && st.text != NULL && *st.text != '\0')
    {
        PangoLayout          *layout;
        PangoFontDescription *fd;
        double                size = st.text_size > 1.0 ? st.text_size : 1.0;

        cairo_save (cr);
        cairo_rectangle (cr, ix, iy, iw, ih);
        cairo_clip (cr);

        layout = pango_cairo_create_layout (cr);
        fd     = pango_font_description_from_string ("Sans");
        pango_font_description_set_absolute_size (fd, size * PANGO_SCALE);
        pango_layout_set_font_description (layout, fd);
        pango_font_description_free (fd);

        pango_layout_set_width (layout, (int) (iw * PANGO_SCALE));
        pango_layout_set_wrap  (layout, PANGO_WRAP_WORD_CHAR);
        pango_layout_set_text  (layout, st.text, -1);

        cairo_set_source_rgba (cr, st.text_color.red, st.text_color.green,
                               st.text_color.blue, st.text_color.alpha);
        cairo_move_to (cr, floor (ix + 0.5), floor (iy + 0.5));
        pango_cairo_show_layout (cr, layout);
        g_object_unref (layout);
        cairo_restore (cr);
    }

    /* Resize handles when selected: small white squares with a frame-
     * coloured outline at the eight edge / corner points. */
    if (self != NULL && selection_contains (self, node))
    {
        double       cx[8], cy[8];
        int          mask[8], i;
        const double hs = PN_COMMENT_HANDLE_SIZE;

        comment_handle_geometry (x, y, st.width, st.height, cx, cy, mask);
        for (i = 0; i < 8; i++)
        {
            cairo_rectangle (cr, cx[i] - hs * 0.5, cy[i] - hs * 0.5, hs, hs);
            cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
            cairo_fill_preserve (cr);
            cairo_set_source_rgb (cr, st.frame_color.red,
                                  st.frame_color.green, st.frame_color.blue);
            cairo_set_line_width (cr, 1.0);
            cairo_stroke (cr);
        }
    }
}

/** Paint a single Node-RED-style node, reading all visual attributes
 *  from a #PnNode instance.  The icon is rendered large and white
 *  inside the darker left panel, mirroring how Node-RED shows its
 *  per-node SVG icon.  When @node is the inject whose fire button is
 *  currently held down, that button is drawn inset for feedback. */
static void
draw_node (
        cairo_t     *cr,
        PnNode      *node,
        PnWorksheet *self)
{
    /* A comment box is a chromeless annotation, not a Node-RED node: it
     * has no icon panel, header, ports or label.  Paint it on its own and
     * skip the whole node-chrome path below. */
    if (PN_IS_COMMENT (node))
    {
        draw_comment_box (cr, node, self);
        return;
    }

    const PnPoint *pos       = pn_node_get_position   (node);
    /* PnColor is layout-identical to GdkRGBA (see pn-color.h). */
    const GdkRGBA *col       = (const GdkRGBA *) pn_node_get_color (node);
    const gchar   *icon      = pn_node_get_icon       (node);
    const gchar   *name      = pn_node_get_name       (node);
    const gchar   *label     = (name != NULL && *name != '\0')
                                   ? name
                                   : pn_node_get_class_name (node);
    const gboolean has_input  = pn_node_get_has_input  (node);
    const gboolean has_output = pn_node_get_has_output (node);
    const gboolean disabled   = pn_node_get_disabled   (node);
    const gboolean has_error  = pn_node_get_has_error  (node);
    /* Busy glow (TODO #42): only when the worksheet owns a real widget
     * (self != NULL), the preference is on, and the node is actually
     * working (disabled wins — an inert node never glows).  Short-circuits
     * before the per-node mutex read when the preference is off. */
    const gboolean processing = self != NULL && !disabled
        && pn_preferences_get_visualize_node_processing (
                   pn_preferences_get_default ())
        && pn_node_is_processing_visible (node, g_get_monotonic_time ());
    const double   x = pos->x;
    const double   y = pos->y;
    /* Standard styling fills the *header* portion of the footprint —
     * a node whose total size exceeds the header (graphs, etc.) paints
     * its extension in a separate post-pass below. */
    double         full_w;
    double         full_h;
    const double   header_h = pn_node_get_header_height (node);
    pn_node_get_size (node, &full_w, &full_h);
    /* Solid body = header + (for multi-input nodes) the stacked input
     * section below it.  The body fill, outline and drop shadow span this
     * height; the icon panel, label and output port stay in the header
     * portion.  Single-input nodes have body_h == header_h, so their
     * painting is unchanged. */
    const gint     n_inputs  = has_input ? pn_node_get_n_inputs (node) : 0;
    const gboolean multi_in  = n_inputs >= 2;
    const double   body_h    = node_body_height (node);
    /* Override the node's body colour: neutral grey while disabled (so
     * the inert state reads at a glance), red while in an error state
     * (so a broken node stands out).  Disabled wins — a node the user
     * turned off is inert and not "erroring".  All downstream shading
     * (outline, icon panel, separator) is derived from r/g/b so this
     * single substitution recolours the entire node. */
    const GdkRGBA  grey      = { 0.72, 0.72, 0.72, 1.0 };
    const GdkRGBA  errcol    = { 0.86, 0.30, 0.28, 1.0 };
    const GdkRGBA *bodycol   = disabled  ? &grey
                             : has_error ? &errcol
                                         : col;
    const double   r = bodycol->red;
    const double   g = bodycol->green;
    const double   b = bodycol->blue;

    if (icon  == NULL) icon  = "";
    if (label == NULL) label = "";

    /* The error state also swaps the node's own glyph for a warning
     * mark, mirroring the body recolour.  Disabled keeps the node's
     * normal icon (greyed), since an inert node is not erroring. */
    if (has_error && !disabled)
        icon = PN_NODE_ERROR_ICON;

    /* Drop shadow — drawn first so it sits underneath the node body.  A
     * busy node swaps the shadow for a neon processing halo (TODO #42),
     * so it reads as glowing from behind rather than resting on the
     * canvas. */
    if (processing)
        paint_processing_halo (cr, x, y, full_w, body_h, PN_NODE_RADIUS);
    else
        paint_drop_shadow (cr, x, y, full_w, body_h, PN_NODE_RADIUS);

    /* Body fill. */
    rounded_rect_path (cr, x, y,
                       full_w, body_h, PN_NODE_RADIUS);
    cairo_set_source_rgb (cr, r, g, b);
    cairo_fill_preserve (cr);

    /* Body outline — drawn as a stacked pair of strokes for an
     * engraved look: a dark trace on the existing outline path, then
     * a 1-px-inset light trace just inside it.  With the canvas lit
     * "from above", the dark outer ring reads as the shadow cast into
     * the surrounding groove and the light inner ring as the
     * highlight on the inner lip — the classic recessed bevel.  The
     * inset highlight is clipped to the body so it doesn't bleed
     * past the rounded corners. */
    cairo_set_source_rgb (cr, r * 0.55, g * 0.55, b * 0.55);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_save (cr);
    rounded_rect_path (cr, x, y, full_w, body_h, PN_NODE_RADIUS);
    cairo_clip (cr);
    rounded_rect_path (cr,
                       x + 1.0, y + 1.0,
                       full_w - 2.0, body_h - 2.0,
                       PN_NODE_RADIUS - 1.0);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.22);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* Fire button on the left edge for #PnInject nodes.  A grey tab
     * containing either a small cairo-drawn play triangle (default
     * look) or, when the user has picked a themed icon, that icon
     * loaded from the GTK icon theme rendered larger inside a wider
     * tab.  Clicking it activates the inject (handled in
     * on_button_press).
     *
     * Idle: drawn at (bx, by) above its drop shadow so the tab looks
     * like it is floating slightly above the canvas, matching the
     * shadow under the node body.
     *
     * Pressed: the shadow is omitted and the whole tab translates
     * down-right by PN_INJECT_BUTTON_PRESS_OFFSET, so the painted
     * artwork ends up exactly where the shadow used to sit — the eye
     * reads it as the button having been physically depressed onto
     * the canvas.  The inner artwork (triangle / icon) is positioned
     * relative to the translated tab origin and therefore follows for
     * free. */
    if (PN_IS_INJECT (node))
    {
        const double    PN_INJECT_BUTTON_PRESS_OFFSET = 2.0;
        PnInject       *inject  = PN_INJECT (node);
        const double    bw      = inject_button_width (inject);
        const double    bh      = header_h;
        const gboolean  pressed =
                self != NULL && self->pressed_inject == inject;
        const double    shift   = pressed ? PN_INJECT_BUTTON_PRESS_OFFSET : 0.0;
        const double    bx      = (x - bw - PN_INJECT_BUTTON_GAP) + shift;
        const double    by      = y                                + shift;
        const gchar    *icon_name = pn_inject_get_button_icon (inject);

        if (!pressed)
            paint_drop_shadow (cr, bx, by, bw, bh, PN_NODE_RADIUS);

        rounded_rect_path (cr, bx, by, bw, bh, PN_NODE_RADIUS);
        cairo_set_source_rgb (cr, 0.55, 0.55, 0.58);
        cairo_fill_preserve (cr);
        cairo_set_source_rgb (cr, 0.20, 0.20, 0.22);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);

        gboolean icon_painted = FALSE;

        if (icon_name != NULL && *icon_name != '\0')
        {
            /* Themed icon variant: load from the GTK icon theme as a
             * pixbuf and paint it into the tab.  A missing icon name
             * silently falls back to the play triangle below — the
             * combo's entry stays editable, so we cannot trust the user
             * picked something the theme actually ships. */
            GtkIconTheme *theme  = gtk_icon_theme_get_default ();
            GdkPixbuf    *pixbuf = gtk_icon_theme_load_icon (
                    theme, icon_name,
                    PN_INJECT_BUTTON_ICON_PIXELS,
                    GTK_ICON_LOOKUP_FORCE_SIZE,
                    NULL);
            if (pixbuf != NULL)
            {
                const double iw  = (double) gdk_pixbuf_get_width  (pixbuf);
                const double ih  = (double) gdk_pixbuf_get_height (pixbuf);
                const double ix  = bx + (bw - iw) / 2.0;
                const double iy  = by + (bh - ih) / 2.0;

                cairo_save (cr);
                gdk_cairo_set_source_pixbuf (cr, pixbuf, ix, iy);
                cairo_paint (cr);
                cairo_restore (cr);

                g_object_unref (pixbuf);
                icon_painted = TRUE;
            }
        }

        if (!icon_painted)
        {
            /* Default look: cairo-drawn play triangle centred in the
             * narrow tab.  Reached both when no icon is configured and
             * when a configured icon failed to load from the theme. */
            const double cx = bx + bw / 2.0;
            const double cy = by + bh / 2.0;
            const double s  = 4.0;
            cairo_move_to (cr, cx - s / 2.0, cy - s);
            cairo_line_to (cr, cx + s / 2.0, cy);
            cairo_line_to (cr, cx - s / 2.0, cy + s);
            cairo_close_path (cr);
            cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.92);
            cairo_fill (cr);
        }
    }

    /* Icon panel: clip to the body shape so the darker tab follows
     * the rounded-left corners. */
    cairo_save (cr);
    rounded_rect_path (cr, x, y,
                       full_w, header_h, PN_NODE_RADIUS);
    cairo_clip (cr);

    cairo_rectangle (cr, x, y, PN_NODE_ICON_WIDTH, header_h);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.12);
    cairo_fill (cr);

    /* Vertical separator between icon panel and label area — drawn as
     * an engraved groove: a dark line on the icon-panel side stacked
     * against a light line on the label-area side.  With the canvas
     * lit "from above (slightly upper-left)" the dark trace reads as
     * the shadow on the groove's left wall and the light trace as the
     * highlight catching the right wall. */
    cairo_set_line_width (cr, 1.0);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.32);
    cairo_move_to (cr, x + PN_NODE_ICON_WIDTH - 0.5, y);
    cairo_line_to (cr, x + PN_NODE_ICON_WIDTH - 0.5, y + header_h);
    cairo_stroke (cr);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.22);
    cairo_move_to (cr, x + PN_NODE_ICON_WIDTH + 0.5, y);
    cairo_line_to (cr, x + PN_NODE_ICON_WIDTH + 0.5, y + header_h);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* Icon glyph, centred in the dark left panel.  Use Pango (not the
     * cairo toy text API) so fontconfig can substitute a font that
     * actually contains the requested symbol — otherwise unusual
     * glyphs come out as tofu. */
    {
        PangoLayout          *layout;
        PangoFontDescription *desc;
        int                   pw;
        int                   ph;

        layout = pango_cairo_create_layout (cr);
        desc   = pango_font_description_from_string ("Sans, FontAwesome Bold 16");
        pango_layout_set_font_description (layout, desc);
        pango_font_description_free (desc);
        pango_layout_set_text (layout, icon, -1);
        pango_layout_get_pixel_size (layout, &pw, &ph);

        /* Raised emboss for the icon glyph — matches the letterpress
         * treatment on the label but inverted in direction, so the
         * icon reads as standing proud of the dark panel rather than
         * carved into it (engraving a near-white glyph into a near-
         * black panel collapses contrast and disappears, so the icon
         * pops out instead).  Highlight one pixel up catches overhead
         * light on the glyph's top rim, shadow one pixel down lays
         * down the cast shadow on the bottom rim, and the off-white
         * face sits on top. */
        {
            const double ix = x + (PN_NODE_ICON_WIDTH - pw) / 2.0;
            const double iy = y + (header_h           - ph) / 2.0;
            cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.55);
            cairo_move_to (cr, ix, iy - 1.0);
            pango_cairo_show_layout (cr, layout);
            cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.55);
            cairo_move_to (cr, ix, iy + 1.0);
            pango_cairo_show_layout (cr, layout);
            cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
            cairo_move_to (cr, ix, iy);
            pango_cairo_show_layout (cr, layout);
        }

        g_object_unref (layout);
    }

    /* Label.  Painted in the area to the right of the icon panel,
     * centred when the text fits and end-ellipsized ("HTTPS Tu…")
     * when it does not — node names like "HTTPS Tunnel Receiver 1"
     * are wider than the default 100 px label area at the Sans 12
     * size we use, and the previous cairo-toy-text path silently let
     * those bleed past the rounded outline.  Pango handles both the
     * truncation decision and the centred alignment when there is
     * slack, so the painter just hands it a width budget; the
     * surrounding cairo_clip is a belt-and-suspenders guard against
     * pixel-level ascent/descent overshoot from fonts with tall
     * glyphs, and against the rounded right-edge of the body shape
     * that a bare rectangular layout would not otherwise respect. */
    {
        PangoLayout          *layout;
        PangoFontDescription *desc;
        int                   pw;
        int                   ph;
        /* A class-owned right-edge decoration (e.g. PnLed's status
         * light) reserves a strip of the header so the centred label
         * doesn't slide under it. */
        const double          right_dec =
                PN_NODE_GET_CLASS (node)->paint_right_decoration_width;
        const double          area_x  = x + PN_NODE_ICON_WIDTH;
        const double          area_w  = full_w - PN_NODE_ICON_WIDTH - right_dec;
        const double          pad     = PN_NODE_LABEL_PADDING;
        const double          inner_w = area_w - 2.0 * pad;

        layout = pango_cairo_create_layout (cr);
        desc   = pango_font_description_from_string ("Sans");
        /* Match the cairo-toy 12 px height of the previous label code
         * exactly — Pango defaults to points, which scale through
         * 96/72 and would land at 16 px (visibly larger) for the
         * naive "Sans 12" spelling. */
        pango_font_description_set_absolute_size (desc, 12.0 * PANGO_SCALE);
        pango_layout_set_font_description (layout, desc);
        pango_font_description_free (desc);
        pango_layout_set_text       (layout, label, -1);
        pango_layout_set_width      (layout,
                                     (int) (inner_w * PANGO_SCALE));
        pango_layout_set_ellipsize  (layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment  (layout, PANGO_ALIGN_CENTER);
        pango_layout_get_pixel_size (layout, &pw, &ph);

        /* Clip to the body's rounded rect first so a future taller
         * label or a slanted font's descender does not poke past the
         * right-edge corner; then narrow the clip to the label sub-
         * rectangle so the text cannot stray into the icon panel
         * either.  cairo_clip intersects with the previous clip, so
         * doing them in sequence gives us the corner-rounded label
         * area without explicit path arithmetic. */
        cairo_save (cr);
        rounded_rect_path (cr, x, y, full_w, header_h, PN_NODE_RADIUS);
        cairo_clip (cr);
        cairo_rectangle (cr, area_x, y, area_w, header_h);
        cairo_clip (cr);

        /* Engraved text — stacked as three passes for a pronounced
         * letterpress emboss: a 1-px-down white highlight catches the
         * light on the lower lip of the carved groove (where light
         * coming from above grazes the inside wall and bounces back
         * out), a 1-px-up dark shadow paints the upper lip in shade
         * (where the same overhead light fails to reach the inside of
         * the cut), and the near-black glyphs sit on top reading as
         * the bottom of the recess.  Stronger alphas than the previous
         * single-highlight pass make the effect read clearly even on
         * dark body colours where a faint 35 % white highlight would
         * disappear against the fill. */
        {
            const double tx = area_x + pad;
            const double ty = y + (header_h - ph) / 2.0;
            cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.65);
            cairo_move_to (cr, tx, ty + 1.0);
            pango_cairo_show_layout (cr, layout);
            cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
            cairo_move_to (cr, tx, ty - 1.0);
            pango_cairo_show_layout (cr, layout);
            cairo_set_source_rgb (cr, 0.25, 0.25, 0.25);
            cairo_move_to (cr, tx, ty);
            pango_cairo_show_layout (cr, layout);
        }
        cairo_restore (cr);

        g_object_unref (layout);
    }

    /* Framing line fencing the header off from the stacked input section
     * of a multi-input node.  Drawn as an engraved groove matching the
     * icon-panel separator — a dark trace on the header side stacked
     * against a light trace on the input-section side — and clipped to
     * the body so it respects the rounded corners. */
    if (multi_in)
    {
        cairo_save (cr);
        rounded_rect_path (cr, x, y, full_w, body_h, PN_NODE_RADIUS);
        cairo_clip (cr);
        cairo_set_line_width (cr, 1.0);
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.32);
        cairo_move_to (cr, x,          y + header_h - 0.5);
        cairo_line_to (cr, x + full_w, y + header_h - 0.5);
        cairo_stroke (cr);
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.22);
        cairo_move_to (cr, x,          y + header_h + 0.5);
        cairo_line_to (cr, x + full_w, y + header_h + 0.5);
        cairo_stroke (cr);
        cairo_restore (cr);
    }

    /* Ports.  Drawn as small rounded tabs straddling the node edge.  A
     * single-input node anchors its tab on the header centerline (so
     * nodes whose footprint extends past the header, e.g. graphs, still
     * expose a port at the expected height); a multi-input node lays its
     * tabs down the lower section, one per row, each labelled with its
     * input name ("value1", "value2", …) to its right. */
    if (has_input)
    {
        gint i;

        for (i = 0; i < n_inputs; i++)
        {
            const double cy = input_port_y (node, i);
            const double px = x - PN_PORT_WIDTH / 2.0;
            const double py = cy - PN_PORT_HEIGHT / 2.0;
            rounded_rect_path (cr, px, py,
                               PN_PORT_WIDTH, PN_PORT_HEIGHT, PN_PORT_RADIUS);
            cairo_set_source_rgb (cr, 0.78, 0.78, 0.80);
            cairo_fill_preserve (cr);
            cairo_set_source_rgb (cr, 0.40, 0.40, 0.42);
            cairo_set_line_width (cr, 1.0);
            cairo_stroke (cr);

            /* Per-input name, painted inside the row beside its tab.
             * Engraved (light-below / dark face) like the node label so
             * it reads on any body colour. */
            if (multi_in)
            {
                PangoLayout          *ilayout;
                PangoFontDescription *idesc;
                const gchar          *iname = pn_node_get_input_name (node, i);
                int                   iw, ih;
                double                tx, ty;

                ilayout = pango_cairo_create_layout (cr);
                idesc   = pango_font_description_from_string ("Sans");
                pango_font_description_set_absolute_size (idesc,
                                                          11.0 * PANGO_SCALE);
                pango_layout_set_font_description (ilayout, idesc);
                pango_font_description_free (idesc);
                pango_layout_set_text (ilayout, iname, -1);
                pango_layout_get_pixel_size (ilayout, &iw, &ih);

                tx = x + PN_PORT_WIDTH / 2.0 + 5.0;
                ty = cy - ih / 2.0;

                cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.55);
                cairo_move_to (cr, tx, ty + 1.0);
                pango_cairo_show_layout (cr, ilayout);
                cairo_set_source_rgb (cr, 0.22, 0.22, 0.22);
                cairo_move_to (cr, tx, ty);
                pango_cairo_show_layout (cr, ilayout);

                g_object_unref (ilayout);

                /* Last value seen on this input, right-aligned in the
                 * remaining row width.  Maintained by the core for every
                 * multi-input node (vectors already elided to a short
                 * sample, "[0, 1, 2, …] (256 values)", like the debug
                 * pane).  Painted only when there is room past the name so
                 * the two never collide; Pango ellipsizes to the budget. */
                {
                    const gchar *ival =
                            pn_node_get_input_value_display (node, i);
                    const double left  = tx + iw + 8.0;
                    const double right = x + full_w - PN_NODE_LABEL_PADDING;
                    const double avail = right - left;

                    if (ival != NULL && *ival != '\0' && avail > 12.0)
                    {
                        PangoLayout          *vlayout;
                        PangoFontDescription *vdesc;
                        int                   vw, vh;
                        double                vty;

                        vlayout = pango_cairo_create_layout (cr);
                        vdesc   = pango_font_description_from_string ("Sans");
                        pango_font_description_set_absolute_size (
                                vdesc, 10.0 * PANGO_SCALE);
                        pango_layout_set_font_description (vlayout, vdesc);
                        pango_font_description_free (vdesc);
                        pango_layout_set_text (vlayout, ival, -1);
                        pango_layout_set_width (vlayout,
                                                (int) (avail * PANGO_SCALE));
                        pango_layout_set_ellipsize (vlayout,
                                                    PANGO_ELLIPSIZE_END);
                        pango_layout_set_alignment (vlayout,
                                                    PANGO_ALIGN_RIGHT);
                        pango_layout_get_pixel_size (vlayout, &vw, &vh);
                        vty = cy - vh / 2.0;

                        /* Engraved like the name, but a touch darker so the
                         * value reads as data against the input label. */
                        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.45);
                        cairo_move_to (cr, left, vty + 1.0);
                        pango_cairo_show_layout (cr, vlayout);
                        cairo_set_source_rgb (cr, 0.12, 0.12, 0.14);
                        cairo_move_to (cr, left, vty);
                        pango_cairo_show_layout (cr, vlayout);

                        g_object_unref (vlayout);
                    }
                }
            }
        }
    }

    if (has_output)
    {
        const double px = x + full_w - PN_PORT_WIDTH / 2.0;
        const double py = y + (header_h - PN_PORT_HEIGHT) / 2.0;
        rounded_rect_path (cr, px, py,
                           PN_PORT_WIDTH, PN_PORT_HEIGHT, PN_PORT_RADIUS);
        cairo_set_source_rgb (cr, 0.78, 0.78, 0.80);
        cairo_fill_preserve (cr);
        cairo_set_source_rgb (cr, 0.40, 0.40, 0.42);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);
    }

    /* Class-owned in-header decoration painted on top of the standard
     * fill/outline/label.  Clipped to the rounded body so a decoration
     * landing on the right edge respects the corner radius. */
    {
        PnNodeClass *klass = PN_NODE_GET_CLASS (node);

        if (klass->paint_header_overlay != NULL)
        {
            cairo_save (cr);
            rounded_rect_path (cr, x, y, full_w, header_h, PN_NODE_RADIUS);
            cairo_clip (cr);
            klass->paint_header_overlay (node, cr, x, y, full_w, header_h);
            cairo_restore (cr);
        }
    }

    /* Post-pass extensions painted below the header.  Any node that
     * exposes a #PnNodeClass.paint_plot vfunc (graph, table, …) opts
     * into this branch; nodes that don't are header-only and the
     * standard path painted above is everything they need.  Skip the
     * plot altogether for the node that is currently zoomed (or
     * animating into / out of the zoom): the worksheet draws it as
     * an overlay after every node has been painted, so painting it
     * here too would double-render it under the lifted copy. */
    {
        PnNodeClass *klass = PN_NODE_GET_CLASS (node);

        if (klass->paint_plot != NULL &&
            (self == NULL || self->zoomed_node != node))
        {
            const double plot_y = y + header_h + 4.0;
            const double plot_h = full_h - header_h - 4.0;

            /* The processing halo (TODO #42) glows only behind the node
             * rectangle (the header body, painted above); the client-area
             * extension keeps its standard drop shadow regardless, unless
             * the class opts out of it. */
            if (!klass->paint_plot_skip_shadow)
                paint_drop_shadow (cr, x, plot_y, full_w, plot_h,
                                   klass->paint_plot_corner_radius);
            klass->paint_plot (node, cr, x, plot_y, full_w, plot_h);
        }
    }
    (void) full_h;
}

/** Paint the drag-preview wireframe: a thin red, no-fill sketch of the
 *  node a palette drop would create at the current cursor position.
 *  Traces the header outline and icon-panel separator, then paints the
 *  icon glyph and the (ellipsized) name in red — mirroring draw_node()'s
 *  geometry so the ghost reads as the node that will land.  Nodes that
 *  report a client area (pn_node_get_client_area — a table's grid, a
 *  graph's plot) additionally get that body outlined below the header,
 *  so the ghost shows the full footprint a drop would occupy; header-
 *  only nodes (Debug, …) get the header outline alone.  The footprint
 *  is centred under the cursor and tracks it smoothly; unlike the
 *  committed drop it is *not* snapped to the grid. */
static void
draw_node_wireframe (
        PnWorksheet *self,
        cairo_t     *cr)
{
    PnNode        *node  = self->drag_preview_node;
    const gchar   *icon  = pn_node_get_icon       (node);
    const gchar   *name  = pn_node_get_name       (node);
    const gchar   *label = (name != NULL && *name != '\0')
                               ? name
                               : pn_node_get_class_name (node);
    const double   header_h = pn_node_get_header_height (node);
    /* Node-RED red, matching the selection halo. */
    const double   rr = 0.85, rg = 0.15, rb = 0.15;
    double         dw, dh;
    double         wx, wy, x, y;

    pn_node_get_size (node, &dw, &dh);

    /* Track the cursor smoothly — the ghost is *not* snapped to the
     * grid; the snap happens only when the node is actually placed on
     * drop (see on_drag_data_received), so the preview shows where the
     * pointer is, not where the node will land. */
    wx = widget_to_world_x (self, (double) self->drag_preview_x);
    wy = widget_to_world_y (self, (double) self->drag_preview_y);
    x  = wx - dw / 2.0;
    y  = wy - dh / 2.0;

    if (icon  == NULL) icon  = "";
    if (label == NULL) label = "";

    cairo_save (cr);
    cairo_set_source_rgb (cr, rr, rg, rb);
    cairo_set_line_width (cr, 1.0);

    /* Body outline — header for ordinary nodes, header + stacked input
     * section for multi-input ones (any paint_plot client-area body is
     * traced separately below). */
    rounded_rect_path (cr, x, y, dw, node_body_height (node), PN_NODE_RADIUS);
    cairo_stroke (cr);

    /* Icon-panel separator, so the sketch reads as a node at a glance. */
    cairo_move_to (cr, x + PN_NODE_ICON_WIDTH + 0.5, y);
    cairo_line_to (cr, x + PN_NODE_ICON_WIDTH + 0.5, y + header_h);
    cairo_stroke (cr);

    /* Framing line below the header for multi-input ghosts, mirroring
     * the painted node. */
    if (pn_node_get_n_inputs (node) >= 2)
    {
        cairo_move_to (cr, x,      y + header_h);
        cairo_line_to (cr, x + dw, y + header_h);
        cairo_stroke (cr);
    }

    /* Icon glyph, centred in the icon panel. */
    {
        PangoLayout          *layout = pango_cairo_create_layout (cr);
        PangoFontDescription *desc   =
                pango_font_description_from_string ("Sans, FontAwesome Bold 16");
        int pw, ph;

        pango_layout_set_font_description (layout, desc);
        pango_font_description_free (desc);
        pango_layout_set_text (layout, icon, -1);
        pango_layout_get_pixel_size (layout, &pw, &ph);

        cairo_move_to (cr,
                       x + (PN_NODE_ICON_WIDTH - pw) / 2.0,
                       y + (header_h           - ph) / 2.0);
        pango_cairo_show_layout (cr, layout);
        g_object_unref (layout);
    }

    /* Name, in the label area right of the icon panel — centred and
     * end-ellipsized exactly as the painted node does it. */
    {
        PangoLayout          *layout = pango_cairo_create_layout (cr);
        PangoFontDescription *desc   = pango_font_description_from_string ("Sans");
        const double          right_dec =
                PN_NODE_GET_CLASS (node)->paint_right_decoration_width;
        const double          area_x  = x + PN_NODE_ICON_WIDTH;
        const double          area_w  = dw - PN_NODE_ICON_WIDTH - right_dec;
        const double          pad     = PN_NODE_LABEL_PADDING;
        const double          inner_w = area_w - 2.0 * pad;
        int pw, ph;

        pango_font_description_set_absolute_size (desc, 12.0 * PANGO_SCALE);
        pango_layout_set_font_description (layout, desc);
        pango_font_description_free (desc);
        pango_layout_set_text       (layout, label, -1);
        pango_layout_set_width      (layout, (int) (inner_w * PANGO_SCALE));
        pango_layout_set_ellipsize  (layout, PANGO_ELLIPSIZE_END);
        pango_layout_set_alignment  (layout, PANGO_ALIGN_CENTER);
        pango_layout_get_pixel_size (layout, &pw, &ph);

        cairo_move_to (cr, area_x + pad, y + (header_h - ph) / 2.0);
        pango_cairo_show_layout (cr, layout);
        g_object_unref (layout);
    }

    /* Client area: a thin rectangle around the body a node draws below
     * its header — a table's grid, a graph's plot, a text view's body.
     * Reported in node-local coordinates by pn_node_get_client_area, so
     * it is offset from this ghost's top-left.  Header-only nodes (Debug,
     * Comparator, …) report none and get just the header outline above. */
    {
        double cx, cy, cw, ch;

        if (pn_node_get_client_area (node, &cx, &cy, &cw, &ch))
        {
            cairo_rectangle (cr, x + cx, y + cy, cw, ch);
            cairo_stroke (cr);
        }
    }

    cairo_restore (cr);
}

/** Draw a Node-RED-style wire: a horizontal cubic bezier from the
 *  source point to the destination point. */
static void
draw_wire (
        cairo_t *cr,
        double   x1,
        double   y1,
        double   x2,
        double   y2)
{
    /* Control-point offset scales with the horizontal distance so the
     * curve stays graceful both for short and long connections. */
    double dx = fabs (x2 - x1) * 0.5;
    if (dx < 30.0)
        dx = 30.0;

    cairo_set_source_rgb (cr, 0.50, 0.50, 0.55);
    cairo_set_line_width (cr, 2.5);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to (cr, x1, y1);
    cairo_curve_to (cr,
                    x1 + dx, y1,
                    x2 - dx, y2,
                    x2,      y2);
    cairo_stroke (cr);
}

/** Compute the on-canvas plot rectangle for a node that exposes a
 *  #PnNodeClass.paint_plot vfunc — the region under the header where
 *  the painter would normally draw.  Centralised so the zoom
 *  hit-test, the "from" rect of the zoom animation, and the painter
 *  all agree on the same geometry — which is the node's client area
 *  (pn_node_get_client_area, reported in node-local coordinates)
 *  translated by the node's position.  Only ever called for nodes that
 *  do have a client area, so the reported rect is always valid here. */
static void
node_plot_rect (
        PnNode *node,
        double *out_x,
        double *out_y,
        double *out_w,
        double *out_h)
{
    const PnPoint *p = pn_node_get_position (node);
    double         cx = 0.0, cy = 0.0, cw = 0.0, ch = 0.0;

    pn_node_get_client_area (node, &cx, &cy, &cw, &ch);

    if (out_x) *out_x = p->x + cx;
    if (out_y) *out_y = p->y + cy;
    if (out_w) *out_w = cw;
    if (out_h) *out_h = ch;
}

/** The rectangle the zoomed plot lands in once the animation has
 *  finished: a 40 px inset from every edge of the *visible viewport*
 *  rather than the drawing area, so a lifted graph fills the part
 *  the user can actually see instead of stretching across the
 *  scrolled-out parts of the canvas.  Returned in drawing-area
 *  coordinates (the same space as event->x/y) so hit-testing and
 *  the painter agree without an extra translate.  Falls back to the
 *  full widget allocation only when the worksheet is not parented in
 *  a #GtkScrolledWindow (currently never, but keeps unit-test
 *  parenting cheap). */
static void
zoom_target_rect (
        PnWorksheet *self,
        double      *out_x,
        double      *out_y,
        double      *out_w,
        double      *out_h)
{
    const double       margin = 40.0;
    GtkScrolledWindow *sw     = find_scrolled_window (self);
    double             vp_x   = 0.0;
    double             vp_y   = 0.0;
    double             vp_w;
    double             vp_h;

    if (sw != NULL)
    {
        GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment (sw);
        GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (sw);

        if (hadj != NULL)
        {
            vp_x = gtk_adjustment_get_value     (hadj);
            vp_w = gtk_adjustment_get_page_size (hadj);
        }
        else
            vp_w = gtk_widget_get_allocated_width (GTK_WIDGET (self));

        if (vadj != NULL)
        {
            vp_y = gtk_adjustment_get_value     (vadj);
            vp_h = gtk_adjustment_get_page_size (vadj);
        }
        else
            vp_h = gtk_widget_get_allocated_height (GTK_WIDGET (self));
    }
    else
    {
        vp_w = gtk_widget_get_allocated_width  (GTK_WIDGET (self));
        vp_h = gtk_widget_get_allocated_height (GTK_WIDGET (self));
    }

    {
        double w = vp_w - 2.0 * margin;
        double h = vp_h - 2.0 * margin;
        double x = vp_x + margin;
        double y = vp_y + margin;

        /* Guard against absurdly small viewports so we never hand
         * cairo a zero-or-negative rectangle. */
        if (w < 60.0) w = 60.0;
        if (h < 60.0) h = 60.0;

        /* Aspect-locked nodes (image previews, file thumbnails) keep the
         * at-rest plot rectangle's shape: fit it inside the margin box
         * and re-centre, so the maximised copy is the same shape as the
         * minimised one rather than stretched to fill both axes. */
        if (self->zoomed_node != NULL &&
            PN_NODE_GET_CLASS (self->zoomed_node)->paint_plot_zoom_keep_aspect)
        {
            double sw, sh;

            node_plot_rect (self->zoomed_node, NULL, NULL, &sw, &sh);

            if (sw > 0.0 && sh > 0.0)
            {
                const double aspect = sw / sh;

                if (w / h > aspect)   /* box wider than content: cap width  */
                    w = h * aspect;
                else                  /* box taller than content: cap height */
                    h = w / aspect;

                /* Re-centre the fitted rectangle inside the full viewport. */
                x = vp_x + (vp_w - w) * 0.5;
                y = vp_y + (vp_h - h) * 0.5;
            }
        }

        if (out_x) *out_x = x;
        if (out_y) *out_y = y;
        if (out_w) *out_w = w;
        if (out_h) *out_h = h;
    }
}

/** Cubic ease-out on the linear time fraction so the zoom decelerates
 *  as it lands, which reads as "snap forward, settle gently". */
static inline double
zoom_ease (double t)
{
    const double u = 1.0 - t;
    return 1.0 - u * u * u;
}

/** Resolve the plot rectangle for the zoomed graph at the current
 *  animation progress.  When zoom_t is 0 the result equals the
 *  on-canvas plot rect; at 1 it equals the centred target rect; in
 *  between it is a per-component lerp.  Caller must have already
 *  established that #self->zoomed_node is non-%NULL. */
static void
zoom_current_rect (
        PnWorksheet *self,
        double      *out_x,
        double      *out_y,
        double      *out_w,
        double      *out_h)
{
    double fx, fy, fw, fh;
    double tx, ty, tw, th;
    const double a = zoom_ease (self->zoom_t);

    node_plot_rect (self->zoomed_node, &fx, &fy, &fw, &fh);
    zoom_target_rect (self,              &tx, &ty, &tw, &th);

    if (out_x) *out_x = fx + (tx - fx) * a;
    if (out_y) *out_y = fy + (ty - fy) * a;
    if (out_w) *out_w = fw + (tw - fw) * a;
    if (out_h) *out_h = fh + (th - fh) * a;
}

/** Per-frame tick driven by #gtk_widget_add_tick_callback while the
 *  zoom-in or zoom-out animation is in flight.  Computes the current
 *  fraction from the frame clock, updates #self->zoom_t accordingly,
 *  queues a redraw, and tears the tick down once the target end is
 *  reached.  Reaching t == 0 on a zoom-out releases the borrowed
 *  pointer to the graph so the canvas returns to its idle state. */
static gboolean
on_zoom_tick (
        GtkWidget     *widget,
        GdkFrameClock *clock,
        gpointer       user_data)
{
    PnWorksheet *self        = PN_WORKSHEET (widget);
    const gint64 duration_us = 250 * 1000;
    gint64       now_us;
    double       frac;

    (void) user_data;

    now_us = gdk_frame_clock_get_frame_time (clock);
    frac   = (double) (now_us - self->zoom_start_us)
           / (double) duration_us;

    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;

    if (self->zoom_dir > 0)
        self->zoom_t = frac;
    else
        self->zoom_t = 1.0 - frac;

    gtk_widget_queue_draw (widget);

    if (frac >= 1.0)
    {
        if (self->zoom_dir < 0)
            self->zoomed_node = NULL;

        self->zoom_dir     = 0;
        self->zoom_tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

/** Kick off a zoom animation in the requested direction, registering
 *  the tick callback if one is not already running.  The start time
 *  is back-dated by however much progress the animation already has
 *  in the new direction so reversing mid-flight feels continuous
 *  rather than snapping back to either end. */
static void
zoom_start_animation (
        PnWorksheet *self,
        int          new_dir)
{
    const gint64    duration_us = 250 * 1000;
    GdkFrameClock  *clock       = gtk_widget_get_frame_clock (GTK_WIDGET (self));
    const gint64    now_us      = clock != NULL
                                      ? gdk_frame_clock_get_frame_time (clock)
                                      : g_get_monotonic_time ();
    const double    progress    = (new_dir > 0) ? self->zoom_t
                                                : 1.0 - self->zoom_t;

    self->zoom_dir      = new_dir;
    self->zoom_start_us = now_us - (gint64) (duration_us * progress);

    if (self->zoom_tick_id == 0)
        self->zoom_tick_id = gtk_widget_add_tick_callback (
                GTK_WIDGET (self), on_zoom_tick, NULL, NULL);

    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ------------------------------------------------------------------ */
/*  Focus pulse                                                        */
/*                                                                     */
/*  Driven by pn_worksheet_focus_node_by_uuid.  A short outline pulse  */
/*  fades up then back down around the target node so the user can     */
/*  identify which node a debug message originated from after          */
/*  clicking the message's `from` shortcut.  Independent of selection  */
/*  state — the user's selection is left untouched.                    */
/* ------------------------------------------------------------------ */

/* Total duration of the pulse in microseconds.  Long enough that the
 * eye can track which node lit up after a panning auto-scroll, short
 * enough that it does not feel like a state change. */
#define PN_WORKSHEET_PULSE_US (700 * 1000)

/* Ctrl+wheel magnification readout: hold the label at full opacity
 * for #HOLD_US after the last zoom step, then fade across #FADE_US.
 * Hold is long enough that a quick double-tick of the wheel still
 * lets the user read the result; fade is short enough to feel like a
 * confirmation that the gesture has ended rather than a slow dissolve. */
#define PN_WORKSHEET_ZOOM_LABEL_HOLD_US (1500 * 1000)
#define PN_WORKSHEET_ZOOM_LABEL_FADE_US ( 500 * 1000)

/** Per-frame tick that drives the Ctrl+wheel magnification label.
 *  Queues a redraw on every clock tick so the painter can recompute
 *  the alpha from the elapsed time, and tears the tick down once the
 *  hold + fade window has elapsed.  Re-stamping #zoom_label_shown_us
 *  in on_scroll extends the window without needing to touch the
 *  tick — it just keeps firing until quiescence. */
static gboolean
on_zoom_label_tick (
        GtkWidget     *widget,
        GdkFrameClock *clock,
        gpointer       user_data)
{
    PnWorksheet *self = PN_WORKSHEET (widget);
    gint64       now_us;

    (void) user_data;

    now_us = gdk_frame_clock_get_frame_time (clock);

    gtk_widget_queue_draw (widget);

    if (now_us - self->zoom_label_shown_us
        >= PN_WORKSHEET_ZOOM_LABEL_HOLD_US + PN_WORKSHEET_ZOOM_LABEL_FADE_US)
    {
        self->zoom_label_tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

/** Per-frame tick that drives the focus pulse.  Queues a redraw on
 *  every clock tick so the painter can recompute the alpha from the
 *  elapsed time, and tears the tick down (plus clears pulse_node)
 *  once the duration has elapsed. */
static gboolean
on_pulse_tick (
        GtkWidget     *widget,
        GdkFrameClock *clock,
        gpointer       user_data)
{
    PnWorksheet *self = PN_WORKSHEET (widget);
    gint64       now_us;

    (void) user_data;

    now_us = gdk_frame_clock_get_frame_time (clock);

    gtk_widget_queue_draw (widget);

    if (now_us - self->pulse_start_us >= PN_WORKSHEET_PULSE_US)
    {
        self->pulse_node    = NULL;
        self->pulse_tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/*  Wire message-flow animation                                       */
/*                                                                    */
/*  When the "animate-messages-on-wires" preference is on, each       */
/*  message crossing a wire spawns a short-lived light that runs      */
/*  along the wire's curve from source to target.  The light's hue    */
/*  is derived from the message id, so one message keeps the same     */
/*  colour at every hop and can be followed across the whole graph.   */
/* ------------------------------------------------------------------ */

/* The light's travel speed (worksheet units per second) is configurable
 * via the "wire-pulse-speed" preference; it scales with zoom like the
 * wires themselves.  Short wires are clamped to a minimum duration so
 * they do not flash by in a single frame. */
#define PN_WORKSHEET_WIRE_PULSE_MIN_US      (120 * 1000)

/* Backstops against a high-frequency source flooding the array: at most
 * this many concurrent lights per wire, and a global ceiling.  When a cap
 * is hit the NEW spawn is dropped — lights already travelling are left
 * alone so they always reach their target. */
#define PN_WORKSHEET_WIRE_PULSE_PER_WIRE    16
#define PN_WORKSHEET_WIRE_PULSE_MAX         1024

/* The animation is driven by a plain timeout rather than a frame-clock
 * tick callback, so the widget wakes only a few times a second while a
 * light is travelling instead of at the full display rate (60–144 Hz).
 * The interval (milliseconds) is the "wire-pulse-interval" preference —
 * larger is cheaper and chunkier — so the CPU/smoothness tradeoff can be
 * tuned live. */

/* The light advances in discrete jumps of this many worksheet units
 * rather than moving continuously, so a redraw only happens once the
 * position has actually changed — fewer, coarser steps, less CPU. */
#define PN_WORKSHEET_WIRE_PULSE_STEP_PX     6.0

/* GArray clear func: release the strong ref each pulse holds. */
static void
wire_pulse_clear (gpointer data)
{
    PnWirePulse *p = data;
    g_clear_object (&p->wire);
}

/* Resolve a wire's on-canvas endpoints, matching the static wire
 * painter exactly so a light tracks the curve pixel-for-pixel.
 * Returns %FALSE for wires that are off this sheet or have a missing
 * endpoint (a wire removed mid-flight nulls its endpoints), which the
 * callers treat as "skip / expire". */
static gboolean
wire_endpoints (
        PnWorksheet *self,
        PnWire      *wire,
        double      *x1,
        double      *y1,
        double      *x2,
        double      *y2)
{
    PnNode        *src;
    PnNode        *dst;
    const PnPoint *ps;
    const PnPoint *pt;
    double         sw, sh, shh;

    if (!wire_on_sheet (self, wire))
        return FALSE;

    src = pn_wire_get_source (wire);
    dst = pn_wire_get_target (wire);   /* both non-NULL per wire_on_sheet */

    ps  = pn_node_get_position (src);
    pt  = pn_node_get_position (dst);
    pn_node_get_size (src, &sw, &sh);
    shh = pn_node_get_header_height (src);
    (void) sh;

    *x1 = ps->x + sw;
    *y1 = ps->y + shh / 2.0;
    *x2 = pt->x;
    *y2 = input_port_y (dst, pn_wire_get_target_input (wire));
    return TRUE;
}

/* Like wire_endpoints(), but the target is the centre of the destination
 * node's header rather than its input tab, so the message-flow light
 * flies past the edge and lands in the middle of the node (it is painted
 * on top of the nodes). */

/* The full path a message-flow light travels, in three segments:
 *   A: straight from the source node's centre out to the wire start;
 *   B: the wire's own cubic bezier (identical to the painted wire);
 *   C: straight from the wire end into the target node's centre.
 * Segment lengths are kept so the light moves at a constant speed across
 * the whole path. */
typedef struct {
    double sx, sy;        /* source node centre   (A start) */
    double x1, y1;        /* wire start           (A end / B start) */
    double x2, y2;        /* wire end / input tab (B end / C start) */
    double tx, ty;        /* target node centre   (C end) */
    double la, lb, lc;    /* segment lengths */
    double total;
} PnPulsePath;

/* Build the travel path for @wire's light.  Returns FALSE for wires off
 * this sheet or with a missing endpoint.  The bezier (segment B) uses the
 * real wire endpoints, so the light tracks the painted wire exactly; only
 * the straight lead-in/lead-out segments reach the node centres. */
static gboolean
wire_pulse_path (
        PnWorksheet *self,
        PnWire      *wire,
        PnPulsePath *pp)
{
    PnNode        *src, *dst;
    const PnPoint *ps, *pt;
    double         sw, sh, shh, dw, dh, dhh, dx;
    double         c1x, c2x;

    if (!wire_endpoints (self, wire, &pp->x1, &pp->y1, &pp->x2, &pp->y2))
        return FALSE;

    src = pn_wire_get_source (wire);
    dst = pn_wire_get_target (wire);
    ps  = pn_node_get_position (src);
    pt  = pn_node_get_position (dst);
    pn_node_get_size (src, &sw, &sh);
    pn_node_get_size (dst, &dw, &dh);
    shh = pn_node_get_header_height (src);
    dhh = pn_node_get_header_height (dst);
    (void) sh; (void) dh;

    /* Node centres (header centre, matching the wire's vertical anchor). */
    pp->sx = ps->x + sw / 2.0;
    pp->sy = ps->y + shh / 2.0;
    pp->tx = pt->x + dw / 2.0;
    pp->ty = pt->y + dhh / 2.0;

    pp->la = hypot (pp->x1 - pp->sx, pp->y1 - pp->sy);
    pp->lc = hypot (pp->tx - pp->x2, pp->ty - pp->y2);

    /* Approximate the bezier length by its control polygon (cheap, and a
     * little long, which only makes the curved middle a touch slower).
     * Same offset rule as wire_point_at, including the no-overshoot cap. */
    dx  = fabs (pp->x2 - pp->x1) * 0.5;
    if (dx < 30.0) dx = 30.0;
    {
        double half = 0.5 * hypot (pp->x2 - pp->x1, pp->y2 - pp->y1);
        if (dx > half) dx = half;
    }
    c1x = pp->x1 + dx;
    c2x = pp->x2 - dx;
    pp->lb = hypot (c1x - pp->x1, 0.0)
           + hypot (c2x - c1x, pp->y2 - pp->y1)
           + hypot (pp->x2 - c2x, 0.0);

    pp->total = pp->la + pp->lb + pp->lc;
    if (pp->total < 1.0)
        pp->total = 1.0;
    return TRUE;
}

/* Point at parameter @t in [0,1] along the cubic bezier the light rides.
 * Same control-point rule as draw_wire (offset 30 px minimum), EXCEPT the
 * offset is capped at half the endpoint distance so the control points
 * never overshoot the ends.  Without that cap a wire shorter than the
 * offset folds back on itself, which the static stroke hides but a moving
 * light exposes as a forward-back-forward stutter.  The cap only changes
 * very short wires (where the drawn fold is invisible anyway); on long or
 * vertical wires it leaves the curve identical to the painted wire. */
static void
wire_point_at (
        double  x1,
        double  y1,
        double  x2,
        double  y2,
        double  t,
        double *ox,
        double *oy)
{
    double dx   = fabs (x2 - x1) * 0.5;
    double half = 0.5 * hypot (x2 - x1, y2 - y1);
    double u, uu, tt, b0, b1, b2, b3;

    if (dx < 30.0)
        dx = 30.0;
    if (dx > half)
        dx = half;

    u  = 1.0 - t;
    uu = u * u;
    tt = t * t;
    b0 = uu * u;
    b1 = 3.0 * uu * t;
    b2 = 3.0 * u  * tt;
    b3 = tt * t;

    *ox = b0 * x1 + b1 * (x1 + dx) + b2 * (x2 - dx) + b3 * x2;
    *oy = b0 * y1 + b1 *  y1       + b2 *  y2        + b3 * y2;
}

/* Point at fraction @frac in [0,1] along the three-segment pulse path.
 * @step quantises the travelled distance so the light advances in
 * discrete jumps (worksheet units; 0 disables) — chunky on purpose, to
 * keep the redraw count down. */
static void
wire_pulse_path_point (
        const PnPulsePath *pp,
        double             frac,
        double             step,
        double            *ox,
        double            *oy)
{
    double d = frac * pp->total;
    double t;

    if (step > 0.0)
        d = floor (d / step + 0.5) * step;
    if (d < 0.0)        d = 0.0;
    if (d > pp->total)  d = pp->total;

    if (d <= pp->la)                       /* segment A: centre → wire start */
    {
        t   = pp->la > 0.0 ? d / pp->la : 1.0;
        *ox = pp->sx + (pp->x1 - pp->sx) * t;
        *oy = pp->sy + (pp->y1 - pp->sy) * t;
    }
    else if (d <= pp->la + pp->lb)         /* segment B: the wire bezier */
    {
        t = pp->lb > 0.0 ? (d - pp->la) / pp->lb : 1.0;
        wire_point_at (pp->x1, pp->y1, pp->x2, pp->y2, t, ox, oy);
    }
    else                                   /* segment C: wire end → centre */
    {
        t   = pp->lc > 0.0 ? (d - pp->la - pp->lb) / pp->lc : 1.0;
        *ox = pp->x2 + (pp->tx - pp->x2) * t;
        *oy = pp->y2 + (pp->ty - pp->y2) * t;
    }
}

/* Plain-double HSV→RGB (h, s, v and outputs all in [0,1]); GTK offers
 * no helper for bare doubles.  @h wraps, so any real hue is valid. */
static void
hsv_to_rgb (
        double  h,
        double  s,
        double  v,
        double *r,
        double *g,
        double *b)
{
    double hh = (h - floor (h)) * 6.0;
    int    i  = (int) hh;
    double f  = hh - i;
    double p  = v * (1.0 - s);
    double q  = v * (1.0 - s * f);
    double t  = v * (1.0 - s * (1.0 - f));

    switch (i)
    {
    case 0:  *r = v; *g = t; *b = p; break;
    case 1:  *r = q; *g = v; *b = p; break;
    case 2:  *r = p; *g = v; *b = t; break;
    case 3:  *r = p; *g = q; *b = v; break;
    case 4:  *r = t; *g = p; *b = v; break;
    default: *r = v; *g = p; *b = q; break;
    }
}

/* Map a message id to a stable hue in [0,1) via FNV-1a.  Returns -1 as
 * a "no id → neutral colour" sentinel. */
static double
hue_from_id (const gchar *id)
{
    guint32 h = 2166136261u;

    if (id == NULL || *id == '\0')
        return -1.0;

    for (; *id != '\0'; id++)
    {
        h ^= (guchar) *id;
        h *= 16777619u;
    }
    return (double) (h % 3600u) / 3600.0;
}

static gboolean on_wire_pulse_timeout (gpointer user_data);

/* Bounding box, in widget (canvas) coords, of every light currently
 * visible, padded by the glow + diffraction-spike reach.  Returns FALSE
 * when nothing is visible.  Mirrors draw_wire_pulses' visibility test. */
static gboolean
wire_pulses_bbox (
        PnWorksheet *self,
        gint64       now_us,
        double      *ox0,
        double      *oy0,
        double      *ox1,
        double      *oy1)
{
    const double pad = 13.0 * 3.0 + 3.0;   /* r_spike + margin (model) */
    gboolean     any = FALSE;
    double       x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    guint        i;

    if (self->wire_pulses == NULL)
        return FALSE;

    for (i = 0; i < self->wire_pulses->len; i++)
    {
        PnWirePulse *p = &g_array_index (self->wire_pulses, PnWirePulse, i);
        PnPulsePath  pp;
        double       px, py, frac;

        frac = (double) (now_us - p->start_us) / (double) p->duration_us;
        if (frac < 0.0 || frac > 1.0)
            continue;
        if (!wire_pulse_path (self, p->wire, &pp))
            continue;
        wire_pulse_path_point (&pp, frac,
                               PN_WORKSHEET_WIRE_PULSE_STEP_PX / self->zoom,
                               &px, &py);

        if (!any)
        {
            x0 = x1 = px;
            y0 = y1 = py;
            any = TRUE;
        }
        else
        {
            if (px < x0) x0 = px;
            if (px > x1) x1 = px;
            if (py < y0) y0 = py;
            if (py > y1) y1 = py;
        }
    }
    if (!any)
        return FALSE;

    *ox0 = (x0 - pad) * self->zoom;
    *oy0 = (y0 - pad) * self->zoom;
    *ox1 = (x1 + pad) * self->zoom;
    *oy1 = (y1 + pad) * self->zoom;
    return TRUE;
}

/* Invalidate just the region around the lights — the union of where they
 * are now and where they were on the last repaint — instead of the whole
 * widget, so the partial-redraw culling in pn_worksheet_draw applies. */
static void
wire_pulses_invalidate (
        PnWorksheet *self,
        gint64       now_us)
{
    double   cx0, cy0, cx1, cy1;
    double   ux0, uy0, ux1, uy1;
    gboolean have_cur  = wire_pulses_bbox (self, now_us,
                                           &cx0, &cy0, &cx1, &cy1);
    gboolean have_prev = self->wire_pulse_dirty_valid;

    if (!have_cur && !have_prev)
        return;

    /* If the (quantised) positions have not moved since the last repaint,
     * there is nothing new to show — skip the redraw entirely.  This is
     * what turns the step size into a CPU saving: between step boundaries
     * the timer fires but does no work. */
    if (have_cur && have_prev
        && fabs (cx0 - self->wire_pulse_dirty_x0) < 0.5
        && fabs (cy0 - self->wire_pulse_dirty_y0) < 0.5
        && fabs (cx1 - self->wire_pulse_dirty_x1) < 0.5
        && fabs (cy1 - self->wire_pulse_dirty_y1) < 0.5)
        return;

    if (have_cur && have_prev)
    {
        ux0 = MIN (cx0, self->wire_pulse_dirty_x0);
        uy0 = MIN (cy0, self->wire_pulse_dirty_y0);
        ux1 = MAX (cx1, self->wire_pulse_dirty_x1);
        uy1 = MAX (cy1, self->wire_pulse_dirty_y1);
    }
    else if (have_cur)
    {
        ux0 = cx0; uy0 = cy0; ux1 = cx1; uy1 = cy1;
    }
    else
    {
        ux0 = self->wire_pulse_dirty_x0; uy0 = self->wire_pulse_dirty_y0;
        ux1 = self->wire_pulse_dirty_x1; uy1 = self->wire_pulse_dirty_y1;
    }

    gtk_widget_queue_draw_area (GTK_WIDGET (self),
                                (int) floor (ux0), (int) floor (uy0),
                                (int) ceil (ux1 - ux0) + 1,
                                (int) ceil (uy1 - uy0) + 1);

    if (have_cur)
    {
        self->wire_pulse_dirty_x0 = cx0; self->wire_pulse_dirty_y0 = cy0;
        self->wire_pulse_dirty_x1 = cx1; self->wire_pulse_dirty_y1 = cy1;
        self->wire_pulse_dirty_valid = TRUE;
    }
    else
        self->wire_pulse_dirty_valid = FALSE;
}

/* Spawn a light on @wire for @message, enforcing the capacity caps and
 * (re)arming the per-frame tick.  @depth is the message-dispatch hop
 * index of this crossing (0 for the first hop): it staggers the light's
 * start by the accumulated travel time of the preceding hops so a single
 * message animates one wire at a time along its path instead of lighting
 * every wire at once.  Sibling hops (a fan-out) share a depth and so
 * start together, which is what we want. */
static void
wire_pulse_spawn (
        PnWorksheet *self,
        PnWire      *wire,
        PnMessage   *message,
        gint         depth)
{
    gint64         now_us, start_us, start_delay = 0;
    double         speed;
    gint64         duration_us;
    guint          i, same = 0;
    double         hue;
    PnPulsePath    pp;
    PnWirePulse    pulse;

    if (self->wire_pulses == NULL)
        return;
    if (!wire_pulse_path (self, wire, &pp))
        return;

    speed = (double) pn_preferences_get_wire_pulse_speed (
            pn_preferences_get_default ());
    if (speed < 1.0)
        speed = 1.0;

    /* Duration covers the whole path (lead-in + wire + lead-out). */
    duration_us = (gint64) (pp.total / speed * 1e6);
    if (duration_us < PN_WORKSHEET_WIRE_PULSE_MIN_US)
        duration_us = PN_WORKSHEET_WIRE_PULSE_MIN_US;

    /* Monotonic time throughout (the timer and painter use the same),
     * so positions are consistent without the frame clock. */
    now_us = g_get_monotonic_time ();
    hue    = hue_from_id (pn_message_get_id (message));

    /* Stagger: this hop begins when the previous one finished.  The
     * parent hop wrote wire_hop_end[depth-1] earlier in the same
     * synchronous cascade (delivery is depth-first, emission pre-order),
     * so it is the time, relative to now, at which the message reaches
     * this wire's source. */
    if (depth < 0)
        depth = 0;
    else if (depth >= PN_WORKSHEET_WIRE_PULSE_DEPTH)
        depth = PN_WORKSHEET_WIRE_PULSE_DEPTH - 1;
    if (depth > 0)
        start_delay = self->wire_hop_end[depth - 1];
    self->wire_hop_end[depth] = start_delay + duration_us;
    start_us = now_us + start_delay;

    /* Capacity caps drop the NEW spawn rather than touching pulses already
     * in flight, so every light that appears is guaranteed to reach its
     * target (never dropped or reset half-way).  Under heavy traffic some
     * messages simply do not get a light — a bounded, complete train. */
    for (i = 0; i < self->wire_pulses->len; i++)
    {
        PnWirePulse *p = &g_array_index (self->wire_pulses, PnWirePulse, i);
        if (p->wire == wire)
            same++;
    }
    if (same >= PN_WORKSHEET_WIRE_PULSE_PER_WIRE)
        return;
    if (self->wire_pulses->len >= PN_WORKSHEET_WIRE_PULSE_MAX)
        return;

    pulse.wire        = g_object_ref (wire);
    pulse.start_us    = start_us;
    pulse.duration_us = duration_us;
    pulse.hue         = hue;
    g_array_append_val (self->wire_pulses, pulse);

    if (self->wire_pulse_timer_id == 0)
        self->wire_pulse_timer_id = g_timeout_add (
                pn_preferences_get_wire_pulse_interval (
                        pn_preferences_get_default ()),
                on_wire_pulse_timeout, self);

    /* No immediate redraw here: the timer repaints at its own coarse rate,
     * so a burst of messages cannot drive the redraw rate (and the CPU)
     * up.  A freshly spawned light shows within one timer step. */
}

/* Per-frame tick: prune finished lights (and any whose wire vanished),
 * repaint at the throttled rate, and tear itself down once none remain. */
static gboolean
on_wire_pulse_timeout (
        gpointer user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    gint64       now_us;
    guint        i;

    now_us = g_get_monotonic_time ();

    for (i = self->wire_pulses->len; i > 0; i--)
    {
        PnWirePulse *p = &g_array_index (self->wire_pulses, PnWirePulse, i - 1);
        PnPulsePath  pp;
        gboolean     dead = (now_us - p->start_us) >= p->duration_us;

        if (!dead && !wire_pulse_path (self, p->wire, &pp))
            dead = TRUE;
        if (dead)
            g_array_remove_index (self->wire_pulses, i - 1);
    }

    /* Repaint just the lights' region (erasing where they were). */
    wire_pulses_invalidate (self, now_us);

    if (self->wire_pulses->len == 0)
    {
        self->wire_pulse_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* Drop every in-flight light and stop the timer.  Used when the feature
 * is switched off mid-flight so the lights vanish at once. */
static void
wire_pulses_clear (PnWorksheet *self)
{
    if (self->wire_pulses != NULL && self->wire_pulses->len > 0)
        g_array_set_size (self->wire_pulses, 0);
    if (self->wire_pulse_timer_id != 0)
    {
        g_source_remove (self->wire_pulse_timer_id);
        self->wire_pulse_timer_id = 0;
    }
    /* Caller follows up with a full queue_draw to erase, so the stale
     * dirty box is no longer needed. */
    self->wire_pulse_dirty_valid = FALSE;
}

/* React to the "animate-messages-on-wires" preference flipping: when it
 * goes off, clear any lights mid-flight so they vanish at once. */
static void
on_prefs_anim_changed (
        GObject    *prefs,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) pspec;

    if (!pn_preferences_get_animate_wire_messages (PN_PREFERENCES (prefs)))
        wire_pulses_clear (self);

    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* React to the redraw-interval preference changing: if the animation is
 * running, re-arm the timer at the new interval so the change takes
 * effect immediately (rather than only on the next animation). */
static void
on_prefs_anim_interval_changed (
        GObject    *prefs,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) pspec;

    if (self->wire_pulse_timer_id != 0)
    {
        g_source_remove (self->wire_pulse_timer_id);
        self->wire_pulse_timer_id = g_timeout_add (
                pn_preferences_get_wire_pulse_interval (
                        PN_PREFERENCES (prefs)),
                on_wire_pulse_timeout, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Node processing-activity glow (TODO #42)                           */
/* ------------------------------------------------------------------ */

/* Invalidate a node's footprint padded for the halo bleed, in widget
 * coordinates (model * zoom).  Used both to paint a fresh glow in and to
 * erase it once the node goes idle. */
static void
processing_invalidate_node (
        PnWorksheet *self,
        PnNode      *node)
{
    const PnPoint *p   = pn_node_get_position (node);
    const double   pad = PN_WORKSHEET_HALO_SPREAD + 2.0;
    double         w, h, x0, y0, x1, y1;

    pn_node_get_size (node, &w, &h);
    x0 = (p->x - pad)     * self->zoom;
    y0 = (p->y - pad)     * self->zoom;
    x1 = (p->x + w + pad) * self->zoom;
    y1 = (p->y + h + pad) * self->zoom;
    gtk_widget_queue_draw_area (GTK_WIDGET (self),
                                (int) floor (x0), (int) floor (y0),
                                (int) ceil (x1 - x0) + 1,
                                (int) ceil (y1 - y0) + 1);
}

/* Poll tick: repaint just the nodes whose visible-busy state changed since
 * the last tick — so a glow appears and, after the minimum-visible linger,
 * erases — and tear the timer down once nothing is busy (zero idle CPU).
 * The per-node "currently lit" bit rides on the node as object-data, so
 * the worksheet holds no node pointers that could dangle if a node is
 * deleted mid-glow. */
static gboolean
on_processing_timeout (
        gpointer user_data)
{
    PnWorksheet *self  = PN_WORKSHEET (user_data);
    const gint64 now   = g_get_monotonic_time ();
    const guint  count = pn_node_store_get_length (self->nodes);
    guint        busy  = 0;
    guint        i;

    for (i = 0; i < count; i++)
    {
        PnNode  *node = pn_node_store_get_node (self->nodes, i);
        gboolean vis, was;

        if (!node_on_sheet (self, node))
            continue;

        vis = pn_node_is_processing_visible (node, now);
        was = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (node),
                                                  PN_WORKSHEET_GLOW_KEY));
        if (vis != was)
        {
            processing_invalidate_node (self, node);
            g_object_set_data (G_OBJECT (node), PN_WORKSHEET_GLOW_KEY,
                               GINT_TO_POINTER (vis));
        }
        if (vis)
            busy++;
    }

    if (busy == 0)
    {
        self->processing_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

/* A node crossed a processing edge ("processing-changed").  Wake the poll
 * and, on the rising edge, light the glow at once rather than waiting up to
 * one poll step.  Costs nothing while the preference is off: the node still
 * emits the signal, but we neither arm the timer nor repaint. */
static void
on_node_processing (
        PnNode  *node,
        gboolean busy,
        gpointer user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);

    if (!pn_preferences_get_visualize_node_processing (
                pn_preferences_get_default ()))
        return;
    if (!node_on_sheet (self, node))
        return;

    if (self->processing_timer_id == 0)
        self->processing_timer_id = g_timeout_add (
                PN_WORKSHEET_PROCESSING_POLL_MS, on_processing_timeout, self);

    if (busy)
    {
        processing_invalidate_node (self, node);
        g_object_set_data (G_OBJECT (node), PN_WORKSHEET_GLOW_KEY,
                           GINT_TO_POINTER (TRUE));
    }
}

/* React to the "visualize-node-processing" preference flipping off: stop
 * the poll and repaint so any lit halos vanish at once. */
static void
on_prefs_visualize_changed (
        GObject    *prefs,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) pspec;

    if (!pn_preferences_get_visualize_node_processing (PN_PREFERENCES (prefs)))
    {
        if (self->processing_timer_id != 0)
        {
            g_source_remove (self->processing_timer_id);
            self->processing_timer_id = 0;
        }
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

/* Paint every live light, inside the worksheet (zoom/pan) transform. */
static void
draw_wire_pulses (
        PnWorksheet *self,
        cairo_t     *cr)
{
    gint64 now_us;
    guint  i;

    if (self->wire_pulses == NULL || self->wire_pulses->len == 0)
        return;

    /* Same monotonic clock the spawn and timer use, so the painted
     * position matches the invalidated region. */
    now_us = g_get_monotonic_time ();

    for (i = 0; i < self->wire_pulses->len; i++)
    {
        PnWirePulse     *p = &g_array_index (self->wire_pulses, PnWirePulse, i);
        PnPulsePath      pp;
        double           px, py;
        double           frac, alpha;
        double           r = 1.0, g = 0.97, b = 0.85;   /* neutral default */
        cairo_pattern_t *halo;

        frac = (double) (now_us - p->start_us) / (double) p->duration_us;
        if (frac < 0.0)
            continue;   /* staggered start still in the future */
        if (frac > 1.0)
            continue;   /* finished; pruned on the next tick */

        if (!wire_pulse_path (self, p->wire, &pp))
            continue;

        wire_pulse_path_point (&pp, frac,
                               PN_WORKSHEET_WIRE_PULSE_STEP_PX / self->zoom,
                               &px, &py);

        if (p->hue >= 0.0)
            hsv_to_rgb (p->hue, 0.85, 1.0, &r, &g, &b);

        /* No fade: the light is on screen only briefly, so a constant
         * full brightness reads better than an ease in/out that would
         * mostly hide it. */
        alpha = 1.0;

        {
            const double r_halo  = 13.0;  /* soft glow radius            */
            const double r_core  =  3.4;  /* bright centre               */
            const double r_spike = r_halo * 3.0;  /* diffraction reach    */
            static const double dir[4][2] =
                { { 1.0, 0.0 }, { -1.0, 0.0 }, { 0.0, 1.0 }, { 0.0, -1.0 } };
            int k;

            /* Soft halo. */
            halo = cairo_pattern_create_radial (px, py, 0.0, px, py, r_halo);
            cairo_pattern_add_color_stop_rgba (halo, 0.0,  r, g, b, 0.85 * alpha);
            cairo_pattern_add_color_stop_rgba (halo, 0.35, r, g, b, 0.40 * alpha);
            cairo_pattern_add_color_stop_rgba (halo, 1.0,  r, g, b, 0.0);
            cairo_set_source (cr, halo);
            cairo_arc (cr, px, py, r_halo, 0.0, 2.0 * G_PI);
            cairo_fill (cr);
            cairo_pattern_destroy (halo);

            /* Diffraction spikes: four thin rays that fade out, giving
             * the light the look of a bright star seen through a
             * telescope. */
            cairo_set_line_width (cr, 1.1);
            cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
            for (k = 0; k < 4; k++)
            {
                double           ex = px + dir[k][0] * r_spike;
                double           ey = py + dir[k][1] * r_spike;
                cairo_pattern_t *sp =
                        cairo_pattern_create_linear (px, py, ex, ey);

                cairo_pattern_add_color_stop_rgba (sp, 0.0, r, g, b,
                                                   0.55 * alpha);
                cairo_pattern_add_color_stop_rgba (sp, 1.0, r, g, b, 0.0);
                cairo_set_source (cr, sp);
                cairo_move_to (cr, px, py);
                cairo_line_to (cr, ex, ey);
                cairo_stroke (cr);
                cairo_pattern_destroy (sp);
            }

            /* Pure-white core; the colour is left to the halo and the
             * diffraction spikes to hint at. */
            cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, alpha);
            cairo_arc (cr, px, py, r_core, 0.0, 2.0 * G_PI);
            cairo_fill (cr);
        }
    }

    /* Fold the region we just painted into the pulse dirty box, so the
     * next wire_pulses_invalidate() erases it regardless of what
     * triggered this draw.  Without this, a draw kicked off by an
     * unrelated partial repaint — e.g. a Knob node redrawing its own
     * rectangle as the wheel turns it — stamps an in-flight light on top
     * of the node, but the pulse-erase machinery (driven solely by the
     * timer) never learns about it: it follows the light out along the
     * wire and erases where the light now is, leaving the stamped star
     * stranded inside the node rectangle.  The timer ticks for as long as
     * any light is alive (and once more after), so a region recorded here
     * is always erased. */
    {
        double bx0, by0, bx1, by1;

        if (wire_pulses_bbox (self, now_us, &bx0, &by0, &bx1, &by1))
        {
            if (self->wire_pulse_dirty_valid)
            {
                self->wire_pulse_dirty_x0 = MIN (self->wire_pulse_dirty_x0, bx0);
                self->wire_pulse_dirty_y0 = MIN (self->wire_pulse_dirty_y0, by0);
                self->wire_pulse_dirty_x1 = MAX (self->wire_pulse_dirty_x1, bx1);
                self->wire_pulse_dirty_y1 = MAX (self->wire_pulse_dirty_y1, by1);
            }
            else
            {
                self->wire_pulse_dirty_x0 = bx0; self->wire_pulse_dirty_y0 = by0;
                self->wire_pulse_dirty_x1 = bx1; self->wire_pulse_dirty_y1 = by1;
                self->wire_pulse_dirty_valid = TRUE;
            }
        }
    }
}

/** Center @node in the enclosing #GtkScrolledWindow's viewport, when
 *  one exists.  Worksheets are always packed inside a scrolled
 *  window today; the NULL guard is purely defensive so this stays
 *  safe in tests where the widget might not have been realised
 *  inside a scroller. */
static void
scroll_to_center_node (
        PnWorksheet *self,
        PnNode      *node)
{
    GtkWidget       *anc;
    GtkAdjustment   *hadj;
    GtkAdjustment   *vadj;
    const PnPoint   *p;
    double           nw, nh;
    double           hview, vview;
    double           cx, cy;

    anc = gtk_widget_get_ancestor (GTK_WIDGET (self),
                                   GTK_TYPE_SCROLLED_WINDOW);
    if (anc == NULL)
        return;

    hadj = gtk_scrolled_window_get_hadjustment (GTK_SCROLLED_WINDOW (anc));
    vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (anc));
    if (hadj == NULL || vadj == NULL)
        return;

    p = pn_node_get_position (node);
    pn_node_get_size (node, &nw, &nh);

    /* Adjustments live in widget coords; positions and sizes are in
     * worksheet coords.  Multiply by the current zoom to bring them
     * into the same space. */
    cx = (p->x + nw / 2.0) * self->zoom;
    cy = (p->y + nh / 2.0) * self->zoom;

    hview = gtk_adjustment_get_page_size (hadj);
    vview = gtk_adjustment_get_page_size (vadj);

    gtk_adjustment_set_value (hadj, cx - hview / 2.0);
    gtk_adjustment_set_value (vadj, cy - vview / 2.0);
}

/** Locate the node carrying @uuid via a linear scan of the store
 *  (flows have tens of nodes — a hash index would be premature).
 *  Returns %NULL when no node matches. */
static PnNode *
find_node_by_uuid (
        PnWorksheet *self,
        const gchar *uuid)
{
    const guint count = pn_node_store_get_length (self->nodes);
    guint       i;

    for (i = 0; i < count; i++)
    {
        PnNode      *node = pn_node_store_get_node (self->nodes, i);
        const gchar *u    = pn_node_get_uuid (node);

        if (!node_on_sheet (self, node))
            continue;
        if (u != NULL && g_strcmp0 (u, uuid) == 0)
            return node;
    }
    return NULL;
}

void
pn_worksheet_focus_node_by_uuid (
        PnWorksheet *self,
        const gchar *uuid)
{
    PnNode        *node;
    GdkFrameClock *clock;

    g_return_if_fail (PN_IS_WORKSHEET (self));

    if (uuid == NULL || *uuid == '\0')
        return;

    node = find_node_by_uuid (self, uuid);
    if (node == NULL)
        return;

    scroll_to_center_node (self, node);

    /* Re-arm the pulse: setting pulse_node and stamping the start
     * time even if a previous pulse is still in flight, so a quick
     * sequence of clicks each restart from full intensity rather
     * than fading together. */
    self->pulse_node = node;
    clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
    self->pulse_start_us = clock != NULL
                              ? gdk_frame_clock_get_frame_time (clock)
                              : g_get_monotonic_time ();

    if (self->pulse_tick_id == 0)
        self->pulse_tick_id = gtk_widget_add_tick_callback (
                GTK_WIDGET (self), on_pulse_tick, NULL, NULL);

    gtk_widget_queue_draw (GTK_WIDGET (self));
}

const gchar *
pn_worksheet_get_focus_pulse_uuid (PnWorksheet *self)
{
    g_return_val_if_fail (PN_IS_WORKSHEET (self), NULL);
    if (self->pulse_node == NULL)
        return NULL;
    return pn_node_get_uuid (self->pulse_node);
}

/** Return the topmost node whose plot extension contains (@x, @y),
 *  or %NULL.  Iterates front-to-back so a node stacked on top of
 *  another wins, mirroring #hit_test_node's order.  A node is
 *  considered to have a plot extension exactly when its class
 *  exposes a #PnNodeClass.paint_plot vfunc. */
static PnNode *
hit_test_node_plot (
        PnWorksheet *self,
        double       x,
        double       y)
{
    const guint count = pn_node_store_get_length (self->nodes);
    gint        i;

    for (i = (gint) count - 1; i >= 0; i--)
    {
        PnNode      *node  = pn_node_store_get_node (self->nodes, (guint) i);
        PnNodeClass *klass = PN_NODE_GET_CLASS (node);
        double       px, py, pw, ph;

        if (klass->paint_plot == NULL)
            continue;
        /* Opt-out: meter-style nodes whose at-rest footprint is
         * already the user-facing reading surface (#PnAnalogMeter
         * and its subclasses) pin this so a press selects / drags
         * the node like any other header-only entry instead of
         * lifting it into the centred zoom overlay. */
        if (klass->paint_plot_skip_zoom)
            continue;
        if (!node_on_sheet (self, node))
            continue;

        node_plot_rect (node, &px, &py, &pw, &ph);

        if (x >= px && x < px + pw &&
            y >= py && y < py + ph)
            return node;
    }

    return NULL;
}

/** Front-to-back search for the #PnFileDrop whose full footprint
 *  (header + drop area) contains (@x, @y), or %NULL when the point
 *  lands on no file-drop node.  Unlike hit_test_node (header only)
 *  this tests the whole rectangle so a file can be dropped anywhere on
 *  the node, including its drop area. */
static PnNode *
hit_test_filedrop (
        PnWorksheet *self,
        double       x,
        double       y)
{
    const guint count = pn_node_store_get_length (self->nodes);
    gint        i;

    for (i = (gint) count - 1; i >= 0; i--)
    {
        PnNode        *node = pn_node_store_get_node (self->nodes, (guint) i);
        const PnPoint *p;
        double         w, h;

        if (!PN_IS_FILEDROP (node))
            continue;
        if (!node_on_sheet (self, node))
            continue;

        p = pn_node_get_position (node);
        pn_node_get_size (node, &w, &h);

        if (x >= p->x && x < p->x + w &&
            y >= p->y && y < p->y + h)
            return node;
    }

    return NULL;
}

/** %TRUE when (@x, @y) lands inside the currently-displayed zoom
 *  rectangle.  The hit-test runs against the live (animated) rect so
 *  clicks register correctly even mid-flight. */
static gboolean
hit_test_zoom_rect (
        PnWorksheet *self,
        double       x,
        double       y)
{
    double zx, zy, zw, zh;

    if (self->zoomed_node == NULL)
        return FALSE;

    zoom_current_rect (self, &zx, &zy, &zw, &zh);

    return x >= zx && x < zx + zw &&
           y >= zy && y < zy + zh;
}

/** Paint the transient "125%" magnification readout that follows a
 *  Ctrl+wheel zoom step.  Drawn in widget coordinates (the caller has
 *  already restored the cairo CTM) and centred horizontally near the
 *  top of the viewport so the label does not occlude the cursor
 *  hot-spot where the user's attention is during the gesture.  Holds
 *  at full opacity for #PN_WORKSHEET_ZOOM_LABEL_HOLD_US, then fades
 *  linearly across #PN_WORKSHEET_ZOOM_LABEL_FADE_US; the tick
 *  callback queues redraws so the fade is animated, and tears itself
 *  down once the window has elapsed. */
static void
draw_zoom_label (
        PnWorksheet *self,
        cairo_t     *cr,
        int          width,
        int          height)
{
    GdkFrameClock        *clock;
    gint64                now_us;
    gint64                elapsed;
    double                alpha = 1.0;
    PangoLayout          *layout;
    PangoFontDescription *desc;
    char                  text[32];
    int                   pw, ph;
    const double          pad_x  = 12.0;
    const double          pad_y  = 6.0;
    const double          radius = 4.0;
    double                bw, bh, bx, by;

    (void) height;

    if (self->zoom_label_tick_id == 0 && self->zoom_label_shown_us == 0)
        return;

    clock  = gtk_widget_get_frame_clock (GTK_WIDGET (self));
    now_us = clock != NULL ? gdk_frame_clock_get_frame_time (clock)
                           : g_get_monotonic_time ();
    elapsed = now_us - self->zoom_label_shown_us;

    if (elapsed >= PN_WORKSHEET_ZOOM_LABEL_HOLD_US
                 + PN_WORKSHEET_ZOOM_LABEL_FADE_US)
        return;
    if (elapsed > PN_WORKSHEET_ZOOM_LABEL_HOLD_US)
        alpha = 1.0
              - (double) (elapsed - PN_WORKSHEET_ZOOM_LABEL_HOLD_US)
              / (double) PN_WORKSHEET_ZOOM_LABEL_FADE_US;

    g_snprintf (text, sizeof text, "%d%%",
                (int) lround (self->zoom * 100.0));

    layout = pango_cairo_create_layout (cr);
    desc   = pango_font_description_from_string ("Sans Bold");
    pango_font_description_set_absolute_size (desc, 14.0 * PANGO_SCALE);
    pango_layout_set_font_description (layout, desc);
    pango_font_description_free (desc);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    bw = pw + 2.0 * pad_x;
    bh = ph + 2.0 * pad_y;
    bx = (width - bw) / 2.0;
    by = 18.0;

    /* White fill with a thin black frame.  Stroke offset by 0.5 so
     * the 1-px border lands on a pixel boundary and reads sharp. */
    rounded_rect_path (cr, bx, by, bw, bh, radius);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, alpha);
    cairo_fill_preserve (cr);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, alpha);
    cairo_set_line_width  (cr, 1.0);
    cairo_stroke (cr);

    cairo_move_to (cr, bx + pad_x, by + pad_y);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, alpha);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
}

/** Paint the dim backdrop and lifted plot when a node zoom is in
 *  progress.  Called as the final pass of #pn_worksheet_draw so the
 *  overlay sits cleanly above the canvas, the wires, and the node
 *  bodies.  No-op when no node is zoomed. */
static void
draw_zoom_overlay (
        PnWorksheet *self,
        cairo_t     *cr,
        int          width,
        int          height)
{
    double zx, zy, zw, zh;
    double a;

    if (self->zoomed_node == NULL)
        return;

    a = zoom_ease (self->zoom_t);

    /* Dim the canvas underneath proportional to the animation
     * progress, so the lifted plot reads as a modal foreground.
     * Capped at 0.35 alpha — heavy enough to recede the canvas, light
     * enough that the user can still see what they came from. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.35 * a);
    cairo_rectangle (cr, 0, 0, width, height);
    cairo_fill (cr);

    zoom_current_rect (self, &zx, &zy, &zw, &zh);

    paint_drop_shadow (cr, zx, zy, zw, zh, 0.0);

    /* Tell a #PnOscilloscope it is being painted maximized so it draws its
     * knob panel; clear it again immediately so the at-rest in-canvas paint
     * (and any other reader) sees the bare CRT.  This is the ONLY place a
     * zoomed node's plot is painted, so bracketing here is sufficient — and
     * the setter is repaint-free, so it cannot loop the frame clock. */
    if (PN_IS_OSCILLOSCOPE (self->zoomed_node))
    {
        PnOscilloscope *osc = PN_OSCILLOSCOPE (self->zoomed_node);
        pn_oscilloscope_set_maximized (osc, TRUE);
        PN_NODE_GET_CLASS (self->zoomed_node)->paint_plot (
                self->zoomed_node, cr, zx, zy, zw, zh);
        pn_oscilloscope_set_maximized (osc, FALSE);
    }
    else
    {
        PN_NODE_GET_CLASS (self->zoomed_node)->paint_plot (
                self->zoomed_node, cr, zx, zy, zw, zh);
    }
}

/** Paint the worksheet background, a faint reference grid, and a
 *  static demo of two connected Node-RED-style nodes. */
static gboolean
pn_worksheet_draw (
        GtkWidget *widget,
        cairo_t   *cr)
{
    PnWorksheet   *self  = PN_WORKSHEET (widget);
    PnPreferences *prefs = pn_preferences_get_default ();
    int width  = gtk_widget_get_allocated_width (widget);
    int height = gtk_widget_get_allocated_height (widget);
    const double grid_step = PN_GRID_STEP * self->zoom;
    const PnGridType grid_kind = pn_preferences_get_grid_type (prefs);
    GdkRGBA      grid_color;
    double x;
    double y;
    double dmg_x0, dmg_y0, dmg_x1, dmg_y1;

    /* Damaged region in widget coords (cairo's current clip).  The whole
     * background+grid is restricted to it so a small partial redraw — e.g.
     * the rectangle around a moving message-flow light — does not repaint
     * the entire grid, which (especially "dots") is the dominant cost of
     * a frame and was making the animation expensive. */
    cairo_clip_extents (cr, &dmg_x0, &dmg_y0, &dmg_x1, &dmg_y1);
    dmg_x0 = MAX (dmg_x0, 0.0);
    dmg_y0 = MAX (dmg_y0, 0.0);
    dmg_x1 = MIN (dmg_x1, (double) width);
    dmg_y1 = MIN (dmg_y1, (double) height);

    /* Solid background, colour from preferences. */
    {
        GdkRGBA bg;
        pn_preferences_get_background_color (prefs, &bg);
        cairo_set_source_rgba (cr, bg.red, bg.green, bg.blue, bg.alpha);
    }
    cairo_rectangle (cr, dmg_x0, dmg_y0, dmg_x1 - dmg_x0, dmg_y1 - dmg_y0);
    cairo_fill (cr);

    /* Reference grid.  Phase the grid by the pan offset so the
     * lines stay anchored to worksheet coordinates rather than to
     * the viewport.  The grid kind and colour come from the
     * preferences singleton; "none" is the early-out, "dots"
     * paints a 2-px round dab at each intersection and "lines"
     * keeps the historical cross-hatched look.  Only the intersections
     * inside the damaged region are emitted. */
    pn_preferences_get_grid_color (prefs, &grid_color);
    cairo_set_source_rgba (cr,
                           grid_color.red,
                           grid_color.green,
                           grid_color.blue,
                           grid_color.alpha);
    cairo_set_line_width (cr, 1.0);

    if (grid_kind != PN_GRID_TYPE_NONE && grid_step > 2.0
        && dmg_x1 > dmg_x0 && dmg_y1 > dmg_y0)
    {
        const double gx0 = floor (dmg_x0 / grid_step) * grid_step;
        const double gy0 = floor (dmg_y0 / grid_step) * grid_step;

        if (grid_kind == PN_GRID_TYPE_LINES)
        {
            for (x = gx0; x < dmg_x1; x += grid_step)
            {
                cairo_move_to (cr, x + 0.5, dmg_y0);
                cairo_line_to (cr, x + 0.5, dmg_y1);
            }
            for (y = gy0; y < dmg_y1; y += grid_step)
            {
                cairo_move_to (cr, dmg_x0, y + 0.5);
                cairo_line_to (cr, dmg_x1, y + 0.5);
            }
            cairo_stroke (cr);
        }
        else /* PN_GRID_TYPE_DOTS */
        {
            for (x = gx0; x < dmg_x1; x += grid_step)
            {
                for (y = gy0; y < dmg_y1; y += grid_step)
                {
                    cairo_arc (cr, x + 0.5, y + 0.5,
                               1.0, 0.0, 2.0 * G_PI);
                    cairo_fill (cr);
                }
            }
        }
    }

    /* Apply the view transform so all subsequent drawing — wires,
     * node bodies, the in-progress wire preview, the rubber-band
     * marquee — lives in worksheet coordinates and scales with the
     * Ctrl+wheel zoom.  Stroke widths and font sizes scale along with
     * the geometry, which is the visually-correct behaviour for a
     * canvas zoom (everything reads larger when zoomed in).  The zoom
     * overlay below is restored back to widget coords so the lifted
     * plot stays anchored to the viewport regardless of canvas zoom. */
    cairo_save (cr);
    cairo_scale (cr, self->zoom, self->zoom);

    {
        const guint wire_count = pn_wire_store_get_length (self->wires);
        const guint node_count = pn_node_store_get_length (self->nodes);
        guint       i;

        /* Clip bounds in worksheet coords (the CTM is already scaled),
         * so wires and nodes outside the damaged region — e.g. when only
         * the small rectangle around a moving light was invalidated — are
         * skipped instead of laid out and painted under the clip. */
        double clip_x0, clip_y0, clip_x1, clip_y1;
        cairo_clip_extents (cr, &clip_x0, &clip_y0, &clip_x1, &clip_y1);

        /* Wires.  Drawn before bodies so the port tabs cover their
         * endpoints.  Skip wires whose endpoints are not both set, or
         * which connect nodes on a different sheet from this one. */
        for (i = 0; i < wire_count; i++)
        {
            PnWire *wire = pn_wire_store_get_wire (self->wires, i);
            double  x1, y1, x2, y2, dx, wx0, wy0, wx1, wy1;

            /* wire_endpoints() skips wires off this sheet or with a
             * missing endpoint and lands the wire on the exact input
             * tab it feeds (so a wire to a multi-input node's second
             * port draws to that port).  The pulse renderer uses the
             * same helper, so lights track the painted curve exactly. */
            if (!wire_endpoints (self, wire, &x1, &y1, &x2, &y2))
                continue;

            /* Cull against the clip using the bezier's control hull. */
            dx  = fabs (x2 - x1) * 0.5;
            if (dx < 30.0) dx = 30.0;
            wx0 = MIN (MIN (x1, x2), MIN (x1 + dx, x2 - dx)) - 2.0;
            wx1 = MAX (MAX (x1, x2), MAX (x1 + dx, x2 - dx)) + 2.0;
            wy0 = MIN (y1, y2) - 2.0;
            wy1 = MAX (y1, y2) + 2.0;
            if (!rects_overlap (wx0, wy0, wx1, wy1,
                                clip_x0, clip_y0, clip_x1, clip_y1))
                continue;

            draw_wire (cr, x1, y1, x2, y2);
        }

        /* In-progress wire being dragged from an output port toward
         * the cursor.  Drawn alongside the committed wires so the
         * preview matches the final look. */
        if (self->wire_source != NULL)
        {
            const PnPoint *ps  = pn_node_get_position (self->wire_source);
            const double   shh = pn_node_get_header_height (self->wire_source);
            double         sw, sh;
            pn_node_get_size (self->wire_source, &sw, &sh);
            (void) sh;

            draw_wire (cr,
                       ps->x + sw,            ps->y + shh / 2.0,
                       self->wire_cursor_x,   self->wire_cursor_y);
        }

        /* In-progress wire being dragged from an input port toward the
         * cursor.  The cursor stands in for the not-yet-chosen output
         * end, so it takes draw_wire's source slot to keep the curve
         * flowing the natural left-to-right way. */
        if (self->wire_dest != NULL)
        {
            const PnPoint *pt = pn_node_get_position (self->wire_dest);

            draw_wire (cr,
                       self->wire_cursor_x, self->wire_cursor_y,
                       pt->x, input_port_y (self->wire_dest,
                                            self->wire_dest_input));
        }

        /* Bodies.  Skip nodes that live on another sheet — invisible
         * here, painted by the per-sheet widget that owns them — and
         * nodes outside the clip (their Pango layout is the dominant
         * paint cost, so skipping them is what makes a small partial
         * redraw cheap). */
        for (i = 0; i < node_count; i++)
        {
            PnNode        *node = pn_node_store_get_node (self->nodes, i);
            const PnPoint *p;
            double         nw, nh;

            if (!node_on_sheet (self, node))
                continue;

            p = pn_node_get_position (node);
            pn_node_get_size (node, &nw, &nh);
            if (!rects_overlap (p->x - 6.0, p->y - 6.0,
                                p->x + nw + 6.0, p->y + nh + 6.0,
                                clip_x0, clip_y0, clip_x1, clip_y1))
                continue;

            draw_node (cr, node, self);
        }

        /* Selection halo: thin red rounded outline traced just outside
         * each selected node's body so the highlight reads clearly
         * against the dark left panel. */
        if (g_hash_table_size (self->selection) > 0)
        {
            cairo_set_source_rgb  (cr, 0.85, 0.15, 0.15);
            cairo_set_line_width  (cr, 1.5);

            for (i = 0; i < node_count; i++)
            {
                PnNode        *node = pn_node_store_get_node (self->nodes, i);
                const PnPoint *p;

                if (!g_hash_table_contains (self->selection, node))
                    continue;
                if (!node_on_sheet (self, node))
                    continue;

                {
                    double nw, nh;
                    pn_node_get_size (node, &nw, &nh);
                    /* Halo the solid body — header plus any multi-input
                     * lower section — but not a graph node's passive plot
                     * extension, which is not framed by the selection. */
                    nh = node_body_height (node);
                    p = pn_node_get_position (node);
                    rounded_rect_path (cr,
                                       p->x - 2.0, p->y - 2.0,
                                       nw + 4.0,
                                       nh + 4.0,
                                       PN_NODE_RADIUS + 2.0);
                    cairo_stroke (cr);
                }
            }
        }

        /* Drag-preview wireframe: the red ghost of the node a palette
         * drop would create, traced on top of the existing bodies so
         * the user sees where and what they are about to drop. */
        if (self->drag_preview_node != NULL)
            draw_node_wireframe (self, cr);

        /* Message-flow lights, painted last so they sit on top of the
         * node bodies and fly into the centre of the target node.  No-op
         * unless the feature is on and pulses are in flight. */
        draw_wire_pulses (self, cr);
    }

    /* Focus pulse: a fading bright outline around the node referenced
     * by the most recent pn_worksheet_focus_node_by_uuid() call.
     * Sits inside the canvas-zoom transform so the outline moves
     * with the node when the user pans/zooms during the pulse.  The
     * alpha follows a sin curve from 0 → 1 → 0 over the configured
     * duration so the pulse reads as a deliberate flash rather than
     * a hard appear/disappear. */
    if (self->pulse_node != NULL)
    {
        GdkFrameClock *clock = gtk_widget_get_frame_clock (GTK_WIDGET (self));
        gint64 now_us = clock != NULL
                            ? gdk_frame_clock_get_frame_time (clock)
                            : g_get_monotonic_time ();
        double frac = (double) (now_us - self->pulse_start_us)
                    / (double) PN_WORKSHEET_PULSE_US;

        if (frac >= 0.0 && frac <= 1.0)
        {
            const PnPoint *p = pn_node_get_position (self->pulse_node);
            double         nw, nh;
            double         alpha = sin (frac * G_PI);

            pn_node_get_size (self->pulse_node, &nw, &nh);
            /* Frame the solid body — header plus any multi-input lower
             * section — matching the selection halo's convention, so a
             * graph node's plot extension does not inflate the pulse
             * rectangle. */
            nh = node_body_height (self->pulse_node);

            /* A bright cyan that contrasts with the red selection
             * halo (so a pulsing node stays distinguishable from a
             * selected one) and the various node body colours. */
            cairo_set_source_rgba (cr, 0.10, 0.55, 0.95, alpha);
            cairo_set_line_width  (cr, 4.0 / self->zoom);
            rounded_rect_path (cr,
                               p->x - 5.0, p->y - 5.0,
                               nw + 10.0, nh + 10.0,
                               PN_NODE_RADIUS + 4.0);
            cairo_stroke (cr);
        }
    }

    /* Rubber-band marquee: thin red rectangle over the area swept out
     * since button-press.  Stored in worksheet coords (event coords
     * are converted on press/motion) so it draws under the same
     * transform as the nodes it selects. */
    if (self->marquee_active)
    {
        double rx, ry, rw, rh;
        marquee_normalise (self, &rx, &ry, &rw, &rh);

        cairo_set_source_rgb (cr, 0.85, 0.15, 0.15);
        cairo_set_line_width (cr, 1.0 / self->zoom);
        cairo_rectangle (cr, rx, ry, rw, rh);
        cairo_stroke (cr);
    }

    /* End of the zoomed/panned canvas pass; revert the cairo CTM so
     * the overlay below paints in widget coordinates. */
    cairo_restore (cr);

    /* Magnification readout: a transient white-on-black label that
     * follows every Ctrl+wheel step, holding briefly before fading
     * out.  Drawn in widget coords (above the canvas transform) so
     * it stays anchored to the viewport. */
    draw_zoom_label (self, cr, width, height);

    /* Zoom overlay last so the lifted plot and dim backdrop sit on
     * top of every other element. */
    draw_zoom_overlay (self, cr, width, height);

    return GDK_EVENT_STOP;
}

/* ------------------------------------------------------------------ */
/*  Demo wiring                                                        */
/* ------------------------------------------------------------------ */

/** Repaint whenever a node property that the painter recolours by flips
 *  — "disabled" (grey body) or "has-error" (red body + warning glyph). */
static void
on_node_notify_appearance (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    gtk_widget_queue_draw (GTK_WIDGET (user_data));
}

/** Re-negotiate the drawing area's preferred size whenever a node
 *  moves so the enclosing #GtkScrolledWindow can grow scrollbars to
 *  reach the new bottom-right extent (or shrink them once the node
 *  comes back inside the canvas). */
static void
on_node_notify_position (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    gtk_widget_queue_resize (GTK_WIDGET (user_data));
}

/** Repaint whenever an animated node (e.g. a graph node refreshing
 *  with new data) signals that its appearance has changed.  Cheaper
 *  than per-property notify hooks and keeps the worksheet ignorant
 *  of which subclasses care. */
static void
on_node_repaint_needed (
        PnNode  *node,
        gpointer user_data)
{
    PnWorksheet   *self = PN_WORKSHEET (user_data);
    const PnPoint *p;
    double         w, h;
    double        *last;
    double         x0, y0, x1, y1;

    /* Only the worksheet that actually shows this node need react. */
    if (!node_on_sheet (self, node))
        return;

    /* A maximised node is painted as the large zoom overlay (centred in
     * the viewport), not at its on-canvas rectangle — so invalidate the
     * overlay region instead, padded for its drop shadow.  Without this a
     * live, scrolling graph would only refresh the sliver of its small
     * on-canvas footprint while maximised. */
    if (node == self->zoomed_node)
    {
        double zx, zy, zw, zh;
        const double pad = 32.0;

        zoom_current_rect (self, &zx, &zy, &zw, &zh);
        gtk_widget_queue_draw_area (GTK_WIDGET (self),
                                    (int) floor (zx - pad),
                                    (int) floor (zy - pad),
                                    (int) ceil (zw + 2.0 * pad) + 1,
                                    (int) ceil (zh + 2.0 * pad) + 1);
        return;
    }

    pn_node_get_size (node, &w, &h);

    /* If the node's footprint changed, fall back to queue_resize so the
     * scroll extent re-tracks it — the File Drop node grows / shrinks its
     * drop area to a dropped image's aspect ratio.  The last-seen size is
     * cached on the node so the common case (an animated node — Dial,
     * meter, graph — repainting in place at up to 60 Hz) takes the cheap
     * path below instead of invalidating and re-measuring the whole
     * canvas every frame, which was redrawing every wire light too. */
    last = g_object_get_data (G_OBJECT (node), "pn-ws-last-size");
    if (last == NULL || last[0] != w || last[1] != h)
    {
        if (last == NULL)
        {
            last = g_new (double, 2);
            g_object_set_data_full (G_OBJECT (node), "pn-ws-last-size",
                                    last, g_free);
        }
        last[0] = w;
        last[1] = h;
        gtk_widget_queue_resize (GTK_WIDGET (self));
        return;
    }

    /* Same footprint: repaint just the node's own rectangle (widget
     * coords = model * zoom), padded a little for borders/halos. */
    p  = pn_node_get_position (node);
    x0 = (p->x - 6.0) * self->zoom;
    y0 = (p->y - 6.0) * self->zoom;
    x1 = (p->x + w + 6.0) * self->zoom;
    y1 = (p->y + h + 6.0) * self->zoom;
    gtk_widget_queue_draw_area (GTK_WIDGET (self),
                                (int) floor (x0), (int) floor (y0),
                                (int) ceil (x1 - x0) + 1,
                                (int) ceil (y1 - y0) + 1);
}

/** Forward the flow's modified flag transitions out as a worksheet
 *  signal so existing widget-side subscribers (PnWindow) can keep
 *  their Save UI gated without learning about PnFlow.  The flow
 *  itself is the single source of truth — every PnWorksheet sharing a
 *  flow forwards the same value, and PnWindow only listens to the
 *  active page's signal at any time. */
static void
on_flow_modified_changed (
        PnFlow   *flow,
        gboolean  modified,
        gpointer  user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) flow; (void) modified;

    g_signal_emit (self, signals[SIG_MODIFIED_CHANGED], 0);
}

/** Repaint whenever the node store changes.  Also re-asks for a
 *  size since a freshly-added node may push the content extent past
 *  the current viewport, and a removal may let the canvas shrink.
 *  The flow tracks the modified flag itself; this hook stays purely
 *  visual. */
static void
on_nodes_changed (
        PnNodeStore *store,
        PnNode      *node,
        guint        index,
        gpointer     user_data)
{
    (void) store;
    (void) node;
    (void) index;

    gtk_widget_queue_resize (GTK_WIDGET (user_data));
    gtk_widget_queue_draw   (GTK_WIDGET (user_data));
}

/** Wire up notify::disabled on each node added to the store so the
 *  canvas follows toggles from the dialog or context menu without
 *  needing a manual queue_draw at every call site. */
static void
on_node_added_to_store (
        PnNodeStore *store,
        PnNode      *node,
        guint        index,
        gpointer     user_data)
{
    (void) store;
    (void) index;

    g_signal_connect (node, "notify::disabled",
                      G_CALLBACK (on_node_notify_appearance),
                      user_data);
    g_signal_connect (node, "notify::has-error",
                      G_CALLBACK (on_node_notify_appearance),
                      user_data);
    g_signal_connect (node, "notify::position",
                      G_CALLBACK (on_node_notify_position),
                      user_data);
    /* A comment box is user-resizable, so its width/height change at
     * runtime — re-measure the canvas extents on those too.  Only the
     * comment owns these properties; the shared disconnect-by-func below
     * unhooks every detail of on_node_notify_position at removal. */
    if (PN_IS_COMMENT (node))
    {
        g_signal_connect (node, "notify::width",
                          G_CALLBACK (on_node_notify_position), user_data);
        g_signal_connect (node, "notify::height",
                          G_CALLBACK (on_node_notify_position), user_data);
    }
    g_signal_connect (node, "repaint-needed",
                      G_CALLBACK (on_node_repaint_needed),
                      user_data);
    g_signal_connect (node, "processing-changed",
                      G_CALLBACK (on_node_processing),
                      user_data);
    /* Generic "modified flag" notify hookup lives on the flow side
     * (see pn-flow.c on_node_added) so multi-widget hosts do not have
     * to deduplicate it across N worksheets sharing one model. */
}

/** Forward the flow's status-bar text up to the worksheet's consumers
 *  (the main window subscribes via "status-message" on the widget).
 *  The flow is the actual producer — it listens on PnDebug nodes —
 *  but the worksheet keeps a same-named signal so existing widget
 *  subscribers don't have to know the model exists. */
static void
on_flow_status_message (
        PnFlow      *flow,
        const gchar *text,
        gpointer     user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) flow;

    g_signal_emit (self, signals[SIG_STATUS_MESSAGE], 0, text);
}

/** Forward the flow's debug-view text up to the worksheet's
 *  consumers.  Mirror of on_flow_status_message() for the new
 *  PN_DEBUG_TARGET_DEBUG_VIEW route. */
static void
on_flow_debug_message (
        PnFlow      *flow,
        const gchar *text,
        gpointer     user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) flow;

    g_signal_emit (self, signals[SIG_DEBUG_MESSAGE], 0, text);
}

/** Drop any reference to @node from the selection and from the
 *  in-progress drag once a node is removed from the store. */
static void
on_node_removed_from_store (
        PnNodeStore *store,
        PnNode      *node,
        guint        index,
        gpointer     user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) store;
    (void) index;

    /* Disconnects every handler registered with this function — both the
     * notify::disabled and notify::has-error connections. */
    g_signal_handlers_disconnect_by_func (
            node, G_CALLBACK (on_node_notify_appearance), self);
    g_signal_handlers_disconnect_by_func (
            node, G_CALLBACK (on_node_notify_position), self);
    g_signal_handlers_disconnect_by_func (
            node, G_CALLBACK (on_node_processing), self);

    if (g_hash_table_remove (self->selection, node))
        g_signal_emit (self, signals[SIG_SELECTION_CHANGED], 0);

    if (self->drag_pivot == node)
        g_clear_object (&self->drag_pivot);

    /* A comment deleted mid-resize: drop the borrowed ref and balance the
     * undo freeze opened when the resize began. */
    if (self->resize_comment == node)
    {
        g_clear_object (&self->resize_comment);
        self->resize_edge = PN_RESIZE_NONE;
        pn_flow_history_thaw (self->flow);
    }

    if ((PnNode *) self->pressed_inject == node)
        g_clear_object (&self->pressed_inject);

    /* zoomed_node is borrowed — drop it (and tear the tick down) if
     * the lifted node is the one being removed, so a delete during
     * the zoom animation can't leave the overlay painting against a
     * freed pointer. */
    if (self->zoomed_node == node)
    {
        self->zoomed_node = NULL;
        self->zoom_t       = 0.0;
        self->zoom_dir     = 0;
        self->sun_path_drag    = FALSE;
        self->sun_path_dragged = FALSE;
        self->osc_drag         = FALSE;
        self->osc_cursor_drag  = FALSE;
        self->osc_cknob_drag   = FALSE;
        if (self->zoom_tick_id != 0)
        {
            gtk_widget_remove_tick_callback (GTK_WIDGET (self),
                                             self->zoom_tick_id);
            self->zoom_tick_id = 0;
        }
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }

    /* Same defensive clear for the chat keyboard-focus pointer.  The
     * worksheet's key-press dispatcher borrows it, so dropping it
     * here keeps a delete during a typing session from routing the
     * next keystroke into freed memory. */
    if ((PnNode *) self->focused_chat == node)
        self->focused_chat = NULL;

    /* Same defensive clear for the focus pulse: the painter borrows
     * pulse_node so it must drop the pointer the moment the store
     * stops holding it. */
    if (self->pulse_node == node)
    {
        self->pulse_node = NULL;
        if (self->pulse_tick_id != 0)
        {
            gtk_widget_remove_tick_callback (GTK_WIDGET (self),
                                             self->pulse_tick_id);
            self->pulse_tick_id = 0;
        }
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }

    if (self->drag_anchors != NULL)
    {
        guint i;
        for (i = 0; i < self->drag_anchors->len; i++)
        {
            PnDragAnchor *a = &g_array_index (self->drag_anchors,
                                              PnDragAnchor, i);
            if (a->node == node)
            {
                g_array_remove_index_fast (self->drag_anchors, i);
                break;
            }
        }
    }
}

/** A message just crossed @wire: animate a light along it, unless the
 *  feature is off (cheap pref read) or the wire is off this sheet
 *  (wire_pulse_spawn skips it via wire_endpoints). */
static void
on_message_passed (
        PnWire    *wire,
        PnMessage *message,
        gpointer   user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);

    if (!pn_preferences_get_animate_wire_messages (
                pn_preferences_get_default ()))
        return;

    /* The dispatch depth at this point is the hop index of this wire on
     * the message's path — read it synchronously here, while the cascade
     * is still on the stack, to stagger the animation hop by hop. */
    wire_pulse_spawn (self, wire, message, pn_node_get_dispatch_depth ());
}

/** A wire was added to the store: repaint and arm its "message-passed"
 *  hook so future messages crossing it animate. */
static void
on_wire_added (
        PnWireStore *store,
        PnWire      *wire,
        guint        index,
        gpointer     user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) store;
    (void) index;

    if (wire != NULL)
        g_signal_connect (wire, "message-passed",
                          G_CALLBACK (on_message_passed), self);

    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/** A wire was removed: repaint, drop its hook, and discard any lights
 *  still in flight on it. */
static void
on_wire_removed (
        PnWireStore *store,
        PnWire      *wire,
        guint        index,
        gpointer     user_data)
{
    PnWorksheet *self = PN_WORKSHEET (user_data);
    (void) store;
    (void) index;

    if (wire != NULL)
        g_signal_handlers_disconnect_by_func (
                wire, G_CALLBACK (on_message_passed), self);

    if (self->wire_pulses != NULL)
    {
        guint i;
        for (i = self->wire_pulses->len; i > 0; i--)
        {
            PnWirePulse *p = &g_array_index (self->wire_pulses,
                                             PnWirePulse, i - 1);
            if (p->wire == wire)
                g_array_remove_index (self->wire_pulses, i - 1);
        }
    }

    gtk_widget_queue_draw (GTK_WIDGET (self));
}

/* ------------------------------------------------------------------ */
/*  Drag-to-move                                                       */
/* ------------------------------------------------------------------ */

/** Return the topmost node whose body contains (@x, @y), or %NULL. */
static PnNode *
hit_test_node (
        PnWorksheet *self,
        double       x,
        double       y)
{
    const guint count = pn_node_store_get_length (self->nodes);
    gint i;

    /* Iterate front-to-back so the visually-topmost node wins.  Hit
     * testing intentionally targets only the header rectangle: the
     * graph plot extension below it is a passive readout, not a
     * selection target, so clicking inside the plot misses the node. */
    for (i = (gint) count - 1; i >= 0; i--)
    {
        PnNode        *node = pn_node_store_get_node (self->nodes, (guint) i);
        const PnPoint *p    = pn_node_get_position   (node);
        double         w, h;

        if (!node_on_sheet (self, node))
            continue;

        pn_node_get_size (node, &w, &h);
        /* Select on the solid body — the header plus any multi-input
         * lower section — but not the passive paint_plot extension. */
        h = node_body_height (node);

        if (x >= p->x && x < p->x + w &&
            y >= p->y && y < p->y + h)
            return node;
    }

    return NULL;
}

typedef enum {
    PN_PORT_NONE,
    PN_PORT_INPUT,
    PN_PORT_OUTPUT,
} PnPortKind;

/** Return the topmost #PnInject whose left-edge fire button contains
 *  (@x, @y), or %NULL when no inject node's button was hit. */
static PnInject *
hit_test_inject_button (
        PnWorksheet *self,
        double       x,
        double       y)
{
    const guint count = pn_node_store_get_length (self->nodes);
    gint i;

    for (i = (gint) count - 1; i >= 0; i--)
    {
        PnNode        *node = pn_node_store_get_node (self->nodes, (guint) i);
        const PnPoint *p;
        double         bw;

        if (!PN_IS_INJECT (node))
            continue;
        if (!node_on_sheet (self, node))
            continue;

        p  = pn_node_get_position (node);
        bw = inject_button_width (PN_INJECT (node));
        if (x >= p->x - bw - PN_INJECT_BUTTON_GAP &&
            x <  p->x - PN_INJECT_BUTTON_GAP &&
            y >= p->y && y < p->y + pn_node_get_header_height (node))
            return PN_INJECT (node);
    }

    return NULL;
}

/** Topmost #PnChat whose entry strip or inline Send button contains
 *  (@x, @y).  @out_hit reports which of the two regions was hit.
 *  Returns %NULL (with @*out_hit == #PN_CHAT_HIT_NONE) when no chat's
 *  bottom-row controls were hit.  Routed before the generic plot
 *  hit-test in on_button_press so the entry / Send rectangles win
 *  over the surrounding bubbles area. */
static PnChat *
hit_test_chat_input (
        PnWorksheet *self,
        double       x,
        double       y,
        PnChatHit   *out_hit)
{
    const guint count = pn_node_store_get_length (self->nodes);
    gint i;

    if (out_hit != NULL)
        *out_hit = PN_CHAT_HIT_NONE;

    for (i = (gint) count - 1; i >= 0; i--)
    {
        PnNode *node = pn_node_store_get_node (self->nodes, (guint) i);
        PnChatHit hit;

        if (!PN_IS_CHAT (node))
            continue;
        if (!node_on_sheet (self, node))
            continue;

        hit = pn_chat_hit_input (PN_CHAT (node), x, y);
        if (hit != PN_CHAT_HIT_NONE)
        {
            if (out_hit != NULL)
                *out_hit = hit;
            return PN_CHAT (node);
        }
    }

    return NULL;
}

/** Return the topmost node whose input or output port contains
 *  (@x, @y), or %NULL.  The hit area is enlarged by a small slop so
 *  the user does not have to land exactly on the 7×10 px tab.  When an
 *  input port is hit and @out_input is non-%NULL it receives the
 *  0-based index of that input (relevant only for multi-input nodes;
 *  always 0 for ordinary single-input nodes and for output hits). */
static PnNode *
hit_test_port (
        PnWorksheet *self,
        double       x,
        double       y,
        PnPortKind  *out_kind,
        gint        *out_input)
{
    const guint  count = pn_node_store_get_length (self->nodes);
    const double slop  = 4.0;
    gint i;

    if (out_input) *out_input = 0;

    for (i = (gint) count - 1; i >= 0; i--)
    {
        PnNode        *node = pn_node_store_get_node (self->nodes, (guint) i);
        const PnPoint *p    = pn_node_get_position   (node);
        const double   cy   = p->y + pn_node_get_header_height (node) / 2.0;
        double         nw, nh;

        if (!node_on_sheet (self, node))
            continue;

        pn_node_get_size (node, &nw, &nh);
        (void) nh;

        if (pn_node_get_has_output (node))
        {
            const double ox = p->x + nw - PN_PORT_WIDTH / 2.0;
            const double oy = cy - PN_PORT_HEIGHT / 2.0;
            if (x >= ox - slop && x < ox + PN_PORT_WIDTH  + slop &&
                y >= oy - slop && y < oy + PN_PORT_HEIGHT + slop)
            {
                if (out_kind) *out_kind = PN_PORT_OUTPUT;
                return node;
            }
        }

        if (pn_node_get_has_input (node))
        {
            const gint   n_inputs = pn_node_get_n_inputs (node);
            const double ix       = p->x - PN_PORT_WIDTH / 2.0;
            gint         k;

            for (k = 0; k < n_inputs; k++)
            {
                const double iy = input_port_y (node, k) - PN_PORT_HEIGHT / 2.0;
                if (x >= ix - slop && x < ix + PN_PORT_WIDTH  + slop &&
                    y >= iy - slop && y < iy + PN_PORT_HEIGHT + slop)
                {
                    if (out_kind)  *out_kind  = PN_PORT_INPUT;
                    if (out_input) *out_input = k;
                    return node;
                }
            }
        }
    }

    if (out_kind) *out_kind = PN_PORT_NONE;
    return NULL;
}

/** Evaluate a cubic Bézier coordinate at parameter @t. */
static inline double
bezier_eval (double a, double b, double c, double d, double t)
{
    const double u  = 1.0 - t;
    return u * u * u * a
         + 3 * u * u * t * b
         + 3 * u * t * t * c
         + t * t * t * d;
}

/** Return the topmost wire whose curve passes within a few pixels of
 *  (@x, @y), or %NULL.  The curve is sampled densely enough that the
 *  worst-case gap between samples sits inside the slop tolerance. */
static PnWire *
hit_test_wire (
        PnWorksheet *self,
        double       x,
        double       y)
{
    const guint  count = pn_wire_store_get_length (self->wires);
    const double slop  = 5.0;
    const double slop2 = slop * slop;
    gint         i;

    for (i = (gint) count - 1; i >= 0; i--)
    {
        PnWire        *wire = pn_wire_store_get_wire (self->wires, (guint) i);
        PnNode        *src  = pn_wire_get_source (wire);
        PnNode        *dst  = pn_wire_get_target (wire);
        const PnPoint *ps;
        const PnPoint *pt;
        double         x1, y1, x2, y2, cx1, cx2, dx;
        guint          step;

        if (src == NULL || dst == NULL)
            continue;
        if (!wire_on_sheet (self, wire))
            continue;

        {
            double sw, sh;
            ps = pn_node_get_position (src);
            pt = pn_node_get_position (dst);
            pn_node_get_size (src, &sw, &sh);
            (void) sh;

            x1 = ps->x + sw;
            y1 = ps->y + pn_node_get_header_height (src) / 2.0;
            x2 = pt->x;
            /* Match the painted wire: land on the addressed input row
             * (lower section for multi-input targets), not the header. */
            y2 = input_port_y (dst, pn_wire_get_target_input (wire));
        }

        dx = fabs (x2 - x1) * 0.5;
        if (dx < 30.0)
            dx = 30.0;
        cx1 = x1 + dx;
        cx2 = x2 - dx;

        for (step = 0; step <= 64; step++)
        {
            const double t  = step / 64.0;
            const double bx = bezier_eval (x1, cx1, cx2, x2, t);
            const double by = bezier_eval (y1, y1,  y2,  y2, t);
            const double ddx = bx - x;
            const double ddy = by - y;

            if (ddx * ddx + ddy * ddy <= slop2)
                return wire;
        }
    }

    return NULL;
}

/** Round @v to the nearest grid intersection -- but only when the
 *  user has the "snap to grid" preference enabled.  Disabling the
 *  preference makes this a pass-through so drags, drops, and paste
 *  offsets land exactly where the cursor is rather than snapping to
 *  the nearest #PN_GRID_STEP multiple. */
static inline double
snap_to_grid (double v)
{
    if (!pn_preferences_get_snap_to_grid (pn_preferences_get_default ()))
        return v;
    return round (v / PN_GRID_STEP) * PN_GRID_STEP;
}

/** Return %TRUE when the store already contains a wire from @source to
 *  @target's input @target_input.  Wires are direction-sensitive, so a
 *  reverse-direction wire is not considered a duplicate; and the port
 *  index is part of the identity, so the same source feeding two
 *  different inputs of one node counts as two distinct wires. */
static gboolean
wire_exists (
        PnWorksheet *self,
        PnNode      *source,
        PnNode      *target,
        gint         target_input)
{
    const guint count = pn_wire_store_get_length (self->wires);
    guint       i;

    for (i = 0; i < count; i++)
    {
        PnWire *wire = pn_wire_store_get_wire (self->wires, i);

        if (pn_wire_get_source (wire) == source &&
            pn_wire_get_target (wire) == target &&
            pn_wire_get_target_input (wire) == target_input)
            return TRUE;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Right-click context menus                                          */
/* ------------------------------------------------------------------ */

/** Drop @node from the worksheet, removing every wire that references
 *  it as either source or target.  Wires are scanned back-to-front so
 *  index shifts during removal stay benign. */
void
pn_worksheet_delete_node (
        PnWorksheet *self,
        PnNode      *node)
{
    gint i;

    g_return_if_fail (PN_IS_WORKSHEET (self));
    g_return_if_fail (PN_IS_NODE (node));

    for (i = (gint) pn_wire_store_get_length (self->wires) - 1; i >= 0; i--)
    {
        PnWire *wire = pn_wire_store_get_wire (self->wires, (guint) i);
        if (pn_wire_get_source (wire) == node ||
            pn_wire_get_target (wire) == node)
            pn_wire_store_remove (self->wires, wire);
    }

    pn_node_store_remove (self->nodes, node);
}

/* Thaw the history freeze a property dialog opened, once it is gone. */
static void
node_dialog_thaw_on_destroy (GtkWidget *dialog, gpointer flow)
{
    (void) dialog;
    pn_flow_history_thaw (PN_FLOW (flow));
}

/* Bracket a node property-dialog session into a single undo step: freeze
 * the history now and thaw it when @dialog is destroyed, however it
 * closes.  The flow is ref'd for the dialog's lifetime so a worksheet
 * torn down while the dialog is still open cannot leave a dangling
 * thaw. */
static void
node_dialog_track_history (GtkWidget *dialog, PnWorksheet *self)
{
    if (self == NULL || self->flow == NULL)
        return;

    pn_flow_history_freeze (self->flow);
    g_signal_connect_data (dialog, "destroy",
                           G_CALLBACK (node_dialog_thaw_on_destroy),
                           g_object_ref (self->flow),
                           (GClosureNotify) g_object_unref,
                           0);
}

static void
on_node_menu_configure (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnNode      *node     = PN_NODE (user_data);
    GtkWidget   *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));
    GtkWidget   *toplevel = attached != NULL
                                ? gtk_widget_get_toplevel (attached) : NULL;
    GtkWindow   *parent   = (toplevel != NULL && GTK_IS_WINDOW (toplevel))
                                ? GTK_WINDOW (toplevel) : NULL;
    GtkWidget   *dialog   = pn_node_dialog_new (parent, node);

    if (attached != NULL && PN_IS_WORKSHEET (attached))
        node_dialog_track_history (dialog, PN_WORKSHEET (attached));

    g_signal_connect_swapped (dialog, "response",
                              G_CALLBACK (gtk_widget_destroy),
                              dialog);
    gtk_widget_show_all (dialog);
}

/** Open (or raise) the per-node Log dialog, tailing the diagnostics the
 *  node reports through pn_node_log(). */
static void
on_node_menu_log (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnNode    *node     = PN_NODE (user_data);
    GtkWidget *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));
    GtkWidget *toplevel = attached != NULL
                              ? gtk_widget_get_toplevel (attached) : NULL;
    GtkWindow *parent   = (toplevel != NULL && GTK_IS_WINDOW (toplevel))
                              ? GTK_WINDOW (toplevel) : NULL;

    pn_log_dialog_present (parent, node);
}

static void
on_node_menu_delete (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnNode      *node     = PN_NODE (user_data);
    GtkWidget   *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));

    if (attached != NULL && PN_IS_WORKSHEET (attached))
        pn_worksheet_delete_node (PN_WORKSHEET (attached), node);
}

static void
on_node_menu_inject (
        GtkMenuItem *item,
        gpointer     user_data)
{
    (void) item;
    pn_inject_fire (PN_INJECT (user_data));
}

static void
on_node_menu_trigger (
        GtkMenuItem *item,
        gpointer     user_data)
{
    (void) item;
    pn_auto_trigger_kick (PN_AUTO_TRIGGER (user_data));
}

static void
on_node_menu_toggle_disabled (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnNode *node = PN_NODE (user_data);
    (void) item;

    pn_node_set_disabled (node, !pn_node_get_disabled (node));
}

/** Ask the host window to open this node's help page.  The worksheet
 *  stays ignorant of help-page paths (mirroring #PnPalette): it emits
 *  PnWorksheet::help-requested carrying the node's #GType name as the
 *  anchor and lets the window resolve <anchor>.html. */
static void
on_node_menu_help (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnNode      *node     = PN_NODE (user_data);
    GtkWidget   *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));

    if (attached != NULL && PN_IS_WORKSHEET (attached))
        g_signal_emit (PN_WORKSHEET (attached),
                       signals[SIG_HELP_REQUESTED], 0,
                       G_OBJECT_TYPE_NAME (node));
}

static void
on_wire_menu_delete (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWire      *wire     = PN_WIRE (user_data);
    GtkWidget   *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));

    if (attached != NULL && PN_IS_WORKSHEET (attached))
        pn_wire_store_remove (PN_WORKSHEET (attached)->wires, wire);
}

static void
on_node_menu_cut (
        GtkMenuItem *item,
        gpointer     user_data)
{
    GtkWidget *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));

    (void) user_data;
    if (attached != NULL && PN_IS_WORKSHEET (attached))
        pn_worksheet_cut_selection (PN_WORKSHEET (attached));
}

static void
on_node_menu_copy (
        GtkMenuItem *item,
        gpointer     user_data)
{
    GtkWidget *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));

    (void) user_data;
    if (attached != NULL && PN_IS_WORKSHEET (attached))
        pn_worksheet_copy_selection (PN_WORKSHEET (attached));
}

static void
on_node_menu_paste (
        GtkMenuItem *item,
        gpointer     user_data)
{
    GtkWidget *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));

    (void) user_data;
    if (attached != NULL && PN_IS_WORKSHEET (attached))
        pn_worksheet_paste_from_clipboard (PN_WORKSHEET (attached));
}

/** Build a #GtkMenuItem whose child is a horizontal box of an icon
 *  and a label, since GtkImageMenuItem is deprecated in GTK 3 and the
 *  modern idiom is to compose the row by hand. */
static GtkWidget *
make_icon_menu_item (
        const gchar *icon_name,
        const gchar *label_text)
{
    GtkWidget *item  = gtk_menu_item_new ();
    GtkWidget *box   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *image = gtk_image_new_from_icon_name (icon_name,
                                                     GTK_ICON_SIZE_MENU);
    GtkWidget *label = gtk_label_new (label_text);

    gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);
    gtk_container_add  (GTK_CONTAINER (item), box);

    return item;
}

/** Pop up the per-node context menu near @event.  The menu keeps its
 *  target alive for as long as it lives by attaching a strong ref via
 *  g_object_set_data_full, since the items only see borrowed pointers
 *  while the user navigates. */
static void
show_node_popup (
        PnWorksheet    *self,
        PnNode         *node,
        GdkEventButton *event)
{
    GtkWidget *menu      = gtk_menu_new ();
    GtkWidget *configure = make_icon_menu_item ("document-properties",
                                                "Configure…");
    GtkWidget *log       = make_icon_menu_item ("text-x-generic",
                                                "Log…");
    const gboolean disabled = pn_node_get_disabled (node);
    GtkWidget *toggle    = make_icon_menu_item (
            disabled ? "media-playback-start" : "media-playback-pause",
            disabled ? "Enable" : "Disable");
    GtkWidget *cut       = make_icon_menu_item ("edit-cut",   "Cut");
    GtkWidget *copy      = make_icon_menu_item ("edit-copy",  "Copy");
    GtkWidget *paste     = make_icon_menu_item ("edit-paste", "Paste");
    GtkWidget *del       = make_icon_menu_item ("edit-delete", "Delete");
    GtkWidget *help      = make_icon_menu_item ("help-browser", "Help");

    gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (self), NULL);
    g_object_set_data_full (G_OBJECT (menu), "pn-target",
                            g_object_ref (node), g_object_unref);

    g_signal_connect (configure, "activate",
                      G_CALLBACK (on_node_menu_configure), node);
    g_signal_connect (log, "activate",
                      G_CALLBACK (on_node_menu_log), node);
    g_signal_connect (help, "activate",
                      G_CALLBACK (on_node_menu_help), node);
    g_signal_connect (toggle, "activate",
                      G_CALLBACK (on_node_menu_toggle_disabled), node);
    g_signal_connect (cut,   "activate", G_CALLBACK (on_node_menu_cut),   NULL);
    g_signal_connect (copy,  "activate", G_CALLBACK (on_node_menu_copy),  NULL);
    g_signal_connect (paste, "activate", G_CALLBACK (on_node_menu_paste), NULL);
    g_signal_connect (del, "activate",
                      G_CALLBACK (on_node_menu_delete), node);

    /* Inject nodes get a "fire once now" entry at the top so it is
     * easy to test wiring without the on-canvas button. */
    if (PN_IS_INJECT (node))
    {
        GtkWidget *inject = make_icon_menu_item ("media-playback-start",
                                                 "Inject");
        g_signal_connect (inject, "activate",
                          G_CALLBACK (on_node_menu_inject), node);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), inject);
    }

    /* Auto-trigger nodes get a "Trigger" entry that fires one extra tick
     * now, without waiting for the periodic delay to elapse. */
    if (PN_IS_AUTO_TRIGGER (node))
    {
        GtkWidget *trigger = make_icon_menu_item ("view-refresh",
                                                  "Trigger");
        /* A disabled node ignores ticks, so a manual trigger would be a
         * no-op -- grey the entry out to make that clear. */
        gtk_widget_set_sensitive (trigger, !disabled);
        g_signal_connect (trigger, "activate",
                          G_CALLBACK (on_node_menu_trigger), node);
        gtk_menu_shell_append (GTK_MENU_SHELL (menu), trigger);
    }

    gtk_menu_shell_append (GTK_MENU_SHELL (menu), toggle);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), cut);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), copy);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), paste);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), del);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), configure);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), log);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), help);
    gtk_widget_show_all (menu);

    gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *) event);
}

static void
on_canvas_menu_add_note (
        GtkMenuItem *item,
        gpointer     user_data)
{
    GtkWidget   *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));
    PnWorksheet *self;
    PnComment   *comment;
    PnNode      *node;
    PnPoint      pos;

    (void) user_data;
    if (attached == NULL || !PN_IS_WORKSHEET (attached))
        return;

    self    = PN_WORKSHEET (attached);
    comment = pn_comment_new ();
    node    = PN_NODE (comment);

    /* Place the box with its top-left at the right-click point (the
     * world-coords stashed by show_canvas_popup), snapped to the grid. */
    pos.x = snap_to_grid (self->paste_target_x);
    pos.y = snap_to_grid (self->paste_target_y);
    pn_node_set_position (node, &pos);

    if (self->sheet_name != NULL)
        pn_node_set_worksheet (node, self->sheet_name);

    pn_flow_add_node (self->flow, node);

    /* Select the fresh note so its resize handles show immediately and a
     * double-click straight away edits its text. */
    selection_clear (self);
    selection_add   (self, node);

    g_object_unref (comment);
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
on_canvas_menu_paste (
        GtkMenuItem *item,
        gpointer     user_data)
{
    GtkWidget *attached = gtk_menu_get_attach_widget (
            GTK_MENU (gtk_widget_get_parent (GTK_WIDGET (item))));

    (void) user_data;
    if (attached != NULL && PN_IS_WORKSHEET (attached))
    {
        PnWorksheet *ws = PN_WORKSHEET (attached);
        ws->paste_at_cursor = TRUE;
        pn_worksheet_paste_from_clipboard (ws);
    }
}

/** Pop up the empty-canvas context menu near @event.  Cut/Copy are
 *  always insensitive (the right-click already cleared the selection
 *  before we got here, so there is nothing to act on); Paste is
 *  always enabled and pastes at the worksheet-coords (@wx, @wy)
 *  captured at button-press time. */
static void
show_canvas_popup (
        PnWorksheet    *self,
        GdkEventButton *event,
        double          wx,
        double          wy)
{
    GtkWidget *menu  = gtk_menu_new ();
    GtkWidget *cut   = make_icon_menu_item ("edit-cut",   "Cut");
    GtkWidget *copy  = make_icon_menu_item ("edit-copy",  "Copy");
    GtkWidget *paste = make_icon_menu_item ("edit-paste", "Paste");
    GtkWidget *sep   = gtk_separator_menu_item_new ();
    GtkWidget *note  = make_icon_menu_item ("insert-text", "Add note");

    gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (self), NULL);

    /* Stash the world-coords paste / insert target on the worksheet so the
     * Paste and Add-note handlers don't need to thread it through
     * user_data. */
    self->paste_target_x = wx;
    self->paste_target_y = wy;

    gtk_widget_set_sensitive (cut,  FALSE);
    gtk_widget_set_sensitive (copy, FALSE);

    g_signal_connect (paste, "activate",
                      G_CALLBACK (on_canvas_menu_paste), NULL);
    g_signal_connect (note, "activate",
                      G_CALLBACK (on_canvas_menu_add_note), NULL);

    gtk_menu_shell_append (GTK_MENU_SHELL (menu), cut);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), copy);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), paste);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), sep);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), note);
    gtk_widget_show_all (menu);

    gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *) event);
}

static void
show_wire_popup (
        PnWorksheet    *self,
        PnWire         *wire,
        GdkEventButton *event)
{
    GtkWidget *menu = gtk_menu_new ();
    GtkWidget *del  = make_icon_menu_item ("edit-delete", "Delete");

    gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (self), NULL);
    g_object_set_data_full (G_OBJECT (menu), "pn-target",
                            g_object_ref (wire), g_object_unref);

    g_signal_connect (del, "activate",
                      G_CALLBACK (on_wire_menu_delete), wire);

    gtk_menu_shell_append (GTK_MENU_SHELL (menu), del);
    gtk_widget_show_all (menu);

    gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *) event);
}

/* ------------------------------------------------------------------ */
/*  Selection helpers                                                  */
/* ------------------------------------------------------------------ */

static void
selection_clear (PnWorksheet *self)
{
    if (g_hash_table_size (self->selection) == 0)
        return;
    g_hash_table_remove_all (self->selection);
    g_signal_emit (self, signals[SIG_SELECTION_CHANGED], 0);
}

static void
selection_add (PnWorksheet *self, PnNode *node)
{
    if (g_hash_table_contains (self->selection, node))
        return;
    g_hash_table_add (self->selection, g_object_ref (node));
    g_signal_emit (self, signals[SIG_SELECTION_CHANGED], 0);
}

static gboolean
selection_contains (PnWorksheet *self, PnNode *node)
{
    return g_hash_table_contains (self->selection, node);
}

/* ------------------------------------------------------------------ */
/*  Selection & view automation surface (TODO #40.12)                  */
/*                                                                     */
/*  Public entry points the org.pipas.pipnode.Worksheet D-Bus interface */
/*  drives so an agent can SHOW the user what it just built — select   */
/*  nodes, read the selection back, recentre/fit the viewport — going  */
/*  through the very same selection set and scroll machinery the mouse */
/*  and keyboard use, so there is no parallel state to drift.          */
/* ------------------------------------------------------------------ */

guint
pn_worksheet_select_nodes_by_uuid (PnWorksheet        *self,
                                   const gchar *const *uuids,
                                   guint               n_uuids)
{
    guint matched = 0;
    guint i;

    g_return_val_if_fail (PN_IS_WORKSHEET (self), 0);

    /* Manipulate the set directly and emit "selection-changed" once at
     * the end, rather than going through selection_clear/_add (which
     * each emit) so a multi-node selection is a single observable event. */
    g_hash_table_remove_all (self->selection);

    for (i = 0; i < n_uuids; i++)
    {
        PnNode *node = find_node_by_uuid (self, uuids[i]);

        if (node != NULL && !g_hash_table_contains (self->selection, node))
        {
            g_hash_table_add (self->selection, g_object_ref (node));
            matched++;
        }
    }

    g_signal_emit (self, signals[SIG_SELECTION_CHANGED], 0);
    gtk_widget_queue_draw (GTK_WIDGET (self));
    return matched;
}

gchar **
pn_worksheet_get_selection_uuids (PnWorksheet *self)
{
    GPtrArray      *out;
    GHashTableIter  iter;
    gpointer        key;

    g_return_val_if_fail (PN_IS_WORKSHEET (self), NULL);

    out = g_ptr_array_new ();
    g_hash_table_iter_init (&iter, self->selection);
    while (g_hash_table_iter_next (&iter, &key, NULL))
    {
        const gchar *uuid = pn_node_get_uuid (PN_NODE (key));
        g_ptr_array_add (out, g_strdup (uuid != NULL ? uuid : ""));
    }
    g_ptr_array_add (out, NULL);

    return (gchar **) g_ptr_array_free (out, FALSE);
}

void
pn_worksheet_clear_selection (PnWorksheet *self)
{
    g_return_if_fail (PN_IS_WORKSHEET (self));
    selection_clear (self);                 /* emits only if non-empty */
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

gboolean
pn_worksheet_center_on_uuid (PnWorksheet *self,
                             const gchar *uuid)
{
    PnNode *node;

    g_return_val_if_fail (PN_IS_WORKSHEET (self), FALSE);

    node = find_node_by_uuid (self, uuid);
    if (node == NULL)
        return FALSE;

    scroll_to_center_node (self, node);
    return TRUE;
}

/** Worksheet-space bounding box of every node on the active sheet.
 *  Returns %FALSE (box untouched) when the sheet has no nodes. */
static gboolean
active_sheet_content_bounds (PnWorksheet *self,
                             double *out_x, double *out_y,
                             double *out_w, double *out_h)
{
    const guint count = pn_node_store_get_length (self->nodes);
    gboolean    any   = FALSE;
    double      minx = 0, miny = 0, maxx = 0, maxy = 0;
    guint       i;

    for (i = 0; i < count; i++)
    {
        PnNode        *node = pn_node_store_get_node (self->nodes, i);
        const PnPoint *p;
        double         w, h;

        if (!node_on_sheet (self, node))
            continue;

        p = pn_node_get_position (node);
        pn_node_get_size (node, &w, &h);

        if (!any)
        {
            minx = p->x;          miny = p->y;
            maxx = p->x + w;      maxy = p->y + h;
            any  = TRUE;
        }
        else
        {
            if (p->x     < minx) minx = p->x;
            if (p->y     < miny) miny = p->y;
            if (p->x + w > maxx) maxx = p->x + w;
            if (p->y + h > maxy) maxy = p->y + h;
        }
    }

    if (!any)
        return FALSE;

    *out_x = minx;
    *out_y = miny;
    *out_w = maxx - minx;
    *out_h = maxy - miny;
    return TRUE;
}

void
pn_worksheet_fit_to_content (PnWorksheet *self)
{
    GtkScrolledWindow *sw;
    GtkAllocation      alloc;
    double             bx, by, bw, bh;
    double             zoom_x, zoom_y, zoom;
    const double       margin = 0.92;   /* leave a little breathing room */

    g_return_if_fail (PN_IS_WORKSHEET (self));

    sw = find_scrolled_window (self);
    if (sw == NULL)
        return;
    if (!active_sheet_content_bounds (self, &bx, &by, &bw, &bh))
        return;
    if (bw <= 0.0 || bh <= 0.0)
        return;

    gtk_widget_get_allocation (GTK_WIDGET (sw), &alloc);
    if (alloc.width <= 0 || alloc.height <= 0)
        return;

    /* Largest zoom (clamped to the wheel-zoom range) at which the whole
     * content box still fits inside the viewport, with a small margin. */
    zoom_x = (alloc.width  * margin) / bw;
    zoom_y = (alloc.height * margin) / bh;
    zoom   = MIN (zoom_x, zoom_y);
    if (zoom < PN_ZOOM_MIN) zoom = PN_ZOOM_MIN;
    if (zoom > PN_ZOOM_MAX) zoom = PN_ZOOM_MAX;

    self->zoom = zoom;

    /* Place the content centre at the viewport centre once the canvas
     * has resized for the new zoom (adjustment uppers grow then), reusing
     * the zoom-anchor idle the wheel zoom rides. */
    self->zoom_anchor_hadj    = (bx + bw / 2.0) * zoom - alloc.width  / 2.0;
    self->zoom_anchor_vadj    = (by + bh / 2.0) * zoom - alloc.height / 2.0;
    self->zoom_anchor_pending = TRUE;
    if (self->zoom_anchor_idle_id == 0)
        self->zoom_anchor_idle_id = g_idle_add (apply_zoom_anchor, self);

    gtk_widget_queue_resize (GTK_WIDGET (self));
    gtk_widget_queue_draw   (GTK_WIDGET (self));
}

void
pn_worksheet_set_scroll (PnWorksheet *self,
                         double       h,
                         double       v)
{
    GtkScrolledWindow *sw;
    GtkAdjustment     *hadj, *vadj;

    g_return_if_fail (PN_IS_WORKSHEET (self));

    sw = find_scrolled_window (self);
    if (sw == NULL)
        return;

    hadj = gtk_scrolled_window_get_hadjustment (sw);
    vadj = gtk_scrolled_window_get_vadjustment (sw);
    if (hadj != NULL) gtk_adjustment_set_value (hadj, h);
    if (vadj != NULL) gtk_adjustment_set_value (vadj, v);
}

/** Begin a group drag with all currently-selected nodes.  @pivot is
 *  the node under the cursor at button-press; the drag offset is
 *  recorded relative to its anchor so motion can recover the
 *  cursor-following position even when the group includes other
 *  nodes far from the cursor. */
static void
drag_begin (PnWorksheet *self, PnNode *pivot, double cx, double cy)
{
    GHashTableIter iter;
    gpointer       key;
    const PnPoint *p;

    p = pn_node_get_position (pivot);

    /* Coalesce the whole drag into one undo step: freeze on entering the
     * active-drag state, thaw in drag_end on leaving it.  Gating on the
     * pivot transition keeps the pair balanced even though drag_end may
     * run more than once (e.g. a double-click cancels then the release
     * fires it again). */
    if (self->drag_pivot == NULL)
        pn_flow_history_freeze (self->flow);

    g_clear_object (&self->drag_pivot);
    self->drag_pivot    = g_object_ref (pivot);
    self->drag_offset_x = cx - p->x;
    self->drag_offset_y = cy - p->y;

    g_clear_pointer (&self->drag_anchors, g_array_unref);
    self->drag_anchors = g_array_new (FALSE, FALSE, sizeof (PnDragAnchor));

    g_hash_table_iter_init (&iter, self->selection);
    while (g_hash_table_iter_next (&iter, &key, NULL))
    {
        PnNode        *n  = key;
        const PnPoint *np = pn_node_get_position (n);
        PnDragAnchor   a  = { n, np->x, np->y };
        g_array_append_val (self->drag_anchors, a);
    }
}

static void
drag_end (PnWorksheet *self)
{
    gboolean was_active = (self->drag_pivot != NULL);

    g_clear_object  (&self->drag_pivot);
    g_clear_pointer (&self->drag_anchors, g_array_unref);

    /* Close the undo span opened in drag_begin, but only if a drag was
     * actually in progress — a no-op end (no pivot) must not thaw a
     * freeze it never took. */
    if (was_active)
        pn_flow_history_thaw (self->flow);
}

/** End a comment-box resize, thawing the undo span opened when it began
 *  so the whole drag coalesces into one step.  A no-op when no resize is
 *  in progress, so it is safe to call on every button release. */
static void
resize_end (PnWorksheet *self)
{
    if (self->resize_comment == NULL)
        return;

    g_clear_object (&self->resize_comment);
    self->resize_edge = PN_RESIZE_NONE;
    pn_flow_history_thaw (self->flow);
}

/** Normalise a marquee rectangle so x0/y0 is the top-left corner and
 *  x1/y1 the bottom-right.  Used by the painter and by the
 *  intersect-with-node check. */
static void
marquee_normalise (
        PnWorksheet *self,
        double      *out_x, double *out_y,
        double      *out_w, double *out_h)
{
    double x0 = MIN (self->marquee_x0, self->marquee_x1);
    double y0 = MIN (self->marquee_y0, self->marquee_y1);
    double x1 = MAX (self->marquee_x0, self->marquee_x1);
    double y1 = MAX (self->marquee_y0, self->marquee_y1);

    *out_x = x0;
    *out_y = y0;
    *out_w = x1 - x0;
    *out_h = y1 - y0;
}

/** %TRUE when the marquee rectangle overlaps @node's body rect. */
static gboolean
marquee_overlaps_node (
        PnWorksheet *self,
        PnNode      *node)
{
    const PnPoint *p = pn_node_get_position (node);
    double rx, ry, rw, rh;
    double nw, nh;

    pn_node_get_size (node, &nw, &nh);
    /* Marquee selects on header overlap only — graph plot extensions
     * are not part of the selectable surface. */
    nh = pn_node_get_header_height (node);
    marquee_normalise (self, &rx, &ry, &rw, &rh);

    return rx <= p->x + nw && rx + rw >= p->x &&
           ry <= p->y + nh && ry + rh >= p->y;
}

/** Mouse-wheel handler with three modes:
 *
 *   - Ctrl held: zoom the worksheet around the cursor, multiplying
 *     #self->zoom by #PN_ZOOM_STEP per scroll-up tick (or dividing
 *     per scroll-down tick), clamped to [PN_ZOOM_MIN, PN_ZOOM_MAX].
 *     The pan offset is adjusted so the world point under the cursor
 *     stays put across the zoom — the standard "zoom-to-cursor"
 *     behaviour familiar from map tools and image viewers.
 *
 *   - Plot zoom active and the node exposes a scroll vfunc: forwarded
 *     to the zoomed node (e.g. a graph adjusts its time window).
 *
 *   - Otherwise: propagate so an enclosing scrolled window can scroll. */
static gboolean
on_scroll (
        GtkWidget      *widget,
        GdkEventScroll *event,
        gpointer        user_data)
{
    PnWorksheet *self  = PN_WORKSHEET (widget);
    PnNodeClass *klass;
    double       dx    = 0.0;
    double       dy    = 0.0;

    (void) user_data;

    switch (event->direction)
    {
    case GDK_SCROLL_UP:    dy = -1.0; break;
    case GDK_SCROLL_DOWN:  dy = +1.0; break;
    case GDK_SCROLL_SMOOTH:
        gdk_event_get_scroll_deltas ((GdkEvent *) event, &dx, &dy);
        break;
    default:
        return GDK_EVENT_PROPAGATE;
    }
    (void) dx;

    /* Ctrl-modifier: canvas zoom around the cursor.  Beats the plot
     * zoom forwarder so the user can re-zoom the whole worksheet even
     * while a plot is lifted (the lifted plot is in widget coords and
     * unaffected by the canvas transform, so this is harmless). */
    if ((event->state & GDK_CONTROL_MASK) != 0 && dy != 0.0)
    {
        double new_zoom = self->zoom * pow (PN_ZOOM_STEP, -dy);

        if (new_zoom < PN_ZOOM_MIN) new_zoom = PN_ZOOM_MIN;
        if (new_zoom > PN_ZOOM_MAX) new_zoom = PN_ZOOM_MAX;

        if (new_zoom != self->zoom)
        {
            GtkScrolledWindow *sw   = find_scrolled_window (self);
            GtkAdjustment     *hadj = sw ? gtk_scrolled_window_get_hadjustment (sw) : NULL;
            GtkAdjustment     *vadj = sw ? gtk_scrolled_window_get_vadjustment (sw) : NULL;
            const double cur_h = hadj ? gtk_adjustment_get_value (hadj) : 0.0;
            const double cur_v = vadj ? gtk_adjustment_get_value (vadj) : 0.0;

            /* event->x/y arrive in drawing-area coordinates already
             * (they include the current scrollbar offset).  The
             * cursor's position relative to the visible viewport is
             * therefore event->x - cur_h. */
            const double viewport_x = event->x - cur_h;
            const double viewport_y = event->y - cur_v;
            const double world_x    = event->x / self->zoom;
            const double world_y    = event->y / self->zoom;

            self->zoom = new_zoom;

            /* After the canvas resizes for the new zoom, scroll the
             * viewport so the world point that was under the cursor
             * lands at the same viewport-relative position again
             * (clamped by the adjustment to its valid range). */
            self->zoom_anchor_hadj    = world_x * new_zoom - viewport_x;
            self->zoom_anchor_vadj    = world_y * new_zoom - viewport_y;
            self->zoom_anchor_pending = TRUE;
            if (self->zoom_anchor_idle_id == 0)
                self->zoom_anchor_idle_id = g_idle_add (
                        apply_zoom_anchor, self);

            /* Re-stamp the magnification readout so the painter holds
             * it at full opacity until the wheel stops moving.  The
             * tick is what drives the fade, so install it on the
             * first step and let on_zoom_label_tick tear it down once
             * the hold + fade window has elapsed. */
            {
                GdkFrameClock *clock =
                        gtk_widget_get_frame_clock (widget);
                self->zoom_label_shown_us = clock != NULL
                        ? gdk_frame_clock_get_frame_time (clock)
                        : g_get_monotonic_time ();
                if (self->zoom_label_tick_id == 0)
                    self->zoom_label_tick_id = gtk_widget_add_tick_callback (
                            widget, on_zoom_label_tick, NULL, NULL);
            }

            gtk_widget_queue_resize (widget);
            gtk_widget_queue_draw   (widget);
        }
        return GDK_EVENT_STOP;
    }

    /* Knob decoration: a plain wheel spin while the pointer sits over a
     * #PnKnob's dial rotates it and emits a fresh "value" message —
     * without the node having to be lifted into the zoom overlay first
     * (knobs have no plot to zoom).  Top-most node wins, mirroring the
     * press-routing order used for the switch slider below.  Only
     * consulted when nothing is zoomed so a lifted plot keeps the wheel. */
    if (self->zoomed_node == NULL && dy != 0.0)
    {
        const double wx    = widget_to_world_x (self, event->x);
        const double wy    = widget_to_world_y (self, event->y);
        const guint  count = pn_node_store_get_length (self->nodes);
        gint         i;

        for (i = (gint) count - 1; i >= 0; i--)
        {
            PnNode *node = pn_node_store_get_node (self->nodes, (guint) i);

            if (!PN_IS_KNOB (node))
                continue;
            if (!node_on_sheet (self, node))
                continue;
            if (pn_knob_hit_knob (PN_KNOB (node), wx, wy))
            {
                pn_knob_scroll (PN_KNOB (node), dy);
                gtk_widget_queue_draw (widget);
                return GDK_EVENT_STOP;
            }
        }
    }

    if (self->zoomed_node == NULL)
        return GDK_EVENT_PROPAGATE;

    /* Maximized oscilloscope: a wheel spin over a knob nudges it, the same
     * value change a vertical drag would make but without the fiddly grab —
     * the knobs are awkward to drag precisely.  One wheel tick stands in for
     * a fixed slice of drag (PN_OSC_WHEEL_STEP_PX pixels, +down), routed
     * through the very same drag_knob seam the pointer drag uses so the wheel
     * and the drag never disagree.  Only while the lift has settled. */
    if (PN_IS_OSCILLOSCOPE (self->zoomed_node) && self->zoom_dir == 0 &&
        dy != 0.0)
    {
        PnOscilloscope *osc = PN_OSCILLOSCOPE (self->zoomed_node);
        double          zx, zy, zw, zh;
        PnOscRect       crt, knobs[PN_OSC_KNOB_N], autobtn[2];
        PnOscRect       curknob[PN_OSC_CUR_N];
        int             k, ck;

        zoom_current_rect (self, &zx, &zy, &zw, &zh);
        pn_oscilloscope_layout (osc, zx, zy, zw, zh, &crt, knobs, autobtn,
                                NULL, curknob);

        k = pn_oscilloscope_hit_knob (knobs, event->x, event->y);
        if (k >= 0)
        {
            gboolean fine = pn_oscilloscope_hit_knob_fine (knobs, k,
                                                           event->x, event->y);
            pn_oscilloscope_drag_knob (osc, k, fine,
                                       dy * PN_OSC_WHEEL_STEP_PX, crt.h);
            gtk_widget_queue_draw (widget);
            return GDK_EVENT_STOP;
        }

        /* A wheel spin over a cursor position knob nudges that line, the same
         * coarse/fine seam a drag uses. */
        ck = pn_oscilloscope_hit_cursor_knob (curknob, event->x, event->y);
        if (ck >= 0)
        {
            gboolean fine = pn_oscilloscope_hit_cursor_knob_fine (
                                curknob, ck, event->x, event->y);
            pn_oscilloscope_drag_cursor_knob (osc, ck, fine,
                                              dy * PN_OSC_WHEEL_STEP_PX, crt.h);
            gtk_widget_queue_draw (widget);
            return GDK_EVENT_STOP;
        }
    }

    klass = PN_NODE_GET_CLASS (self->zoomed_node);
    if (klass->scroll == NULL)
        return GDK_EVENT_PROPAGATE;

    klass->scroll (self->zoomed_node, dy);
    return GDK_EVENT_STOP;
}

static gboolean
on_button_press (
        GtkWidget      *widget,
        GdkEventButton *event,
        gpointer        user_data)
{
    PnWorksheet *self = PN_WORKSHEET (widget);
    PnPortKind   port_kind;
    PnNode      *port_node;
    PnNode      *body_hit;
    /* Worksheet-space cursor position.  Hit tests and drag/marquee
     * state all live in worksheet coords so they survive Ctrl+wheel
     * zoom and pan; only #hit_test_zoom_rect — which targets the
     * widget-space zoom overlay — keeps the raw event coordinates. */
    const double wx = widget_to_world_x (self, event->x);
    const double wy = widget_to_world_y (self, event->y);

    (void) user_data;

    /* Take keyboard focus on every press so subsequent key events
     * (e.g. Delete to drop the selection) reach this widget. */
    gtk_widget_grab_focus (widget);

    /* Right-click: per-node menu (Configure, Delete) when the press
     * lands on a node body, per-wire menu (Delete) when it lands on a
     * wire curve, otherwise propagate. */
    if (event->button == GDK_BUTTON_SECONDARY &&
        event->type   == GDK_BUTTON_PRESS)
    {
        PnNode *hit_node = hit_test_node (self, wx, wy);
        if (hit_node != NULL)
        {
            /* The popup's Cut/Copy operate on the current selection,
             * so a right-click on an unselected node first promotes it
             * to a single-node selection.  When the click lands inside
             * an existing multi-selection the selection is left alone
             * so the menu acts on the whole group. */
            if (!selection_contains (self, hit_node))
            {
                selection_clear (self);
                selection_add   (self, hit_node);
                gtk_widget_queue_draw (widget);
            }
            show_node_popup (self, hit_node, event);
            return GDK_EVENT_STOP;
        }

        {
            PnWire *hit_wire = hit_test_wire (self, wx, wy);
            if (hit_wire != NULL)
            {
                show_wire_popup (self, hit_wire, event);
                return GDK_EVENT_STOP;
            }
        }

        /* Empty canvas: clear any existing selection (mirroring the
         * primary-press marquee start, but without sweeping a rect)
         * and pop up the background menu whose Paste lands at this
         * cursor position. */
        selection_clear (self);
        gtk_widget_queue_draw (widget);
        show_canvas_popup (self, event, wx, wy);
        return GDK_EVENT_STOP;
    }

    if (event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;

    /* Zoom overlay: while a graph is lifted off the canvas, a primary
     * press on the enlarged plot animates it back into place.  All
     * other interactions on the dimmed canvas are absorbed so the
     * user can't accidentally start a drag, marquee, or fire-button
     * press through the overlay.  The click is only meaningful once
     * the in-animation has completed (zoom_dir == 0); presses that
     * land mid-flight reverse the direction.
     *
     * Special case: when the zoomed node is a #PnChat, route presses
     * on the lifted chat's entry / Send rectangles to the chat's
     * input handlers FIRST, before treating them as a zoom-out
     * trigger — otherwise the user can never type into or send from
     * a maximised chat because every click would just collapse the
     * overlay.  The bubbles area above the strip keeps the original
     * un-zoom behaviour so the gesture of "click anywhere on the
     * lifted chat to put it back" still works for the part of the
     * chat that has no controls. */
    if (self->zoomed_node != NULL)
    {
        /* Double-click a maximized oscilloscope knob → reset it: a range/
         * offset knob returns its axis to auto-fit (the same as its "Auto"
         * button, but right on the knob); a Focus / Intensity knob returns
         * to its default. */
        if (event->type == GDK_2BUTTON_PRESS &&
            PN_IS_OSCILLOSCOPE (self->zoomed_node) && self->zoom_dir == 0 &&
            hit_test_zoom_rect (self, event->x, event->y))
        {
            PnOscilloscope *osc = PN_OSCILLOSCOPE (self->zoomed_node);
            double          zx, zy, zw, zh;
            PnOscRect       crt, knobs[PN_OSC_KNOB_N], autobtn[2];
            PnOscRect       curknob[PN_OSC_CUR_N];
            int             k, ck;

            zoom_current_rect (self, &zx, &zy, &zw, &zh);
            pn_oscilloscope_layout (osc, zx, zy, zw, zh,
                                    &crt, knobs, autobtn, NULL, curknob);

            k = pn_oscilloscope_hit_knob (knobs, event->x, event->y);
            if (k >= 0)
            {
                self->osc_drag = FALSE;   /* cancel the drag the 1st press armed */
                pn_oscilloscope_reset_knob (osc, k);
                gtk_widget_queue_draw (widget);
                return GDK_EVENT_STOP;
            }

            /* Double-clicking a cursor position knob recentres that line. */
            ck = pn_oscilloscope_hit_cursor_knob (curknob, event->x, event->y);
            if (ck >= 0)
            {
                self->osc_cknob_drag = FALSE;  /* cancel the 1st press's arm */
                pn_oscilloscope_reset_cursor_knob (osc, ck);
                gtk_widget_queue_draw (widget);
            }
            return GDK_EVENT_STOP;
        }

        if (event->type == GDK_BUTTON_PRESS &&
            hit_test_zoom_rect (self, event->x, event->y))
        {
            if (PN_IS_CHAT (self->zoomed_node))
            {
                double    zx, zy, zw, zh;
                PnChat   *chat = PN_CHAT (self->zoomed_node);
                PnChatHit hit;

                zoom_current_rect (self, &zx, &zy, &zw, &zh);
                hit = pn_chat_hit_input_in_rect (chat, zx, zy, zw, zh,
                                                 event->x, event->y);
                if (hit != PN_CHAT_HIT_NONE)
                {
                    /* Drop focus from any previously-focused chat
                     * that is not the one being pressed — same shape
                     * as the un-zoomed branch below. */
                    if (self->focused_chat != NULL &&
                        self->focused_chat != chat)
                    {
                        pn_chat_set_focused (self->focused_chat, FALSE);
                        self->focused_chat = NULL;
                    }

                    if (hit == PN_CHAT_HIT_ENTRY)
                    {
                        pn_chat_set_focused (chat, TRUE);
                        self->focused_chat = chat;
                    }
                    else /* PN_CHAT_HIT_SEND */
                    {
                        pn_chat_submit (chat);
                        pn_chat_set_focused (chat, TRUE);
                        self->focused_chat = chat;
                    }
                    return GDK_EVENT_STOP;
                }
            }

            /* Sun Path: a press on the lifted dome begins a drag-to-
             * rotate gesture rather than collapsing the overlay.  The
             * collapse is deferred to the release and only fires when the
             * pointer barely moved (a click, not a drag).  Only meaningful
             * once the in-animation has settled (zoom_dir == 0). */
            if (PN_IS_SUN_PATH (self->zoomed_node) && self->zoom_dir == 0)
            {
                self->sun_path_drag    = TRUE;
                self->sun_path_dragged = FALSE;
                self->sun_path_press_x = event->x;
                self->sun_path_press_y = event->y;
                self->sun_path_last_x  = event->x;
                self->sun_path_last_y  = event->y;
                return GDK_EVENT_STOP;
            }

            /* Oscilloscope: a press on a knob begins a value drag; a press
             * on an "Auto" button returns that axis to auto-fit; a press
             * anywhere else on the lifted face collapses the overlay as
             * usual.  Only meaningful once the lift has settled. */
            if (PN_IS_OSCILLOSCOPE (self->zoomed_node) && self->zoom_dir == 0)
            {
                PnOscilloscope *osc = PN_OSCILLOSCOPE (self->zoomed_node);
                double          zx, zy, zw, zh;
                PnOscRect       crt, knobs[PN_OSC_KNOB_N], autobtn[2];
                PnOscRect       curbtn[PN_OSC_CUR_N], curknob[PN_OSC_CUR_N];
                int             a, k, cb, cl, ck;

                zoom_current_rect (self, &zx, &zy, &zw, &zh);
                pn_oscilloscope_layout (osc, zx, zy, zw, zh,
                                        &crt, knobs, autobtn, curbtn, curknob);

                a = pn_oscilloscope_hit_auto (autobtn, event->x, event->y);
                if (a >= 0)
                {
                    /* Toggle: a lit "Auto" key turns its axis off (pinned at
                     * the current view); an unlit one turns auto back on. */
                    pn_oscilloscope_toggle_axis_auto (osc, a == 0);
                    gtk_widget_queue_draw (widget);
                    return GDK_EVENT_STOP;
                }

                /* A press on a "V1/V2/H1/H2" key shows or hides that cursor. */
                cb = pn_oscilloscope_hit_cursor_button (curbtn,
                                                        event->x, event->y);
                if (cb >= 0)
                {
                    pn_oscilloscope_toggle_cursor (osc, cb);
                    gtk_widget_queue_draw (widget);
                    return GDK_EVENT_STOP;
                }

                /* A press on a shown cursor LINE grabs it for dragging. */
                cl = pn_oscilloscope_hit_cursor (osc, &crt,
                                                 event->x, event->y);
                if (cl >= 0)
                {
                    self->osc_cursor_drag = TRUE;
                    self->osc_cursor      = cl;
                    pn_oscilloscope_drag_cursor_to (osc, cl, &crt,
                                                    event->x, event->y);
                    gtk_widget_queue_draw (widget);
                    return GDK_EVENT_STOP;
                }

                /* A press on a cursor POSITION knob begins a coarse/fine drag
                 * of that line (and reveals a hidden one on the first turn). */
                ck = pn_oscilloscope_hit_cursor_knob (curknob,
                                                      event->x, event->y);
                if (ck >= 0)
                {
                    self->osc_cknob_drag   = TRUE;
                    self->osc_cknob        = ck;
                    self->osc_cknob_fine   = pn_oscilloscope_hit_cursor_knob_fine (
                                                 curknob, ck, event->x, event->y);
                    self->osc_cknob_last_y = event->y;
                    return GDK_EVENT_STOP;
                }

                k = pn_oscilloscope_hit_knob (knobs, event->x, event->y);
                if (k >= 0)
                {
                    pn_oscilloscope_begin_knob (osc, k);
                    self->osc_drag    = TRUE;
                    self->osc_knob    = k;
                    self->osc_fine    = pn_oscilloscope_hit_knob_fine (
                                            knobs, k, event->x, event->y);
                    self->osc_press_x = event->x;
                    self->osc_press_y = event->y;
                    self->osc_last_x  = event->x;
                    self->osc_last_y  = event->y;
                    return GDK_EVENT_STOP;
                }
                /* not on a control → fall through to collapse */
            }

            zoom_start_animation (self, -1);
        }
        return GDK_EVENT_STOP;
    }

    /* Chat node bottom-row controls: a primary press on the entry
     * rectangle of a #PnChat focuses it for canvas-resident text
     * input (the worksheet's key-press handler then routes Enter /
     * Backspace / printable characters into the chat).  A press on
     * the inline Send button submits the current draft immediately.
     * Routed before #hit_test_node_plot so the entry / Send wins
     * over the surrounding bubbles area. */
    if (event->type == GDK_BUTTON_PRESS)
    {
        PnChatHit hit  = PN_CHAT_HIT_NONE;
        PnChat   *chat = hit_test_chat_input (self, wx, wy, &hit);
        if (chat != NULL)
        {
            /* Defocus any previously-focused chat unless this press
             * is on the same chat's entry — a click on the same
             * entry rectangle should keep typing live. */
            if (self->focused_chat != NULL &&
                self->focused_chat != chat)
            {
                pn_chat_set_focused (self->focused_chat, FALSE);
                self->focused_chat = NULL;
            }

            if (hit == PN_CHAT_HIT_ENTRY)
            {
                pn_chat_set_focused (chat, TRUE);
                self->focused_chat = chat;
            }
            else if (hit == PN_CHAT_HIT_SEND)
            {
                pn_chat_submit (chat);
                /* Submitting keeps the chat focused so the user can
                 * keep typing the next message without a second
                 * click. */
                pn_chat_set_focused (chat, TRUE);
                self->focused_chat = chat;
            }
            return GDK_EVENT_STOP;
        }
    }

    /* Any other primary press releases the focused chat — clicks
     * outside the entry strip should drop typing focus the same way
     * a GtkEntry loses focus when the user clicks elsewhere. */
    if (event->type == GDK_BUTTON_PRESS && self->focused_chat != NULL)
    {
        pn_chat_set_focused (self->focused_chat, FALSE);
        self->focused_chat = NULL;
    }

    /* Plot extension: a primary press on the (un-zoomed) plot of any
     * node that exposes a #PnNodeClass.paint_plot vfunc lifts it off
     * the canvas into a centred enlarged rectangle.  The press is
     * consumed here so it never falls through to the empty-space
     * marquee branch below. */
    if (event->type == GDK_BUTTON_PRESS)
    {
        PnNode *node = hit_test_node_plot (self, wx, wy);
        if (node != NULL)
        {
            self->zoomed_node = node;
            self->zoom_t      = 0.0;
            zoom_start_animation (self, +1);
            return GDK_EVENT_STOP;
        }
    }

    /* Inject node's fire button.  Intercept any primary press over
     * the button area — including the synthetic GDK_2BUTTON_PRESS
     * event delivered after a double-click — so the press never
     * falls through to the empty-space branch and starts a marquee.
     * Only the first press of each click fires the inject and arms
     * the inset feedback; subsequent press events from the same
     * click sequence are absorbed. */
    {
        PnInject *inject = hit_test_inject_button (self, wx, wy);
        if (inject != NULL)
        {
            if (event->type == GDK_BUTTON_PRESS)
            {
                g_clear_object (&self->pressed_inject);
                self->pressed_inject = g_object_ref (inject);
                pn_inject_fire (inject);
                gtk_widget_queue_draw (widget);
            }
            return GDK_EVENT_STOP;
        }
    }

    /* PnSwitch's slide-switch decoration.  A primary press on the
     * slider toggles the latched state and emits a fresh "value"
     * message; the synthetic 2BUTTON_PRESS that follows a double-
     * click is absorbed so a fast double-click flips the switch only
     * once (mirroring how PnInject treats its fire button). */
    if (event->type == GDK_BUTTON_PRESS)
    {
        PnSwitch *toggled = NULL;
        const guint count = pn_node_store_get_length (self->nodes);
        gint i;

        for (i = (gint) count - 1; i >= 0; i--)
        {
            PnNode *node = pn_node_store_get_node (self->nodes, (guint) i);

            if (!PN_IS_SWITCH (node))
                continue;
            if (!node_on_sheet (self, node))
                continue;
            if (pn_switch_hit_slider (PN_SWITCH (node), wx, wy))
            {
                toggled = PN_SWITCH (node);
                break;
            }
        }

        if (toggled != NULL)
        {
            pn_switch_toggle (toggled);
            gtk_widget_queue_draw (widget);
            return GDK_EVENT_STOP;
        }
    }
    else if (event->type == GDK_2BUTTON_PRESS)
    {
        const guint count = pn_node_store_get_length (self->nodes);
        gint i;

        for (i = (gint) count - 1; i >= 0; i--)
        {
            PnNode *node = pn_node_store_get_node (self->nodes, (guint) i);

            if (!PN_IS_SWITCH (node))
                continue;
            if (!node_on_sheet (self, node))
                continue;
            if (pn_switch_hit_slider (PN_SWITCH (node), wx, wy))
                return GDK_EVENT_STOP;
        }
    }

    /* Double-click on a node body opens the property editor.  Ports
     * are deliberately not considered here so a double-click on a
     * port still cancels into the wire-drag flow. */
    if (event->type == GDK_2BUTTON_PRESS)
    {
        PnNode *hit = hit_test_node (self, wx, wy);
        if (hit != NULL)
        {
            GtkWidget *toplevel = gtk_widget_get_toplevel (widget);
            GtkWindow *parent   = GTK_IS_WINDOW (toplevel)
                                      ? GTK_WINDOW (toplevel) : NULL;
            GtkWidget *dialog   = pn_node_dialog_new (parent, hit);

            node_dialog_track_history (dialog, self);

            /* The single-press that preceded this double-click may
             * have armed a drag; cancel it so releasing the dialog
             * does not snap the node to a stale offset. */
            drag_end (self);
            g_clear_object (&self->wire_source);
            g_clear_object (&self->wire_dest);

            g_signal_connect_swapped (dialog, "response",
                                      G_CALLBACK (gtk_widget_destroy),
                                      dialog);
            gtk_widget_show_all (dialog);
            return GDK_EVENT_STOP;
        }
    }

    /* Port hit takes priority over body hit so the user can pull a
     * wire out of a port without first dragging the node.  A drag may
     * start from either end: from an output it runs toward an input
     * (wire_source), from an input it runs toward an output
     * (wire_dest) — both resolve to the same source-output→target-input
     * wire on release. */
    {
        gint port_input = 0;
        port_node = hit_test_port (self, wx, wy, &port_kind, &port_input);
        if (port_node != NULL && port_kind == PN_PORT_OUTPUT)
        {
            self->wire_source   = g_object_ref (port_node);
            self->wire_cursor_x = wx;
            self->wire_cursor_y = wy;
            gtk_widget_queue_draw (widget);
            return GDK_EVENT_STOP;
        }
        if (port_node != NULL && port_kind == PN_PORT_INPUT)
        {
            self->wire_dest       = g_object_ref (port_node);
            self->wire_dest_input = port_input;
            self->wire_cursor_x   = wx;
            self->wire_cursor_y   = wy;
            gtk_widget_queue_draw (widget);
            return GDK_EVENT_STOP;
        }
    }

    /* Comment-box resize: a press on a selected comment's handle starts a
     * resize drag instead of selecting / moving.  Search front-to-back so
     * the topmost selected comment wins; only selected comments draw
     * handles, so only those are resizable. */
    {
        const guint count = pn_node_store_get_length (self->nodes);
        gint        i;

        for (i = (gint) count - 1; i >= 0; i--)
        {
            PnNode *n = pn_node_store_get_node (self->nodes, (guint) i);
            int     edge;

            if (!PN_IS_COMMENT (n) || !node_on_sheet (self, n)
                || !selection_contains (self, n))
                continue;

            edge = hit_test_comment_handle (PN_COMMENT (n), wx, wy);
            if (edge == PN_RESIZE_NONE)
                continue;

            {
                const PnPoint *p = pn_node_get_position (n);
                double         w, h;

                pn_node_get_size (n, &w, &h);
                g_clear_object (&self->resize_comment);
                self->resize_comment  = g_object_ref (n);
                self->resize_edge     = edge;
                self->resize_press_x  = wx;
                self->resize_press_y  = wy;
                self->resize_anchor_x = p->x;
                self->resize_anchor_y = p->y;
                self->resize_anchor_w = w;
                self->resize_anchor_h = h;
                /* Coalesce the whole resize into one undo step. */
                pn_flow_history_freeze (self->flow);
            }
            return GDK_EVENT_STOP;
        }
    }

    body_hit = hit_test_node (self, wx, wy);
    if (body_hit == NULL)
    {
        /* Empty space: clear any existing selection and start a
         * rubber-band marquee at the press position. */
        selection_clear (self);
        self->marquee_active = TRUE;
        self->marquee_x0 = self->marquee_x1 = wx;
        self->marquee_y0 = self->marquee_y1 = wy;
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    /* Body hit: ensure body_hit is part of the selection (replacing
     * the selection when it was not), then start a group drag of
     * everything currently selected. */
    if (!selection_contains (self, body_hit))
    {
        selection_clear (self);
        selection_add (self, body_hit);
    }

    drag_begin (self, body_hit, wx, wy);
    gtk_widget_queue_draw (widget);
    return GDK_EVENT_STOP;
}

static gboolean
on_motion_notify (
        GtkWidget      *widget,
        GdkEventMotion *event,
        gpointer        user_data)
{
    PnWorksheet *self = PN_WORKSHEET (widget);
    const double wx   = widget_to_world_x (self, event->x);
    const double wy   = widget_to_world_y (self, event->y);

    (void) user_data;

    /* Sun Path orbit drag: while a lifted dome is being dragged, feed
     * the per-frame pointer delta to the camera — horizontal spins the
     * yaw (full circle), vertical tips the pitch.  Dragging up looks more
     * top-down, so an upward (negative) dy raises the pitch. */
    if (self->sun_path_drag && self->zoomed_node != NULL &&
        PN_IS_SUN_PATH (self->zoomed_node))
    {
        const double rot_per_px = 0.5;
        double dx = event->x - self->sun_path_last_x;
        double dy = event->y - self->sun_path_last_y;

        self->sun_path_last_x = event->x;
        self->sun_path_last_y = event->y;

        if (ABS (event->x - self->sun_path_press_x) > 3.0 ||
            ABS (event->y - self->sun_path_press_y) > 3.0)
            self->sun_path_dragged = TRUE;

        pn_sun_path_rotate (PN_SUN_PATH (self->zoomed_node),
                            dx * rot_per_px, -dy * rot_per_px);
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    /* Oscilloscope cursor drag: move the grabbed reference line so it follows
     * the pointer (mapped back to a data value by the core). */
    if (self->osc_cursor_drag && self->zoomed_node != NULL &&
        PN_IS_OSCILLOSCOPE (self->zoomed_node))
    {
        PnOscilloscope *osc = PN_OSCILLOSCOPE (self->zoomed_node);
        double          zx, zy, zw, zh;
        PnOscRect       crt, knobs[PN_OSC_KNOB_N], autobtn[2];

        zoom_current_rect (self, &zx, &zy, &zw, &zh);
        pn_oscilloscope_layout (osc, zx, zy, zw, zh,
                                &crt, knobs, autobtn, NULL, NULL);

        pn_oscilloscope_drag_cursor_to (osc, self->osc_cursor, &crt,
                                        event->x, event->y);
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    /* Oscilloscope cursor-knob drag: feed the vertical delta to the grabbed
     * cursor position knob, the coarse ring or its fine inner vernier. */
    if (self->osc_cknob_drag && self->zoomed_node != NULL &&
        PN_IS_OSCILLOSCOPE (self->zoomed_node))
    {
        PnOscilloscope *osc = PN_OSCILLOSCOPE (self->zoomed_node);
        double          zx, zy, zw, zh;
        PnOscRect       crt, knobs[PN_OSC_KNOB_N], autobtn[2];
        double          dy = event->y - self->osc_cknob_last_y;

        self->osc_cknob_last_y = event->y;

        zoom_current_rect (self, &zx, &zy, &zw, &zh);
        pn_oscilloscope_layout (osc, zx, zy, zw, zh,
                                &crt, knobs, autobtn, NULL, NULL);

        pn_oscilloscope_drag_cursor_knob (osc, self->osc_cknob,
                                          self->osc_cknob_fine, dy, crt.h);
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    /* Oscilloscope knob drag: feed the vertical delta to the grabbed knob.
     * The CRT height (from the live layout) scales the offset knobs so one
     * screen-height of drag pans exactly one screenful. */
    if (self->osc_drag && self->zoomed_node != NULL &&
        PN_IS_OSCILLOSCOPE (self->zoomed_node))
    {
        PnOscilloscope *osc = PN_OSCILLOSCOPE (self->zoomed_node);
        double          zx, zy, zw, zh;
        PnOscRect       crt, knobs[PN_OSC_KNOB_N], autobtn[2];
        double          dy = event->y - self->osc_last_y;

        self->osc_last_x = event->x;
        self->osc_last_y = event->y;

        zoom_current_rect (self, &zx, &zy, &zw, &zh);
        pn_oscilloscope_layout (osc, zx, zy, zw, zh,
                                &crt, knobs, autobtn, NULL, NULL);

        pn_oscilloscope_drag_knob (osc, self->osc_knob, self->osc_fine,
                                   dy, crt.h);
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    /* Comment-box resize drag: recompute the box geometry from the
     * anchor (its footprint at press) plus the pointer delta.  The
     * dragged edge moves; the opposite edge stays put.  Left / top edges
     * also move the box origin.  Everything is grid-snapped and clamped
     * to the minimum size. */
    if (self->resize_comment != NULL)
    {
        const int    edge = self->resize_edge;
        const double dx   = wx - self->resize_press_x;
        const double dy   = wy - self->resize_press_y;
        const double ax   = self->resize_anchor_x;
        const double ay   = self->resize_anchor_y;
        const double aw   = self->resize_anchor_w;
        const double ah   = self->resize_anchor_h;
        double       nx = ax, ny = ay, nw = aw, nh = ah;
        PnComment   *comment = PN_COMMENT (self->resize_comment);
        PnPoint      pos;

        if (edge & PN_RESIZE_RIGHT)
            nw = snap_to_grid (ax + aw + dx) - ax;
        if (edge & PN_RESIZE_BOTTOM)
            nh = snap_to_grid (ay + ah + dy) - ay;
        if (edge & PN_RESIZE_LEFT)
        {
            nx = snap_to_grid (ax + dx);
            nw = (ax + aw) - nx;
        }
        if (edge & PN_RESIZE_TOP)
        {
            ny = snap_to_grid (ay + dy);
            nh = (ay + ah) - ny;
        }

        /* Clamp to the minimum size; a shrunk left/top edge stops at the
         * fixed opposite edge rather than crossing it. */
        if (nw < PN_COMMENT_MIN_WIDTH)
        {
            if (edge & PN_RESIZE_LEFT)
                nx = (ax + aw) - PN_COMMENT_MIN_WIDTH;
            nw = PN_COMMENT_MIN_WIDTH;
        }
        if (nh < PN_COMMENT_MIN_HEIGHT)
        {
            if (edge & PN_RESIZE_TOP)
                ny = (ay + ah) - PN_COMMENT_MIN_HEIGHT;
            nh = PN_COMMENT_MIN_HEIGHT;
        }

        pos.x = nx;
        pos.y = ny;
        pn_node_set_position (PN_NODE (comment), &pos);
        pn_comment_set_size (comment, nw, nh);
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    if (self->wire_source != NULL || self->wire_dest != NULL)
    {
        self->wire_cursor_x = wx;
        self->wire_cursor_y = wy;
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    if (self->marquee_active)
    {
        self->marquee_x1 = wx;
        self->marquee_y1 = wy;
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    if (self->drag_pivot == NULL)
        return GDK_EVENT_PROPAGATE;

    /* Snap the pivot to the grid the same way the single-node drag
     * did, then translate every selected node by the same delta from
     * its anchor so relative spacing within the group is preserved
     * and floating-point error does not accumulate across frames. */
    {
        PnDragAnchor *pivot_anchor = NULL;
        double new_pivot_x = snap_to_grid (wx - self->drag_offset_x);
        double new_pivot_y = snap_to_grid (wy - self->drag_offset_y);
        double dx, dy;
        guint  i;

        for (i = 0; i < self->drag_anchors->len; i++)
        {
            PnDragAnchor *a = &g_array_index (self->drag_anchors,
                                              PnDragAnchor, i);
            if (a->node == self->drag_pivot)
            {
                pivot_anchor = a;
                break;
            }
        }
        if (pivot_anchor == NULL)
            return GDK_EVENT_STOP;

        dx = new_pivot_x - pivot_anchor->ax;
        dy = new_pivot_y - pivot_anchor->ay;

        for (i = 0; i < self->drag_anchors->len; i++)
        {
            PnDragAnchor *a    = &g_array_index (self->drag_anchors,
                                                 PnDragAnchor, i);
            PnPoint       next = { a->ax + dx, a->ay + dy };
            pn_node_set_position (a->node, &next);
        }

        gtk_widget_queue_draw (widget);
    }

    return GDK_EVENT_STOP;
}

static gboolean
on_button_release (
        GtkWidget      *widget,
        GdkEventButton *event,
        gpointer        user_data)
{
    PnWorksheet *self = PN_WORKSHEET (widget);

    (void) user_data;

    if (event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;

    /* End a Sun Path orbit drag.  A press that barely moved is a click,
     * so collapse the overlay; a real drag just settles, leaving the
     * rotated view lifted. */
    if (self->sun_path_drag)
    {
        gboolean dragged = self->sun_path_dragged;

        self->sun_path_drag    = FALSE;
        self->sun_path_dragged = FALSE;

        if (!dragged && self->zoomed_node != NULL && self->zoom_dir == 0)
            zoom_start_animation (self, -1);
        return GDK_EVENT_STOP;
    }

    /* End an oscilloscope cursor drag.  The overlay stays lifted. */
    if (self->osc_cursor_drag)
    {
        self->osc_cursor_drag = FALSE;
        return GDK_EVENT_STOP;
    }

    /* End an oscilloscope cursor-knob drag.  The overlay stays lifted. */
    if (self->osc_cknob_drag)
    {
        self->osc_cknob_drag = FALSE;
        return GDK_EVENT_STOP;
    }

    /* End an oscilloscope knob drag.  The overlay stays lifted (the user is
     * adjusting the scope, not dismissing it); a plain click elsewhere on
     * the face still collapses it via the press handler's fall-through. */
    if (self->osc_drag)
    {
        self->osc_drag = FALSE;
        return GDK_EVENT_STOP;
    }

    /* End a comment-box resize: close the coalesced undo span. */
    if (self->resize_comment != NULL)
    {
        resize_end (self);
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    /* Inject buttons fire on press; release just drops the
     * pressed-down feedback so the tab pops back out.  Also cancel
     * any marquee that an intervening synthetic press might have
     * armed, so the rectangle never outlives the click. */
    if (self->pressed_inject != NULL)
    {
        g_clear_object (&self->pressed_inject);
        self->marquee_active = FALSE;
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    if (self->wire_source != NULL)
    {
        PnPortKind   kind;
        gint         target_input = 0;
        const double wx     = widget_to_world_x (self, event->x);
        const double wy     = widget_to_world_y (self, event->y);
        PnNode      *target = hit_test_port (self, wx, wy, &kind, &target_input);

        /* Only commit when the release lands on a different node's
         * input port — self-loops, stray drops, and duplicates of an
         * existing source→target wire are silently cancelled.  Cross-
         * sheet wires are also forbidden by design (the message
         * runtime is sheet-agnostic, but a wire whose endpoints live
         * on different sheets would be invisible to both per-sheet
         * widgets and surprise the user); defensively reject them
         * here even though hit_test_port already filters non-active
         * sheets out, so pasting or future plumbing cannot sneak one
         * in.  The duplicate check keys on the target *port*, so the
         * same source may feed two different inputs of a multi-input
         * node while a second identical wire is still rejected. */
        if (target != NULL && kind == PN_PORT_INPUT &&
            target != self->wire_source &&
            node_on_sheet (self, self->wire_source) &&
            node_on_sheet (self, target) &&
            !wire_exists (self, self->wire_source, target, target_input))
        {
            PnWire *wire = pn_wire_new_full (self->wire_source, target,
                                             target_input);
            pn_wire_store_add (self->wires, wire);
            g_object_unref (wire);
        }

        g_clear_object (&self->wire_source);
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    /* Reverse drag: started on an input port, so the release must land
     * on a different node's output port.  The committed wire still runs
     * source-output → target-input, so the dropped-on output becomes the
     * source and the grabbed input stays the target.  Same self-loop,
     * cross-sheet, and duplicate guards as the forward direction. */
    if (self->wire_dest != NULL)
    {
        PnPortKind   kind;
        const double wx     = widget_to_world_x (self, event->x);
        const double wy     = widget_to_world_y (self, event->y);
        PnNode      *source = hit_test_port (self, wx, wy, &kind, NULL);

        if (source != NULL && kind == PN_PORT_OUTPUT &&
            source != self->wire_dest &&
            node_on_sheet (self, source) &&
            node_on_sheet (self, self->wire_dest) &&
            !wire_exists (self, source, self->wire_dest,
                          self->wire_dest_input))
        {
            PnWire *wire = pn_wire_new_full (source, self->wire_dest,
                                             self->wire_dest_input);
            pn_wire_store_add (self->wires, wire);
            g_object_unref (wire);
        }

        g_clear_object (&self->wire_dest);
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    if (self->marquee_active)
    {
        guint count = pn_node_store_get_length (self->nodes);
        guint i;

        for (i = 0; i < count; i++)
        {
            PnNode *node = pn_node_store_get_node (self->nodes, i);
            if (!node_on_sheet (self, node))
                continue;
            if (marquee_overlaps_node (self, node))
                selection_add (self, node);
        }

        self->marquee_active = FALSE;
        gtk_widget_queue_draw (widget);
        return GDK_EVENT_STOP;
    }

    if (self->drag_pivot == NULL)
        return GDK_EVENT_PROPAGATE;

    drag_end (self);
    return GDK_EVENT_STOP;
}

/* ------------------------------------------------------------------ */
/*  Key bindings                                                       */
/* ------------------------------------------------------------------ */

/** Delete every node currently in the selection.  The selection is
 *  snapshotted into a list before any deletion runs so the
 *  node-removed handler can prune entries from @self->selection
 *  without disturbing iteration. */
static void
delete_selected_nodes (PnWorksheet *self)
{
    GHashTableIter iter;
    gpointer       key;
    GList         *victims = NULL;
    GList         *l;

    g_hash_table_iter_init (&iter, self->selection);
    while (g_hash_table_iter_next (&iter, &key, NULL))
        victims = g_list_prepend (victims, g_object_ref (key));

    for (l = victims; l != NULL; l = l->next)
        pn_worksheet_delete_node (self, PN_NODE (l->data));

    g_list_free_full (victims, g_object_unref);
}

static gboolean
on_key_press (
        GtkWidget   *widget,
        GdkEventKey *event,
        gpointer     user_data)
{
    PnWorksheet *self = PN_WORKSHEET (widget);

    (void) user_data;

    /* Chat keyboard input takes priority over every other shortcut:
     * while a chat is focused the user is typing into its entry
     * strip and Backspace / Delete / printable characters must reach
     * the chat instead of running the worksheet's "delete selected
     * nodes" path.  pn_chat_handle_key_press() returns FALSE for
     * keystrokes it doesn't consume (e.g. Ctrl-modified accelerators
     * the worksheet may want to handle), so propagation still works
     * for those. */
    if (self->focused_chat != NULL)
    {
        if (pn_chat_handle_key_press (self->focused_chat, event))
        {
            /* Escape clears focus from inside the chat — drop our
             * borrowed pointer so the next keystroke falls through
             * to the standard handlers below. */
            if (!pn_chat_get_focused (self->focused_chat))
                self->focused_chat = NULL;
            return GDK_EVENT_STOP;
        }
    }

    if (event->keyval == GDK_KEY_Delete ||
        event->keyval == GDK_KEY_KP_Delete)
    {
        if (g_hash_table_size (self->selection) == 0)
            return GDK_EVENT_PROPAGATE;

        delete_selected_nodes (self);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

/* ------------------------------------------------------------------ */
/*  Drop target — files dropped from the desktop / a file manager      */
/* ------------------------------------------------------------------ */

/** Route a text/uri-list drop to the #PnFileDrop node under the
 *  cursor.  Each local-file URI is resolved to a path and handed to
 *  pn_filedrop_drop_file(), which loads it, updates the preview, and
 *  emits the message.  A drop that lands on no file-drop node (or
 *  carries no usable local files) is reported as unsuccessful so the
 *  source app can show the "rejected" cursor. */
static void
handle_file_uri_drop (
        PnWorksheet      *self,
        GdkDragContext   *context,
        gint              x,
        gint              y,
        GtkSelectionData *data,
        guint             time)
{
    gchar    **uris;
    PnNode    *target;
    gboolean   delivered = FALSE;
    guint      i;

    uris = gtk_selection_data_get_uris (data);
    if (uris == NULL)
    {
        gtk_drag_finish (context, FALSE, FALSE, time);
        return;
    }

    /* Drop coordinates arrive in widget space; convert to worksheet
     * space so the hit test honours the current zoom / pan. */
    target = hit_test_filedrop (self,
                                widget_to_world_x (self, (double) x),
                                widget_to_world_y (self, (double) y));
    if (target == NULL)
    {
        g_strfreev (uris);
        gtk_drag_finish (context, FALSE, FALSE, time);
        return;
    }

    for (i = 0; uris[i] != NULL; i++)
    {
        gchar *path = g_filename_from_uri (uris[i], NULL, NULL);
        if (path == NULL)
            continue;   /* skip non-local (http://, …) URIs */

        pn_filedrop_drop_file (PN_FILEDROP (target), path);
        delivered = TRUE;
        g_free (path);
    }

    g_strfreev (uris);
    gtk_drag_finish (context, delivered, FALSE, time);
}

/* ------------------------------------------------------------------ */
/*  Drop target — receive new nodes from the palette                  */
/* ------------------------------------------------------------------ */

/** Tear down any live drag-preview wireframe and repaint over it.
 *  Called when the drag leaves the worksheet and just before a drop is
 *  committed, so the red ghost never lingers behind the real node. */
static void
drag_preview_clear (PnWorksheet *self)
{
    if (self->drag_preview_node != NULL)
    {
        g_clear_object (&self->drag_preview_node);
        gtk_widget_queue_draw (GTK_WIDGET (self));
    }
}

static void
on_drag_data_received (
        GtkWidget        *widget,
        GdkDragContext   *context,
        gint              x,
        gint              y,
        GtkSelectionData *data,
        guint             info,
        guint             time,
        gpointer          user_data)
{
    PnWorksheet  *self = PN_WORKSHEET (widget);
    const guchar *raw;
    gint          len;
    gchar        *type_name;
    GType         type;
    PnNode       *node;
    PnPoint       pos;

    (void) user_data;

    /* The drop is committing — drop any preview ghost so it can't
     * linger behind the real node (drag-leave usually clears it first,
     * but be defensive). */
    drag_preview_clear (self);

    /* A desktop file drop is a different payload shape (a uri-list, not
     * a node-type name) handled entirely by its own routine. */
    if (info == PN_WS_DND_INFO_URI)
    {
        handle_file_uri_drop (self, context, x, y, data, time);
        return;
    }

    raw = gtk_selection_data_get_data   (data);
    len = gtk_selection_data_get_length (data);

    if (raw == NULL || len <= 0)
    {
        gtk_drag_finish (context, FALSE, FALSE, time);
        return;
    }

    type_name = g_strndup ((const gchar *) raw, len);
    type      = pn_node_factory_lookup (pn_node_factory_get_default (),
                                        type_name);

    if (type == G_TYPE_INVALID || !g_type_is_a (type, PN_TYPE_NODE))
    {
        g_warning ("pn-worksheet: drop payload \"%s\" is not a known node type",
                   type_name);
        g_free (type_name);
        gtk_drag_finish (context, FALSE, FALSE, time);
        return;
    }
    g_free (type_name);

    node = pn_node_factory_create_for_type (
            pn_node_factory_get_default (), type);

    /* Drop the node centred under the pointer, snapped to the grid.
     * Query the freshly-built instance for its size so variable-size
     * nodes (graphs etc.) are still centred on the cursor.  The drop
     * coordinates arrive in widget space; convert to worksheet space
     * so the new node lands under the cursor regardless of the
     * current canvas zoom or pan. */
    {
        double       dw, dh;
        const double wx = widget_to_world_x (self, (double) x);
        const double wy = widget_to_world_y (self, (double) y);
        pn_node_get_size (PN_NODE (node), &dw, &dh);
        pos.x = snap_to_grid (wx - dw / 2.0);
        pos.y = snap_to_grid (wy - dh / 2.0);
    }
    pn_node_set_position (PN_NODE (node), &pos);

    /* The drop landed on this widget's sheet — tag the new node with
     * the same sheet name so the multi-sheet host can keep showing it
     * here.  (pn_node_set_worksheet collapses NULL/empty back to
     * "Worksheet" itself, so a worksheet whose sheet_name is somehow
     * absent still ends up tagged sensibly.) */
    if (self->sheet_name != NULL)
        pn_node_set_worksheet (PN_NODE (node), self->sheet_name);

    pn_flow_add_node (self->flow, PN_NODE (node));
    g_object_unref (node);

    gtk_drag_finish (context, TRUE, FALSE, time);
}

/** Track a palette drag hovering over the worksheet so the painter can
 *  ghost the node under the cursor.  The dragged node type is read
 *  straight off the source #PnPalette (this is a same-app drag), so no
 *  selection round-trip is needed — fetching the payload with
 *  gtk_drag_get_data() here would risk spinning a nested main loop
 *  while the DnD pointer grab is held and freezing the display.  Only a
 *  palette node-type drag gets a wireframe; a cross-app file-URI drag
 *  has no source widget here and is left to the default highlight.
 *  Returns %FALSE so the GTK_DEST_DEFAULT_MOTION machinery still sets
 *  the drag status. */
static gboolean
on_drag_motion (
        GtkWidget      *widget,
        GdkDragContext *context,
        gint            x,
        gint            y,
        guint           time,
        gpointer        user_data)
{
    PnWorksheet *self = PN_WORKSHEET (widget);
    GtkWidget   *src;
    GType        type = G_TYPE_INVALID;

    (void) time;
    (void) user_data;

    /* Walk up from the drag source to its palette (the source widget is
     * the palette's tree view); a cross-app drag has no source widget. */
    src = gtk_drag_get_source_widget (context);
    while (src != NULL && !PN_IS_PALETTE (src))
        src = gtk_widget_get_parent (src);
    if (src != NULL)
        type = pn_palette_get_drag_node_type (PN_PALETTE (src));

    if (type == G_TYPE_INVALID || !g_type_is_a (type, PN_TYPE_NODE))
    {
        drag_preview_clear (self);   /* not a node drag — no ghost */
        return FALSE;
    }

    self->drag_preview_x = x;
    self->drag_preview_y = y;

    /* Build the throwaway instance the painter measures, rebuilding
     * only if the dragged type changed (it does not within one drag,
     * but stays correct across drags). */
    if (self->drag_preview_node == NULL ||
        G_OBJECT_TYPE (self->drag_preview_node) != type)
    {
        g_clear_object (&self->drag_preview_node);
        self->drag_preview_node = pn_node_factory_create_for_type (
                pn_node_factory_get_default (), type);
    }

    gtk_widget_queue_draw (widget);
    return FALSE;
}

/** Drop the preview when the drag leaves (GTK also emits this just
 *  before a drop, which is exactly when the ghost should give way to
 *  the committed node). */
static void
on_drag_leave (
        GtkWidget      *widget,
        GdkDragContext *context,
        guint           time,
        gpointer        user_data)
{
    (void) context;
    (void) time;
    (void) user_data;

    drag_preview_clear (PN_WORKSHEET (widget));
}

/* ------------------------------------------------------------------ */
/*  Public API — thin forwarders to the embedded #PnFlow model        */
/* ------------------------------------------------------------------ */

void
pn_worksheet_clear (PnWorksheet *self)
{
    g_return_if_fail (PN_IS_WORKSHEET (self));

    /* The flow itself owns the modified flag and suppresses
     * "modified-changed" emissions during its own clear, so the
     * cascade of node-removed events here lands quietly. */
    pn_flow_clear (self->flow);

    gtk_widget_queue_draw (GTK_WIDGET (self));
}

PnNodeStore *
pn_worksheet_get_nodes (PnWorksheet *self)
{
    g_return_val_if_fail (PN_IS_WORKSHEET (self), NULL);
    return self->nodes;
}

PnWireStore *
pn_worksheet_get_wires (PnWorksheet *self)
{
    g_return_val_if_fail (PN_IS_WORKSHEET (self), NULL);
    return self->wires;
}

gboolean
pn_worksheet_load_from_file (
        PnWorksheet *self,
        const gchar *path,
        GError     **error)
{
    gboolean ok;

    g_return_val_if_fail (PN_IS_WORKSHEET (self), FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    /* The flow's load path mutes modified-changed across the cascade
     * itself; here we only need to repaint when it returns TRUE. */
    ok = pn_flow_load_from_file (self->flow, path, error);
    if (ok)
        gtk_widget_queue_draw (GTK_WIDGET (self));
    return ok;
}

gboolean
pn_worksheet_save_to_file (
        PnWorksheet *self,
        const gchar *path,
        GError     **error)
{
    g_return_val_if_fail (PN_IS_WORKSHEET (self), FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    return pn_flow_save_to_file (self->flow, path, error);
}

gboolean
pn_worksheet_is_modified (PnWorksheet *self)
{
    g_return_val_if_fail (PN_IS_WORKSHEET (self), FALSE);
    return pn_flow_is_modified (self->flow);
}

PnFlow *
pn_worksheet_get_flow (PnWorksheet *self)
{
    g_return_val_if_fail (PN_IS_WORKSHEET (self), NULL);
    return self->flow;
}

const gchar *
pn_worksheet_get_sheet_name (PnWorksheet *self)
{
    g_return_val_if_fail (PN_IS_WORKSHEET (self), NULL);
    return self->sheet_name;
}

void
pn_worksheet_set_sheet_name (PnWorksheet *self, const gchar *name)
{
    g_return_if_fail (PN_IS_WORKSHEET (self));
    g_return_if_fail (name != NULL && *name != '\0');

    if (g_strcmp0 (self->sheet_name, name) == 0)
        return;

    g_free (self->sheet_name);
    self->sheet_name = g_strdup (name);

    /* The content extent depends on which nodes pass the filter, so
     * a rename can grow or shrink the canvas — queue both a redraw
     * and a resize. */
    gtk_widget_queue_resize (GTK_WIDGET (self));
    gtk_widget_queue_draw   (GTK_WIDGET (self));
}

/* ------------------------------------------------------------------ */
/*  Clipboard — cut / copy / paste                                    */
/* ------------------------------------------------------------------ */

/** Get the default clipboard for this widget's display.  Falls back
 *  to the global default when the widget is not yet realised. */
static GtkClipboard *
worksheet_clipboard (PnWorksheet *self)
{
    GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (self));
    if (display == NULL)
        return gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
    return gtk_clipboard_get_for_display (display, GDK_SELECTION_CLIPBOARD);
}

gboolean
pn_worksheet_has_selection (PnWorksheet *self)
{
    g_return_val_if_fail (PN_IS_WORKSHEET (self), FALSE);
    return g_hash_table_size (self->selection) > 0;
}

/** Build a GList of the currently-selected nodes, ordered by their
 *  position in the node store so wires (which reference by name) keep
 *  a stable ordering across cut/copy events. */
static GList *
selection_to_ordered_list (PnWorksheet *self)
{
    GList *out = NULL;
    guint  count;
    guint  i;

    count = pn_node_store_get_length (self->nodes);
    for (i = 0; i < count; i++)
    {
        PnNode *node = pn_node_store_get_node (self->nodes, i);
        if (!node_on_sheet (self, node))
            continue;
        if (g_hash_table_contains (self->selection, node))
            out = g_list_prepend (out, node);
    }
    return g_list_reverse (out);
}

void
pn_worksheet_copy_selection (PnWorksheet *self)
{
    GList        *nodes;
    gchar        *json;
    GtkClipboard *cb;

    g_return_if_fail (PN_IS_WORKSHEET (self));

    if (g_hash_table_size (self->selection) == 0)
        return;

    nodes = selection_to_ordered_list (self);
    json  = pn_flow_serialize_nodes (self->flow, nodes);
    g_list_free (nodes);

    if (json == NULL)
        return;

    cb = worksheet_clipboard (self);
    gtk_clipboard_set_text (cb, json, -1);
    g_free (json);
}

void
pn_worksheet_cut_selection (PnWorksheet *self)
{
    g_return_if_fail (PN_IS_WORKSHEET (self));

    if (g_hash_table_size (self->selection) == 0)
        return;

    pn_worksheet_copy_selection (self);
    delete_selected_nodes (self);
}

/** Walk a clipboard-format JSON document and find the bounding-box
 *  top-left of the nodes it carries (taking the per-axis minimum, so
 *  the corner can come from two different nodes).  Returns %TRUE iff
 *  at least one node carried a `position` object — callers fall back
 *  to the default offset paste when no position can be read. */
static gboolean
clipboard_min_position (
        const gchar *text,
        double      *out_x,
        double      *out_y)
{
    JsonParser *parser = json_parser_new ();
    JsonNode   *root;
    JsonObject *obj;
    JsonArray  *arr;
    guint       n, i;
    double      min_x = 0.0;
    double      min_y = 0.0;
    gboolean    found = FALSE;

    if (!json_parser_load_from_data (parser, text, -1, NULL))
        goto out;

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
        goto out;

    obj = json_node_get_object (root);
    if (!json_object_has_member (obj, "nodes"))
        goto out;

    arr = json_object_get_array_member (obj, "nodes");
    if (arr == NULL)
        goto out;

    n = json_array_get_length (arr);
    for (i = 0; i < n; i++)
    {
        JsonNode   *e = json_array_get_element (arr, i);
        JsonObject *no;
        JsonObject *po;
        double      x, y;

        if (!JSON_NODE_HOLDS_OBJECT (e))
            continue;
        no = json_node_get_object (e);
        if (!json_object_has_member (no, "position"))
            continue;
        po = json_object_get_object_member (no, "position");
        if (po == NULL)
            continue;

        x = json_object_has_member (po, "x")
                ? json_object_get_double_member (po, "x") : 0.0;
        y = json_object_has_member (po, "y")
                ? json_object_get_double_member (po, "y") : 0.0;

        if (!found || x < min_x) min_x = x;
        if (!found || y < min_y) min_y = y;
        found = TRUE;
    }

out:
    g_object_unref (parser);
    if (found)
    {
        *out_x = min_x;
        *out_y = min_y;
    }
    return found;
}

static void
on_clipboard_text (
        GtkClipboard *clipboard,
        const gchar  *text,
        gpointer      user_data)
{
    PnWorksheet *self = user_data;
    GError      *error = NULL;
    GList       *added;
    GList       *l;
    double       dx = 2.0 * PN_GRID_STEP;
    double       dy = 2.0 * PN_GRID_STEP;

    (void) clipboard;

    /* The widget may have gone away while the clipboard request was
     * in flight; the strong ref taken by the requestor is what keeps
     * us alive long enough to reach this callback. */
    if (!PN_IS_WORKSHEET (self))
        goto done;

    if (text == NULL || *text == '\0')
        goto done;

    /* Worksheet-background popup armed a cursor-relative paste before
     * launching the async fetch — translate the bounding-box top-left
     * of the clipboard payload to that target so the pasted group
     * lands under the right-click cursor.  Falls back to the default
     * offset when the payload carries no parseable positions. */
    if (self->paste_at_cursor)
    {
        double min_x, min_y;
        if (clipboard_min_position (text, &min_x, &min_y))
        {
            dx = self->paste_target_x - min_x;
            dy = self->paste_target_y - min_y;
        }
        self->paste_at_cursor = FALSE;
    }

    added = pn_flow_paste_from_string (
            self->flow, text,
            dx, dy,
            &error);

    if (error != NULL)
    {
        g_warning ("pn-worksheet: paste failed: %s", error->message);
        g_error_free (error);
        goto done;
    }

    if (added == NULL)
        goto done;

    /* Retag pasted nodes onto this widget's sheet.  A clipboard
     * payload from another sheet (this window or another) would
     * otherwise carry the source sheet's name and disappear straight
     * back into invisibility on a sister tab. */
    if (self->sheet_name != NULL)
    {
        for (l = added; l != NULL; l = l->next)
            pn_node_set_worksheet (PN_NODE (l->data), self->sheet_name);
    }

    selection_clear (self);
    for (l = added; l != NULL; l = l->next)
        selection_add (self, PN_NODE (l->data));
    g_list_free (added);

    gtk_widget_queue_draw (GTK_WIDGET (self));

done:
    g_object_unref (self);
}

void
pn_worksheet_paste_from_clipboard (PnWorksheet *self)
{
    GtkClipboard *cb;

    g_return_if_fail (PN_IS_WORKSHEET (self));

    cb = worksheet_clipboard (self);
    /* Take a ref so the widget can't be finalised while the async
     * clipboard fetch is in flight; the callback drops it. */
    gtk_clipboard_request_text (cb, on_clipboard_text,
                                g_object_ref (self));
}

/* ------------------------------------------------------------------ */
/*  GObject boilerplate                                                */
/* ------------------------------------------------------------------ */

/** Disconnect every handler this worksheet attached to the shared
 *  #PnFlow / #PnNodeStore / #PnWireStore — and to each #PnNode inside
 *  the store — so an emission that lands here after the widget is
 *  destroyed does not dereference freed memory.  Lives in dispose
 *  rather than finalize so it runs before the borrowed pointers are
 *  cleared (and before any reference cycle that would otherwise pin
 *  the widget alive can be broken).  Required because the multi-
 *  worksheet host shares one flow across N PnWorksheet widgets:
 *  removing a sheet (e.g. when a fresh load through
 *  pn_flow_load_from_file replaces the default "Worksheet" tab with
 *  the file's own sheet list) destroys the stale PnWorksheet widget
 *  but leaves the shared stores alive — without this hook the next
 *  node-added emission walks the stale handler list and crashes in
 *  g_type_check_instance on the freed @self pointer. */
static void
pn_worksheet_dispose (GObject *object)
{
    PnWorksheet *self = PN_WORKSHEET (object);

    if (self->nodes != NULL)
    {
        const guint n = pn_node_store_get_length (self->nodes);
        for (guint i = 0; i < n; i++)
        {
            PnNode *node = pn_node_store_get_node (self->nodes, i);
            if (node != NULL)
                g_signal_handlers_disconnect_by_data (node, self);
        }
        g_signal_handlers_disconnect_by_data (self->nodes, self);
    }
    if (self->wires != NULL)
    {
        const guint n = pn_wire_store_get_length (self->wires);
        for (guint i = 0; i < n; i++)
        {
            PnWire *wire = pn_wire_store_get_wire (self->wires, i);
            if (wire != NULL)
                g_signal_handlers_disconnect_by_data (wire, self);
        }
        g_signal_handlers_disconnect_by_data (self->wires, self);
    }
    if (self->flow != NULL)
        g_signal_handlers_disconnect_by_data (self->flow, self);

    /* The preferences singleton outlives every worksheet -- drop our
     * notify:: handlers so a settings change after dispose cannot
     * land in queue_draw on a freed widget. */
    {
        PnPreferences *prefs = pn_preferences_get_default ();
        if (self->prefs_grid_type_handler != 0)
        {
            g_signal_handler_disconnect (prefs,
                                         self->prefs_grid_type_handler);
            self->prefs_grid_type_handler = 0;
        }
        if (self->prefs_grid_color_handler != 0)
        {
            g_signal_handler_disconnect (prefs,
                                         self->prefs_grid_color_handler);
            self->prefs_grid_color_handler = 0;
        }
        if (self->prefs_bg_handler != 0)
        {
            g_signal_handler_disconnect (prefs,
                                         self->prefs_bg_handler);
            self->prefs_bg_handler = 0;
        }
        if (self->prefs_anim_handler != 0)
        {
            g_signal_handler_disconnect (prefs,
                                         self->prefs_anim_handler);
            self->prefs_anim_handler = 0;
        }
        if (self->prefs_anim_interval_handler != 0)
        {
            g_signal_handler_disconnect (prefs,
                                         self->prefs_anim_interval_handler);
            self->prefs_anim_interval_handler = 0;
        }
        if (self->prefs_visualize_handler != 0)
        {
            g_signal_handler_disconnect (prefs,
                                         self->prefs_visualize_handler);
            self->prefs_visualize_handler = 0;
        }
    }

    G_OBJECT_CLASS (pn_worksheet_parent_class)->dispose (object);
}

static void
pn_worksheet_finalize (GObject *object)
{
    PnWorksheet *self = PN_WORKSHEET (object);

    g_clear_object  (&self->drag_pivot);
    g_clear_object  (&self->resize_comment);
    g_clear_object  (&self->drag_preview_node);
    g_clear_object  (&self->wire_source);
    g_clear_object  (&self->wire_dest);
    g_clear_object  (&self->pressed_inject);
    g_clear_pointer (&self->drag_anchors, g_array_unref);
    g_clear_pointer (&self->selection, g_hash_table_unref);

    if (self->zoom_tick_id != 0)
    {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self),
                                         self->zoom_tick_id);
        self->zoom_tick_id = 0;
    }
    if (self->pulse_tick_id != 0)
    {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self),
                                         self->pulse_tick_id);
        self->pulse_tick_id = 0;
    }
    if (self->zoom_label_tick_id != 0)
    {
        gtk_widget_remove_tick_callback (GTK_WIDGET (self),
                                         self->zoom_label_tick_id);
        self->zoom_label_tick_id = 0;
    }
    if (self->wire_pulse_timer_id != 0)
    {
        g_source_remove (self->wire_pulse_timer_id);
        self->wire_pulse_timer_id = 0;
    }
    if (self->processing_timer_id != 0)
    {
        g_source_remove (self->processing_timer_id);
        self->processing_timer_id = 0;
    }
    /* Clear func unrefs each pulse's wire. */
    g_clear_pointer (&self->wire_pulses, g_array_unref);
    self->pulse_node = NULL;
    /* The chat keyboard-focus pointer is borrowed too — drop it so a
     * dispose mid-typing-session can't leave a stale pointer for the
     * next press / key handler to dereference. */
    self->focused_chat = NULL;
    if (self->zoom_anchor_idle_id != 0)
    {
        g_source_remove (self->zoom_anchor_idle_id);
        self->zoom_anchor_idle_id = 0;
    }
    self->zoomed_node = NULL;
    /* nodes/wires are borrowed pointers into self->flow — don't unref. */
    self->nodes = NULL;
    self->wires = NULL;
    g_clear_object  (&self->flow);
    g_clear_pointer (&self->sheet_name, g_free);

    G_OBJECT_CLASS (pn_worksheet_parent_class)->finalize (object);
}

/** Compute the canvas size the drawing area should ask for, in
 *  widget pixels.  Walks every node, takes the max of (x + width)
 *  and (y + height), pads the result, scales by the current zoom,
 *  and finally clamps up to PN_CANVAS_MIN_{WIDTH,HEIGHT} so an empty
 *  worksheet still gets a sensible viewport.  Either out pointer may
 *  be %NULL. */
static void
compute_content_extent (
        PnWorksheet *self,
        gint        *out_w,
        gint        *out_h)
{
    double max_x = 0.0;
    double max_y = 0.0;

    if (self->nodes != NULL)
    {
        guint n = pn_node_store_get_length (self->nodes);
        guint i;

        for (i = 0; i < n; i++)
        {
            PnNode        *node = pn_node_store_get_node (self->nodes, i);
            const PnPoint *p    = pn_node_get_position (node);
            double         w, h;

            if (!node_on_sheet (self, node))
                continue;

            pn_node_get_size (node, &w, &h);

            if (p->x + w > max_x) max_x = p->x + w;
            if (p->y + h > max_y) max_y = p->y + h;
        }
    }

    max_x = (max_x + PN_CANVAS_PADDING) * self->zoom;
    max_y = (max_y + PN_CANVAS_PADDING) * self->zoom;

    if (max_x < PN_CANVAS_MIN_WIDTH)  max_x = PN_CANVAS_MIN_WIDTH;
    if (max_y < PN_CANVAS_MIN_HEIGHT) max_y = PN_CANVAS_MIN_HEIGHT;

    if (out_w) *out_w = (gint) ceil (max_x);
    if (out_h) *out_h = (gint) ceil (max_y);
}

static void
pn_worksheet_get_preferred_width (
        GtkWidget *widget,
        gint      *minimum,
        gint      *natural)
{
    gint w;
    compute_content_extent (PN_WORKSHEET (widget), &w, NULL);
    if (minimum) *minimum = w;
    if (natural) *natural = w;
}

static void
pn_worksheet_get_preferred_height (
        GtkWidget *widget,
        gint      *minimum,
        gint      *natural)
{
    gint h;
    compute_content_extent (PN_WORKSHEET (widget), NULL, &h);
    if (minimum) *minimum = h;
    if (natural) *natural = h;
}

static void
pn_worksheet_class_init (PnWorksheetClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->dispose          = pn_worksheet_dispose;
    object_class->finalize         = pn_worksheet_finalize;
    widget_class->draw             = pn_worksheet_draw;
    widget_class->get_preferred_width  = pn_worksheet_get_preferred_width;
    widget_class->get_preferred_height = pn_worksheet_get_preferred_height;

    /* Forwarded text from #PnDebug nodes whose target is the status
     * bar.  The main window subscribes to push the text into its
     * #GtkStatusbar. */
    signals[SIG_STATUS_MESSAGE] = g_signal_new (
            "status-message",
            PN_TYPE_WORKSHEET,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);

    /* Forwarded text from #PnDebug nodes whose target is the debug
     * view.  The main window subscribes to append a row to its
     * collapsible debug pane. */
    signals[SIG_DEBUG_MESSAGE] = g_signal_new (
            "debug-message",
            PN_TYPE_WORKSHEET,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);

    /* Fired whenever the set of selected nodes mutates.  The window
     * subscribes to toggle Cut/Copy menu and toolbar sensitivity. */
    signals[SIG_SELECTION_CHANGED] = g_signal_new (
            "selection-changed",
            PN_TYPE_WORKSHEET,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            0);

    /* Fired whenever pn_worksheet_is_modified() would return a
     * different value than it did at the last emission.  The window
     * subscribes to gate the Save toolbar button and menu item so
     * they are insensitive while the worksheet matches the on-disk
     * copy and ungate the moment the user makes any edit. */
    signals[SIG_MODIFIED_CHANGED] = g_signal_new (
            "modified-changed",
            PN_TYPE_WORKSHEET,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            0);

    /**
     * PnWorksheet::help-requested:
     * @self: the worksheet
     * @anchor: #GType name of the node whose context menu was opened
     *          (e.g. "PnRtc").  Receivers use it to locate the per-node
     *          help page <anchor>.html.
     *
     * Emitted when the user activates the "Help" entry of a node's
     * context menu.  The host window is expected to open the
     * application's help browser; the worksheet deliberately does not
     * know any help-page paths.
     */
    signals[SIG_HELP_REQUESTED] = g_signal_new (
            "help-requested",
            PN_TYPE_WORKSHEET,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);
}

/** Suppress GtkScrolledWindow's "ensure focused widget visible"
 *  behaviour for the worksheet.
 *
 *  The worksheet is hosted inside a #GtkScrolledWindow → #GtkViewport
 *  pair.  Whenever the worksheet calls #gtk_widget_grab_focus
 *  (it does so on every primary button-press so subsequent key
 *  events land here), the viewport's focus-tracking path runs
 *  #gtk_adjustment_clamp_page on its focus adjustments to bring the
 *  newly-focused child into view — which, since the worksheet
 *  occupies the viewport's entire allocation, snaps both
 *  adjustments back to 0/0 and undoes any scroll set by
 *  pn_worksheet_focus_node_by_uuid().
 *
 *  Clearing the viewport's focus_h/v_adjustment turns that
 *  auto-scroll off without affecting the user's own scrolling, so
 *  programmatic centring (and just plain "click in empty canvas")
 *  no longer reset the viewport.  We do this from "hierarchy-changed"
 *  rather than at construction time because the viewport is only
 *  attached when the widget is packed into the scrolled window. */
static void
pn_worksheet_on_hierarchy_changed (
        GtkWidget *widget,
        GtkWidget *previous_toplevel,
        gpointer   user_data)
{
    GtkWidget *parent;

    (void) previous_toplevel;
    (void) user_data;

    parent = gtk_widget_get_parent (widget);
    if (parent == NULL || !GTK_IS_VIEWPORT (parent))
        return;

    gtk_container_set_focus_hadjustment (GTK_CONTAINER (parent), NULL);
    gtk_container_set_focus_vadjustment (GTK_CONTAINER (parent), NULL);
}

/** Wire @flow into a freshly-constructed worksheet.  Called by both
 *  pn_worksheet_new() (which mints its own flow) and
 *  pn_worksheet_new_for_flow() (which receives one borrowed from a
 *  multi-sheet host).  Connects every per-store / per-flow signal
 *  the widget cares about; for a shared flow that already carries
 *  nodes, it also walks the existing store and arms the per-node
 *  visual hooks (notify::disabled, notify::position, repaint-needed)
 *  on each — those signals would normally fire from on_node_added,
 *  but the nodes were added to the flow before the widget existed. */
static void
worksheet_attach_flow (
        PnWorksheet *self,
        PnFlow      *flow,
        const gchar *sheet_name,
        gboolean     owns_flow)
{
    self->flow      = g_object_ref (flow);
    self->owns_flow = owns_flow;
    self->nodes     = pn_flow_get_nodes (self->flow);
    self->wires     = pn_flow_get_wires (self->flow);
    self->sheet_name = (sheet_name != NULL && *sheet_name != '\0')
                       ? g_strdup (sheet_name)
                       : g_strdup ("Worksheet");

    /* Repaint on any model change. */
    g_signal_connect (self->nodes, "node-added",
                      G_CALLBACK (on_nodes_changed), self);
    g_signal_connect (self->nodes, "node-removed",
                      G_CALLBACK (on_nodes_changed), self);

    /* Subscribe each new node's "notify::disabled" so toggles from
     * the dialog or context menu repaint the canvas. */
    g_signal_connect (self->nodes, "node-added",
                      G_CALLBACK (on_node_added_to_store), self);

    /* Drop selection/drag references to nodes that disappear. */
    g_signal_connect (self->nodes, "node-removed",
                      G_CALLBACK (on_node_removed_from_store), self);
    g_signal_connect (self->wires, "wire-added",
                      G_CALLBACK (on_wire_added), self);
    g_signal_connect (self->wires, "wire-removed",
                      G_CALLBACK (on_wire_removed), self);

    /* Re-emit the flow's status-message and debug-message text so
     * existing widget-side subscribers (PnWindow) keep working
     * unchanged. */
    g_signal_connect (self->flow, "status-message",
                      G_CALLBACK (on_flow_status_message), self);
    g_signal_connect (self->flow, "debug-message",
                      G_CALLBACK (on_flow_debug_message), self);

    /* Forward modified-changed up to the worksheet so the host's Save
     * UI can keep listening on the active page's widget. */
    g_signal_connect (self->flow, "modified-changed",
                      G_CALLBACK (on_flow_modified_changed), self);

    /* For a borrowed flow that already carries nodes (e.g. one that
     * just finished pn_flow_load_from_file before this widget was
     * built), arm the per-node visual hooks now — node-added will
     * never fire for them. */
    {
        const guint n = pn_node_store_get_length (self->nodes);
        guint       i;
        for (i = 0; i < n; i++)
            on_node_added_to_store (self->nodes,
                                    pn_node_store_get_node (self->nodes, i),
                                    i, self);
    }

    /* Likewise arm the "message-passed" hook on wires that already
     * exist on a borrowed flow — wire-added will not fire for them. */
    {
        const guint n = pn_wire_store_get_length (self->wires);
        guint       i;
        for (i = 0; i < n; i++)
            on_wire_added (self->wires,
                           pn_wire_store_get_wire (self->wires, i),
                           i, self);
    }
}

static void
pn_worksheet_init (PnWorksheet *self)
{
    self->selection = g_hash_table_new_full (
            g_direct_hash, g_direct_equal, g_object_unref, NULL);

    self->wire_pulses = g_array_new (FALSE, FALSE, sizeof (PnWirePulse));
    g_array_set_clear_func (self->wire_pulses,
                            (GDestroyNotify) wire_pulse_clear);

    self->zoom = 1.0;

    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

    /* Event masks for editing interactions. */
    gtk_widget_add_events (GTK_WIDGET (self),
                           GDK_BUTTON_PRESS_MASK
                           | GDK_BUTTON_RELEASE_MASK
                           | GDK_POINTER_MOTION_MASK
                           | GDK_SCROLL_MASK
                           | GDK_KEY_PRESS_MASK);
    gtk_widget_set_can_focus (GTK_WIDGET (self), TRUE);

    /* See pn_worksheet_on_hierarchy_changed: nullify the viewport's
     * focus adjustments so grab_focus from on_button_press doesn't
     * snap the scrolled window back to (0, 0). */
    g_signal_connect (self, "hierarchy-changed",
                      G_CALLBACK (pn_worksheet_on_hierarchy_changed), NULL);

    /* Drag-to-move with grid snapping. */
    g_signal_connect (self, "button-press-event",
                      G_CALLBACK (on_button_press),   NULL);
    g_signal_connect (self, "motion-notify-event",
                      G_CALLBACK (on_motion_notify),  NULL);
    g_signal_connect (self, "button-release-event",
                      G_CALLBACK (on_button_release), NULL);
    g_signal_connect (self, "key-press-event",
                      G_CALLBACK (on_key_press),      NULL);
    g_signal_connect (self, "scroll-event",
                      G_CALLBACK (on_scroll),         NULL);

    /* Accept two kinds of drop:
     *   - from the palette (same-app): payload is a node #GType name,
     *     which creates that node under the cursor;
     *   - from the desktop / a file manager (any app): a text/uri-list
     *     of dropped files, routed to the #PnFileDrop node under the
     *     cursor.
     * The info code in each #GtkTargetEntry tells on_drag_data_received
     * which payload arrived. */
    {
        GtkTargetEntry targets[] = {
            { (gchar *) pn_palette_drag_target_name (),
              GTK_TARGET_SAME_APP, PN_WS_DND_INFO_PALETTE },
            { (gchar *) "text/uri-list",
              0, PN_WS_DND_INFO_URI },
        };

        gtk_drag_dest_set (GTK_WIDGET (self),
                           GTK_DEST_DEFAULT_ALL,
                           targets, G_N_ELEMENTS (targets),
                           GDK_ACTION_COPY);

        g_signal_connect (self, "drag-data-received",
                          G_CALLBACK (on_drag_data_received), NULL);

        /* Drag-preview wireframe: motion tracks the cursor and reads
         * the dragged node-type off the source palette, leave tears the
         * ghost down.  Both return %FALSE so the GTK_DEST_DEFAULT_ALL
         * highlight/status/drop handling still runs underneath, and the
         * real node is still created by on_drag_data_received on drop. */
        g_signal_connect (self, "drag-motion",
                          G_CALLBACK (on_drag_motion), NULL);
        g_signal_connect (self, "drag-leave",
                          G_CALLBACK (on_drag_leave), NULL);
    }

    /* Repaint live when the grid-rendering preferences change.
     * snap-to-grid is consulted on demand in snap_to_grid() so no
     * redraw is needed for that one. */
    {
        PnPreferences *prefs = pn_preferences_get_default ();
        self->prefs_grid_type_handler = g_signal_connect_swapped (
                prefs, "notify::grid-type",
                G_CALLBACK (gtk_widget_queue_draw), self);
        self->prefs_grid_color_handler = g_signal_connect_swapped (
                prefs, "notify::grid-color",
                G_CALLBACK (gtk_widget_queue_draw), self);
        self->prefs_bg_handler = g_signal_connect_swapped (
                prefs, "notify::background-color",
                G_CALLBACK (gtk_widget_queue_draw), self);
        /* Not _swapped: turning the feature off has to drop in-flight
         * lights, so it needs the real handler, not a bare queue_draw. */
        self->prefs_anim_handler = g_signal_connect (
                prefs, "notify::animate-messages-on-wires",
                G_CALLBACK (on_prefs_anim_changed), self);
        self->prefs_anim_interval_handler = g_signal_connect (
                prefs, "notify::wire-pulse-interval",
                G_CALLBACK (on_prefs_anim_interval_changed), self);
        /* Not _swapped: turning the feature off has to stop the poll and
         * drop any lit halos, so it needs the real handler. */
        self->prefs_visualize_handler = g_signal_connect (
                prefs, "notify::visualize-node-processing",
                G_CALLBACK (on_prefs_visualize_changed), self);
    }
}

GtkWidget *
pn_worksheet_new (void)
{
    PnWorksheet *self = g_object_new (PN_TYPE_WORKSHEET, NULL);
    PnFlow      *flow = pn_flow_new ();
    worksheet_attach_flow (self, flow, "Worksheet", TRUE);
    g_object_unref (flow);
    return GTK_WIDGET (self);
}

GtkWidget *
pn_worksheet_new_for_flow (
        PnFlow      *flow,
        const gchar *sheet_name)
{
    PnWorksheet *self;

    g_return_val_if_fail (PN_IS_FLOW (flow), NULL);

    self = g_object_new (PN_TYPE_WORKSHEET, NULL);
    worksheet_attach_flow (self, flow, sheet_name, FALSE);
    return GTK_WIDGET (self);
}
