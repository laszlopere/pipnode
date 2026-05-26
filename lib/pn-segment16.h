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

#ifndef PN_SEGMENT16_H
#define PN_SEGMENT16_H

#include "pn-node.h"
#include "pn-color.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSegment16                                                        */
/*                                                                     */
/*  Sink node that paints an incoming string on a row of sixteen-      */
/*  segment LED cells — the alphanumeric cousin of #PnCountdown's      */
/*  seven-segment readout.  Each cell shows one ASCII character: every */
/*  glyph the classic 16-segment "starburst" font supports (0-9, A-Z,  */
/*  a small set of punctuation, and a fall-through to uppercase for    */
/*  a-z).  Anything else lights no segments, so the cell reads as      */
/*  blank on the dim off-state ghost.                                  */
/*                                                                     */
/*  The message string is data.output (same convention as #PnLabel).   */
/*  A message with no string output blanks the row.  Text is left-     */
/*  aligned and cropped to the configured cell count — no scrolling,   */
/*  matching how a fixed-cell LED bank reads.  Being a pure sink the   */
/*  node never forwards.                                               */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SEGMENT16 (pn_segment16_get_type ())

G_DECLARE_FINAL_TYPE (PnSegment16, pn_segment16, PN, SEGMENT16, PnNode)

PnSegment16 *pn_segment16_new (void);

/**
 * pn_segment16_set_text:
 * @self:   segment16 instance
 * @text:   (nullable): the string to display, or %NULL / "" to blank the row
 *
 * Push a fresh string into the readout — identical to what the built-in
 * receiver does when a message lands.  Schedules a repaint only when the
 * visible text actually changes.
 */
void pn_segment16_set_text (PnSegment16 *self, const gchar *text);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Everything the painter (pn-segment16-gui.c) needs to draw a frame, */
/*  snapshotted by value through pn_segment16_get_paint_state().  The  */
/*  @text pointer borrows the node's storage and is valid until the    */
/*  next property change.                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    const gchar  *text;            /* never %NULL ("" means blank row) */
    guint         cells;           /* number of 16-segment cells (1..32) */

    PnColor       background_color;
    PnColor       segment_color;        /* lit bar */
    PnColor       unlit_segment_color;  /* off-state ghost */
} PnSegment16PaintState;

/**
 * pn_segment16_get_paint_state:
 * @self: segment16 instance
 * @out:  (out): caller-provided snapshot
 *
 * Copy the fields the gui-tier painter needs into @out.  GTK-free.
 */
void pn_segment16_get_paint_state (PnSegment16           *self,
                                   PnSegment16PaintState *out);

G_END_DECLS

#endif /* PN_SEGMENT16_H */
