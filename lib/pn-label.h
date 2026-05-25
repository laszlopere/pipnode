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

#ifndef PN_LABEL_H
#define PN_LABEL_H

#include "pn-node.h"
#include "pn-color.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnLabel                                                             */
/*                                                                     */
/*  Sink node that shows the text of an incoming message on a small     */
/*  styled panel — the text counterpart of the Digital Clock.  Each     */
/*  message's data.output member is read as a (possibly multi-line)     */
/*  string and the node displays its LAST one or two lines, like a live */
/*  tail of a log or a command's output.  A message that carries no     */
/*  output blanks the label.                                            */
/*                                                                     */
/*  On the worksheet the client area is a fixed magnified view of the   */
/*  same readout the XFCE panel applet mirrors — always exactly the     */
/*  Digital Clock's footprint, so it never resizes with the data, like  */
/*  every other node.  The text is fitted to that box: shrunk down to a */
/*  minimum font and then clipped, so it never spills out.              */
/*                                                                     */
/*  Being a pure sink it never forwards a message.  The cairo/Pango     */
/*  drawing lives in the gui tier (pn-label-gui.c); this headless core  */
/*  keeps the logic, the tail computation and the GTK-free paint-state  */
/*  seam the painter reads.                                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LABEL (pn_label_get_type ())

G_DECLARE_FINAL_TYPE (PnLabel, pn_label, PN, LABEL, PnNode)

/* ------------------------------------------------------------------ */
/*  Geometry (shared with the gui tier)                                */
/*                                                                     */
/*  The worksheet footprint is fixed at the Digital Clock's, so the     */
/*  node never resizes with its data: a 40-px header, a 4-px gap, then  */
/*  a 100-px-tall, 206-px-wide body.  The body (the client area) has a  */
/*  fixed aspect ratio (PN_LABEL_ASPECT); the panel-applet widget uses  */
/*  the same aspect, and font, padding and corners are all fractions of */
/*  the box height — so the worksheet node, the panel-editor preview    */
/*  and the live panel widget are the same readout at three scales.     */
/*  The font fills a configured fraction of the box (font-scale) and is */
/*  never resized for long text; overflow is cropped on the right /     */
/*  bottom so the start of each line stays visible.                     */
/* ------------------------------------------------------------------ */

#define PN_LABEL_HEADER_HEIGHT    40.0
#define PN_LABEL_GAP               4.0
#define PN_LABEL_WIDTH           206.0   /* same as the Digital Clock */
#define PN_LABEL_BODY_HEIGHT     100.0
#define PN_LABEL_ASPECT  (PN_LABEL_WIDTH / PN_LABEL_BODY_HEIGHT)

/* Layout ratios, expressed against the client-box height, shared by the
 * gui-tier painter and (re-stated, since panel-widgets cannot include this
 * header) the PnTextDisplay panel widget.  Keep the two in sync.  The
 * vertical padding is deliberately small so the font fills the height the
 * way a panel clock does; the horizontal padding keeps a side margin. */
#define PN_LABEL_PAD_X_RATIO      0.07   /* left/right padding / box height */
#define PN_LABEL_PAD_Y_RATIO      0.02   /* top/bottom padding / box height */
#define PN_LABEL_CORNER_RATIO     0.08   /* corner radius / box height     */
/* Baseline-to-baseline spacing as a Pango line-spacing factor: tight, so
 * the two lines pack like a panel clock's.  The font size that fills the
 * box is then measured rather than guessed (see the gui painter). */
#define PN_LABEL_LINE_SPACING     0.78

/* Horizontal alignment of the text within the box. */
typedef enum
{
    PN_LABEL_ALIGN_LEFT = 0,
    PN_LABEL_ALIGN_CENTER,
    PN_LABEL_ALIGN_RIGHT
} PnLabelAlign;

PnLabel *pn_label_new (void);

/**
 * pn_label_set_text:
 * @self:   label instance
 * @output: (nullable): the raw output text, or %NULL / "" to blank the
 *          label
 *
 * Push a fresh output string into the display — identical to what the
 * built-in receiver does when a message lands.  The string may contain
 * several lines separated by `\n`; the label keeps only the last one or
 * two (see the `lines` property).  Passing %NULL or an empty string
 * clears the readout.  Schedules a repaint only when the visible text
 * actually changes.
 */
void pn_label_set_text (PnLabel *self, const gchar *output);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Everything the gui-tier painter (pn-label-gui.c) needs to draw a    */
/*  frame, snapshotted through pn_label_get_paint_state().  The tail of */
/*  the output is computed here so the line-splitting stays GTK-free    */
/*  and unit-testable; the painter only positions glyphs.  The string   */
/*  pointers are BORROWED from the node and valid until the next        */
/*  property change — copy them if you need to outlive that.            */
/* ------------------------------------------------------------------ */

typedef struct
{
    const gchar  *text;          /* the tailed display text, never %NULL  */
    const gchar  *font_family;   /* never %NULL                           */

    gint          lines;         /* 1 or 2 — how many trailing lines      */
    gint          font_scale;    /* text height, % of its line's share    */
    gint          weight;        /* Pango weight number (400/500/600/700) */
    gboolean      italic;
    PnLabelAlign  align;

    PnColor       text_color;
    PnColor       background_color;
} PnLabelPaintState;

/**
 * pn_label_get_paint_state:
 * @self: label instance
 * @out:  (out): caller-provided snapshot filled with the current
 *        drawing state
 *
 * Copy the fields the gui-tier painter needs into @out, with the output
 * already tailed to the last one or two lines.  GTK-free.  The @text and
 * @font_family pointers borrow the node's storage (see above).
 */
void pn_label_get_paint_state (PnLabel *self, PnLabelPaintState *out);

G_END_DECLS

#endif /* PN_LABEL_H */
