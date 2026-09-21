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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-keypad.h"
#include "pn-message.h"

/* fa-keyboard-o U+F11C.  Deliberately *not* the fa-calculator glyph
 * the Calculator (#PnExpression) node wears: the two are partners, but
 * a palette has to tell the keys apart from the arithmetic. */
#define PN_KEYPAD_ICON "\xef\x84\x9c"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Narrower than the data sinks (a keypad is a control, not a         */
/*  readout) but with the same 40 px standard header and 4 px gap, so  */
/*  a keypad dropped beside a Knob or a Switch lines up.  The body is  */
/*  a plain grid of cells, one key each, except where a layout doubles */
/*  a key up — "=" over two rows and "0" over two columns, the two     */
/*  places a pocket calculator always does.                            */
/*                                                                     */
/*  The width is per layout so the key *size* stays the same on both   */
/*  pads: dropping a column narrows the node instead of fattening its  */
/*  keys.  The height is shared, which is what keeps a 4-row and a     */
/*  5-row pad side by side looking like two members of one family.     */
/* ------------------------------------------------------------------ */

#define PN_KEYPAD_HEADER_HEIGHT  40.0
#define PN_KEYPAD_GAP             4.0
#define PN_KEYPAD_BODY_HEIGHT   200.0

/* Padding between the body edge and the outermost keys, and the gap
 * between two adjacent keys.  Shared with the painter through
 * pn_keypad_key_rect_in() so the two can never drift apart. */
#define PN_KEYPAD_INSET           6.0
#define PN_KEYPAD_KEY_GAP         4.0

/* How long a pressed key stays highlighted.  Long enough to register
 * as a button press, short enough that a fast run of digits still
 * flashes each one separately. */
#define PN_KEYPAD_PRESS_FLASH_MS 140

/* ------------------------------------------------------------------ */
/*  The layouts                                                        */
/*                                                                     */
/*  Standard pocket-calculator arrangement, in paint order:            */
/*                                                                     */
/*      C   CE   /   *                                                 */
/*      7   8    9   -                                                 */
/*      4   5    6   +                                                 */
/*      1   2    3   =    <- "=" covers this row and the next          */
/*      0 (wide)     .                                                 */
/*                                                                     */
/*  The operator keys paint the typographic signs a calculator has on  */
/*  its buttons but emit the ASCII codes, so `data.key` can be         */
/*  concatenated straight into an expression string for the            */
/*  Calculator Engine (#PnCalcEngine).                                 */
/* ------------------------------------------------------------------ */

static const PnKeypadKey keypad_keys_calculator[] = {
    /* label  code  kind                  col row cspan rspan sub   accent */
    { "C",    "C",  PN_KEYPAD_CLEAR,       0,  0,  1,  1, NULL, FALSE },
    { "CE",   "CE", PN_KEYPAD_CLEAR_ENTRY, 1,  0,  1,  1, NULL, FALSE },
    { "\xc3\xb7", "/", PN_KEYPAD_OPERATOR, 2, 0, 1,  1, NULL, TRUE  },
    { "\xc3\x97", "*", PN_KEYPAD_OPERATOR, 3, 0, 1,  1, NULL, TRUE  },

    { "7",    "7",  PN_KEYPAD_DIGIT,       0,  1,  1,  1, NULL, FALSE },
    { "8",    "8",  PN_KEYPAD_DIGIT,       1,  1,  1,  1, NULL, FALSE },
    { "9",    "9",  PN_KEYPAD_DIGIT,       2,  1,  1,  1, NULL, FALSE },
    { "\xe2\x88\x92", "-", PN_KEYPAD_OPERATOR, 3, 1, 1, 1, NULL, TRUE },

    { "4",    "4",  PN_KEYPAD_DIGIT,       0,  2,  1,  1, NULL, FALSE },
    { "5",    "5",  PN_KEYPAD_DIGIT,       1,  2,  1,  1, NULL, FALSE },
    { "6",    "6",  PN_KEYPAD_DIGIT,       2,  2,  1,  1, NULL, FALSE },
    { "+",    "+",  PN_KEYPAD_OPERATOR,    3,  2,  1,  1, NULL, TRUE  },

    { "1",    "1",  PN_KEYPAD_DIGIT,       0,  3,  1,  1, NULL, FALSE },
    { "2",    "2",  PN_KEYPAD_DIGIT,       1,  3,  1,  1, NULL, FALSE },
    { "3",    "3",  PN_KEYPAD_DIGIT,       2,  3,  1,  1, NULL, FALSE },
    { "=",    "=",  PN_KEYPAD_EQUALS,      3,  3,  1,  2, NULL, TRUE  },

    { "0",    "0",  PN_KEYPAD_DIGIT,       0,  4,  2,  1, NULL, FALSE },
    { ".",    ".",  PN_KEYPAD_POINT,       2,  4,  1,  1, NULL, FALSE },
};

/* The typographic signs above: U+00F7 divide, U+00D7 multiply,
 * U+2212 minus.  They are what the key paints; the ASCII code beside
 * each is what it emits. */

/* ------------------------------------------------------------------ */
/*  The decimal keyboard — a code-entry pad, in telephone order:       */
/*                                                                     */
/*      1   2   3                                                      */
/*      4   5   6                                                      */
/*      7   8   9                                                      */
/*      *   0   #                                                      */
/*                                                                     */
/*  Digits ascend down the pad, the way a door panel or a phone lays   */
/*  them out rather than the calculator's bottom-up rows.  "*" and     */
/*  "#" are the two extra keys such a pad always carries; they emit    */
/*  kind "symbol" because on a code pad they mean nothing on their     */
/*  own — the graph downstream decides whether "#" ends the entry.     */
/*  No decimal point, no operators, no clear keys: a PIN has none.     */
/* ------------------------------------------------------------------ */

static const PnKeypadKey keypad_keys_decimal[] = {
    /* label  code  kind             col row cspan rspan sub   accent */
    { "1",    "1",  PN_KEYPAD_DIGIT,  0,  0,  1,  1,  NULL, FALSE },
    { "2",    "2",  PN_KEYPAD_DIGIT,  1,  0,  1,  1,  NULL, FALSE },
    { "3",    "3",  PN_KEYPAD_DIGIT,  2,  0,  1,  1,  NULL, FALSE },

    { "4",    "4",  PN_KEYPAD_DIGIT,  0,  1,  1,  1,  NULL, FALSE },
    { "5",    "5",  PN_KEYPAD_DIGIT,  1,  1,  1,  1,  NULL, FALSE },
    { "6",    "6",  PN_KEYPAD_DIGIT,  2,  1,  1,  1,  NULL, FALSE },

    { "7",    "7",  PN_KEYPAD_DIGIT,  0,  2,  1,  1,  NULL, FALSE },
    { "8",    "8",  PN_KEYPAD_DIGIT,  1,  2,  1,  1,  NULL, FALSE },
    { "9",    "9",  PN_KEYPAD_DIGIT,  2,  2,  1,  1,  NULL, FALSE },

    { "*",    "*",  PN_KEYPAD_SYMBOL, 0,  3,  1,  1,  NULL, TRUE  },
    { "0",    "0",  PN_KEYPAD_DIGIT,  1,  3,  1,  1,  NULL, FALSE },
    { "#",    "#",  PN_KEYPAD_SYMBOL, 2,  3,  1,  1,  NULL, TRUE  },
};

/* ------------------------------------------------------------------ */
/*  The hex pad — byte entry, counting left to right:                  */
/*                                                                     */
/*      0   1   2   3                                                  */
/*      4   5   6   7                                                  */
/*      8   9   A   B                                                  */
/*      C   D   E   F                                                  */
/*                                                                     */
/*  "A".."F" are digits, not symbols: they carry kind "digit" and a    */
/*  data.value of 10..15, so a downstream accumulator can fold a       */
/*  keystroke in with acc * 16 + value without special-casing the      */
/*  letters.  They only *paint* differently, through the key table's   */
/*  accent flag.  Note "C" here is the hex digit twelve, not the       */
/*  calculator pad's clear key — the layouts are separate tables and   */
/*  the kind tells the two apart.                                      */
/* ------------------------------------------------------------------ */

static const PnKeypadKey keypad_keys_hex[] = {
    /* label  code  kind             col row cspan rspan sub   accent */
    { "0",    "0",  PN_KEYPAD_DIGIT,  0,  0,  1,  1,  NULL, FALSE },
    { "1",    "1",  PN_KEYPAD_DIGIT,  1,  0,  1,  1,  NULL, FALSE },
    { "2",    "2",  PN_KEYPAD_DIGIT,  2,  0,  1,  1,  NULL, FALSE },
    { "3",    "3",  PN_KEYPAD_DIGIT,  3,  0,  1,  1,  NULL, FALSE },

    { "4",    "4",  PN_KEYPAD_DIGIT,  0,  1,  1,  1,  NULL, FALSE },
    { "5",    "5",  PN_KEYPAD_DIGIT,  1,  1,  1,  1,  NULL, FALSE },
    { "6",    "6",  PN_KEYPAD_DIGIT,  2,  1,  1,  1,  NULL, FALSE },
    { "7",    "7",  PN_KEYPAD_DIGIT,  3,  1,  1,  1,  NULL, FALSE },

    { "8",    "8",  PN_KEYPAD_DIGIT,  0,  2,  1,  1,  NULL, FALSE },
    { "9",    "9",  PN_KEYPAD_DIGIT,  1,  2,  1,  1,  NULL, FALSE },
    { "A",    "A",  PN_KEYPAD_DIGIT,  2,  2,  1,  1,  NULL, TRUE  },
    { "B",    "B",  PN_KEYPAD_DIGIT,  3,  2,  1,  1,  NULL, TRUE  },

    { "C",    "C",  PN_KEYPAD_DIGIT,  0,  3,  1,  1,  NULL, TRUE  },
    { "D",    "D",  PN_KEYPAD_DIGIT,  1,  3,  1,  1,  NULL, TRUE  },
    { "E",    "E",  PN_KEYPAD_DIGIT,  2,  3,  1,  1,  NULL, TRUE  },
    { "F",    "F",  PN_KEYPAD_DIGIT,  3,  3,  1,  1,  NULL, TRUE  },
};

/* ------------------------------------------------------------------ */
/*  The phone pad — the decimal keyboard with the letter groups        */
/*  printed under the digits:                                          */
/*                                                                     */
/*      1       2 ABC   3 DEF                                          */
/*      4 GHI   5 JKL   6 MNO                                          */
/*      7 PQRS  8 TUV   9 WXYZ                                         */
/*      *       0       #                                              */
/*                                                                     */
/*  Same codes, same kinds and the same data.value as the decimal pad  */
/*  — a flow reading digits cannot tell the two apart, which is the    */
/*  point.  The lettered keys add data.letters ("PQRS"), so a          */
/*  downstream node can spell as well as count.  Wider keys than the   */
/*  decimal pad's, because "PQRS" has to fit under the 7.              */
/* ------------------------------------------------------------------ */

static const PnKeypadKey keypad_keys_phone[] = {
    /* label  code  kind             col row cspan rspan sub     accent */
    { "1",    "1",  PN_KEYPAD_DIGIT,  0,  0,  1,  1,  NULL,   FALSE },
    { "2",    "2",  PN_KEYPAD_DIGIT,  1,  0,  1,  1,  "ABC",  FALSE },
    { "3",    "3",  PN_KEYPAD_DIGIT,  2,  0,  1,  1,  "DEF",  FALSE },

    { "4",    "4",  PN_KEYPAD_DIGIT,  0,  1,  1,  1,  "GHI",  FALSE },
    { "5",    "5",  PN_KEYPAD_DIGIT,  1,  1,  1,  1,  "JKL",  FALSE },
    { "6",    "6",  PN_KEYPAD_DIGIT,  2,  1,  1,  1,  "MNO",  FALSE },

    { "7",    "7",  PN_KEYPAD_DIGIT,  0,  2,  1,  1,  "PQRS", FALSE },
    { "8",    "8",  PN_KEYPAD_DIGIT,  1,  2,  1,  1,  "TUV",  FALSE },
    { "9",    "9",  PN_KEYPAD_DIGIT,  2,  2,  1,  1,  "WXYZ", FALSE },

    { "*",    "*",  PN_KEYPAD_SYMBOL, 0,  3,  1,  1,  NULL,   TRUE  },
    { "0",    "0",  PN_KEYPAD_DIGIT,  1,  3,  1,  1,  NULL,   FALSE },
    { "#",    "#",  PN_KEYPAD_SYMBOL, 2,  3,  1,  1,  NULL,   TRUE  },
};

/* One row per #PnKeypadLayout value, indexed by the enum, so adding a
 * pad is a table entry plus its key list and nothing else. */
typedef struct
{
    const PnKeypadKey *keys;
    guint              n_keys;
    int                cols;
    int                rows;
    double             width;
} PnKeypadLayoutInfo;

static const PnKeypadLayoutInfo keypad_layouts[] = {
    /* PN_KEYPAD_LAYOUT_CALCULATOR */
    { keypad_keys_calculator, G_N_ELEMENTS (keypad_keys_calculator),
      4, 5, 200.0 },
    /* PN_KEYPAD_LAYOUT_DECIMAL: one column fewer, and 152 px is what
     * keeps its keys the same 44 px wide as the calculator pad's. */
    { keypad_keys_decimal,    G_N_ELEMENTS (keypad_keys_decimal),
      3, 4, 152.0 },
    /* PN_KEYPAD_LAYOUT_HEX: four columns again, so the same width as
     * the calculator pad and squarer keys (one row fewer). */
    { keypad_keys_hex,        G_N_ELEMENTS (keypad_keys_hex),
      4, 4, 200.0 },
    /* PN_KEYPAD_LAYOUT_PHONE: the decimal grid with wider keys —
     * 56 px, enough for "PQRS" under the 7. */
    { keypad_keys_phone,      G_N_ELEMENTS (keypad_keys_phone),
      3, 4, 188.0 },
};

/** The layout descriptor for @layout, falling back to the calculator
 *  pad for a value this build does not know — a worksheet written by a
 *  newer one still draws a keypad instead of nothing. */
static const PnKeypadLayoutInfo *
layout_info (PnKeypadLayout layout)
{
    if ((guint) layout >= G_N_ELEMENTS (keypad_layouts))
        return &keypad_layouts[PN_KEYPAD_LAYOUT_CALCULATOR];
    return &keypad_layouts[layout];
}

GType
pn_keypad_layout_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        /* The nicks double as the combo-box labels and are what the
           saved file carries, so they are fixed once shipped. */
        static const GEnumValue values[] = {
            { PN_KEYPAD_LAYOUT_CALCULATOR, "PN_KEYPAD_LAYOUT_CALCULATOR",
              "Basic Calculator" },
            { PN_KEYPAD_LAYOUT_DECIMAL,    "PN_KEYPAD_LAYOUT_DECIMAL",
              "Decimal Keyboard" },
            { PN_KEYPAD_LAYOUT_HEX,        "PN_KEYPAD_LAYOUT_HEX",
              "Hex Keyboard" },
            { PN_KEYPAD_LAYOUT_PHONE,      "PN_KEYPAD_LAYOUT_PHONE",
              "Phone Keyboard" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnKeypadLayout", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

const PnKeypadKey *
pn_keypad_layout_get_keys (PnKeypadLayout layout, guint *out_n_keys)
{
    const PnKeypadLayoutInfo *info = layout_info (layout);

    if (out_n_keys != NULL)
        *out_n_keys = info->n_keys;
    return info->keys;
}

void
pn_keypad_layout_get_grid (
        PnKeypadLayout  layout,
        int            *out_cols,
        int            *out_rows)
{
    const PnKeypadLayoutInfo *info = layout_info (layout);

    if (out_cols != NULL) *out_cols = info->cols;
    if (out_rows != NULL) *out_rows = info->rows;
}

const gchar *
pn_keypad_kind_to_string (PnKeypadKeyKind kind)
{
    switch (kind)
    {
    case PN_KEYPAD_DIGIT:       return "digit";
    case PN_KEYPAD_POINT:       return "point";
    case PN_KEYPAD_OPERATOR:    return "operator";
    case PN_KEYPAD_EQUALS:      return "equals";
    case PN_KEYPAD_CLEAR:       return "clear";
    case PN_KEYPAD_CLEAR_ENTRY: return "clear-entry";
    case PN_KEYPAD_SYMBOL:      return "symbol";
    default:                    return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnKeypad
{
    PnNode parent_instance;

    /* User-facing properties. */
    PnKeypadLayout layout;

    PnColor background_color;
    PnColor key_color;
    PnColor accent_color;
    PnColor text_color;

    /* Transient press feedback: the key painting its highlight and the
     * timeout that will clear it.  Never serialized — a reloaded
     * worksheet starts with no key lit. */
    gint   pressed_index;
    guint  press_flash_id;
};

G_DEFINE_TYPE (PnKeypad, pn_keypad, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_LAYOUT,
    PROP_BACKGROUND_COLOR,
    PROP_KEY_COLOR,
    PROP_ACCENT_COLOR,
    PROP_TEXT_COLOR,
    N_PROPS
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Grid geometry                                                      */
/*                                                                     */
/*  Expressed against an arbitrary body rectangle rather than against  */
/*  the node's stored position, so the painter (which is handed a      */
/*  rectangle by the worksheet) and the hit-test (which derives one    */
/*  from the node's position) run the identical arithmetic.            */
/* ------------------------------------------------------------------ */

gboolean
pn_keypad_key_rect_in (
        PnKeypadLayout  layout,
        double          rect_x,
        double          rect_y,
        double          rect_w,
        double          rect_h,
        guint           index,
        double         *out_x,
        double         *out_y,
        double         *out_w,
        double         *out_h)
{
    const PnKeypadLayoutInfo *info = layout_info (layout);
    const PnKeypadKey *k;
    const double inset   = PN_KEYPAD_INSET;
    const double gap     = PN_KEYPAD_KEY_GAP;
    const double inner_w = rect_w - 2.0 * inset;
    const double inner_h = rect_h - 2.0 * inset;
    double       cell_w, cell_h;

    if (index >= info->n_keys)
        return FALSE;
    if (inner_w <= 0.0 || inner_h <= 0.0)
        return FALSE;

    k = &info->keys[index];

    cell_w = (inner_w - gap * (info->cols - 1)) / info->cols;
    cell_h = (inner_h - gap * (info->rows - 1)) / info->rows;

    if (out_x != NULL)
        *out_x = rect_x + inset + k->col * (cell_w + gap);
    if (out_y != NULL)
        *out_y = rect_y + inset + k->row * (cell_h + gap);
    /* A spanning key swallows the gaps it straddles, so "0" reads as
     * one wide key rather than two keys with the seam painted over. */
    if (out_w != NULL)
        *out_w = k->colspan * cell_w + (k->colspan - 1) * gap;
    if (out_h != NULL)
        *out_h = k->rowspan * cell_h + (k->rowspan - 1) * gap;

    return TRUE;
}

/** The keypad body's rectangle in worksheet coordinates — the same
 *  rectangle the worksheet hands paint_plot, derived from the node's
 *  position and its (default) client area. */
static void
keypad_body_rect (
        PnKeypad *self,
        double   *out_x,
        double   *out_y,
        double   *out_w,
        double   *out_h)
{
    const PnPoint *p = pn_node_get_position (PN_NODE (self));

    if (out_x != NULL) *out_x = p->x;
    if (out_y != NULL) *out_y = p->y + PN_KEYPAD_HEADER_HEIGHT + PN_KEYPAD_GAP;
    if (out_w != NULL) *out_w = layout_info (self->layout)->width;
    if (out_h != NULL) *out_h = PN_KEYPAD_BODY_HEIGHT;
}

gint
pn_keypad_hit_key (
        PnKeypad *self,
        double    px,
        double    py)
{
    double bx, by, bw, bh;
    guint  n_keys, i;

    g_return_val_if_fail (PN_IS_KEYPAD (self), -1);

    keypad_body_rect (self, &bx, &by, &bw, &bh);
    n_keys = layout_info (self->layout)->n_keys;

    for (i = 0; i < n_keys; i++)
    {
        double kx, ky, kw, kh;

        if (!pn_keypad_key_rect_in (self->layout, bx, by, bw, bh, i,
                                    &kx, &ky, &kw, &kh))
            continue;

        if (px >= kx && px < kx + kw &&
            py >= ky && py < ky + kh)
            return (gint) i;
    }

    /* The gaps between keys, and the inset border: a deliberate miss,
     * so a click that lands between two keys presses neither. */
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Press feedback                                                     */
/* ------------------------------------------------------------------ */

static gboolean
press_flash_expired (gpointer data)
{
    PnKeypad *self = PN_KEYPAD (data);

    self->press_flash_id = 0;
    self->pressed_index  = -1;
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
press_flash_start (PnKeypad *self, guint index)
{
    if (self->press_flash_id != 0)
        g_source_remove (self->press_flash_id);

    self->pressed_index  = (gint) index;
    self->press_flash_id = g_timeout_add (PN_KEYPAD_PRESS_FLASH_MS,
                                          press_flash_expired, self);
    pn_node_request_repaint (PN_NODE (self));
}

gint
pn_keypad_get_pressed_index (PnKeypad *self)
{
    g_return_val_if_fail (PN_IS_KEYPAD (self), -1);
    return self->pressed_index;
}

/* ------------------------------------------------------------------ */
/*  Message emission                                                   */
/* ------------------------------------------------------------------ */

void
pn_keypad_press (PnKeypad *self, guint index)
{
    const PnKeypadLayoutInfo *info;
    const PnKeypadKey *k;
    PnNode            *node;
    PnMessage         *msg;

    g_return_if_fail (PN_IS_KEYPAD (self));

    info = layout_info (self->layout);
    g_return_if_fail (index < info->n_keys);

    k    = &info->keys[index];
    node = PN_NODE (self);
    msg  = pn_message_new (node, NULL);

    pn_message_set_string (msg, "key",  k->code);
    pn_message_set_string (msg, "kind", pn_keypad_kind_to_string (k->kind));

    /* Only the digit keys carry a number.  Leaving `value` off the
     * operator and clear keys is what lets a downstream numeric node
     * consume the digits and quietly ignore everything else, instead
     * of having to guess what "the value of +" would mean.
     *
     * Read as a hex digit, so the hex pad's "A".."F" come through as
     * 10..15 while "0".."9" keep meaning what they always did. */
    if (k->kind == PN_KEYPAD_DIGIT)
    {
        gint v = g_ascii_xdigit_value (k->code[0]);

        if (v >= 0)
            pn_message_set_double (msg, "value", (double) v);
    }

    /* The phone pad's letter groups travel with the key, so a flow can
     * spell as well as count. */
    if (k->sublabel != NULL)
        pn_message_set_string (msg, "letters", k->sublabel);

    press_flash_start (self, index);

    /* Light the processing glow for the emission.  As a manual source
     * the keypad emits on a click but never goes through the receive
     * or auto-trigger seams that bracket every other node, so without
     * this it would stay dark while its downstream nodes light up —
     * the same reason #PnKnob does it. */
    pn_node_processing_begin (node);
    pn_node_emit_message (node, msg);
    pn_node_processing_end (node);

    g_object_unref (msg);
}

gboolean
pn_keypad_press_code (PnKeypad *self, const gchar *code)
{
    const PnKeypadLayoutInfo *info;
    guint i;

    g_return_val_if_fail (PN_IS_KEYPAD (self), FALSE);
    g_return_val_if_fail (code != NULL, FALSE);

    info = layout_info (self->layout);

    for (i = 0; i < info->n_keys; i++)
    {
        if (g_strcmp0 (info->keys[i].code, code) == 0)
        {
            pn_keypad_press (self, i);
            return TRUE;
        }
    }

    return FALSE;
}

PnKeypadLayout
pn_keypad_get_layout (PnKeypad *self)
{
    g_return_val_if_fail (PN_IS_KEYPAD (self), PN_KEYPAD_LAYOUT_CALCULATOR);
    return self->layout;
}

/* ------------------------------------------------------------------ */
/*  GUI read seam                                                      */
/* ------------------------------------------------------------------ */

void
pn_keypad_get_paint_state (PnKeypad *self, PnKeypadPaintState *out)
{
    g_return_if_fail (PN_IS_KEYPAD (self));
    g_return_if_fail (out != NULL);

    out->layout           = self->layout;
    out->background_color = self->background_color;
    out->key_color        = self->key_color;
    out->accent_color     = self->accent_color;
    out->text_color       = self->text_color;
    out->pressed_index    = self->pressed_index;
}

/* ------------------------------------------------------------------ */
/*  Geometry vfuncs                                                    */
/* ------------------------------------------------------------------ */

static void
pn_keypad_get_size (
        PnNode *self,
        double *out_width,
        double *out_height)
{
    /* The width follows the layout — the decimal pad is a column
     * narrower — so a layout change resizes the node on the canvas
     * rather than stretching its keys. */
    if (out_width != NULL)
        *out_width = layout_info (PN_KEYPAD (self)->layout)->width;
    if (out_height != NULL)
        *out_height = PN_KEYPAD_HEADER_HEIGHT + PN_KEYPAD_GAP +
                      PN_KEYPAD_BODY_HEIGHT;
}

static double
pn_keypad_get_header_height (PnNode *self)
{
    (void) self;
    return PN_KEYPAD_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  GObject boilerplate                                                */
/* ------------------------------------------------------------------ */

static void
pn_keypad_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnKeypad *self = PN_KEYPAD (object);

    switch (prop_id)
    {
    case PROP_LAYOUT:
        g_value_set_enum (value, self->layout);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_KEY_COLOR:
        g_value_set_boxed (value, &self->key_color);
        break;
    case PROP_ACCENT_COLOR:
        g_value_set_boxed (value, &self->accent_color);
        break;
    case PROP_TEXT_COLOR:
        g_value_set_boxed (value, &self->text_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_keypad_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnKeypad *self = PN_KEYPAD (object);

    switch (prop_id)
    {
    case PROP_LAYOUT:
    {
        PnKeypadLayout v = (PnKeypadLayout) g_value_get_enum (value);

        if (self->layout != v)
        {
            self->layout = v;
            /* Key indices name a different table now, so drop the
             * pressed highlight instead of lighting whatever key
             * happens to sit at the old index. */
            if (self->press_flash_id != 0)
            {
                g_source_remove (self->press_flash_id);
                self->press_flash_id = 0;
            }
            self->pressed_index = -1;
            /* The node's width changes with the column count; a
             * repaint is what carries the new size onto the canvas,
             * the way #PnComment resizes. */
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_BACKGROUND_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_KEY_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->key_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_ACCENT_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->accent_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_TEXT_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->text_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_keypad_dispose (GObject *object)
{
    PnKeypad *self = PN_KEYPAD (object);

    if (self->press_flash_id != 0)
    {
        g_source_remove (self->press_flash_id);
        self->press_flash_id = 0;
    }

    G_OBJECT_CLASS (pn_keypad_parent_class)->dispose (object);
}

static void
pn_keypad_class_init (PnKeypadClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_keypad_get_property;
    object_class->set_property = pn_keypad_set_property;
    object_class->dispose      = pn_keypad_dispose;

    node_class->get_size          = pn_keypad_get_size;
    node_class->get_header_height = pn_keypad_get_header_height;
    /* The cairo key painter (paint_plot) is installed onto this class
     * by the gui tier — pn_keypad_gui_install() in pn-keypad-gui.c —
     * so the headless core carries no GTK/cairo.  The layout, the
     * hit-test and the emission all live here, which is what lets a
     * headless test press a key and observe the message. */

    /* The keypad body is the control itself, not a readout that wants
     * magnifying: a press has to land on a key, so opt out of the
     * centred zoom overlay the other plot-extension nodes lift into. */
    node_class->paint_plot_skip_zoom      = TRUE;
    node_class->paint_plot_corner_radius  = 6.0;

    node_class->palette_icon = PN_KEYPAD_ICON;
    node_class->class_name   = "Keypad";
    node_class->icon         = PN_KEYPAD_ICON;
    /* Shares the Calculator (#PnExpression) node's violet so the two
     * read as partners on a worksheet — the pad types, the Calculator
     * evaluates. */
    node_class->color        = (PnColor){ 0.55, 0.45, 0.80, 1.0 };
    node_class->category     = "Sources";
    node_class->has_input    = FALSE;
    node_class->has_output   = TRUE;

    props[PROP_LAYOUT] = g_param_spec_enum (
            "layout", "Layout",
            "Which keys the pad carries: the pocket-calculator pad, "
            "the decimal keyboard \"0\"..\"9\" + \"*\" and \"#\" a code "
            "entry pad has, the hex pad \"0\"..\"F\" for byte entry, or "
            "the phone pad with its letter groups under the digits.",
            PN_TYPE_KEYPAD_LAYOUT,
            PN_KEYPAD_LAYOUT_CALCULATOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the keypad's case — the panel the keys "
            "sit on.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_KEY_COLOR] = g_param_spec_boxed (
            "key-color", "Key colour",
            "Face colour of the digit, decimal-point and clear keys.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ACCENT_COLOR] = g_param_spec_boxed (
            "accent-color", "Accent colour",
            "Face colour of the keys a pad picks out in a second "
            "colour: the operators and \"=\" on the calculator pad, "
            "\"*\" and \"#\" on the decimal and phone keyboards, "
            "\"A\"..\"F\" on the hex one.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the key legends.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_keypad_init (PnKeypad *self)
{
    PnNode *node = PN_NODE (self);

    self->layout = PN_KEYPAD_LAYOUT_CALCULATOR;

    /* A dark case with light keys and an amber operator column — the
     * look of the pocket calculator the node is imitating. */
    self->background_color = (PnColor){ 0.16, 0.16, 0.19, 1.0 };
    self->key_color        = (PnColor){ 0.30, 0.30, 0.34, 1.0 };
    self->accent_color     = (PnColor){ 0.80, 0.52, 0.18, 1.0 };
    self->text_color       = (PnColor){ 0.95, 0.95, 0.96, 1.0 };

    self->pressed_index  = -1;
    self->press_flash_id = 0;

    pn_node_set_class_name (node, "Keypad");
    pn_node_set_icon       (node, PN_KEYPAD_ICON);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    {
        PnColor violet = { 0.55, 0.45, 0.80, 1.0 };
        pn_node_set_color (node, &violet);
    }
}

PnKeypad *
pn_keypad_new (void)
{
    return g_object_new (PN_TYPE_KEYPAD, NULL);
}
