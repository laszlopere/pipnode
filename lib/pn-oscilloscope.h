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

#ifndef PN_OSCILLOSCOPE_H
#define PN_OSCILLOSCOPE_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnOscilloscope                                                     */
/*                                                                     */
/*  A display sink styled as an old square-screen, green-phosphor CRT  */
/*  oscilloscope (the analog-computer toy's display end — TODO #44).   */
/*  It plots a Y value against an X value, both plucked from each       */
/*  message and each allowed to be EITHER a plain number OR a           */
/*  $pnvector (TODO #43):                                               */
/*                                                                     */
/*    • value is a VECTOR  → the whole vector is the trace for this     */
/*      frame (a captured waveform), replacing what was on screen.  If  */
/*      x is also a vector it supplies the X coordinate per sample      */
/*      (parametric / Lissajous); otherwise the sample index is used.   */
/*                                                                     */
/*    • value is a NUMBER  → only the single current (x, value) point   */
/*      is shown, drawn as a dot; no history is kept (each scalar        */
/*      replaces the last).  x is the matching number, or 0 when no x    */
/*      is supplied (the dot then rides the auto-centred middle line).   */
/*                                                                     */
/*  Both axes auto-range over the visible data so the curve always      */
/*  fits the screen.  The header is the standard Node-RED card; the     */
/*  CRT screen is painted below it.                                     */
/*                                                                     */
/*  When the card is MAXIMIZED into the worksheet zoom overlay the CRT  */
/*  retreats to the top-left corner and four bench-scope knobs appear   */
/*  in the freed space (a column to its right, a strip below): X range,  */
/*  X offset, Y range, Y offset.  Turning a knob switches that axis      */
/*  from auto-fit to a manual window (centre = offset, span = range);    */
/*  an "Auto" button (or a double-click on a knob) returns it to         */
/*  auto-fit.  At rest the small card carries no controls.               */
/* ------------------------------------------------------------------ */

#define PN_TYPE_OSCILLOSCOPE (pn_oscilloscope_get_type ())

G_DECLARE_FINAL_TYPE (PnOscilloscope, pn_oscilloscope, PN, OSCILLOSCOPE, PnNode)

PnOscilloscope *pn_oscilloscope_new (void);

/* Number of scalar points currently on screen: 1 when a scalar point is
 * live, else 0 (no data yet, or a vector snapshot is showing).  A
 * read-only inspection seam for the headless tests, since the trace
 * store is private. */
guint pn_oscilloscope_get_point_count (PnOscilloscope *self);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The CRT painter lives in the gui-tier file pn-oscilloscope-gui.c,  */
/*  but the trace it draws is plain data assembled by receive() in the */
/*  GTK-free core.  The painter pulls a decimated, display-ordered     */
/*  copy of the current trace (plus the data bounds) through the        */
/*  accessors below, so the core never references GTK or cairo.        */
/* ------------------------------------------------------------------ */

/* Scalar drawing configuration, snapshotted by value for one paint.
 * The colours are #PnColor (layout-identical to GdkRGBA). */
typedef struct
{
    PnColor   screen_color;   /* CRT phosphor backdrop                  */
    PnColor   trace_color;    /* the beam / waveform                    */
    PnColor   grid_color;     /* graticule divisions                    */
    gboolean  show_graticule; /* draw the division grid                 */
    guint     trace_width;    /* beam stroke width in pixels            */
    gboolean  x_from_zero;    /* anchor the X axis at zero              */
    gboolean  y_from_zero;    /* anchor the Y axis at zero              */
} PnOscilloscopePaintState;

/**
 * pn_oscilloscope_get_paint_state:
 * @self: oscilloscope instance
 * @out:  (out): caller-provided snapshot filled with the current scalar
 *        drawing configuration.
 */
void pn_oscilloscope_get_paint_state (PnOscilloscope            *self,
                                      PnOscilloscopePaintState  *out);

/**
 * pn_oscilloscope_read_trace:
 * @self:  oscilloscope instance
 * @cap:   maximum number of points to write into @out_x / @out_y; when
 *         the trace is longer it is decimated to fit (per-bucket extrema
 *         are kept so a dense waveform's envelope survives).  Must be > 0.
 * @out_x: (out caller-allocates): X coordinates, display order
 * @out_y: (out caller-allocates): Y coordinates, display order
 * @xmin:  (out) (nullable): smallest X over the FULL trace
 * @xmax:  (out) (nullable): largest  X over the FULL trace
 * @ymin:  (out) (nullable): smallest Y over the FULL trace
 * @ymax:  (out) (nullable): largest  Y over the FULL trace
 *
 * Copies the current trace — whichever of the vector snapshot or the
 * single scalar point is live — into the caller arrays in left-to-right
 * display order, decimating to at most @cap points.  @out_x and @out_y
 * must each have room for @cap entries.  The bounds, when requested, frame
 * the auto-fit: for a vector snapshot they are the exact extent of every
 * sample (not just the decimated subset); for the scalar dot they are the
 * accumulated extent of every point seen since the last snapshot, so the
 * lone dot moves within a stable frame instead of being pinned centre.
 *
 * Returns: the number of points written (0 when there is no trace yet).
 */
guint pn_oscilloscope_read_trace (PnOscilloscope *self,
                                  guint           cap,
                                  gdouble        *out_x,
                                  gdouble        *out_y,
                                  gdouble        *xmin,
                                  gdouble        *xmax,
                                  gdouble        *ymin,
                                  gdouble        *ymax);

/* Maximum number of afterglow trail points the core retains — the cap the
 * painter sizes its scratch arrays to when reading the trail back. */
#define PN_OSCILLOSCOPE_AFTERGLOW_MAX 64

/**
 * pn_oscilloscope_read_afterglow:
 * @self:     oscilloscope instance
 * @cap:      capacity of each output array (use #PN_OSCILLOSCOPE_AFTERGLOW_MAX)
 * @out_x:    (out caller-allocates): X of each trail point, oldest→newest
 * @out_y:    (out caller-allocates): Y of each trail point, oldest→newest
 * @out_life: (out caller-allocates): remaining glow of each point in (0, 1],
 *            1 just-vacated, fading to 0 as it ages out of the persistence
 *            window.  The caller shapes brightness from this however it likes.
 *
 * Reports the phosphor afterglow trail: the recent positions the scalar dot
 * has moved AWAY from, in data coordinates (same frame as the live trace, so
 * the caller maps them with the bounds from pn_oscilloscope_read_trace()).
 * Each older position lingers and dims so a moving dot streaks like a real
 * CRT; drawing a segment from each point to the next-newer one (and from the
 * newest to the live dot) renders the fading connector between successive
 * positions.  Only meaningful in scalar (dot) mode — a vector snapshot keeps
 * no trail, so this returns 0.
 *
 * Returns: the number of live trail points written (0 when none).
 */
guint pn_oscilloscope_read_afterglow (PnOscilloscope *self,
                                      guint           cap,
                                      gdouble        *out_x,
                                      gdouble        *out_y,
                                      gdouble        *out_life);

/* ------------------------------------------------------------------ */
/*  Maximized controls — knobs (GTK-free seam)                         */
/*                                                                     */
/*  The four bench-scope knobs and the two "Auto" buttons only appear  */
/*  while the card is maximized into the worksheet zoom overlay.  Their */
/*  geometry, hit-testing and value maths all live here in the GTK-free */
/*  core so the painter (pn-oscilloscope-gui.c) and the worksheet's     */
/*  pointer handlers agree on a single source of truth — exactly the    */
/*  shape PnKnob / PnChat use for their own in-card controls.           */
/* ------------------------------------------------------------------ */

/* The four knobs, in layout order (right column, 2×2 grid). */
typedef enum
{
    PN_OSC_KNOB_X_RANGE,
    PN_OSC_KNOB_X_OFFSET,
    PN_OSC_KNOB_Y_RANGE,
    PN_OSC_KNOB_Y_OFFSET,
    PN_OSC_KNOB_N
} PnOscKnob;

/* A plain rectangle in the same coordinate space as the plot rect handed
 * to pn_oscilloscope_layout (and to the painter's paint_plot). */
typedef struct
{
    gdouble x, y, w, h;
} PnOscRect;

/* Mark the node maximized (lifted into the zoom overlay) or not.  Pure
 * state — never requests a repaint (it is toggled around every overlay
 * paint, so a repaint here would loop).  The painter draws the knob
 * panel exactly when this is set. */
void     pn_oscilloscope_set_maximized (PnOscilloscope *self, gboolean on);
gboolean pn_oscilloscope_get_maximized (PnOscilloscope *self);

/**
 * pn_oscilloscope_layout:
 * @px-@ph: the maximized plot rectangle (the whole instrument face).
 * @crt:     (out): the CRT sub-rectangle, anchored top-left.
 * @knobs:   (out caller-allocates PN_OSC_KNOB_N): the four knob cells.
 * @autobtn: (out caller-allocates 2): the "Auto X" / "Auto Y" buttons.
 *
 * Single source of truth for the maximized layout, used by both the
 * painter and the worksheet so the drawn knobs and the hit-tested ones
 * are the same rectangles.
 */
void pn_oscilloscope_layout (PnOscilloscope *self,
                             gdouble         px,
                             gdouble         py,
                             gdouble         pw,
                             gdouble         ph,
                             PnOscRect      *crt,
                             PnOscRect       knobs[PN_OSC_KNOB_N],
                             PnOscRect       autobtn[2]);

/* Index of the knob / Auto button under (@x, @y), or -1 for none. */
int pn_oscilloscope_hit_knob (const PnOscRect knobs[PN_OSC_KNOB_N],
                              gdouble x, gdouble y);
int pn_oscilloscope_hit_auto (const PnOscRect autobtn[2],
                              gdouble x, gdouble y);

/* Begin manipulating @knob: if its axis is still in auto-fit, snapshot
 * the current effective window into the axis's range/offset and switch it
 * to manual, so the following drag grabs from where the trace already is
 * (no jump).  A no-op when the axis is already manual. */
void pn_oscilloscope_begin_knob (PnOscilloscope *self, int knob);

/* Apply a vertical drag @dy (pixels, +down) to @knob.  Range knobs scale
 * multiplicatively (up = wider); offset knobs translate the window by a
 * fraction of @crt_extent (the CRT height) times the current range (up =
 * trace moves up).  Switches the axis to manual if it was not already. */
void pn_oscilloscope_drag_knob (PnOscilloscope *self,
                                int             knob,
                                gdouble         dy,
                                gdouble         crt_extent);

/* Return @x_axis (TRUE = X) to auto-fit. */
void pn_oscilloscope_reset_axis (PnOscilloscope *self, gboolean x_axis);

/**
 * pn_oscilloscope_axis_window:
 * @x_axis:           TRUE for the X axis, FALSE for Y.
 * @raw_lo/@raw_hi:   the raw data extent of that axis (from read_trace).
 * @lo/@hi:           (out): the visible window to map the trace into.
 *
 * For an auto axis this pads the raw extent (honouring *-from-zero), the
 * same auto-fit the scope has always done; for a manual axis it returns
 * offset ± range/2.  One definition shared by the painter's data→screen
 * mapping and the knob-grab seeding.
 */
void pn_oscilloscope_axis_window (PnOscilloscope *self,
                                  gboolean        x_axis,
                                  gdouble         raw_lo,
                                  gdouble         raw_hi,
                                  gdouble        *lo,
                                  gdouble        *hi);

/* The effective range/offset of an axis right now — the manual values
 * when manual, else those derived from the padded auto window.  Used by
 * the painter to label the knobs (and show the live auto value before the
 * user grabs one).  @raw_lo/@raw_hi as for pn_oscilloscope_axis_window. */
void pn_oscilloscope_axis_knob_values (PnOscilloscope *self,
                                       gboolean        x_axis,
                                       gdouble         raw_lo,
                                       gdouble         raw_hi,
                                       gdouble        *range,
                                       gdouble        *offset);

/* TRUE when the given axis is currently in auto-fit mode. */
gboolean pn_oscilloscope_axis_is_auto (PnOscilloscope *self, gboolean x_axis);

G_END_DECLS

#endif /* PN_OSCILLOSCOPE_H */
