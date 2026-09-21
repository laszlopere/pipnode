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

/* Unit tests for PnKeypad: the key pad.  A press emits one message
 * naming the key (data.key), its family (data.kind) and, for the
 * digits only, its numeric value (data.value).  The node does no
 * arithmetic and holds no accumulator, so the tests are about the two
 * layouts (the pocket calculator and the decimal code-entry pad), the
 * hit-test that turns a click into a key, and the shape of the
 * message a press produces.
 *
 * The grid geometry is exercised through the same GTK-free
 * pn_keypad_key_rect_in() the cairo painter uses, so "what the user
 * clicked" and "what the user saw" are checked against one source. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-keypad.h"

/* The last message the node emitted, kept so a test can inspect the
 * payload rather than only count emissions. */
typedef struct
{
    guint      emits;
    PnMessage *last;
} Capture;

static void
on_message (PnNode *node, PnMessage *message, gpointer user_data)
{
    Capture *cap = user_data;

    (void) node;
    cap->emits++;
    g_clear_object (&cap->last);
    cap->last = g_object_ref (message);
}

static PnKeypad *
make_node (Capture *cap)
{
    PnKeypad *node = g_object_new (PN_TYPE_KEYPAD, NULL);

    cap->emits = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_message), cap);
    return node;
}

static void
capture_clear (Capture *cap)
{
    g_clear_object (&cap->last);
}

/* Index of the key whose emitted code is @code in @layout, or -1. */
static gint
key_index_in (PnKeypadLayout layout, const gchar *code)
{
    const PnKeypadKey *keys;
    guint              n = 0, i;

    keys = pn_keypad_layout_get_keys (layout, &n);
    for (i = 0; i < n; i++)
        if (g_strcmp0 (keys[i].code, code) == 0)
            return (gint) i;
    return -1;
}

/* The same on the calculator pad, which is the node's default. */
static gint
key_index (const gchar *code)
{
    return key_index_in (PN_KEYPAD_LAYOUT_CALCULATOR, code);
}

/* The key at (@col, @row) of @layout, or -1 -- so a test can state
 * "7 sits on the third row" without counting table entries. */
static gint
key_at (PnKeypadLayout layout, int col, int row)
{
    const PnKeypadKey *keys;
    guint              n = 0, i;

    keys = pn_keypad_layout_get_keys (layout, &n);
    for (i = 0; i < n; i++)
        if (keys[i].col == col && keys[i].row == row)
            return (gint) i;
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Layout                                                             */
/* ------------------------------------------------------------------ */

static void
test_layout_has_every_key (void)
{
    const PnKeypadKey *keys;
    guint              n = 0;
    const gchar       *expected[] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
        ".", "+", "-", "*", "/", "=", "C", "CE",
    };
    guint i;

    keys = pn_keypad_layout_get_keys (PN_KEYPAD_LAYOUT_CALCULATOR, &n);
    PN_CHECK (keys != NULL);

    /* Ten digits, the point, four operators, "=", and the two clear
     * keys: everything a pocket calculator has, and nothing else. */
    PN_CHECK_CMPINT (n, ==, G_N_ELEMENTS (expected));

    for (i = 0; i < G_N_ELEMENTS (expected); i++)
        PN_CHECK_CMPINT (key_index (expected[i]), >=, 0);
}

static void
test_operator_labels_differ_from_codes (void)
{
    const PnKeypadKey *keys =
        pn_keypad_layout_get_keys (PN_KEYPAD_LAYOUT_CALCULATOR, NULL);

    /* The pad paints the typographic signs a calculator has on its
     * buttons but emits ASCII, so data.key can be pasted straight into
     * an expression string for the Calculator node. */
    PN_CHECK_CMPSTR (keys[key_index ("/")].label, ==, "\xc3\xb7");     /* ÷ */
    PN_CHECK_CMPSTR (keys[key_index ("*")].label, ==, "\xc3\x97");     /* × */
    PN_CHECK_CMPSTR (keys[key_index ("-")].label, ==, "\xe2\x88\x92"); /* − */

    /* "+" has no separate typographic form. */
    PN_CHECK_CMPSTR (keys[key_index ("+")].label, ==, "+");
}

static void
test_key_kinds (void)
{
    const PnKeypadKey *keys =
        pn_keypad_layout_get_keys (PN_KEYPAD_LAYOUT_CALCULATOR, NULL);

    PN_CHECK_CMPINT (keys[key_index ("7")].kind,  ==, PN_KEYPAD_DIGIT);
    PN_CHECK_CMPINT (keys[key_index (".")].kind,  ==, PN_KEYPAD_POINT);
    PN_CHECK_CMPINT (keys[key_index ("*")].kind,  ==, PN_KEYPAD_OPERATOR);
    PN_CHECK_CMPINT (keys[key_index ("=")].kind,  ==, PN_KEYPAD_EQUALS);
    PN_CHECK_CMPINT (keys[key_index ("C")].kind,  ==, PN_KEYPAD_CLEAR);
    PN_CHECK_CMPINT (keys[key_index ("CE")].kind, ==, PN_KEYPAD_CLEAR_ENTRY);

    PN_CHECK_CMPSTR (pn_keypad_kind_to_string (PN_KEYPAD_DIGIT),
                     ==, "digit");
    PN_CHECK_CMPSTR (pn_keypad_kind_to_string (PN_KEYPAD_CLEAR_ENTRY),
                     ==, "clear-entry");
    PN_CHECK_CMPSTR (pn_keypad_kind_to_string (PN_KEYPAD_SYMBOL),
                     ==, "symbol");
}

/* ------------------------------------------------------------------ */
/*  The decimal keyboard                                               */
/* ------------------------------------------------------------------ */

static void
test_decimal_layout_has_only_its_keys (void)
{
    const PnKeypadLayout dec = PN_KEYPAD_LAYOUT_DECIMAL;
    const PnKeypadKey   *keys;
    guint                n = 0;
    const gchar         *expected[] = {
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "*", "#",
    };
    const gchar         *absent[] = { ".", "+", "-", "/", "=", "C", "CE" };
    guint                i;

    keys = pn_keypad_layout_get_keys (dec, &n);
    PN_CHECK (keys != NULL);

    /* Ten digits plus the two extra keys a code-entry pad carries --
     * and nothing a calculator would add. */
    PN_CHECK_CMPINT (n, ==, G_N_ELEMENTS (expected));
    for (i = 0; i < G_N_ELEMENTS (expected); i++)
        PN_CHECK_CMPINT (key_index_in (dec, expected[i]), >=, 0);
    for (i = 0; i < G_N_ELEMENTS (absent); i++)
        PN_CHECK_CMPINT (key_index_in (dec, absent[i]), ==, -1);

    /* "*" and "#" are symbols here, not the multiply operator the
     * calculator pad spells the same way. */
    PN_CHECK_CMPINT (keys[key_index_in (dec, "*")].kind, ==,
                     PN_KEYPAD_SYMBOL);
    PN_CHECK_CMPINT (keys[key_index_in (dec, "#")].kind, ==,
                     PN_KEYPAD_SYMBOL);
    PN_CHECK_CMPINT (keys[key_index_in (dec, "7")].kind, ==,
                     PN_KEYPAD_DIGIT);

    /* Every key is a plain single cell: no spanning keys on this pad. */
    for (i = 0; i < n; i++)
    {
        PN_CHECK_CMPINT (keys[i].colspan, ==, 1);
        PN_CHECK_CMPINT (keys[i].rowspan, ==, 1);
    }
}

static void
test_decimal_layout_is_telephone_order (void)
{
    const PnKeypadLayout dec = PN_KEYPAD_LAYOUT_DECIMAL;
    int                  cols = 0, rows = 0;

    pn_keypad_layout_get_grid (dec, &cols, &rows);
    PN_CHECK_CMPINT (cols, ==, 3);
    PN_CHECK_CMPINT (rows, ==, 4);

    /* Digits ascend down the pad -- 1-2-3 on the top row, * 0 # on the
     * bottom -- the way a door panel lays them out, not the
     * calculator's bottom-up rows. */
    PN_CHECK_CMPINT (key_at (dec, 0, 0), ==, key_index_in (dec, "1"));
    PN_CHECK_CMPINT (key_at (dec, 2, 0), ==, key_index_in (dec, "3"));
    PN_CHECK_CMPINT (key_at (dec, 0, 2), ==, key_index_in (dec, "7"));
    PN_CHECK_CMPINT (key_at (dec, 0, 3), ==, key_index_in (dec, "*"));
    PN_CHECK_CMPINT (key_at (dec, 1, 3), ==, key_index_in (dec, "0"));
    PN_CHECK_CMPINT (key_at (dec, 2, 3), ==, key_index_in (dec, "#"));

    /* The calculator pad keeps its own order, bottom-up. */
    PN_CHECK_CMPINT (key_at (PN_KEYPAD_LAYOUT_CALCULATOR, 0, 1), ==,
                     key_index ("7"));
}

static void
test_layout_property_switches_the_pad (void)
{
    Capture    cap;
    PnKeypad  *node = make_node (&cap);
    GEnumClass *eclass;
    GEnumValue *ev;
    double     calc_w, dec_w, h;

    /* Existing worksheets were drawn with the calculator pad, so that
     * stays the default. */
    PN_CHECK_CMPINT (pn_keypad_get_layout (node), ==,
                     PN_KEYPAD_LAYOUT_CALCULATOR);
    PN_CHECK (pn_keypad_press_code (node, "="));
    pn_node_get_size (PN_NODE (node), &calc_w, &h);

    g_object_set (node, "layout", PN_KEYPAD_LAYOUT_DECIMAL, NULL);
    PN_CHECK_CMPINT (pn_keypad_get_layout (node), ==,
                     PN_KEYPAD_LAYOUT_DECIMAL);

    /* The pressed highlight names a key in the *old* table, so the
     * switch drops it rather than lighting whatever now sits there. */
    PN_CHECK_CMPINT (pn_keypad_get_pressed_index (node), ==, -1);

    /* A column fewer: the node narrows instead of fattening its keys,
     * and the height is shared so the two pads line up. */
    pn_node_get_size (PN_NODE (node), &dec_w, &h);
    PN_CHECK (dec_w < calc_w);

    /* The nick is what the saved file carries -- changing one would
     * silently reset every keypad in every worksheet. */
    eclass = g_type_class_ref (PN_TYPE_KEYPAD_LAYOUT);
    PN_CHECK_CMPINT (eclass->n_values, ==, 2);
    ev = g_enum_get_value (eclass, PN_KEYPAD_LAYOUT_CALCULATOR);
    PN_CHECK_CMPSTR (ev->value_nick, ==, "Basic Calculator");
    ev = g_enum_get_value (eclass, PN_KEYPAD_LAYOUT_DECIMAL);
    PN_CHECK_CMPSTR (ev->value_nick, ==, "Decimal Keyboard");
    g_type_class_unref (eclass);

    capture_clear (&cap);
    g_object_unref (node);
}

static void
test_decimal_keys_emit_their_own_kinds (void)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);

    g_object_set (node, "layout", PN_KEYPAD_LAYOUT_DECIMAL, NULL);

    PN_CHECK (pn_keypad_press_code (node, "5"));
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "kind"), ==, "digit");
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 5.0, 0.001);

    /* "*" is the multiply operator on the calculator pad; here it is a
     * code-pad symbol and carries no number. */
    PN_CHECK (pn_keypad_press_code (node, "*"));
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "key"),  ==, "*");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "kind"), ==, "symbol");
    PN_CHECK_FALSE  (pn_test_has (cap.last, "value"));

    PN_CHECK (pn_keypad_press_code (node, "#"));
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "key"),  ==, "#");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "kind"), ==, "symbol");
    PN_CHECK_FALSE  (pn_test_has (cap.last, "value"));

    PN_CHECK_CMPINT (cap.emits, ==, 3);

    /* Keys this pad does not have press nothing -- pressing by a code
     * the *other* layout carries is a miss, not a stray emission. */
    PN_CHECK_FALSE  (pn_keypad_press_code (node, "."));
    PN_CHECK_FALSE  (pn_keypad_press_code (node, "="));
    PN_CHECK_FALSE  (pn_keypad_press_code (node, "C"));
    PN_CHECK_CMPINT (cap.emits, ==, 3);

    capture_clear (&cap);
    g_object_unref (node);
}

static void
check_keys_do_not_overlap (PnKeypadLayout layout)
{
    const double rx = 0.0, ry = 0.0, rw = 200.0, rh = 200.0;
    guint        n = 0, i, j;

    pn_keypad_layout_get_keys (layout, &n);

    /* Every key must land inside the body and touch no other key --
     * otherwise a click would be ambiguous and the painter would draw
     * one legend over another. */
    for (i = 0; i < n; i++)
    {
        double ax, ay, aw, ah;

        PN_CHECK (pn_keypad_key_rect_in (layout, rx, ry, rw, rh, i,
                                         &ax, &ay, &aw, &ah));
        PN_CHECK (ax >= rx && ay >= ry);
        PN_CHECK (ax + aw <= rx + rw);
        PN_CHECK (ay + ah <= ry + rh);
        PN_CHECK (aw > 0.0 && ah > 0.0);

        for (j = i + 1; j < n; j++)
        {
            double bx, by, bw, bh;
            gboolean disjoint;

            pn_keypad_key_rect_in (layout, rx, ry, rw, rh, j,
                                   &bx, &by, &bw, &bh);

            disjoint = (ax + aw <= bx) || (bx + bw <= ax) ||
                       (ay + ah <= by) || (by + bh <= ay);
            PN_CHECK (disjoint);
        }
    }
}

static void
test_keys_do_not_overlap (void)
{
    check_keys_do_not_overlap (PN_KEYPAD_LAYOUT_CALCULATOR);
    check_keys_do_not_overlap (PN_KEYPAD_LAYOUT_DECIMAL);
}

static void
test_wide_and_tall_keys (void)
{
    const double rw = 200.0, rh = 200.0;
    double       zw, zh, one_w, one_h, eq_h;

    const PnKeypadLayout calc = PN_KEYPAD_LAYOUT_CALCULATOR;

    pn_keypad_key_rect_in (calc, 0, 0, rw, rh, (guint) key_index ("0"),
                           NULL, NULL, &zw, &zh);
    pn_keypad_key_rect_in (calc, 0, 0, rw, rh, (guint) key_index ("1"),
                           NULL, NULL, &one_w, &one_h);
    pn_keypad_key_rect_in (calc, 0, 0, rw, rh, (guint) key_index ("="),
                           NULL, NULL, NULL, &eq_h);

    /* "0" spans two columns and "=" two rows -- and each swallows the
     * gap it straddles, so it is strictly wider / taller than two
     * single cells minus that gap. */
    PN_CHECK (zw > one_w * 1.9);
    PN_CHECK_NEAR (zh, one_h, 0.001);
    PN_CHECK (eq_h > one_h * 1.9);
}

static void
test_rect_rejects_bad_index (void)
{
    const PnKeypadLayout calc = PN_KEYPAD_LAYOUT_CALCULATOR;
    guint  n = 0;
    double x = 42.0;

    pn_keypad_layout_get_keys (calc, &n);

    PN_CHECK_FALSE (pn_keypad_key_rect_in (calc, 0, 0, 200, 200, n,
                                           &x, NULL, NULL, NULL));
    /* A rejected call leaves the caller's outputs alone. */
    PN_CHECK_NEAR (x, 42.0, 0.001);

    /* A degenerate rectangle (a node squeezed to nothing) reports no
     * key rather than negative-width ones. */
    PN_CHECK_FALSE (pn_keypad_key_rect_in (calc, 0, 0, 1.0, 1.0, 0,
                                           NULL, NULL, NULL, NULL));

    /* An index the *other*, shorter layout does not reach. */
    PN_CHECK_FALSE (pn_keypad_key_rect_in (PN_KEYPAD_LAYOUT_DECIMAL,
                                           0, 0, 200, 200, n - 1,
                                           NULL, NULL, NULL, NULL));
}

/* ------------------------------------------------------------------ */
/*  Hit-testing                                                        */
/* ------------------------------------------------------------------ */

/* The keypad's body rectangle in worksheet coordinates, mirroring the
 * node's own geometry: the client area sits below the 40 px header and
 * the 4 px gap. */
static void
body_rect (PnKeypad *node, double *bx, double *by, double *bw, double *bh)
{
    const PnPoint *p = pn_node_get_position (PN_NODE (node));
    double         w, h, hh;

    pn_node_get_size (PN_NODE (node), &w, &h);
    hh = pn_node_get_header_height (PN_NODE (node));

    *bx = p->x;
    *by = p->y + hh + 4.0;
    *bw = w;
    *bh = h - hh - 4.0;
}

static void
check_hit_test_finds_each_key (PnKeypadLayout layout)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);
    PnPoint   pos  = { 130.0, 70.0 };
    double    bx, by, bw, bh;
    guint     n = 0, i;

    g_object_set (node, "layout", layout, NULL);

    /* Away from the origin, so a hit-test that forgot to add the
     * node's position would fail. */
    pn_node_set_position (PN_NODE (node), &pos);
    body_rect (node, &bx, &by, &bw, &bh);
    pn_keypad_layout_get_keys (layout, &n);

    for (i = 0; i < n; i++)
    {
        double kx, ky, kw, kh;

        pn_keypad_key_rect_in (layout, bx, by, bw, bh, i,
                               &kx, &ky, &kw, &kh);

        /* The centre of the painted key resolves to that key. */
        PN_CHECK_CMPINT (pn_keypad_hit_key (node,
                                            kx + kw / 2.0,
                                            ky + kh / 2.0),
                         ==, (gint) i);
    }

    capture_clear (&cap);
    g_object_unref (node);
}

static void
test_hit_test_finds_each_key (void)
{
    /* Both pads, since the hit-test reads the node's own layout: on
     * the decimal one a click must resolve against the narrower grid,
     * not the calculator's. */
    check_hit_test_finds_each_key (PN_KEYPAD_LAYOUT_CALCULATOR);
    check_hit_test_finds_each_key (PN_KEYPAD_LAYOUT_DECIMAL);
}

static void
test_hit_test_misses_outside (void)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);
    double    bx, by, bw, bh;

    body_rect (node, &bx, &by, &bw, &bh);

    /* The header is not the keypad -- a press there selects and drags
     * the node, so the hit-test must not claim it. */
    PN_CHECK_CMPINT (pn_keypad_hit_key (node, bx + bw / 2.0, by - 10.0),
                     ==, -1);
    /* Outside the body entirely. */
    PN_CHECK_CMPINT (pn_keypad_hit_key (node, bx - 5.0, by + 5.0), ==, -1);
    PN_CHECK_CMPINT (pn_keypad_hit_key (node, bx + bw + 5.0, by + 5.0),
                     ==, -1);
    PN_CHECK_CMPINT (pn_keypad_hit_key (node, bx + 5.0, by + bh + 5.0),
                     ==, -1);
    /* The case inset above the first row of keys: a deliberate miss. */
    PN_CHECK_CMPINT (pn_keypad_hit_key (node, bx + bw / 2.0, by + 1.0),
                     ==, -1);

    capture_clear (&cap);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Emission                                                           */
/* ------------------------------------------------------------------ */

static void
test_digit_press_carries_value (void)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);

    PN_CHECK (pn_keypad_press_code (node, "7"));
    PN_CHECK_CMPINT (cap.emits, ==, 1);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "key"),  ==, "7");
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "kind"), ==, "digit");
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 7.0, 0.001);

    /* "0" is the spanning key -- check it emits like any other digit. */
    PN_CHECK (pn_keypad_press_code (node, "0"));
    PN_CHECK_CMPINT (cap.emits, ==, 2);
    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 0.0, 0.001);

    capture_clear (&cap);
    g_object_unref (node);
}

static void
test_non_digit_keys_carry_no_value (void)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);
    const struct { const gchar *code; const gchar *kind; } cases[] = {
        { "+",  "operator"    },
        { "-",  "operator"    },
        { "*",  "operator"    },
        { "/",  "operator"    },
        { "=",  "equals"      },
        { ".",  "point"       },
        { "C",  "clear"       },
        { "CE", "clear-entry" },
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS (cases); i++)
    {
        PN_CHECK (pn_keypad_press_code (node, cases[i].code));
        PN_CHECK_CMPSTR (pn_test_str (cap.last, "key"),  ==, cases[i].code);
        PN_CHECK_CMPSTR (pn_test_str (cap.last, "kind"), ==, cases[i].kind);
        /* No number on a key that has none: a downstream numeric node
         * consumes the digits and ignores the rest, instead of having
         * to guess what "the value of +" would be. */
        PN_CHECK_FALSE  (pn_test_has (cap.last, "value"));
    }

    PN_CHECK_CMPINT (cap.emits, ==, G_N_ELEMENTS (cases));

    capture_clear (&cap);
    g_object_unref (node);
}

static void
test_press_emits_exactly_once (void)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);

    /* One press, one message -- and no startup announce: a keypad has
     * no state to report, so a freshly-loaded worksheet stays quiet
     * until the user actually presses a key. */
    while (g_main_context_iteration (NULL, FALSE))
        ;
    PN_CHECK_CMPINT (cap.emits, ==, 0);

    pn_keypad_press (node, (guint) key_index ("5"));
    PN_CHECK_CMPINT (cap.emits, ==, 1);

    while (g_main_context_iteration (NULL, FALSE))
        ;
    PN_CHECK_CMPINT (cap.emits, ==, 1);

    capture_clear (&cap);
    g_object_unref (node);
}

static void
test_unknown_code_presses_nothing (void)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);

    PN_CHECK_FALSE  (pn_keypad_press_code (node, "%"));
    PN_CHECK_FALSE  (pn_keypad_press_code (node, ""));
    PN_CHECK_CMPINT (cap.emits, ==, 0);

    /* The painted labels are not codes: pressing by the typographic
     * sign is a miss, the ASCII operator is the key. */
    PN_CHECK_FALSE  (pn_keypad_press_code (node, "\xc3\x97"));
    PN_CHECK_CMPINT (cap.emits, ==, 0);

    capture_clear (&cap);
    g_object_unref (node);
}

static void
test_pressed_key_highlights_then_clears (void)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);
    gint      idx  = key_index ("4");

    /* Nothing lit on a fresh node. */
    PN_CHECK_CMPINT (pn_keypad_get_pressed_index (node), ==, -1);

    pn_keypad_press (node, (guint) idx);
    PN_CHECK_CMPINT (pn_keypad_get_pressed_index (node), ==, idx);

    /* A second key takes the highlight over from the first. */
    pn_keypad_press (node, (guint) key_index ("9"));
    PN_CHECK_CMPINT (pn_keypad_get_pressed_index (node),
                     ==, key_index ("9"));

    capture_clear (&cap);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Node metadata                                                      */
/* ------------------------------------------------------------------ */

static void
test_node_shape (void)
{
    Capture   cap;
    PnKeypad *node = make_node (&cap);
    double    w, h, hh;

    /* An input device: one output, no input. */
    PN_CHECK_FALSE (pn_node_get_has_input  (PN_NODE (node)));
    PN_CHECK       (pn_node_get_has_output (PN_NODE (node)));

    /* The footprint extends below the header, which is what makes the
     * worksheet hand the keypad a client area to paint its keys in. */
    pn_node_get_size (PN_NODE (node), &w, &h);
    hh = pn_node_get_header_height (PN_NODE (node));
    PN_CHECK (h > hh);
    PN_CHECK (pn_node_get_client_area (PN_NODE (node),
                                       NULL, NULL, NULL, NULL));

    capture_clear (&cap);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-keypad");
    pn_test_add ("layout_complete",      test_layout_has_every_key);
    pn_test_add ("operator_labels",      test_operator_labels_differ_from_codes);
    pn_test_add ("key_kinds",            test_key_kinds);
    pn_test_add ("decimal_layout",       test_decimal_layout_has_only_its_keys);
    pn_test_add ("decimal_order",        test_decimal_layout_is_telephone_order);
    pn_test_add ("layout_property",      test_layout_property_switches_the_pad);
    pn_test_add ("decimal_emission",     test_decimal_keys_emit_their_own_kinds);
    pn_test_add ("keys_disjoint",        test_keys_do_not_overlap);
    pn_test_add ("spanning_keys",        test_wide_and_tall_keys);
    pn_test_add ("rect_bad_index",       test_rect_rejects_bad_index);
    pn_test_add ("hit_every_key",        test_hit_test_finds_each_key);
    pn_test_add ("hit_misses_outside",   test_hit_test_misses_outside);
    pn_test_add ("digit_carries_value",  test_digit_press_carries_value);
    pn_test_add ("no_value_elsewhere",   test_non_digit_keys_carry_no_value);
    pn_test_add ("press_emits_once",     test_press_emits_exactly_once);
    pn_test_add ("unknown_code",         test_unknown_code_presses_nothing);
    pn_test_add ("press_highlight",      test_pressed_key_highlights_then_clears);
    pn_test_add ("node_shape",           test_node_shape);
    return pn_test_run ();
}
