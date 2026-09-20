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
/*  a plain 4x5 grid: every key is one cell, except "=" (two rows) and */
/*  "0" (two columns), the two places a pocket calculator always       */
/*  doubles up.                                                        */
/* ------------------------------------------------------------------ */

#define PN_KEYPAD_WIDTH         200.0
#define PN_KEYPAD_HEADER_HEIGHT  40.0
#define PN_KEYPAD_GAP             4.0
#define PN_KEYPAD_BODY_HEIGHT   200.0
#define PN_KEYPAD_TOTAL_HEIGHT  (PN_KEYPAD_HEADER_HEIGHT + \
                                 PN_KEYPAD_GAP +           \
                                 PN_KEYPAD_BODY_HEIGHT)

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
/*  The layout                                                         */
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
/*  Calculator (#PnExpression) node.                                   */
/* ------------------------------------------------------------------ */

static const PnKeypadKey keypad_keys[] = {
    /* label   code  kind                   col row span_c span_r */
    { "C",     "C",  PN_KEYPAD_CLEAR,        0,  0,  1,  1 },
    { "CE",    "CE", PN_KEYPAD_CLEAR_ENTRY,  1,  0,  1,  1 },
    { "\xc3\xb7", "/", PN_KEYPAD_OPERATOR,   2,  0,  1,  1 },  /* U+00F7 */
    { "\xc3\x97", "*", PN_KEYPAD_OPERATOR,   3,  0,  1,  1 },  /* U+00D7 */

    { "7",     "7",  PN_KEYPAD_DIGIT,        0,  1,  1,  1 },
    { "8",     "8",  PN_KEYPAD_DIGIT,        1,  1,  1,  1 },
    { "9",     "9",  PN_KEYPAD_DIGIT,        2,  1,  1,  1 },
    { "\xe2\x88\x92", "-", PN_KEYPAD_OPERATOR, 3, 1, 1,  1 },  /* U+2212 */

    { "4",     "4",  PN_KEYPAD_DIGIT,        0,  2,  1,  1 },
    { "5",     "5",  PN_KEYPAD_DIGIT,        1,  2,  1,  1 },
    { "6",     "6",  PN_KEYPAD_DIGIT,        2,  2,  1,  1 },
    { "+",     "+",  PN_KEYPAD_OPERATOR,     3,  2,  1,  1 },

    { "1",     "1",  PN_KEYPAD_DIGIT,        0,  3,  1,  1 },
    { "2",     "2",  PN_KEYPAD_DIGIT,        1,  3,  1,  1 },
    { "3",     "3",  PN_KEYPAD_DIGIT,        2,  3,  1,  1 },
    { "=",     "=",  PN_KEYPAD_EQUALS,       3,  3,  1,  2 },

    { "0",     "0",  PN_KEYPAD_DIGIT,        0,  4,  2,  1 },
    { ".",     ".",  PN_KEYPAD_POINT,        2,  4,  1,  1 },
};

#define PN_KEYPAD_N_KEYS G_N_ELEMENTS (keypad_keys)

const PnKeypadKey *
pn_keypad_get_keys (guint *out_n_keys)
{
    if (out_n_keys != NULL)
        *out_n_keys = PN_KEYPAD_N_KEYS;
    return keypad_keys;
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
        double  rect_x,
        double  rect_y,
        double  rect_w,
        double  rect_h,
        guint   index,
        double *out_x,
        double *out_y,
        double *out_w,
        double *out_h)
{
    const PnKeypadKey *k;
    const double inset   = PN_KEYPAD_INSET;
    const double gap     = PN_KEYPAD_KEY_GAP;
    const double inner_w = rect_w - 2.0 * inset;
    const double inner_h = rect_h - 2.0 * inset;
    double       cell_w, cell_h;

    if (index >= PN_KEYPAD_N_KEYS)
        return FALSE;
    if (inner_w <= 0.0 || inner_h <= 0.0)
        return FALSE;

    k = &keypad_keys[index];

    cell_w = (inner_w - gap * (PN_KEYPAD_COLS - 1)) / PN_KEYPAD_COLS;
    cell_h = (inner_h - gap * (PN_KEYPAD_ROWS - 1)) / PN_KEYPAD_ROWS;

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
    if (out_w != NULL) *out_w = PN_KEYPAD_WIDTH;
    if (out_h != NULL) *out_h = PN_KEYPAD_BODY_HEIGHT;
}

gint
pn_keypad_hit_key (
        PnKeypad *self,
        double    px,
        double    py)
{
    double bx, by, bw, bh;
    guint  i;

    g_return_val_if_fail (PN_IS_KEYPAD (self), -1);

    keypad_body_rect (self, &bx, &by, &bw, &bh);

    for (i = 0; i < PN_KEYPAD_N_KEYS; i++)
    {
        double kx, ky, kw, kh;

        if (!pn_keypad_key_rect_in (bx, by, bw, bh, i, &kx, &ky, &kw, &kh))
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
    const PnKeypadKey *k;
    PnNode            *node;
    PnMessage         *msg;

    g_return_if_fail (PN_IS_KEYPAD (self));
    g_return_if_fail (index < PN_KEYPAD_N_KEYS);

    k    = &keypad_keys[index];
    node = PN_NODE (self);
    msg  = pn_message_new (node, NULL);

    pn_message_set_string (msg, "key",  k->code);
    pn_message_set_string (msg, "kind", pn_keypad_kind_to_string (k->kind));

    /* Only the digit keys carry a number.  Leaving `value` off the
     * operator and clear keys is what lets a downstream numeric node
     * consume the digits and quietly ignore everything else, instead
     * of having to guess what "the value of +" would mean. */
    if (k->kind == PN_KEYPAD_DIGIT)
        pn_message_set_double (msg, "value", (double) (k->code[0] - '0'));

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
    guint i;

    g_return_val_if_fail (PN_IS_KEYPAD (self), FALSE);
    g_return_val_if_fail (code != NULL, FALSE);

    for (i = 0; i < PN_KEYPAD_N_KEYS; i++)
    {
        if (g_strcmp0 (keypad_keys[i].code, code) == 0)
        {
            pn_keypad_press (self, i);
            return TRUE;
        }
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  GUI read seam                                                      */
/* ------------------------------------------------------------------ */

void
pn_keypad_get_paint_state (PnKeypad *self, PnKeypadPaintState *out)
{
    g_return_if_fail (PN_IS_KEYPAD (self));
    g_return_if_fail (out != NULL);

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
    (void) self;
    if (out_width  != NULL) *out_width  = PN_KEYPAD_WIDTH;
    if (out_height != NULL) *out_height = PN_KEYPAD_TOTAL_HEIGHT;
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
            "Face colour of the operator keys and \"=\" — the column "
            "a calculator traditionally picks out in a second colour.",
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
