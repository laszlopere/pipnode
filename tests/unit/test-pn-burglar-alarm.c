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

/* Unit tests for PnBurglarAlarm: the five-input, five-output state
 * machine of an intruder alarm panel.  The delays are driven by
 * pn_burglar_alarm_tick() rather than by the node's own 1 Hz timer, so
 * a thirty-second exit delay is thirty cheap calls and the suite never
 * waits.  Headless: no IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-burglar-alarm.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Recorder                                                           */
/* ------------------------------------------------------------------ */

/* Every output is watched at once: the panel's whole point is that one
 * event writes to several of them, so a test asserts on the set. */
typedef struct
{
    guint      count[PN_BURGLAR_ALARM_N_OUTPUTS];
    PnMessage *last[PN_BURGLAR_ALARM_N_OUTPUTS];
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r   = user_data;
    gint      out = pn_node_current_output ();

    (void) node;
    if (out < 0 || out >= PN_BURGLAR_ALARM_N_OUTPUTS)
        return;

    r->count[out]++;
    g_clear_object (&r->last[out]);
    r->last[out] = g_object_ref (message);
}

static void
recorder_clear (Recorder *r)
{
    gint i;

    for (i = 0; i < PN_BURGLAR_ALARM_N_OUTPUTS; i++)
    {
        r->count[i] = 0;
        g_clear_object (&r->last[i]);
    }
}

/* A panel with nothing yet run: its startup announce is still sitting
 * on the main loop.  Only the announce test itself wants this. */
static PnBurglarAlarm *
make_node_raw (Recorder *rec)
{
    PnBurglarAlarm *node = g_object_new (PN_TYPE_BURGLAR_ALARM, NULL);

    memset (rec, 0, sizeof *rec);
    g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    return node;
}

/* A panel in the state every other test wants to start from: the
 * startup announce has run and been forgotten, exactly as it has by the
 * time a user touches a loaded worksheet. */
static PnBurglarAlarm *
make_node (Recorder *rec)
{
    PnBurglarAlarm *node = make_node_raw (rec);

    while (g_main_context_iteration (NULL, FALSE))
        ;
    recorder_clear (rec);
    return node;
}

static void
destroy_node (PnBurglarAlarm *node, Recorder *rec)
{
    recorder_clear (rec);
    g_object_unref (node);
}

static PnMessage *
seen (Recorder *r, gint output)
{
    return r->last[output];
}

/* ------------------------------------------------------------------ */
/*  Driving                                                            */
/* ------------------------------------------------------------------ */

/* Type a whole sequence, one key per space-separated token, so a test
 * reads like the keys the user pressed: "1 2 3 4 #". */
static void
type (PnBurglarAlarm *node, const gchar *keys)
{
    gchar **tokens = g_strsplit (keys, " ", -1);
    guint   i;

    for (i = 0; tokens[i] != NULL; i++)
        if (tokens[i][0] != '\0')
            pn_burglar_alarm_press (node, tokens[i]);

    g_strfreev (tokens);
}

/* Deliver one message on @input the way a wire would, so that
 * pn_node_current_input() is set inside receive(). */
static void
send_on (PnBurglarAlarm *node, gint input, PnMessage *m)
{
    pn_node_receive_message_on_input (PN_NODE (node), m, input);
    g_object_unref (m);
}

static void
send_key (PnBurglarAlarm *node, const gchar *key)
{
    PnMessage *m = pn_message_new (NULL, NULL);

    pn_message_set_string (m, "key", key);
    send_on (node, PN_BURGLAR_ALARM_IN_KEYPAD, m);
}

static void
send_zone_value (PnBurglarAlarm *node, gint input, gdouble value)
{
    PnMessage *m = pn_message_new (NULL, NULL);

    pn_message_set_double (m, "value", value);
    send_on (node, input, m);
}

static void
tick_n (PnBurglarAlarm *node, guint n)
{
    guint i;

    for (i = 0; i < n; i++)
        pn_burglar_alarm_tick (node);
}

/* The first line of the readout — the banner. */
static gchar *
banner (PnBurglarAlarm *node)
{
    gchar  *text  = pn_burglar_alarm_get_display_text (node);
    gchar **lines = g_strsplit (text, "\n", -1);
    gchar  *first = g_strdup (lines[0]);

    g_strfreev (lines);
    g_free (text);
    return first;
}

/* Arm the panel and let the exit delay run out, quietly. */
static void
arm_now (PnBurglarAlarm *node)
{
    g_object_set (node, "exit-delay", 0u, NULL);
    type (node, "1 2 3 4 #");
}

/* ------------------------------------------------------------------ */
/*  Shape                                                              */
/* ------------------------------------------------------------------ */

static void
test_node_shape (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    PnNode         *n    = PN_NODE (node);

    PN_CHECK (pn_node_get_has_input  (n));
    PN_CHECK (pn_node_get_has_output (n));
    PN_CHECK_CMPINT (pn_node_get_n_inputs  (n), ==, 5);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (n), ==, 5);

    PN_CHECK_CMPSTR (pn_node_get_input_name (n, 0), ==, "keypad");
    PN_CHECK_CMPSTR (pn_node_get_input_name (n, 1), ==, "delayed");
    PN_CHECK_CMPSTR (pn_node_get_input_name (n, 4), ==, "zone 3");

    PN_CHECK_CMPSTR (pn_node_get_output_name (n, 0), ==, "alarm");
    PN_CHECK_CMPSTR (pn_node_get_output_name (n, 1), ==, "display");
    PN_CHECK_CMPSTR (pn_node_get_output_name (n, 2), ==, "armed");
    PN_CHECK_CMPSTR (pn_node_get_output_name (n, 3), ==, "speech");
    PN_CHECK_CMPSTR (pn_node_get_output_name (n, 4), ==, "sms");

    /* A fresh panel is off, ready, and quiet. */
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_DISARMED);
    PN_CHECK (pn_burglar_alarm_get_ready (node));
    PN_CHECK_FALSE (pn_burglar_alarm_get_siren (node));
    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "");

    destroy_node (node, &rec);
}

static void
test_startup_announce (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node_raw (&rec);

    /* The announce is a main-loop idle, so nothing has been said yet. */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_DISPLAY], ==, 0);

    while (g_main_context_iteration (NULL, FALSE))
        ;

    /* The three outputs that carry state speak once: a wired LCD is not
     * left blank and a wired siren is explicitly told to be quiet. */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_ALARM],   ==, 1);
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_ARMED],   ==, 1);
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_DISPLAY], ==, 1);
    PN_CHECK_NEAR (pn_test_num (seen (&rec, PN_BURGLAR_ALARM_OUT_ALARM),
                                "value"), 0.0, 1e-9);
    PN_CHECK_NEAR (pn_test_num (seen (&rec, PN_BURGLAR_ALARM_OUT_ARMED),
                                "value"), 0.0, 1e-9);

    /* The two that carry words stay silent: opening a worksheet must
     * not talk to the room or send a radio message. */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SPEECH], ==, 0);
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SMS],    ==, 0);

    destroy_node (node, &rec);
}

/* ------------------------------------------------------------------ */
/*  Arming                                                             */
/* ------------------------------------------------------------------ */

static void
test_code_arms_through_the_exit_delay (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    gchar          *b;

    g_object_set (node, "exit-delay", 3u, NULL);
    type (node, "1 2 3 4 #");

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_EXIT);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_remaining (node), ==, 3);

    /* Not armed yet: the armed output still says nothing has changed. */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_ARMED], ==, 0);

    b = banner (node);
    PN_CHECK_CMPSTR (b, ==, "EXIT 3");
    g_free (b);

    tick_n (node, 1);
    b = banner (node);
    PN_CHECK_CMPSTR (b, ==, "EXIT 2");
    g_free (b);

    tick_n (node, 2);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ARMED);
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_ARMED], ==, 1);
    PN_CHECK_NEAR (pn_test_num (seen (&rec, PN_BURGLAR_ALARM_OUT_ARMED),
                                "value"), 1.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_ARMED),
                                  "output"), ==, "ARMED");
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_ARMED),
                                  "state"), ==, "armed");

    destroy_node (node, &rec);
}

static void
test_zero_exit_delay_arms_at_once (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "exit-delay", 0u, NULL);
    type (node, "1 2 3 4 #");

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ARMED);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_remaining (node), ==, 0);

    destroy_node (node, &rec);
}

static void
test_wrong_code_changes_nothing (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    gchar          *text;

    type (node, "9 9 9 9 #");

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_DISARMED);
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_ARMED], ==, 0);

    text = pn_burglar_alarm_get_display_text (node);
    PN_CHECK (strstr (text, "WRONG CODE") != NULL);
    g_free (text);

    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_SPEECH),
                                  "output"), ==, "Wrong code.");

    /* The complaint is a beep, not a latch: the next keystroke clears
     * it, and the code that follows is judged on its own. */
    type (node, "1 2 3 4 #");
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_EXIT);

    destroy_node (node, &rec);
}

static void
test_empty_code_means_no_code (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "code", "", "exit-delay", 0u, NULL);
    type (node, "#");

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ARMED);

    destroy_node (node, &rec);
}

static void
test_open_instant_zone_refuses_arming (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    gchar          *b;

    pn_burglar_alarm_set_zone (node, 2, TRUE);      /* "zone 2" open */
    PN_CHECK_FALSE (pn_burglar_alarm_get_ready (node));

    b = banner (node);
    PN_CHECK_CMPSTR (b, ==, "NOT READY");
    g_free (b);

    type (node, "1 2 3 4 #");
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_DISARMED);
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_SPEECH),
                                  "output"), ==, "Not ready. zone 2 is open.");

    /* Close it and the same code works. */
    pn_burglar_alarm_set_zone (node, 2, FALSE);
    PN_CHECK (pn_burglar_alarm_get_ready (node));

    g_object_set (node, "exit-delay", 0u, NULL);
    type (node, "1 2 3 4 #");
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ARMED);

    destroy_node (node, &rec);
}

static void
test_open_delayed_zone_still_arms (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    /* The front door standing open is the door being left through, not
     * a reason to refuse. */
    pn_burglar_alarm_set_zone (node, PN_BURGLAR_ALARM_ZONE_DELAYED, TRUE);
    PN_CHECK (pn_burglar_alarm_get_ready (node));

    g_object_set (node, "exit-delay", 1u, "entry-delay", 5u, NULL);
    type (node, "1 2 3 4 #");
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_EXIT);

    /* Still open when the exit delay runs out: the panel arms and then
     * immediately starts the entry delay on it. */
    tick_n (node, 1);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ENTRY);

    destroy_node (node, &rec);
}

static void
test_momentary_trip_leaves_the_zone_closed (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    /* A PIR firing while the panel is disarmed must not keep it from
     * arming — it has no resting "open" state to clear. */
    pn_burglar_alarm_trip_zone (node, 1);
    PN_CHECK_FALSE (pn_burglar_alarm_get_zone_open (node, 1));
    PN_CHECK (pn_burglar_alarm_get_ready (node));

    /* And it says nothing: a PIR in a hallway would otherwise flood the
     * display wire all day. */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_DISPLAY], ==, 0);

    destroy_node (node, &rec);
}

/* ------------------------------------------------------------------ */
/*  Zones                                                              */
/* ------------------------------------------------------------------ */

static void
test_instant_zone_alarms_at_once (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    arm_now (node);
    recorder_clear (&rec);

    pn_burglar_alarm_trip_zone (node, 3);

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ALARM);
    PN_CHECK (pn_burglar_alarm_get_siren (node));
    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "zone 3");

    PN_CHECK_NEAR (pn_test_num (seen (&rec, PN_BURGLAR_ALARM_OUT_ALARM),
                                "value"), 1.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_ALARM),
                                  "output"), ==, "ALARM: zone 3");
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_ALARM),
                                  "cause"), ==, "zone 3");
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_SPEECH),
                                  "output"), ==, "Alarm! zone 3.");
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SMS], ==, 1);

    /* Still armed while ringing — an alarm does not disarm the panel. */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_ARMED], ==, 0);

    /* And the node paints itself red on the worksheet. */
    PN_CHECK (pn_node_get_has_error (PN_NODE (node)));

    destroy_node (node, &rec);
}

static void
test_delayed_zone_counts_before_the_siren (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    gchar          *b;

    g_object_set (node, "entry-delay", 3u, NULL);
    arm_now (node);
    recorder_clear (&rec);

    pn_burglar_alarm_set_zone (node, PN_BURGLAR_ALARM_ZONE_DELAYED, TRUE);

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ENTRY);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_remaining (node), ==, 3);
    PN_CHECK_FALSE (pn_burglar_alarm_get_siren (node));
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_ALARM], ==, 0);

    b = banner (node);
    PN_CHECK_CMPSTR (b, ==, "ENTRY 3");
    g_free (b);

    tick_n (node, 3);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ALARM);
    PN_CHECK (pn_burglar_alarm_get_siren (node));
    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "delayed");

    destroy_node (node, &rec);
}

static void
test_code_during_entry_delay_disarms (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "entry-delay", 5u, NULL);
    arm_now (node);
    pn_burglar_alarm_trip_zone (node, PN_BURGLAR_ALARM_ZONE_DELAYED);
    recorder_clear (&rec);

    tick_n (node, 2);
    type (node, "1 2 3 4 #");

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_DISARMED);
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_ALARM], ==, 0);
    PN_CHECK_NEAR (pn_test_num (seen (&rec, PN_BURGLAR_ALARM_OUT_ARMED),
                                "value"), 0.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_SPEECH),
                                  "output"), ==, "System disarmed.");

    /* Nothing happened, so nothing was texted. */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SMS], ==, 0);

    /* The count is over: further ticks must not resurrect it. */
    tick_n (node, 10);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_DISARMED);

    destroy_node (node, &rec);
}

static void
test_instant_zone_cuts_the_entry_delay_short (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "entry-delay", 30u, NULL);
    arm_now (node);
    pn_burglar_alarm_trip_zone (node, PN_BURGLAR_ALARM_ZONE_DELAYED);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ENTRY);

    /* Whoever came in walked past the keypad into the lounge. */
    pn_burglar_alarm_trip_zone (node, 2);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ALARM);
    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "zone 2");

    destroy_node (node, &rec);
}

static void
test_zones_are_ignored_during_the_exit_delay (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "exit-delay", 5u, NULL);
    type (node, "1 2 3 4 #");

    /* Walking through the hall on the way out is not a burglary. */
    pn_burglar_alarm_trip_zone (node, 1);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_EXIT);

    /* But a window left open is read again the moment arming bites. */
    pn_burglar_alarm_set_zone (node, 2, TRUE);
    tick_n (node, 5);
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ALARM);
    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "zone 2");

    destroy_node (node, &rec);
}

static void
test_second_zone_during_an_alarm (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    arm_now (node);
    pn_burglar_alarm_trip_zone (node, 1);
    recorder_clear (&rec);

    pn_burglar_alarm_trip_zone (node, 3);

    /* The newest zone is what the panel now names ... */
    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "zone 3");
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_ALARM),
                                  "cause"), ==, "zone 3");

    /* ... but nobody is texted twice for one break-in. */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SMS], ==, 0);

    destroy_node (node, &rec);
}

static void
test_zone_names_follow_the_input_names (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    pn_node_set_input_name (PN_NODE (node),
                            PN_BURGLAR_ALARM_IN_ZONE1, "Kitchen window");
    arm_now (node);
    recorder_clear (&rec);

    pn_burglar_alarm_trip_zone (node, 1);

    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "Kitchen window");
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_SPEECH),
                                  "output"), ==, "Alarm! Kitchen window.");

    destroy_node (node, &rec);
}

/* ------------------------------------------------------------------ */
/*  The siren                                                          */
/* ------------------------------------------------------------------ */

static void
test_siren_cuts_off_but_the_alarm_stays (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "siren-time", 3u, NULL);
    arm_now (node);
    pn_burglar_alarm_trip_zone (node, 1);
    recorder_clear (&rec);

    tick_n (node, 2);
    PN_CHECK (pn_burglar_alarm_get_siren (node));

    tick_n (node, 1);
    PN_CHECK_FALSE (pn_burglar_alarm_get_siren (node));
    PN_CHECK_NEAR (pn_test_num (seen (&rec, PN_BURGLAR_ALARM_OUT_ALARM),
                                "value"), 0.0, 1e-9);

    /* The panel is still in alarm, and still remembers why. */
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ALARM);
    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "zone 1");
    PN_CHECK (pn_node_get_has_error (PN_NODE (node)));

    /* Only the code ends it. */
    type (node, "1 2 3 4 #");
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_DISARMED);
    PN_CHECK_FALSE (pn_node_get_has_error (PN_NODE (node)));
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SMS], ==, 1);
    PN_CHECK (strstr (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_SMS),
                                   "output"), "cleared") != NULL);

    destroy_node (node, &rec);
}

static void
test_zero_siren_time_rings_until_disarmed (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "siren-time", 0u, NULL);
    arm_now (node);
    pn_burglar_alarm_trip_zone (node, 1);

    tick_n (node, 1000);
    PN_CHECK (pn_burglar_alarm_get_siren (node));

    destroy_node (node, &rec);
}

/* ------------------------------------------------------------------ */
/*  The readout                                                        */
/* ------------------------------------------------------------------ */

static void
test_two_line_readout (void)
{
    Recorder        rec;
    PnBurglarAlarm *node  = make_node (&rec);
    gchar          *text;

    text = pn_burglar_alarm_get_display_text (node);
    PN_CHECK_CMPSTR (text, ==, "READY\nAll zones closed");
    g_free (text);

    /* Two lines have no room for a code line, so the masked code takes
     * the detail line while it is being typed. */
    type (node, "1 2 3");
    text = pn_burglar_alarm_get_display_text (node);
    PN_CHECK_CMPSTR (text, ==, "READY\nCode: ***");
    g_free (text);

    destroy_node (node, &rec);
}

static void
test_four_line_readout (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    gchar          *text;

    g_object_set (node, "display-lines", 4, NULL);

    text = pn_burglar_alarm_get_display_text (node);
    PN_CHECK_CMPSTR (text, ==, "READY\nAll zones closed\nZones ....\n");
    g_free (text);

    /* From three lines up the zone map has a line of its own, and the
     * code no longer displaces the detail. */
    pn_burglar_alarm_set_zone (node, PN_BURGLAR_ALARM_ZONE_DELAYED, TRUE);
    pn_burglar_alarm_set_zone (node, 2, TRUE);
    type (node, "1 2");

    text = pn_burglar_alarm_get_display_text (node);
    PN_CHECK_CMPSTR (text, ==,
                     "NOT READY\nOpen: zone 2\nZones D.2.\nCode: **");
    g_free (text);

    destroy_node (node, &rec);
}

static void
test_three_line_readout_drops_the_code_line (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    gchar          *text;

    g_object_set (node, "display-lines", 3, NULL);
    type (node, "1 2");

    text = pn_burglar_alarm_get_display_text (node);
    PN_CHECK_CMPSTR (text, ==, "READY\nAll zones closed\nZones ....");
    g_free (text);

    destroy_node (node, &rec);
}

static void
test_display_carries_the_countdown (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "exit-delay", 10u, NULL);
    type (node, "1 2 3 4 #");

    PN_CHECK_NEAR (pn_test_num (seen (&rec, PN_BURGLAR_ALARM_OUT_DISPLAY),
                                "value"), 10.0, 1e-9);
    PN_CHECK_CMPSTR (pn_test_str (seen (&rec, PN_BURGLAR_ALARM_OUT_DISPLAY),
                                  "state"), ==, "exit");

    tick_n (node, 4);
    PN_CHECK_NEAR (pn_test_num (seen (&rec, PN_BURGLAR_ALARM_OUT_DISPLAY),
                                "value"), 6.0, 1e-9);

    destroy_node (node, &rec);
}

/* ------------------------------------------------------------------ */
/*  Keys and messages                                                  */
/* ------------------------------------------------------------------ */

static void
test_clear_key_rubs_out_the_entry (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    gchar          *text;

    /* "*" on the decimal pad, "C" and "CE" on the calculator one. */
    type (node, "1 2 3 *");
    text = pn_burglar_alarm_get_display_text (node);
    PN_CHECK_CMPSTR (text, ==, "READY\nAll zones closed");
    g_free (text);

    type (node, "9 9 C 1 2 3 4 =");
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_EXIT);

    destroy_node (node, &rec);
}

static void
test_unknown_keys_are_ignored (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    PN_CHECK_FALSE (pn_burglar_alarm_press (node, "+"));
    PN_CHECK_FALSE (pn_burglar_alarm_press (node, "A"));
    PN_CHECK_FALSE (pn_burglar_alarm_press (node, ""));
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_DISPLAY], ==, 0);

    destroy_node (node, &rec);
}

static void
test_keypad_messages_drive_the_panel (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    g_object_set (node, "exit-delay", 0u, NULL);

    send_key (node, "1");
    send_key (node, "2");
    send_key (node, "3");
    send_key (node, "4");
    send_key (node, "#");

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ARMED);

    /* A message with neither a key nor a string is not a keystroke and
     * must not rub out a half-typed code. */
    {
        PnMessage *m = pn_message_new (NULL, NULL);

        send_key (node, "1");
        pn_message_set_double (m, "value", 7.0);
        send_on (node, PN_BURGLAR_ALARM_IN_KEYPAD, m);

        send_key (node, "2");
        send_key (node, "3");
        send_key (node, "4");
        send_key (node, "#");
        PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                         PN_BURGLAR_ALARM_DISARMED);
    }

    destroy_node (node, &rec);
}

static void
test_whole_code_submitted_at_once (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);
    PnMessage      *m;

    g_object_set (node, "exit-delay", 0u, NULL);

    /* What an MQTT feed or a Text node arriving on the keypad input
     * with only a data.output string does. */
    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "output", "1234");
    send_on (node, PN_BURGLAR_ALARM_IN_KEYPAD, m);

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ARMED);

    m = pn_message_new (NULL, NULL);
    pn_message_set_string (m, "output", "0000");
    send_on (node, PN_BURGLAR_ALARM_IN_KEYPAD, m);

    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ARMED);

    destroy_node (node, &rec);
}

static void
test_zone_messages_tell_held_from_momentary (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    /* A numeric value is a contact: 1.0 opens it, 0.0 closes it. */
    send_zone_value (node, PN_BURGLAR_ALARM_IN_ZONE1, 1.0);
    PN_CHECK (pn_burglar_alarm_get_zone_open (node, 1));
    send_zone_value (node, PN_BURGLAR_ALARM_IN_ZONE1, 0.0);
    PN_CHECK_FALSE (pn_burglar_alarm_get_zone_open (node, 1));

    /* No numeric value at all is a PIR's momentary trip. */
    {
        PnMessage *m = pn_message_new (NULL, NULL);

        pn_message_set_string (m, "output", "motion");
        send_on (node, PN_BURGLAR_ALARM_IN_ZONE2, m);
    }
    PN_CHECK_FALSE (pn_burglar_alarm_get_zone_open (node, 2));

    /* Armed, that trip is a break-in all the same. */
    arm_now (node);
    {
        PnMessage *m = pn_message_new (NULL, NULL);

        pn_message_set_string (m, "output", "motion");
        send_on (node, PN_BURGLAR_ALARM_IN_ZONE2, m);
    }
    PN_CHECK_CMPINT (pn_burglar_alarm_get_state (node), ==,
                     PN_BURGLAR_ALARM_ALARM);
    PN_CHECK_CMPSTR (pn_burglar_alarm_get_cause (node), ==, "zone 2");

    destroy_node (node, &rec);
}

static void
test_sms_on_arming_is_opt_in (void)
{
    Recorder        rec;
    PnBurglarAlarm *node = make_node (&rec);

    arm_now (node);
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SMS], ==, 0);

    g_object_set (node, "sms-on-arming", TRUE, NULL);
    type (node, "1 2 3 4 #");                  /* disarm */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SMS], ==, 1);
    type (node, "1 2 3 4 #");                  /* arm again */
    PN_CHECK_CMPINT (rec.count[PN_BURGLAR_ALARM_OUT_SMS], ==, 2);

    destroy_node (node, &rec);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-burglar-alarm");
    pn_test_add ("node_shape",          test_node_shape);
    pn_test_add ("startup_announce",    test_startup_announce);
    pn_test_add ("exit_delay",          test_code_arms_through_the_exit_delay);
    pn_test_add ("zero_exit_delay",     test_zero_exit_delay_arms_at_once);
    pn_test_add ("wrong_code",          test_wrong_code_changes_nothing);
    pn_test_add ("empty_code",          test_empty_code_means_no_code);
    pn_test_add ("not_ready",           test_open_instant_zone_refuses_arming);
    pn_test_add ("delayed_zone_open",   test_open_delayed_zone_still_arms);
    pn_test_add ("momentary_trip",      test_momentary_trip_leaves_the_zone_closed);
    pn_test_add ("instant_zone",        test_instant_zone_alarms_at_once);
    pn_test_add ("entry_delay",         test_delayed_zone_counts_before_the_siren);
    pn_test_add ("disarm_in_entry",     test_code_during_entry_delay_disarms);
    pn_test_add ("entry_cut_short",     test_instant_zone_cuts_the_entry_delay_short);
    pn_test_add ("exit_ignores_zones",  test_zones_are_ignored_during_the_exit_delay);
    pn_test_add ("second_zone",         test_second_zone_during_an_alarm);
    pn_test_add ("zone_names",          test_zone_names_follow_the_input_names);
    pn_test_add ("siren_cut_off",       test_siren_cuts_off_but_the_alarm_stays);
    pn_test_add ("siren_no_cut_off",    test_zero_siren_time_rings_until_disarmed);
    pn_test_add ("two_line_readout",    test_two_line_readout);
    pn_test_add ("four_line_readout",   test_four_line_readout);
    pn_test_add ("three_line_readout",  test_three_line_readout_drops_the_code_line);
    pn_test_add ("display_countdown",   test_display_carries_the_countdown);
    pn_test_add ("clear_key",           test_clear_key_rubs_out_the_entry);
    pn_test_add ("unknown_keys",        test_unknown_keys_are_ignored);
    pn_test_add ("keypad_messages",     test_keypad_messages_drive_the_panel);
    pn_test_add ("whole_code",          test_whole_code_submitted_at_once);
    pn_test_add ("held_vs_momentary",   test_zone_messages_tell_held_from_momentary);
    pn_test_add ("sms_opt_in",          test_sms_on_arming_is_opt_in);
    return pn_test_run ();
}
