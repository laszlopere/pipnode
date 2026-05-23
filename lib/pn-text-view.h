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

#ifndef PN_TEXT_VIEW_H
#define PN_TEXT_VIEW_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTextView                                                         */
/*                                                                     */
/*  Sink-with-passthrough that renders the latest `data.output`        */
/*  string of an incoming message as a read-only multi-line text       */
/*  block in a monospace font -- the "terminal pane" complement to     */
/*  #PnTableView's structured rendering of `data.table`.               */
/*                                                                     */
/*  Default colours are a green-on-black classic-terminal scheme;      */
/*  every visual property is exposed as a GObject property so the      */
/*  inspector can switch a node to e.g. amber-on-black or              */
/*  white-on-blue without code changes.                                */
/*                                                                     */
/*  Each received message REPLACES the displayed text (rather than     */
/*  appending, the way a real terminal scroll-back would), keeping     */
/*  the contract identical to #PnTableView: the latest snapshot wins.  */
/*                                                                     */
/*  The node carries an output port and re-emits every received        */
/*  message verbatim, so it can sit mid-pipeline as an inline          */
/*  inspector without breaking a downstream consumer.                  */
/*                                                                     */
/*  Visually the node mirrors #PnTableView (280 px header / 173 px    */
/*  body) so a row of mixed view sinks aligns cleanly on the canvas.  */
/*  Mouse-wheel scroll moves through long output one line at a time.   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TEXT_VIEW (pn_text_view_get_type ())

G_DECLARE_FINAL_TYPE (PnTextView, pn_text_view, PN, TEXT_VIEW, PnNode)

PnTextView *pn_text_view_new (void);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The cairo text painter lives in the gui-tier file                  */
/*  pn-text-view-gui.c, but it has to read the same line snapshot +     */
/*  colours the core fills.  Those are plain data (no GTK), so a scalar */
/*  snapshot and a borrowed-pointer line accessor are published here.   */
/*  Scroll clamping crosses back: the painter knows the live extents,  */
/*  so it pins the stored offset before reading it.                     */
/* ------------------------------------------------------------------ */

/* Scalar drawing configuration snapshotted by value for one paint.  The
 * colours are #PnColor (layout-identical to GdkRGBA). */
typedef struct
{
    PnColor  background_color;
    PnColor  text_color;
    gdouble  font_size;
} PnTextViewPaintState;

/**
 * pn_text_view_get_paint_state:
 * @self: text-view instance
 * @out:  (out): caller-provided snapshot filled with the current scalar
 *        drawing configuration.
 */
void pn_text_view_get_paint_state (PnTextView           *self,
                                   PnTextViewPaintState *out);

/* The current line snapshot (borrowed): an array of @n_out lines,
 * replaced on every receive. */
const gchar *const *pn_text_view_peek_lines (PnTextView *self, guint *n_out);

/* Scroll bookkeeping (GTK-free, both only touch an int).  The painter
 * computes the maximum offset from the live line height + visible-line
 * count, clamps the stored value, then reads it back. */
int  pn_text_view_get_scroll_offset   (PnTextView *self);
void pn_text_view_clamp_scroll_offset (PnTextView *self, int max_offset);

G_END_DECLS

#endif /* PN_TEXT_VIEW_H */
