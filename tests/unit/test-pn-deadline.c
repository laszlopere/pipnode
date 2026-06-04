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

/* Unit tests for PnDeadline: takes an incoming data.value holding the
 * current Unix epoch, interprets a configured "yyyy-mm-dd hh:MM:ss"
 * target in a fixed-offset timezone, and forwards the message with
 * data.value rewritten to the seconds remaining (target - now), which
 * may go negative once the target has passed.  Emission is synchronous
 * in receive(), so no main loop is needed. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-deadline.h"

/* Records the data.value of every forwarded message. */
typedef struct
{
    guint   count;
    gdouble seen[16];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;

    (void) node;
    if (r->count < G_N_ELEMENTS (r->seen))
        r->seen[r->count] = pn_test_num (message, "value");
    r->count++;
}

static PnNode *
make_node (const gchar *target, const gchar *timezone, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_deadline_new ());

    if (target != NULL)
        g_object_set (node, "target", target, NULL);
    if (timezone != NULL)
        g_object_set (node, "timezone", timezone, NULL);

    rec->count = 0;
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

/* Feed one numeric "now" epoch through the node. */
static void
feed (PnNode *node, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", value);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* The Unix epoch of a UTC wall-clock instant, for building expectations
 * the same way the node resolves its target. */
static gint64
epoch_utc (gint y, gint mo, gint d, gint h, gint mi, gint s)
{
    GDateTime *dt = g_date_time_new_utc (y, mo, d, h, mi, (gdouble) s);
    gint64     e  = g_date_time_to_unix (dt);
    g_date_time_unref (dt);
    return e;
}

/* A future target in UTC: feeding a "now" one hour earlier than the
 * target yields exactly 3600 seconds remaining. */
static void
test_future_target_utc (void)
{
    Recorder rec;
    PnNode  *node;
    gint64   target = epoch_utc (2030, 1, 1, 0, 0, 0);

    node = make_node ("2030-01-01 00:00:00",
                      "UTC — Coordinated Universal Time", &rec);

    feed (node, (gdouble) (target - 3600));

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 3600.0, 1e-6);

    g_object_unref (node);
}

/* The same wall-clock target read in CET (UTC+1) is one hour EARLIER in
 * absolute time than in UTC, so for an identical "now" the seconds
 * remaining are 3600 smaller. */
static void
test_offset_applied (void)
{
    Recorder rec_utc, rec_cet;
    PnNode  *utc, *cet;
    gint64   target = epoch_utc (2030, 1, 1, 0, 0, 0);
    gdouble  now    = (gdouble) (target - 5000);

    utc = make_node ("2030-01-01 00:00:00",
                     "UTC — Coordinated Universal Time", &rec_utc);
    cet = make_node ("2030-01-01 00:00:00",
                     "CET — Central European Time", &rec_cet);

    feed (utc, now);
    feed (cet, now);

    PN_CHECK_CMPINT (rec_utc.count, ==, 1);
    PN_CHECK_CMPINT (rec_cet.count, ==, 1);
    PN_CHECK_NEAR   (rec_utc.seen[0], 5000.0, 1e-6);
    PN_CHECK_NEAR   (rec_cet.seen[0], 5000.0 - 3600.0, 1e-6);

    g_object_unref (utc);
    g_object_unref (cet);
}

/* An empty timezone is treated as GMT (UTC+0), so it matches the UTC
 * result exactly. */
static void
test_empty_timezone_is_gmt (void)
{
    Recorder rec;
    PnNode  *node;
    gint64   target = epoch_utc (2030, 1, 1, 0, 0, 0);

    node = make_node ("2030-01-01 00:00:00", "", &rec);

    feed (node, (gdouble) (target - 100));

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 100.0, 1e-6);

    g_object_unref (node);
}

/* A target in the past yields a negative count. */
static void
test_negative_after_target (void)
{
    Recorder rec;
    PnNode  *node;
    gint64   now = epoch_utc (2030, 1, 1, 0, 0, 0);

    node = make_node ("2000-01-01 00:00:00",
                      "UTC — Coordinated Universal Time", &rec);

    feed (node, (gdouble) now);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK        (rec.seen[0] < 0.0);

    g_object_unref (node);
}

/* A malformed target string emits nothing; a non-numeric value emits
 * nothing either. */
static void
test_bad_input_silent (void)
{
    Recorder   rec;
    PnNode    *node;
    PnMessage *m;

    /* Unparseable target -> silent even with a valid numeric value. */
    node = make_node ("not a date",
                      "UTC — Coordinated Universal Time", &rec);
    feed (node, 1000000.0);
    PN_CHECK_CMPINT (rec.count, ==, 0);
    g_object_unref (node);

    /* Valid target, but a string value -> silent. */
    node = make_node ("2030-01-01 00:00:00",
                      "UTC — Coordinated Universal Time", &rec);
    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "value", "soon");
    pn_node_receive_message (node, m);
    g_object_unref (m);
    PN_CHECK_CMPINT (rec.count, ==, 0);
    g_object_unref (node);
}

/* The default timezone is "Not Set": with no fixed zone and no message-
 * level abbreviation, the node interprets the target as GMT (UTC+0). */
static void
test_default_timezone_is_not_set (void)
{
    Recorder rec;
    PnNode  *node;
    gchar   *tz     = NULL;
    gint64   target = epoch_utc (2030, 1, 1, 0, 0, 0);

    /* Construct without overriding the timezone — the default applies. */
    node = make_node ("2030-01-01 00:00:00", NULL, &rec);

    g_object_get (node, "timezone", &tz, NULL);
    PN_CHECK_CMPSTR (tz, ==, "Not Set");

    feed (node, (gdouble) (target - 100));

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 100.0, 1e-6);

    g_free (tz);
    g_object_unref (node);
}

/* With "Not Set" and a recognised abbreviation on the message, the node
 * interprets the target in that zone — CEST is UTC+2, so a target read
 * in CEST resolves to an absolute instant 7200s earlier than the same
 * wall clock in UTC. */
static void
test_not_set_uses_message_abbreviation (void)
{
    Recorder   rec;
    PnNode    *node;
    PnMessage *m;
    gint64     target_utc = epoch_utc (2030, 6, 1, 12, 0, 0);

    node = make_node ("2030-06-01 12:00:00", "Not Set", &rec);

    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", (gdouble) target_utc);
    pn_message_set_string (m, "timezone", "CEST");
    pn_node_receive_message (node, m);
    g_object_unref (m);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], -7200.0, 1e-6);

    g_object_unref (node);
}

/* An unknown abbreviation on the wire falls back to GMT — so the
 * remaining count matches the empty-tz / GMT cases. */
static void
test_not_set_unknown_abbreviation_is_gmt (void)
{
    Recorder   rec;
    PnNode    *node;
    PnMessage *m;
    gint64     target = epoch_utc (2030, 1, 1, 0, 0, 0);

    node = make_node ("2030-01-01 00:00:00", "Not Set", &rec);

    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", (gdouble) (target - 250));
    pn_message_set_string (m, "timezone", "ZZZ");
    pn_node_receive_message (node, m);
    g_object_unref (m);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 250.0, 1e-6);

    g_object_unref (node);
}

/* "CST" names two rows (US Central and China); the table flags China as the
 * preferred resolution, so the wire abbreviation now reads the target as
 * China Standard Time (UTC+8).  The target wall clock is then 8h = 28800s
 * earlier in absolute time than at GMT, so the same "now" instant sits
 * 28800s past the GMT reading: 17 - 28800 = -28783 seconds remaining. */
static void
test_not_set_cst_resolves_to_china (void)
{
    Recorder   rec;
    PnNode    *node;
    PnMessage *m;
    gint64     target = epoch_utc (2030, 1, 1, 0, 0, 0);

    node = make_node ("2030-01-01 00:00:00", "Not Set", &rec);

    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", (gdouble) (target - 17));
    pn_message_set_string (m, "timezone", "CST");
    pn_node_receive_message (node, m);
    g_object_unref (m);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 17.0 - 28800.0, 1e-6);

    g_object_unref (node);
}

/* A configured timezone wins outright: even if the message says CEST,
 * the dialog's choice rules the resolution. */
static void
test_configured_overrides_message (void)
{
    Recorder   rec;
    PnNode    *node;
    PnMessage *m;
    gint64     target_utc = epoch_utc (2030, 6, 1, 12, 0, 0);

    node = make_node ("2030-06-01 12:00:00",
                      "UTC — Coordinated Universal Time", &rec);

    m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, "value", (gdouble) target_utc);
    pn_message_set_string (m, "timezone", "CEST");   /* ignored */
    pn_node_receive_message (node, m);
    g_object_unref (m);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (rec.seen[0], 0.0, 1e-6);

    g_object_unref (node);
}

/* receive() mutates and forwards in place, so unrelated members and the
 * topic survive; only "value" is rewritten. */
static void
test_preserves_other_members (void)
{
    Recorder   rec;
    PnNode    *node;
    PnMessage *m;
    gint64     target = epoch_utc (2030, 1, 1, 0, 0, 0);

    node = make_node ("2030-01-01 00:00:00",
                      "UTC — Coordinated Universal Time", &rec);

    m = pn_message_new (NULL, "clock/tick");
    pn_message_set_double (m, "value", (gdouble) (target - 42));
    pn_message_set_string (m, "timezone", "CET");
    pn_node_receive_message (node, m);

    PN_CHECK_CMPINT (rec.count, ==, 1);
    PN_CHECK_NEAR   (pn_test_num (m, "value"), 42.0, 1e-6);
    PN_CHECK_CMPSTR (pn_test_str (m, "timezone"), ==, "CET");
    PN_CHECK_CMPSTR (pn_message_get_topic (m), ==, "clock/tick");

    g_object_unref (m);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-deadline");
    pn_test_add ("future_target_utc",   test_future_target_utc);
    pn_test_add ("offset_applied",      test_offset_applied);
    pn_test_add ("empty_tz_is_gmt",     test_empty_timezone_is_gmt);
    pn_test_add ("negative_after",      test_negative_after_target);
    pn_test_add ("bad_input_silent",    test_bad_input_silent);
    pn_test_add ("default_timezone_is_not_set",
                 test_default_timezone_is_not_set);
    pn_test_add ("not_set_uses_message_abbreviation",
                 test_not_set_uses_message_abbreviation);
    pn_test_add ("not_set_unknown_abbreviation_is_gmt",
                 test_not_set_unknown_abbreviation_is_gmt);
    pn_test_add ("not_set_cst_resolves_to_china",
                 test_not_set_cst_resolves_to_china);
    pn_test_add ("configured_overrides_message",
                 test_configured_overrides_message);
    pn_test_add ("preserves_members",   test_preserves_other_members);
    return pn_test_run ();
}
