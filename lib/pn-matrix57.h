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

#ifndef PN_MATRIX57_H
#define PN_MATRIX57_H

#include "pn-node.h"
#include "pn-color.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnMatrix57                                                         */
/*                                                                     */
/*  Sink node that paints an incoming string on a row of 5x7 dot-      */
/*  matrix character cells — the LCD cousin of #PnSegment16's LED      */
/*  starburst readout.  Each cell shows one ASCII glyph drawn as a     */
/*  grid of square dark pixels over the greenish-yellow LCD face, the  */
/*  way an HD44780 character module reads.                             */
/*                                                                     */
/*  The message string is data.output (same convention as #PnLabel).   */
/*  A message with no string output blanks the row.  Text is left-     */
/*  aligned and cropped to the configured cell count — no scrolling,   */
/*  matching how a fixed-cell character LCD reads.  Being a pure sink  */
/*  the node never forwards.                                           */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MATRIX57 (pn_matrix57_get_type ())

G_DECLARE_FINAL_TYPE (PnMatrix57, pn_matrix57, PN, MATRIX57, PnNode)

PnMatrix57 *pn_matrix57_new (void);

/**
 * pn_matrix57_set_text:
 * @self:   matrix57 instance
 * @text:   (nullable): the string to display, or %NULL / "" to blank the row
 *
 * Push a fresh string into the readout — identical to what the built-in
 * receiver does when a message lands.  Schedules a repaint only when the
 * visible text actually changes.
 */
void pn_matrix57_set_text (PnMatrix57 *self, const gchar *text);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Everything the painter (pn-matrix57-gui.c) needs to draw a frame,  */
/*  snapshotted by value through pn_matrix57_get_paint_state().  The   */
/*  @text pointer borrows the node's storage and is valid until the    */
/*  next property change.                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    const gchar  *text;            /* never %NULL ("" means blank row) */
    guint         cells;           /* number of 5x7 character cells (1..40) */

    PnColor       background_color;     /* greenish-yellow LCD face */
    PnColor       pixel_color;          /* lit (dark) dot */
    PnColor       unlit_pixel_color;    /* off-state ghost over the face */
} PnMatrix57PaintState;

/**
 * pn_matrix57_get_paint_state:
 * @self: matrix57 instance
 * @out:  (out): caller-provided snapshot
 *
 * Copy the fields the gui-tier painter needs into @out.  GTK-free.
 */
void pn_matrix57_get_paint_state (PnMatrix57           *self,
                                  PnMatrix57PaintState *out);

G_END_DECLS

#endif /* PN_MATRIX57_H */
