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

#ifndef PN_COMMENT_H
#define PN_COMMENT_H

#include "pn-node.h"
#include "pn-color.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnComment                                                           */
/*                                                                     */
/*  A free-text annotation box on the worksheet — a white, resizable    */
/*  rectangle with a grey frame holding a few paragraphs of wrapped      */
/*  text, used to comment on a flow.  It is a port-less #PnNode (no      */
/*  input, no output) that never touches the message graph: being a      */
/*  node it gets selection, dragging, the marquee, Delete, copy/paste,   */
/*  per-sheet membership, JSON persistence and undo/redo for free, like  */
/*  the decorative #PnLabel.  Unlike every other node it is user-        */
/*  resizable (its width/height are stored properties the worksheet      */
/*  edits through resize handles), and it is drawn chromeless — no icon  */
/*  panel, header or ports — by a dedicated branch in the worksheet      */
/*  painter that reads the GTK-free snapshot pn_comment_get_paint_state. */
/* ------------------------------------------------------------------ */

#define PN_TYPE_COMMENT (pn_comment_get_type ())

G_DECLARE_FINAL_TYPE (PnComment, pn_comment, PN, COMMENT, PnNode)

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/* ------------------------------------------------------------------ */

#define PN_COMMENT_DEFAULT_WIDTH   200.0
#define PN_COMMENT_DEFAULT_HEIGHT  120.0
#define PN_COMMENT_MIN_WIDTH        80.0
#define PN_COMMENT_MIN_HEIGHT       40.0
#define PN_COMMENT_DEFAULT_TEXT_SIZE 13.0   /* px */

/* Inner padding and corner radius (absolute px, not height fractions —
 * a comment grows freely so a fixed inset reads better than a ratio). */
#define PN_COMMENT_PADDING           8.0
#define PN_COMMENT_CORNER_RADIUS     4.0

/* Size of a square resize handle drawn at the box corners (world px). */
#define PN_COMMENT_HANDLE_SIZE      10.0

PnComment *pn_comment_new (void);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Everything the worksheet painter needs to draw a comment box,       */
/*  snapshotted through pn_comment_get_paint_state().  The @text        */
/*  pointer is BORROWED from the node and valid until the next property */
/*  change — copy it if you need to outlive that.                       */
/* ------------------------------------------------------------------ */

typedef struct
{
    const gchar *text;             /* the comment body, never %NULL     */

    double       width;
    double       height;
    double       text_size;        /* font size in px                   */

    PnColor      background_color;  /* box fill — white by default       */
    PnColor      frame_color;       /* box outline — grey by default     */
    PnColor      text_color;        /* text colour — dark by default     */
} PnCommentPaintState;

/**
 * pn_comment_get_paint_state:
 * @self: comment instance
 * @out:  (out): caller-provided snapshot filled with the current
 *        drawing state
 *
 * Copy the fields the worksheet painter needs into @out.  GTK-free.  The
 * @text pointer borrows the node's storage (see above).
 */
void pn_comment_get_paint_state (PnComment *self, PnCommentPaintState *out);

/**
 * pn_comment_set_size:
 * @self:   comment instance
 * @width:  new width  (clamped to %PN_COMMENT_MIN_WIDTH)
 * @height: new height (clamped to %PN_COMMENT_MIN_HEIGHT)
 *
 * Set the box footprint, clamping to the minimum size and notifying the
 * "width" / "height" properties (so the worksheet re-measures the canvas
 * and repaints).  Used by the worksheet's resize-handle drag.
 */
void pn_comment_set_size (PnComment *self, double width, double height);

G_END_DECLS

#endif /* PN_COMMENT_H */
