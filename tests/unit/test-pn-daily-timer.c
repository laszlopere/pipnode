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

/* Unit tests for PnDailyTimer: a weekly on/off schedule that emits
 * data.value = 1.0 on entering an interval and 0.0 on leaving it.
 *
 * The containment tests drive pn_daily_timer_state_at(), which takes
 * the instant as an argument instead of reading the clock -- that is
 * what makes the interval semantics (half-open bounds, midnight wrap,
 * "Every day" rows) assertable without waiting for real time to pass.
 *
 * The emission tests drive pn_auto_trigger_run_once_sync() against the
 * real clock, so they assert only the clock-independent contract: the
 * first poll always announces, an unchanged state stays silent, and the
 * announced value agrees with what state_at() reports for right now. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-daily-timer.h"

/* Weekday numbering matches g_date_time_get_day_of_week(). */
#define MON 1
#define TUE 2
#define WED 3
#define FRI 5
#define SAT 6
#define SUN 7

/* Quiescent instance: "autostart" = FALSE keeps PnAutoTrigger from
 * spawning its worker thread, so the test drives every tick by hand. */
static PnDailyTimer *
make_timer (const gchar *schedule)
{
    PnDailyTimer *timer = g_object_new (PN_TYPE_DAILY_TIMER,
                                        "autostart", FALSE,
                                        NULL);

    if (schedule != NULL)
        g_object_set (timer, "schedule", schedule, NULL);

    return timer;
}

static void
test_empty_schedule_is_always_off (void)
{
    /* Explicitly empty: the default schedule is a worked example, not
     * an empty array. */
    PnDailyTimer *timer = make_timer ("[]");

    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, MON,  0,  0));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, WED, 12, 30));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, SUN, 23, 59));

    g_object_unref (timer);
}

/* A freshly constructed node carries one example interval -- every day,
 * 07:00 to 09:00 -- so dropping a Daily Timer onto a sheet and opening
 * it shows what an interval looks like instead of a blank grid. */
static void
test_default_schedule_is_example_interval (void)
{
    PnDailyTimer *timer = make_timer (NULL);

    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, MON,  6, 59));
    PN_CHECK       (pn_daily_timer_state_at (timer, MON,  7,  0));
    PN_CHECK       (pn_daily_timer_state_at (timer, SUN,  8, 30));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, SUN,  9,  0));

    g_object_unref (timer);
}

static void
test_interval_bounds_are_half_open (void)
{
    /* Monday 07:00 -> 09:00.  The on minute is inside the interval, the
     * off minute is not, so two back-to-back intervals neither overlap
     * nor leave a one-minute hole between them. */
    PnDailyTimer *timer = make_timer (
            "[{\"day\":1,\"on_hour\":7,\"on_minute\":0,"
            "  \"off_hour\":9,\"off_minute\":0}]");

    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, MON, 6, 59));
    PN_CHECK       (pn_daily_timer_state_at (timer, MON, 7,  0));
    PN_CHECK       (pn_daily_timer_state_at (timer, MON, 8, 30));
    PN_CHECK       (pn_daily_timer_state_at (timer, MON, 8, 59));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, MON, 9,  0));

    /* Same clock time on any other day is outside a Monday-only row. */
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, TUE, 8, 30));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, SUN, 8, 30));

    g_object_unref (timer);
}

static void
test_every_day_matches_all_weekdays (void)
{
    /* day = -1 fires on all seven days. */
    PnDailyTimer *timer = make_timer (
            "[{\"day\":-1,\"on_hour\":22,\"on_minute\":30,"
            "  \"off_hour\":23,\"off_minute\":0}]");
    gint day;

    for (day = MON; day <= SUN; day++)
    {
        PN_CHECK       (pn_daily_timer_state_at (timer, day, 22, 45));
        PN_CHECK_FALSE (pn_daily_timer_state_at (timer, day, 23, 15));
    }

    g_object_unref (timer);
}

static void
test_interval_wraps_past_midnight (void)
{
    /* Friday 22:00 -> 06:00: an off time at or before the on time runs
     * into the following day, so the row is in force on Friday evening
     * *and* on Saturday morning -- but not on Friday morning, and not
     * once Saturday's own 06:00 has passed. */
    PnDailyTimer *timer = make_timer (
            "[{\"day\":5,\"on_hour\":22,\"on_minute\":0,"
            "  \"off_hour\":6,\"off_minute\":0}]");

    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, FRI, 21, 59));
    PN_CHECK       (pn_daily_timer_state_at (timer, FRI, 22,  0));
    PN_CHECK       (pn_daily_timer_state_at (timer, FRI, 23, 59));
    PN_CHECK       (pn_daily_timer_state_at (timer, SAT,  0,  0));
    PN_CHECK       (pn_daily_timer_state_at (timer, SAT,  5, 59));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, SAT,  6,  0));

    /* Friday's small hours belong to Thursday's (absent) row. */
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, FRI, 2, 0));

    g_object_unref (timer);
}

static void
test_sunday_wrap_lands_on_monday (void)
{
    /* The previous-day lookup wraps the week, not just the day: a
     * Sunday 23:00 -> 01:00 row is still on at 00:30 on Monday. */
    PnDailyTimer *timer = make_timer (
            "[{\"day\":7,\"on_hour\":23,\"on_minute\":0,"
            "  \"off_hour\":1,\"off_minute\":0}]");

    PN_CHECK       (pn_daily_timer_state_at (timer, SUN, 23, 30));
    PN_CHECK       (pn_daily_timer_state_at (timer, MON,  0, 30));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, MON,  1,  0));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, SAT, 23, 30));

    g_object_unref (timer);
}

static void
test_overlapping_intervals_union (void)
{
    /* Any matching interval turns the timer on: overlapping rows union
     * rather than fight, so 06:00-08:00 plus 07:00-10:00 is on for the
     * whole 06:00-10:00 span. */
    PnDailyTimer *timer = make_timer (
            "[{\"day\":1,\"on_hour\":6,\"on_minute\":0,"
            "  \"off_hour\":8,\"off_minute\":0},"
            " {\"day\":1,\"on_hour\":7,\"on_minute\":0,"
            "  \"off_hour\":10,\"off_minute\":0}]");

    PN_CHECK       (pn_daily_timer_state_at (timer, MON, 6, 30));
    PN_CHECK       (pn_daily_timer_state_at (timer, MON, 7, 30));
    PN_CHECK       (pn_daily_timer_state_at (timer, MON, 9, 30));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, MON, 10, 0));

    g_object_unref (timer);
}

static void
test_degenerate_and_invalid_rows_ignored (void)
{
    /* on == off is degenerate (zero-length and all-day are equally
     * defensible readings) and is dropped at compile time.  An
     * out-of-range day falls back to "every day" rather than vanishing;
     * out-of-range times clamp instead of wrapping. */
    PnDailyTimer *timer = make_timer (
            "[{\"day\":1,\"on_hour\":8,\"on_minute\":0,"
            "  \"off_hour\":8,\"off_minute\":0},"
            " {\"day\":99,\"on_hour\":40,\"on_minute\":0,"
            "  \"off_hour\":23,\"off_minute\":90}]");

    /* The degenerate Monday row contributes nothing at its own time. */
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, MON, 8, 0));

    /* The salvaged row became every-day 23:00 -> 23:59. */
    PN_CHECK       (pn_daily_timer_state_at (timer, WED, 23, 30));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, WED, 23, 59));

    g_object_unref (timer);
}

/* Swallows the warning compile_schedule() logs for a malformed schedule
 * string so the expected-failure case below does not spew to stderr.
 * (The harness does not run under g_test_init(), so the
 * g_test_expect_message() machinery is unavailable here.) */
static void
swallow_log (const gchar    *domain,
             GLogLevelFlags  level,
             const gchar    *message,
             gpointer        user_data)
{
    (void) domain; (void) level; (void) message; (void) user_data;
}

static void
test_malformed_json_compiles_to_empty (void)
{
    PnDailyTimer *timer = make_timer (NULL);
    GLogFunc      prev;

    /* A hand-mangled file must not take the node down: the schedule
     * compiles to nothing and the timer simply stays off. */
    prev = g_log_set_default_handler (swallow_log, NULL);
    g_object_set (timer, "schedule", "{ not json", NULL);
    g_log_set_default_handler (prev, NULL);

    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, WED, 12, 0));

    g_object_unref (timer);
}

/* ------------------------------------------------------------------ */
/*  Emission: first tick announces, unchanged ticks stay silent         */
/* ------------------------------------------------------------------ */

static void
test_first_tick_announces_current_state (void)
{
    guint         emits = 0;
    PnDailyTimer *timer = make_timer (
            "[{\"day\":-1,\"on_hour\":0,\"on_minute\":0,"
            "  \"off_hour\":23,\"off_minute\":59}]");

    g_signal_connect (timer, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* The node has never polled, so it has no idea what its downstream
     * consumers believe: the first tick emits whichever state it finds,
     * which is the worksheet-load announce.  This schedule is on for
     * all but the last minute of every day; the assertion below is
     * written against state_at() rather than a hardcoded 1.0 so it
     * holds during that minute too. */
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (timer));
    PN_CHECK_CMPINT (emits, ==, 1);

    /* Nothing about the clock changed the state, so the node stays
     * quiet: a downstream relay sees one message per switch-over, not
     * one per poll. */
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (timer));
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (timer));
    PN_CHECK_CMPINT (emits, ==, 1);

    g_object_unref (timer);
}

/* Capture the value / success / output of the last emitted message. */
static void
capture_message (PnNode *node, PnMessage *message, gpointer user_data)
{
    PnMessage **out = user_data;

    (void) node;

    g_clear_object (out);
    *out = g_object_ref (message);
}

static void
test_announced_payload_matches_schedule (void)
{
    PnMessage    *last  = NULL;
    PnDailyTimer *timer = make_timer (
            "[{\"day\":-1,\"on_hour\":0,\"on_minute\":0,"
            "  \"off_hour\":23,\"off_minute\":59}]");
    GDateTime    *now;
    gboolean      expected;

    g_signal_connect (timer, "message", G_CALLBACK (capture_message), &last);

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (timer));

    now      = g_date_time_new_now_local ();
    expected = pn_daily_timer_state_at (timer,
                                        g_date_time_get_day_of_week (now),
                                        g_date_time_get_hour        (now),
                                        g_date_time_get_minute      (now));
    g_date_time_unref (now);

    PN_CHECK (last != NULL);
    PN_CHECK_NEAR   (pn_test_num  (last, "value"), expected ? 1.0 : 0.0, 1e-9);
    PN_CHECK_CMPINT (pn_test_bool (last, "success"), ==, expected);
    PN_CHECK_CMPSTR (pn_test_str  (last, "output"), ==,
                     expected ? "on" : "off");

    /* The state the node reports back matches what it just announced. */
    PN_CHECK_CMPINT (pn_daily_timer_get_active (timer), ==, expected);

    g_clear_object (&last);
    g_object_unref (timer);
}

static void
test_schedule_edit_forces_reannounce (void)
{
    guint         emits = 0;
    PnDailyTimer *timer = make_timer (
            "[{\"day\":-1,\"on_hour\":0,\"on_minute\":0,"
            "  \"off_hour\":23,\"off_minute\":59}]");

    g_signal_connect (timer, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (timer));
    PN_CHECK_CMPINT (emits, ==, 1);

    /* Editing the schedule invalidates the remembered state.  Even
     * though this new schedule may well imply the *same* state as the
     * old one, the next poll re-announces: the user changed the rules,
     * and the downstream relay is entitled to hear the verdict. */
    g_object_set (timer, "schedule",
                  "[{\"day\":-1,\"on_hour\":1,\"on_minute\":0,"
                  "  \"off_hour\":23,\"off_minute\":58}]",
                  NULL);

    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (timer));
    PN_CHECK_CMPINT (emits, ==, 2);

    /* ... and then goes quiet again. */
    pn_auto_trigger_run_once_sync (PN_AUTO_TRIGGER (timer));
    PN_CHECK_CMPINT (emits, ==, 2);

    g_object_unref (timer);
}

static void
test_schedule_property_round_trips (void)
{
    const gchar  *schedule =
            "[{\"day\":3,\"on_hour\":7,\"on_minute\":15,"
            "\"off_hour\":9,\"off_minute\":45}]";
    PnDailyTimer *timer = make_timer (schedule);
    gchar        *out   = NULL;

    /* The property is the serialization format: what the editor writes
     * is what the file carries and what a reload compiles. */
    g_object_get (timer, "schedule", &out, NULL);
    PN_CHECK_CMPSTR (out, ==, schedule);

    PN_CHECK       (pn_daily_timer_state_at (timer, WED, 8, 0));
    PN_CHECK_FALSE (pn_daily_timer_state_at (timer, WED, 7, 14));

    g_free (out);
    g_object_unref (timer);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-daily-timer");
    pn_test_add ("empty_always_off",     test_empty_schedule_is_always_off);
    pn_test_add ("default_is_example",   test_default_schedule_is_example_interval);
    pn_test_add ("bounds_half_open",     test_interval_bounds_are_half_open);
    pn_test_add ("every_day_matches",    test_every_day_matches_all_weekdays);
    pn_test_add ("wraps_past_midnight",  test_interval_wraps_past_midnight);
    pn_test_add ("sunday_wraps_to_mon",  test_sunday_wrap_lands_on_monday);
    pn_test_add ("overlaps_union",       test_overlapping_intervals_union);
    pn_test_add ("bad_rows_ignored",     test_degenerate_and_invalid_rows_ignored);
    pn_test_add ("malformed_json_empty", test_malformed_json_compiles_to_empty);
    pn_test_add ("first_tick_announces", test_first_tick_announces_current_state);
    pn_test_add ("payload_matches",      test_announced_payload_matches_schedule);
    pn_test_add ("edit_reannounces",     test_schedule_edit_forces_reannounce);
    pn_test_add ("property_round_trips", test_schedule_property_round_trips);
    return pn_test_run ();
}
