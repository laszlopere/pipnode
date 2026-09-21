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
/*  Manual source node whose client area is a key pad.  Clicking a key */
/*  emits exactly one message naming the key that was pressed —        */
/*  nothing more.  The node does no arithmetic and keeps no            */
/*  accumulator: it is an input device, and the sum is whatever the    */
/*  downstream graph makes of the keystrokes (a #PnCalcEngine is the   */
/*  natural partner).                                                  */
/*                                                                     */
/*  Which keys the pad carries is the "layout" property — a pocket     */
/*  calculator, a bare decimal entry pad, a hex byte pad or a phone    */
/*  pad with its letter groups, see #PnKeypadLayout.                   */
/*                                                                     */
/*  Every press emits, on the single output:                           */
/*                                                                     */
/*    data.key    the key's machine-readable code — "0".."9", and      */
/*                then whatever the layout adds: ".", "+", "-", "*",   */
/*                "/", "=", "C", "CE" on the calculator pad, "*" and   */
/*                "#" on the decimal and phone ones, "A".."F" on the   */
/*                hex one.  The operator keys paint the typographic    */
/*                signs (× ÷ −) but always emit the ASCII ones, so the */
/*                code drops straight into an expression string.       */
/*    data.kind   the key's family: "digit", "point", "operator",      */
/*                "equals", "clear", "clear-entry" or "symbol" — a     */
/*                Filter or Value Router can split the stream without  */
/*                matching ten separate digit codes.                   */
/*    data.value  the digit's numeric value, digit keys only: 0..9,    */
/*                or 0..15 on the hex pad, where "A".."F" are digits   */
/*                like any other.  Absent on every other key, so a     */
/*                downstream numeric node sees digits and ignores the  */
/*                rest.                                                */
/*    data.letters the telephone letter group ("ABC"), on the phone    */
/*                pad's lettered keys only.                            */
/*                                                                     */
/*  Geometry mirrors the other gauge-style nodes: a 40 px standard     */
/*  header with the keypad body hanging below it.  The body is the     */
/*  interaction surface rather than a passive readout, so the class    */
/*  pins #PnNodeClass.paint_plot_skip_zoom — a press lands on a key    */
/*  instead of lifting the node into the centred zoom overlay.  The    */
/*  node's width follows the layout's column count, so both pads keep  */
/*  the same key size.                                                 */
/* ------------------------------------------------------------------ */

#define PN_TYPE_KEYPAD (pn_keypad_get_type ())

G_DECLARE_FINAL_TYPE (PnKeypad, pn_keypad, PN, KEYPAD, PnNode)

/**
 * PnKeypadLayout:
 * @PN_KEYPAD_LAYOUT_CALCULATOR: the pocket-calculator pad — ten
 *   digits, the decimal point, the four operators, "=", "C" and "CE"
 *   on a 4x5 grid.
 * @PN_KEYPAD_LAYOUT_DECIMAL: the code-entry pad — the ten digits in
 *   telephone order plus "*" and "#" on a 3x4 grid, and nothing else.
 * @PN_KEYPAD_LAYOUT_HEX: the byte-entry pad — "0".."9" and "A".."F"
 *   on a 4x4 grid, counting left to right and top to bottom.  The
 *   letters are digits too: "C" emits `data.value` 12.
 * @PN_KEYPAD_LAYOUT_PHONE: the decimal pad with the telephone letter
 *   groups printed under the digits (2 = ABC, 9 = WXYZ).  Those keys
 *   add `data.letters` to the message; everything else emits exactly
 *   what the decimal pad emits.
 *
 * Which keys the pad carries.  The numeric values are part of the
 * saved-file format (the nick is what lands in the JSON), so existing
 * worksheets keep the calculator pad they were drawn with.
 */
typedef enum
{
    PN_KEYPAD_LAYOUT_CALCULATOR = 0,
    PN_KEYPAD_LAYOUT_DECIMAL    = 1,
    PN_KEYPAD_LAYOUT_HEX        = 2,
    PN_KEYPAD_LAYOUT_PHONE      = 3,
} PnKeypadLayout;

#define PN_TYPE_KEYPAD_LAYOUT (pn_keypad_layout_get_type ())

GType pn_keypad_layout_get_type (void) G_GNUC_CONST;

/**
 * PnKeypadKeyKind:
 * @PN_KEYPAD_DIGIT:       one of "0".."9"
 * @PN_KEYPAD_POINT:       the decimal point
 * @PN_KEYPAD_OPERATOR:    one of "+", "-", "*", "/"
 * @PN_KEYPAD_EQUALS:      the "=" key
 * @PN_KEYPAD_CLEAR:       "C" — clear everything
 * @PN_KEYPAD_CLEAR_ENTRY: "CE" — clear the current entry
 * @PN_KEYPAD_SYMBOL:      "*" or "#" on the decimal and phone pads —
 *                         a key that means nothing by itself, the way
 *                         it means nothing on a door code panel
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
    PN_KEYPAD_SYMBOL      = 6,
} PnKeypadKeyKind;

/**
 * PnKeypadKey:
 * @label:    what the key paints — the typographic operator signs
 *            (×, ÷, −) where they differ from the emitted code
 * @code:     what the key emits under `data.key` (always ASCII)
 * @kind:     the key's family
 * @col:      zero-based grid column of the key's left edge
 * @row:      zero-based grid row of the key's top edge
 * @colspan:  how many columns the key covers (1 for most keys)
 * @rowspan:  how many rows the key covers (1 for most keys)
 * @sublabel: a small second line under the legend — the telephone
 *            letter group on the phone pad — or %NULL.  A key that has
 *            one also emits it as `data.letters`.
 * @accent:   %TRUE for the keys the pad picks out in accent-color: the
 *            operators and "=", "*" and "#", "A".."F".  A paint hint
 *            kept apart from @kind on purpose, so the hex pad can
 *            highlight its letters while still calling them digits.
 *
 * One key in a keypad layout.  The tables are static, shared by every
 * instance, and published here because the gui-tier painter and the
 * core's hit-test must walk exactly the same grid.
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
    const gchar     *sublabel;
    gboolean         accent;
} PnKeypadKey;

PnKeypad *pn_keypad_new (void);

/**
 * pn_keypad_get_layout:
 * @self: the keypad node
 *
 * Returns the layout @self currently carries — the key table its
 * indices refer to.
 */
PnKeypadLayout pn_keypad_get_layout (PnKeypad *self);

/**
 * pn_keypad_layout_get_keys:
 * @layout:     which pad's table to return
 * @out_n_keys: (out) (optional): number of entries in the returned table
 *
 * Returns @layout's key table, in paint order.  The table is owned by
 * the node class and lives for the process lifetime.  An unknown
 * @layout falls back to the calculator pad, so a worksheet saved by a
 * newer build still draws something.
 */
const PnKeypadKey *pn_keypad_layout_get_keys (PnKeypadLayout  layout,
                                              guint          *out_n_keys);

/**
 * pn_keypad_layout_get_grid:
 * @layout:    which pad to describe
 * @out_cols: (out) (optional): the layout's column count
 * @out_rows: (out) (optional): the layout's row count
 *
 * The grid the layout's @col / @row coordinates are expressed in.
 */
void pn_keypad_layout_get_grid (PnKeypadLayout  layout,
                                int            *out_cols,
                                int            *out_rows);

/**
 * pn_keypad_kind_to_string:
 * @kind: a key family
 *
 * Returns the stable string a message's `data.kind` carries for
 * @kind — "digit", "point", "operator", "equals", "clear",
 * "clear-entry" or "symbol".  Never %NULL.
 */
const gchar *pn_keypad_kind_to_string (PnKeypadKeyKind kind);

/**
 * pn_keypad_key_rect_in:
 * @layout: the layout @index indexes into
 * @rect_x: left edge of the rectangle the keypad body occupies
 * @rect_y: top edge of that rectangle
 * @rect_w: width of that rectangle
 * @rect_h: height of that rectangle
 * @index:  index into the pn_keypad_layout_get_keys() table
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
gboolean pn_keypad_key_rect_in (PnKeypadLayout layout,
                                double         rect_x,
                                double         rect_y,
                                double         rect_w,
                                double         rect_h,
                                guint          index,
                                double        *out_x,
                                double        *out_y,
                                double        *out_w,
                                double        *out_h);

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
 * @index: index into the node's current layout table
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
 * names no key on the pad the node currently wears.
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

    PnKeypadLayout layout;
    gint           pressed_index;
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
