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

#ifndef PN_KEYPAD_H
#define PN_KEYPAD_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnKeypad                                                           */
/*                                                                     */
/*  Manual source node whose client area is a pocket-calculator key    */
/*  pad: ten digits, a decimal point, the four basic operators, "=",   */
/*  and the two clear keys.  Clicking a key emits exactly one          */
/*  message naming the key that was pressed — nothing more.  The node  */
/*  does no arithmetic and keeps no accumulator: it is an input        */
/*  device, and the sum is whatever the downstream graph makes of the  */
/*  keystrokes (a #PnExpression "Calculator" is the natural partner).  */
/*                                                                     */
/*  Every press emits, on the single output:                           */
/*                                                                     */
/*    data.key    the key's machine-readable code — "0".."9", ".",     */
/*                "+", "-", "*", "/", "=", "C", "CE".  The operator    */
/*                keys paint the typographic signs (× ÷ −) but always  */
/*                emit the ASCII ones, so the code drops straight into */
/*                an expression string.                                */
/*    data.kind   the key's family: "digit", "point", "operator",      */
/*                "equals", "clear" or "clear-entry" — a Filter or     */
/*                Value Router can split the stream without matching   */
/*                ten separate digit codes.                            */
/*    data.value  the numeric value 0..9, digit keys only.  Absent on   */
/*                every other key, so a downstream numeric node sees    */
/*                digits and ignores the rest.                         */
/*                                                                     */
/*  Geometry mirrors the other gauge-style nodes: a 40 px standard     */
/*  header with the keypad body hanging below it.  The body is the     */
/*  interaction surface rather than a passive readout, so the class    */
/*  pins #PnNodeClass.paint_plot_skip_zoom — a press lands on a key    */
/*  instead of lifting the node into the centred zoom overlay.         */
/* ------------------------------------------------------------------ */

#define PN_TYPE_KEYPAD (pn_keypad_get_type ())

G_DECLARE_FINAL_TYPE (PnKeypad, pn_keypad, PN, KEYPAD, PnNode)

/**
 * PnKeypadKeyKind:
 * @PN_KEYPAD_DIGIT:       one of "0".."9"
 * @PN_KEYPAD_POINT:       the decimal point
 * @PN_KEYPAD_OPERATOR:    one of "+", "-", "*", "/"
 * @PN_KEYPAD_EQUALS:      the "=" key
 * @PN_KEYPAD_CLEAR:       "C" — clear everything
 * @PN_KEYPAD_CLEAR_ENTRY: "CE" — clear the current entry
 *
 * The family a key belongs to.  Emitted as the message's `data.kind`
 * (see pn_keypad_kind_to_string()) and used by the painter to pick
 * the key's face colour.
 */
typedef enum
{
    PN_KEYPAD_DIGIT       = 0,
    PN_KEYPAD_POINT       = 1,
    PN_KEYPAD_OPERATOR    = 2,
    PN_KEYPAD_EQUALS      = 3,
    PN_KEYPAD_CLEAR       = 4,
    PN_KEYPAD_CLEAR_ENTRY = 5,
} PnKeypadKeyKind;

/**
 * PnKeypadKey:
 * @label:   what the key paints — the typographic operator signs
 *           (×, ÷, −) where they differ from the emitted code
 * @code:    what the key emits under `data.key` (always ASCII)
 * @kind:    the key's family
 * @col:     zero-based grid column of the key's left edge
 * @row:     zero-based grid row of the key's top edge
 * @colspan: how many columns the key covers (1 for most keys)
 * @rowspan: how many rows the key covers (1 for most keys)
 *
 * One key in the fixed keypad layout.  The table is static, shared by
 * every instance, and published here because the gui-tier painter and
 * the core's hit-test must walk exactly the same grid.
 */
typedef struct
{
    const gchar     *label;
    const gchar     *code;
    PnKeypadKeyKind  kind;
    int              col;
    int              row;
    int              colspan;
    int              rowspan;
} PnKeypadKey;

/* Grid dimensions the layout below is expressed in. */
#define PN_KEYPAD_COLS 4
#define PN_KEYPAD_ROWS 5

PnKeypad *pn_keypad_new (void);

/**
 * pn_keypad_get_keys:
 * @out_n_keys: (out) (optional): number of entries in the returned table
 *
 * Returns the static keypad layout, in paint order.  The table is
 * owned by the node class and lives for the process lifetime.
 */
const PnKeypadKey *pn_keypad_get_keys (guint *out_n_keys);

/**
 * pn_keypad_kind_to_string:
 * @kind: a key family
 *
 * Returns the stable string a message's `data.kind` carries for
 * @kind — "digit", "point", "operator", "equals", "clear" or
 * "clear-entry".  Never %NULL.
 */
const gchar *pn_keypad_kind_to_string (PnKeypadKeyKind kind);

/**
 * pn_keypad_key_rect_in:
 * @rect_x: left edge of the rectangle the keypad body occupies
 * @rect_y: top edge of that rectangle
 * @rect_w: width of that rectangle
 * @rect_h: height of that rectangle
 * @index:  index into the pn_keypad_get_keys() table
 * @out_x: (out) (optional): the key's left edge
 * @out_y: (out) (optional): the key's top edge
 * @out_w: (out) (optional): the key's width
 * @out_h: (out) (optional): the key's height
 *
 * Places key @index inside an arbitrary keypad-body rectangle.  Pure
 * geometry with no GTK, shared by the gui-tier painter and the core's
 * hit-test so the pixel the user sees and the key the press resolves
 * to can never disagree.  Returns %FALSE (leaving the outputs
 * untouched) when @index is out of range.
 */
gboolean pn_keypad_key_rect_in (double  rect_x,
                                double  rect_y,
                                double  rect_w,
                                double  rect_h,
                                guint   index,
                                double *out_x,
                                double *out_y,
                                double *out_w,
                                double *out_h);

/**
 * pn_keypad_hit_key:
 * @self: the keypad node
 * @px:   x in worksheet coordinates
 * @py:   y in worksheet coordinates
 *
 * Resolves (@px, @py) against the keypad's on-canvas key grid,
 * returning the index of the key under the point or -1 when the point
 * misses every key (the gaps between keys, or anywhere outside the
 * body).  Exposed so the worksheet can route a primary press straight
 * into pn_keypad_press() without duplicating the layout arithmetic.
 */
gint pn_keypad_hit_key (PnKeypad *self,
                        double    px,
                        double    py);

/**
 * pn_keypad_press:
 * @self:  the keypad node
 * @index: index into the pn_keypad_get_keys() table
 *
 * Presses key @index: emits one message carrying the key's code,
 * kind and (for digits) numeric value, and lights the key's pressed
 * highlight for a moment so the click reads as a button press.  Must
 * be called from the main thread.  This is the entry point the
 * worksheet's click handler uses; programmatic callers — the D-Bus
 * automation surface, tests — may use it the same way.
 */
void pn_keypad_press (PnKeypad *self,
                      guint     index);

/**
 * pn_keypad_press_code:
 * @self: the keypad node
 * @code: a key code as it appears in the table ("7", "+", "CE", …)
 *
 * Convenience wrapper around pn_keypad_press() that looks the key up
 * by its emitted code.  Returns %FALSE, pressing nothing, when @code
 * names no key on the pad.
 */
gboolean pn_keypad_press_code (PnKeypad    *self,
                               const gchar *code);

/**
 * pn_keypad_get_pressed_index:
 * @self: the keypad node
 *
 * Returns the index of the key currently painting its pressed
 * highlight, or -1 when no key is lit.  The gui-tier painter's read
 * seam for the press feedback; the highlight clears itself a moment
 * after pn_keypad_press().
 */
gint pn_keypad_get_pressed_index (PnKeypad *self);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The cairo painter lives in pn-keypad-gui.c and cannot see this     */
/*  node's private instance struct, so the scalar drawing              */
/*  configuration crosses the tier boundary as a snapshot, exactly as  */
/*  #PnChat does it.  The colours are #PnColor (layout-identical to    */
/*  GdkRGBA).                                                          */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnColor background_color;
    PnColor key_color;
    PnColor accent_color;
    PnColor text_color;

    gint    pressed_index;
} PnKeypadPaintState;

/**
 * pn_keypad_get_paint_state:
 * @self: keypad instance
 * @out:  (out): caller-provided snapshot filled with the current
 *        scalar drawing configuration.
 */
void pn_keypad_get_paint_state (PnKeypad           *self,
                                PnKeypadPaintState *out);

G_END_DECLS

#endif /* PN_KEYPAD_H */
