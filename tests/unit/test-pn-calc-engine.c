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

/* Unit tests for PnCalcEngine: the four-function state machine that
 * turns a stream of Keypad keystrokes into a running display.  Each
 * accepted key emits the whole display (data.value,
 * data.output, data.success), so the tests drive whole key sequences
 * and assert on what the readout would show. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-calc-engine.h"

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

static PnCalcEngine *
make_node (Capture *cap)
{
    PnCalcEngine *node = g_object_new (PN_TYPE_CALC_ENGINE, NULL);

    cap->emits = 0;
    cap->last  = NULL;
    g_signal_connect (node, "message", G_CALLBACK (on_message), cap);
    return node;
}

/* Feed one keystroke the way the Keypad does: a message carrying the
 * key code under data.key. */
static void
send_key (PnCalcEngine *node, const gchar *code)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_string (msg, "key", code);
    pn_node_receive_message (PN_NODE (node), msg);
    g_object_unref (msg);
}

/* Type a whole sequence, one key per space-separated token, so a test
 * reads like the keys the user pressed: "1 2 + 3 =". */
static void
type (PnCalcEngine *node, const gchar *keys)
{
    gchar **tokens = g_strsplit (keys, " ", -1);
    guint   i;

    for (i = 0; tokens[i] != NULL; i++)
        if (tokens[i][0] != '\0')
            send_key (node, tokens[i]);

    g_strfreev (tokens);
}

static gdouble
display (PnCalcEngine *node)
{
    return pn_calc_engine_get_display (node);
}

static const gchar *
display_text (PnCalcEngine *node)
{
    return pn_calc_engine_get_display_text (node);
}

/* ------------------------------------------------------------------ */
/*  Arithmetic                                                         */
/* ------------------------------------------------------------------ */

static void
test_four_operations (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    type (node, "2 + 3 =");
    PN_CHECK_NEAR (display (node), 5.0, 1e-9);

    type (node, "C 1 0 - 3 =");
    PN_CHECK_NEAR (display (node), 7.0, 1e-9);

    type (node, "C 7 * 6 =");
    PN_CHECK_NEAR (display (node), 42.0, 1e-9);

    type (node, "C 1 0 / 4 =");
    PN_CHECK_NEAR (display (node), 2.5, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_chained_operators_show_running_total (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    /* The second operator folds what is typed into the accumulator and
     * shows the running total -- the behaviour that makes a chain of
     * additions work without brackets. */
    type (node, "2 + 3");
    PN_CHECK_NEAR (display (node), 3.0, 1e-9);   /* still typing "3" */

    type (node, "+");
    PN_CHECK_NEAR (display (node), 5.0, 1e-9);   /* 2 + 3 folded */

    type (node, "4 =");
    PN_CHECK_NEAR (display (node), 9.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_equals_without_operator (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    /* "5 =" is just 5: with no pending operation the typed entry is
     * the running total. */
    type (node, "5 =");
    PN_CHECK_NEAR (display (node), 5.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_digit_after_result_starts_new_number (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    type (node, "2 + 3 =");
    PN_CHECK_NEAR (display (node), 5.0, 1e-9);

    /* A digit after a result replaces it rather than appending to it:
     * pressing 7 here gives 7, not 57. */
    type (node, "7");
    PN_CHECK_NEAR (display (node), 7.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Typing                                                             */
/* ------------------------------------------------------------------ */

static void
test_leading_zero_is_a_placeholder (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    /* "0" then "5" is 5, not 05. */
    type (node, "0 5");
    PN_CHECK_CMPSTR (display_text (node), ==, "5");
    PN_CHECK_NEAR   (display (node), 5.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_decimal_point (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    /* A point mid-number keeps showing as typed, so the readout tracks
     * "1." before the fractional digit lands. */
    type (node, "1 .");
    PN_CHECK_CMPSTR (display_text (node), ==, "1.");
    PN_CHECK_NEAR   (display (node), 1.0, 1e-9);

    type (node, "5");
    PN_CHECK_CMPSTR (display_text (node), ==, "1.5");
    PN_CHECK_NEAR   (display (node), 1.5, 1e-9);

    /* A second point is ignored -- one per number. */
    type (node, ". .");
    PN_CHECK_CMPSTR (display_text (node), ==, "1.5");

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_point_first_starts_zero_point (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    type (node, ". 2 5");
    PN_CHECK_CMPSTR (display_text (node), ==, "0.25");
    PN_CHECK_NEAR   (display (node), 0.25, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_max_digits_caps_typing (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    g_object_set (node, "max-digits", 4, NULL);

    type (node, "1 2 3 4 5 6");
    PN_CHECK_CMPSTR (display_text (node), ==, "1234");

    /* The cap is on typing only: a computed result is shown in full. */
    type (node, "* 1 0 0 =");
    PN_CHECK_NEAR (display (node), 123400.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Clearing                                                           */
/* ------------------------------------------------------------------ */

static void
test_clear_everything (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    type (node, "9 + 9");
    type (node, "C");
    PN_CHECK_NEAR   (display (node), 0.0, 1e-9);
    PN_CHECK_CMPSTR (display_text (node), ==, "0");

    /* The pending "+" went with it: 4 = is 4, not 9 + 4. */
    type (node, "4 =");
    PN_CHECK_NEAR (display (node), 4.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_clear_entry_keeps_the_sum (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    /* CE drops only the number being typed: the accumulator and the
     * pending operator survive, so a mistyped operand is retyped
     * without restarting the sum. */
    type (node, "2 + 9 CE");
    PN_CHECK_CMPSTR (display_text (node), ==, "0");

    type (node, "3 =");
    PN_CHECK_NEAR (display (node), 5.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Errors                                                             */
/* ------------------------------------------------------------------ */

static void
test_divide_by_zero_latches (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    type (node, "8 / 0 =");
    PN_CHECK       (pn_calc_engine_get_error (node));
    PN_CHECK       (pn_node_get_has_error (PN_NODE (node)));
    PN_CHECK_CMPSTR (display_text (node), ==, "Error");
    PN_CHECK_NEAR   (display (node), 0.0, 1e-9);

    /* The emitted message says so too, so a downstream Filter can act
     * on it. */
    PN_CHECK_FALSE  (pn_test_bool (cap.last, "success"));
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "output"), ==, "Error");

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_error_is_a_dead_end_until_clear (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);
    guint         at_error;

    type (node, "8 / 0 =");
    at_error = cap.emits;

    /* Every key but C is ignored -- and emits nothing, so a stuck
     * display is not spamming the readout. */
    type (node, "5 + 3 = .");
    PN_CHECK_CMPINT (cap.emits, ==, at_error);
    PN_CHECK        (pn_calc_engine_get_error (node));

    type (node, "C");
    PN_CHECK_FALSE  (pn_calc_engine_get_error (node));
    PN_CHECK_FALSE  (pn_node_get_has_error (PN_NODE (node)));
    PN_CHECK_CMPINT (cap.emits, ==, at_error + 1);

    /* And it calculates again. */
    type (node, "6 + 1 =");
    PN_CHECK_NEAR (display (node), 7.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Message shape                                                      */
/* ------------------------------------------------------------------ */

static void
test_emits_the_display_on_every_key (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    /* Five accepted keys, five messages: the readout has to track the
     * typing digit by digit, not only land on "=". */
    type (node, "1 2 + 3 =");
    PN_CHECK_CMPINT (cap.emits, ==, 5);

    PN_CHECK_NEAR   (pn_test_num (cap.last, "value"), 15.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (cap.last, "output"), ==, "15");
    PN_CHECK        (pn_test_bool (cap.last, "success"));

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_ignores_messages_without_a_key (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);
    PnMessage    *msg;

    type (node, "4 +");

    /* A reading that happens to flow in here must not disturb a
     * half-typed sum. */
    msg = pn_message_new (NULL, NULL);
    pn_message_set_double (msg, "value", 99.0);
    pn_node_receive_message (PN_NODE (node), msg);
    g_object_unref (msg);

    /* A non-string "key" is not a keystroke either. */
    msg = pn_message_new (NULL, NULL);
    pn_message_set_double (msg, "key", 7.0);
    pn_node_receive_message (PN_NODE (node), msg);
    g_object_unref (msg);

    PN_CHECK_CMPINT (cap.emits, ==, 2);      /* only "4" and "+" */

    type (node, "1 =");
    PN_CHECK_NEAR (display (node), 5.0, 1e-9);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_unknown_key_is_ignored (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    PN_CHECK_FALSE (pn_calc_engine_press (node, "%"));
    PN_CHECK_FALSE (pn_calc_engine_press (node, "sqrt"));
    PN_CHECK_FALSE (pn_calc_engine_press (node, ""));

    /* The typographic operator signs the keypad *paints* are not
     * codes -- the ASCII form is what drives the engine. */
    PN_CHECK_FALSE (pn_calc_engine_press (node, "\xc3\x97"));

    type (node, "%");
    PN_CHECK_CMPINT (cap.emits, ==, 0);

    g_clear_object (&cap.last);
    g_object_unref (node);
}

static void
test_node_shape (void)
{
    Capture       cap;
    PnCalcEngine *node = make_node (&cap);

    PN_CHECK (pn_node_get_has_input  (PN_NODE (node)));
    PN_CHECK (pn_node_get_has_output (PN_NODE (node)));

    /* A fresh engine reads zero, so a display wired up before the
     * first keystroke is not showing stale arithmetic. */
    PN_CHECK_NEAR   (display (node), 0.0, 1e-9);
    PN_CHECK_CMPSTR (display_text (node), ==, "0");
    PN_CHECK_FALSE  (pn_calc_engine_get_error (node));

    g_clear_object (&cap.last);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-calc-engine");
    pn_test_add ("four_operations",     test_four_operations);
    pn_test_add ("chained_operators",   test_chained_operators_show_running_total);
    pn_test_add ("equals_no_operator",  test_equals_without_operator);
    pn_test_add ("digit_after_result",  test_digit_after_result_starts_new_number);
    pn_test_add ("leading_zero",        test_leading_zero_is_a_placeholder);
    pn_test_add ("decimal_point",       test_decimal_point);
    pn_test_add ("point_first",         test_point_first_starts_zero_point);
    pn_test_add ("max_digits",          test_max_digits_caps_typing);
    pn_test_add ("clear_all",           test_clear_everything);
    pn_test_add ("clear_entry",         test_clear_entry_keeps_the_sum);
    pn_test_add ("divide_by_zero",      test_divide_by_zero_latches);
    pn_test_add ("error_dead_end",      test_error_is_a_dead_end_until_clear);
    pn_test_add ("emits_every_key",     test_emits_the_display_on_every_key);
    pn_test_add ("ignores_non_keys",    test_ignores_messages_without_a_key);
    pn_test_add ("unknown_key",         test_unknown_key_is_ignored);
    pn_test_add ("node_shape",          test_node_shape);
    return pn_test_run ();
}
