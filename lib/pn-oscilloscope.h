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
/*    • value is a NUMBER  → the (x, value) point is appended to a      */
/*      rolling ring and the recent window is drawn as a sweeping       */
/*      trace.  x is the matching number, or a running sample counter   */
/*      when no x is supplied.                                          */
/*                                                                     */
/*  Both axes auto-range over the visible data so the curve always      */
/*  fits the screen.  The header is the standard Node-RED card; the     */
/*  CRT screen is painted below it.  Display only — no on-screen knobs. */
/* ------------------------------------------------------------------ */

#define PN_TYPE_OSCILLOSCOPE (pn_oscilloscope_get_type ())

G_DECLARE_FINAL_TYPE (PnOscilloscope, pn_oscilloscope, PN, OSCILLOSCOPE, PnNode)

PnOscilloscope *pn_oscilloscope_new (void);

/* Number of points currently held in the rolling scalar ring.  A
 * read-only inspection seam for the headless tests, since the trace
 * store is private.  Returns 0 while a vector snapshot is on screen. */
guint pn_oscilloscope_get_ring_count (PnOscilloscope *self);

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
 * rolling scalar ring is live — into the caller arrays in left-to-right
 * display order, decimating to at most @cap points.  @out_x and @out_y
 * must each have room for @cap entries.  The bounds, when requested, are
 * always computed over every retained sample (not just the decimated
 * subset) so the auto-fit is exact.
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

G_END_DECLS

#endif /* PN_OSCILLOSCOPE_H */
