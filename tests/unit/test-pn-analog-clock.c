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

/* Unit tests for PnAnalogClock.  The node mirrors PnDigitalClock's
 * logic: a pure sink that reads a seconds count off data.value and,
 * taking it modulo a day, breaks it into the hours / minutes / seconds
 * the analogue face sweeps to.  The breakdown is GTK-free
 * (pn_analog_clock_get_paint_state), so these tests assert on it
 * directly without ever opening a display; the node never forwards a
 * message. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-analog-clock.h"

/* Feed one numeric data.value through the node's input. */
static void
feed (PnNode *node, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* Feed a value with a "timezone" abbreviation alongside it — the same
 * envelope shape the Clock node emits. */
static void
feed_tz (PnNode *node, gdouble value, const gchar *tz_abbr)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    if (tz_abbr != NULL)
        pn_message_set_string (m, "timezone", tz_abbr);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* Feed a value with a numeric "timezone" member — a raw UTC offset in
 * minutes east of Greenwich, the second wire form the node accepts. */
static void
feed_tz_min (PnNode *node, gdouble value, gint off_min)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_message_set_int (m, "timezone", off_min);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* Build a total seconds count for a d/h/m/s breakdown. */
static gint64
secs (gint64 d, gint h, gint m, gint s)
{
    return ((d * 24 + h) * 60 + m) * (gint64) 60 + s;
}

/* A sink never forwards: feeding it must emit nothing. */
static void
test_is_a_sink (void)
{
    guint   emits;
    PnNode *node = PN_NODE (pn_analog_clock_new ());

    emits = 0;
    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    feed (node, 12345.0);
    feed (node, 6789.0);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (node);
}

/* A within-a-day value splits into the expected hours / minutes / seconds. */
static void
test_breakdown (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    feed (PN_NODE (ac), (gdouble) secs (0, 20, 42, 50));
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 20);
    PN_CHECK_CMPINT (st.minutes, ==, 42);
    PN_CHECK_CMPINT (st.seconds, ==, 50);

    g_object_unref (ac);
}

/* A value beyond a day wraps into a 24-hour face — the whole-day part is
 * dropped rather than overflowing the hours field. */
static void
test_wraps_at_a_day (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    feed (PN_NODE (ac), (gdouble) secs (887, 1, 2, 3));
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 1);
    PN_CHECK_CMPINT (st.minutes, ==, 2);
    PN_CHECK_CMPINT (st.seconds, ==, 3);

    g_object_unref (ac);
}

/* Sub-hour and sub-minute remainders land in the right field. */
static void
test_breakdown_small (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    feed (PN_NODE (ac), (gdouble) secs (0, 0, 1, 5));   /* 65 seconds */
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 0);
    PN_CHECK_CMPINT (st.minutes, ==, 1);
    PN_CHECK_CMPINT (st.seconds, ==, 5);

    g_object_unref (ac);
}

/* Zero and negative values read as all zeroes rather than as a negative
 * clock. */
static void
test_zero_and_negative (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    feed (PN_NODE (ac), 0.0);
    pn_analog_clock_get_paint_state (ac, &st);
    PN_CHECK_CMPINT (st.hours, ==, 0);
    PN_CHECK_CMPINT (st.minutes, ==, 0);
    PN_CHECK_CMPINT (st.seconds, ==, 0);

    feed (PN_NODE (ac), -5000.0);
    pn_analog_clock_get_paint_state (ac, &st);
    PN_CHECK_CMPINT (st.hours, ==, 0);
    PN_CHECK_CMPINT (st.minutes, ==, 0);
    PN_CHECK_CMPINT (st.seconds, ==, 0);

    g_object_unref (ac);
}

/* Before any message the hands park at 12 (all zeroes). */
static void
test_initial_is_zero (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    pn_analog_clock_get_paint_state (ac, &st);
    PN_CHECK_CMPINT (st.hours, ==, 0);
    PN_CHECK_CMPINT (st.minutes, ==, 0);
    PN_CHECK_CMPINT (st.seconds, ==, 0);

    g_object_unref (ac);
}

/* An int64 value decodes the same as a double; a non-numeric value is
 * ignored and leaves the previous reading intact. */
static void
test_value_types (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;
    PnMessage              *m;

    m = pn_message_new (NULL, NULL);
    pn_message_set_int64 (m, "value", secs (0, 2, 3, 4));
    pn_node_receive_message (PN_NODE (ac), m);
    g_object_unref (m);

    pn_analog_clock_get_paint_state (ac, &st);
    PN_CHECK_CMPINT (st.hours,   ==, 2);
    PN_CHECK_CMPINT (st.minutes, ==, 3);
    PN_CHECK_CMPINT (st.seconds, ==, 4);

    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "soon");
    pn_node_receive_message (PN_NODE (ac), m);
    g_object_unref (m);

    pn_analog_clock_get_paint_state (ac, &st);
    PN_CHECK_CMPINT (st.hours,   ==, 2);
    PN_CHECK_CMPINT (st.seconds, ==, 4);

    g_object_unref (ac);
}

/* A fractional value floors toward zero. */
static void
test_fractional_floors (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    feed (PN_NODE (ac), 59.9);
    pn_analog_clock_get_paint_state (ac, &st);
    PN_CHECK_CMPINT (st.seconds, ==, 59);
    PN_CHECK_CMPINT (st.minutes, ==, 0);

    g_object_unref (ac);
}

/* The text/colour/show-seconds properties round-trip and surface in the
 * snapshot. */
static void
test_properties_round_trip (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;
    gboolean                seconds_on = TRUE;
    PnColor                 hand_in = { 0.10, 0.20, 0.30, 1.0 };
    PnColor                *hand_out = NULL;
    gchar                  *txt = NULL;

    g_object_set (ac,
                  "text",              "Pipnode",
                  "show-seconds",      FALSE,
                  "minute-hand-color", &hand_in,
                  NULL);
    g_object_get (ac,
                  "text",              &txt,
                  "show-seconds",      &seconds_on,
                  "minute-hand-color", &hand_out,
                  NULL);

    PN_CHECK        (seconds_on == FALSE);
    PN_CHECK        (g_strcmp0 (txt, "Pipnode") == 0);
    PN_CHECK        (pn_color_equal (hand_out, &hand_in));

    pn_analog_clock_get_paint_state (ac, &st);
    PN_CHECK        (st.show_seconds == FALSE);
    PN_CHECK        (g_strcmp0 (st.text, "Pipnode") == 0);
    PN_CHECK        (pn_color_equal (&st.minute_hand_color, &hand_in));

    g_free (txt);
    pn_color_free (hand_out);
    g_object_unref (ac);
}

/* A Unix-epoch reading is shown at UTC by default — value % 86400 gives
 * the seconds-of-day at GMT. */
static void
test_default_timezone_is_gmt (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    /* 2026-01-01 12:34:56 UTC = 1767270896 */
    feed (PN_NODE (ac), 1767270896.0);
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 12);
    PN_CHECK_CMPINT (st.minutes, ==, 34);
    PN_CHECK_CMPINT (st.seconds, ==, 56);

    g_object_unref (ac);
}

/* Setting the timezone property to a known label shifts the display by
 * the fixed offset; CET = UTC+1, so 12:34:56 UTC reads 13:34:56. */
static void
test_configured_timezone_shifts_display (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    g_object_set (ac, "timezone", "CET — Central European Time", NULL);

    feed (PN_NODE (ac), 1767270896.0);
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 13);
    PN_CHECK_CMPINT (st.minutes, ==, 34);
    PN_CHECK_CMPINT (st.seconds, ==, 56);

    g_object_unref (ac);
}

/* A negative-offset zone wraps the wall clock back over midnight: PST
 * is UTC-8, so 03:00 UTC the day after reads 19:00 the previous day. */
static void
test_negative_offset_wraps (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    g_object_set (ac, "timezone", "PST — Pacific Standard Time", NULL);

    /* 2026-01-02 03:00:00 UTC = 1767322800 */
    feed (PN_NODE (ac), 1767322800.0);
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 19);
    PN_CHECK_CMPINT (st.minutes, ==, 0);
    PN_CHECK_CMPINT (st.seconds, ==, 0);

    g_object_unref (ac);
}

/* With "Not Set" and a recognised abbreviation on the message, the
 * face follows the wire — exactly what the Clock node would supply.
 * CEST is UTC+2, so 10:00:00 UTC reads 12:00:00. */
static void
test_not_set_uses_message_abbreviation (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    /* 2026-06-01 10:00:00 UTC = 1780308000 */
    feed_tz (PN_NODE (ac), 1780308000.0, "CEST");
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 12);
    PN_CHECK_CMPINT (st.minutes, ==, 0);
    PN_CHECK_CMPINT (st.seconds, ==, 0);

    g_object_unref (ac);
}

/* "CST" names two rows (US Central and China); the table flags China as the
 * preferred resolution, so the wire abbreviation now lands on China Standard
 * Time (UTC+8) rather than falling back to GMT.  04:00 UTC reads 12:00. */
static void
test_not_set_cst_resolves_to_china (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    /* 2026-06-01 04:00:00 UTC = 1780286400 */
    feed_tz (PN_NODE (ac), 1780286400.0, "CST");
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 12);
    PN_CHECK_CMPINT (st.minutes, ==, 0);
    PN_CHECK_CMPINT (st.seconds, ==, 0);

    g_object_unref (ac);
}

/* A numeric "timezone" member is taken as a raw UTC offset in minutes.  With
 * "Not Set", +330 (India, +5h30) turns 10:00:00 UTC into 15:30:00. */
static void
test_not_set_numeric_offset (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    feed_tz_min (PN_NODE (ac), 1780308000.0, 330);
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 15);
    PN_CHECK_CMPINT (st.minutes, ==, 30);
    PN_CHECK_CMPINT (st.seconds, ==, 0);

    g_object_unref (ac);
}

/* A configured timezone wins outright: even if the message says CEST,
 * the dialog's choice rules the display. */
static void
test_configured_overrides_message (void)
{
    PnAnalogClock          *ac = pn_analog_clock_new ();
    PnAnalogClockPaintState st;

    g_object_set (ac, "timezone", "GMT — Greenwich Mean Time", NULL);
    feed_tz (PN_NODE (ac), 1780308000.0, "CEST");
    pn_analog_clock_get_paint_state (ac, &st);

    PN_CHECK_CMPINT (st.hours,   ==, 10);   /* GMT wins, not CEST */
    PN_CHECK_CMPINT (st.minutes, ==, 0);
    PN_CHECK_CMPINT (st.seconds, ==, 0);

    /* A numeric wire offset is overridden the same way. */
    feed_tz_min (PN_NODE (ac), 1780308000.0, 330);
    pn_analog_clock_get_paint_state (ac, &st);
    PN_CHECK_CMPINT (st.hours,   ==, 10);   /* still GMT, not +330 */
    PN_CHECK_CMPINT (st.minutes, ==, 0);

    g_object_unref (ac);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-analog-clock");
    pn_test_add ("is_a_sink",             test_is_a_sink);
    pn_test_add ("breakdown",             test_breakdown);
    pn_test_add ("wraps_at_a_day",        test_wraps_at_a_day);
    pn_test_add ("breakdown_small",       test_breakdown_small);
    pn_test_add ("zero_and_negative",     test_zero_and_negative);
    pn_test_add ("initial_is_zero",       test_initial_is_zero);
    pn_test_add ("value_types",           test_value_types);
    pn_test_add ("fractional_floors",     test_fractional_floors);
    pn_test_add ("properties_round_trip", test_properties_round_trip);
    pn_test_add ("default_timezone_is_gmt",
                 test_default_timezone_is_gmt);
    pn_test_add ("configured_timezone_shifts_display",
                 test_configured_timezone_shifts_display);
    pn_test_add ("negative_offset_wraps",
                 test_negative_offset_wraps);
    pn_test_add ("not_set_uses_message_abbreviation",
                 test_not_set_uses_message_abbreviation);
    pn_test_add ("not_set_cst_resolves_to_china",
                 test_not_set_cst_resolves_to_china);
    pn_test_add ("not_set_numeric_offset",
                 test_not_set_numeric_offset);
    pn_test_add ("configured_overrides_message",
                 test_configured_overrides_message);
    return pn_test_run ();
}
