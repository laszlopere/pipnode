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

#ifndef PN_JUMP_H
#define PN_JUMP_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Jump flags — wireless connections by name                          */
/*                                                                     */
/*  A pair of pennant-shaped nodes that carry a message across the      */
/*  worksheet without a wire, the way a schematic-capture tool uses a   */
/*  global label instead of dragging a net across the page.  A message  */
/*  arriving at a #PnJumpIn is delivered to every #PnJumpOut carrying   */
/*  the same "tag", which then emits it onto its own outgoing wires.    */
/*                                                                     */
/*  Matching is exact, case-sensitive, and spans the WHOLE document —   */
/*  every sheet, not just the one the flag sits on.  That is the point: */
/*  a wire cannot cross sheets (pn_flow_load_from_data drops those), so */
/*  jump flags are the sanctioned way to route between them.           */
/*                                                                     */
/*  The tag lives in its own "tag" property rather than reusing the     */
/*  node's display name.  Renaming a node is a cosmetic act people do   */
/*  casually, and a cosmetic rename must not silently re-route a signal */
/*  on a sheet nobody is looking at.                                    */
/*                                                                     */
/*  Fan-out clones the message per target, exactly as #PnWire does, so  */
/*  two jump-outs sharing a tag cannot mutate each other's data bag.    */
/*                                                                     */
/*  Cycles need no special guard here: a jump-out emits onto wires that */
/*  deliver through pn_node_receive_message_on_input(), which carries   */
/*  the thread-local dispatch-depth counter.  A tag loop therefore      */
/*  trips PN_NODE_MAX_DISPATCH_DEPTH like any other feedback path.      */
/* ------------------------------------------------------------------ */

/* Geometry of the pennant, in world px.
 *
 * The FOOTPRINT is what the rest of the editor sees: the ports are
 * derived from it (input_port_y gives pos->y + header_height/2, and the
 * output port sits at pos->x + width), so both tips land on the grid if
 * and only if the footprint is a whole number of grid steps in width and
 * an even number of steps in height.  Hence a 2-step height, and a width
 * that pn_jump_measure() rounds UP to the next step rather than taking
 * the raw text width.
 *
 * The drawn SHAPE is slimmer than the footprint and centred inside it,
 * which keeps the label looking like a schematic pennant rather than a
 * node header while leaving the tip exactly on the footprint's vertical
 * centre — that is, on the grid.  PN_JUMP_POINT is the depth of the
 * triangular tip that gives the shape its direction; the tip is where
 * the wire attaches. */
#define PN_JUMP_GRID          20.0   /* must match PN_GRID_STEP */
#define PN_JUMP_HEIGHT        (PN_JUMP_GRID * 2)   /* footprint:  40 */
#define PN_JUMP_SHAPE_HEIGHT   24.0                /* drawn pennant   */
#define PN_JUMP_MIN_WIDTH     (PN_JUMP_GRID * 3)   /* footprint:  60 */
#define PN_JUMP_POINT         12.0
#define PN_JUMP_PADDING       10.0
#define PN_JUMP_CORNER_RADIUS  3.0

/* Nominal advance width of one tag character at the label font size.
 * #PnNodeClass.get_size is core (GTK-free) and so cannot ask Pango to
 * measure the string; this estimate sizes the pennant, and the painter
 * ellipsizes should the real text overrun it. */
#define PN_JUMP_CHAR_ADVANCE   8.5
#define PN_JUMP_FONT_SIZE     13.0

/**
 * pn_jump_measure:
 * @tag:        the flag's tag, may be %NULL or empty
 * @out_width:  (out) (optional): footprint width in world px
 * @out_height: (out) (optional): footprint height in world px
 *
 * Shared #PnNodeClass.get_size implementation for both flag directions.
 * Both results are whole multiples of %PN_JUMP_GRID, which is what keeps
 * the flag's connection point on the grid.
 */
void pn_jump_measure (const gchar *tag,
                      double      *out_width,
                      double      *out_height);

/**
 * pn_jump_collect_outputs:
 * @flow: the owning document, may be %NULL
 * @tag:  tag to match; %NULL or empty matches nothing
 *
 * Every enabled #PnJumpOut in @flow whose tag equals @tag, across all
 * sheets.
 *
 * Returns: (transfer container) (element-type PnNode): borrowed nodes in
 *   a list the caller frees with g_list_free(), or %NULL when none match.
 */
GList *pn_jump_collect_outputs (PnFlow *flow, const gchar *tag);

/**
 * pn_jump_refresh_errors:
 * @flow: the document to re-check, may be %NULL
 *
 * Recompute the has-error state of every jump flag in @flow.  A flag is
 * in error when its tag is empty, when a #PnJumpIn has no #PnJumpOut to
 * deliver to, or when a #PnJumpOut has no #PnJumpIn feeding it — each an
 * authoring mistake that would otherwise swallow messages in silence.
 *
 * Cheap and edit-time only: #PnFlow calls it when a node is added or
 * removed and when a "tag" property changes, never per message.
 */
void pn_jump_refresh_errors (PnFlow *flow);

G_END_DECLS

#endif /* PN_JUMP_H */
