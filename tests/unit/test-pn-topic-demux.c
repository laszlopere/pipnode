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

/* Unit tests for PnTopicDemux — one input, N outputs, each message
 * forwarded by the first output whose topic pattern matches.  Emission
 * is synchronous in receive(), so no main loop is needed.  Headless: no
 * IO, no GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-topic-demux.h"
#include "pn-wire.h"

#include <string.h>

typedef struct
{
    guint     count;
    gint      output[16];
    PnMessage *last;
} Recorder;

static void
recorder_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *r = user_data;
    (void) node;
    if (r->count < G_N_ELEMENTS (r->output))
        r->output[r->count] = pn_node_current_output ();
    g_clear_object (&r->last);
    r->last = g_object_ref (message);
    r->count++;
}

static PnNode *
make_node (gint outputs, const gchar *topics, Recorder *rec)
{
    PnNode *node = PN_NODE (pn_topic_demux_new ());

    g_object_set (node, "outputs", outputs, "topics", topics, NULL);
    if (rec != NULL)
    {
        memset (rec, 0, sizeof *rec);
        g_signal_connect (node, "message", G_CALLBACK (recorder_cb), rec);
    }
    return node;
}

static void
send (PnNode *node, const gchar *topic)
{
    PnMessage *m = pn_message_new (NULL, topic);
    pn_message_set_double (m, "value", 1.0);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

#define MATCH(p, t) pn_topic_demux_topic_matches ((p), (t))

/* ------------------------------------------------------------------ */
/*  Matching                                                           */
/* ------------------------------------------------------------------ */

static void
test_match_exact (void)
{
    PN_CHECK ( MATCH ("a/b", "a/b"));
    PN_CHECK (!MATCH ("a/b", "a/bc"));
    PN_CHECK (!MATCH ("a/bc", "a/b"));
    PN_CHECK (!MATCH ("a/b", "a/b/c"));
    PN_CHECK (!MATCH ("a", "A"));
    PN_CHECK ( MATCH ("/lead", "/lead"));
    PN_CHECK (!MATCH ("", ""));
    PN_CHECK (!MATCH (NULL, "a"));
    PN_CHECK (!MATCH ("a", NULL));
}

static void
test_match_mqtt_plus (void)
{
    PN_CHECK ( MATCH ("tele/+/SENSOR", "tele/plug1/SENSOR"));
    PN_CHECK (!MATCH ("tele/+/SENSOR", "tele/a/b/SENSOR"));
    PN_CHECK (!MATCH ("tele/+/SENSOR", "tele/plug1/STATE"));
    PN_CHECK ( MATCH ("+/+", "a/b"));
    PN_CHECK (!MATCH ("a/+", "a"));
    PN_CHECK ( MATCH ("a/+", "a/"));
    /* '+' only counts as a wildcard when it is a whole level. */
    PN_CHECK (!MATCH ("a+", "ab"));
    PN_CHECK ( MATCH ("a+", "a+"));
}

static void
test_match_mqtt_hash (void)
{
    PN_CHECK ( MATCH ("#", "anything/at/all"));
    PN_CHECK ( MATCH ("#", ""));
    PN_CHECK ( MATCH ("#", NULL));
    PN_CHECK ( MATCH ("stat/#", "stat/plug/POWER"));
    PN_CHECK ( MATCH ("stat/#", "stat"));
    PN_CHECK (!MATCH ("stat/#", "status"));
    PN_CHECK ( MATCH ("+/plug/#", "stat/plug/POWER"));
}

static void
test_match_glob (void)
{
    PN_CHECK ( MATCH ("*SENSOR", "tele/a/b/SENSOR"));
    PN_CHECK ( MATCH ("tele/*", "tele/x/y"));
    PN_CHECK ( MATCH ("t?le", "tele"));
    PN_CHECK (!MATCH ("t?le", "tle"));
    PN_CHECK ( MATCH ("*", ""));
}

/* ------------------------------------------------------------------ */
/*  Routing                                                            */
/* ------------------------------------------------------------------ */

static void
test_default_ports (void)
{
    PnNode *node    = PN_NODE (pn_topic_demux_new ());
    gint    outputs = 0;
    gchar  *topics  = NULL;

    g_object_get (node, "outputs", &outputs, "topics", &topics, NULL);
    PN_CHECK_CMPINT (outputs, ==, PN_TOPIC_DEMUX_DEF_OUTPUTS);
    PN_CHECK_CMPSTR (topics, ==, "[]");
    PN_CHECK_CMPINT (pn_node_get_n_inputs  (node), ==, 1);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 4);
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "out1");

    g_free (topics);
    g_object_unref (node);
}

static void
test_routes_to_matching_output (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, "[\"a\",\"b\",\"c\"]", &rec);

    send (node, "b");
    PN_CHECK_CMPINT (rec.count, ==, 1u);
    PN_CHECK_CMPINT (rec.output[0], ==, 1);

    send (node, "c");
    send (node, "a");
    PN_CHECK_CMPINT (rec.count, ==, 3u);
    PN_CHECK_CMPINT (rec.output[1], ==, 2);
    PN_CHECK_CMPINT (rec.output[2], ==, 0);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

static void
test_unmatched_dropped (void)
{
    Recorder rec;
    PnNode  *node = make_node (2, "[\"a\",\"b\"]", &rec);

    send (node, "z");
    send (node, NULL);
    PN_CHECK_CMPINT (rec.count, ==, 0u);

    g_object_unref (node);
}

/* Overlapping patterns: the first output wins, so a catch-all on the
 * last output only sees what nothing earlier claimed. */
static void
test_first_match_wins (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, "[\"tele/+/SENSOR\",\"tele/#\",\"#\"]", &rec);

    send (node, "tele/p1/SENSOR");
    send (node, "tele/p1/STATE");
    send (node, "stat/p1/POWER");
    PN_CHECK_CMPINT (rec.count, ==, 3u);
    PN_CHECK_CMPINT (rec.output[0], ==, 0);
    PN_CHECK_CMPINT (rec.output[1], ==, 1);
    PN_CHECK_CMPINT (rec.output[2], ==, 2);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

/* An empty slot is unused: it matches nothing, not even an empty topic. */
static void
test_empty_slot_matches_nothing (void)
{
    Recorder rec;
    PnNode  *node = make_node (3, "[\"\",\"x\"]", &rec);

    send (node, "");
    send (node, NULL);
    PN_CHECK_CMPINT (rec.count, ==, 0u);
    send (node, "x");
    PN_CHECK_CMPINT (rec.count, ==, 1u);
    PN_CHECK_CMPINT (rec.output[0], ==, 1);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

/* The message goes out unchanged — topic and data. */
static void
test_message_forwarded_unchanged (void)
{
    Recorder   rec;
    PnNode    *node = make_node (2, "[\"x\",\"y\"]", &rec);
    PnMessage *m    = pn_message_new (NULL, "y");

    pn_message_set_string (m, "note", "hello");
    pn_node_receive_message (node, m);

    PN_CHECK_CMPINT (rec.count, ==, 1u);
    PN_CHECK (rec.last != NULL);
    if (rec.last != NULL)
    {
        PN_CHECK_CMPSTR (pn_message_get_topic (rec.last), ==, "y");
        PN_CHECK_CMPSTR (pn_test_str (rec.last, "note"), ==, "hello");
    }

    g_object_unref (m);
    g_clear_object (&rec.last);
    g_object_unref (node);
}

/* Patterns beyond the output count are kept but do not route; raising
 * the count brings them back. */
static void
test_shrink_keeps_patterns (void)
{
    Recorder rec;
    PnNode  *node = make_node (4, "[\"a\",\"b\",\"c\",\"d\"]", &rec);
    gchar   *topics = NULL;

    g_object_set (node, "outputs", 2, NULL);
    PN_CHECK_CMPINT (pn_node_get_n_outputs (node), ==, 2);
    send (node, "d");
    PN_CHECK_CMPINT (rec.count, ==, 0u);

    g_object_get (node, "topics", &topics, NULL);
    PN_CHECK_CMPSTR (topics, ==, "[\"a\",\"b\",\"c\",\"d\"]");
    g_free (topics);

    g_object_set (node, "outputs", 4, NULL);
    send (node, "d");
    PN_CHECK_CMPINT (rec.count, ==, 1u);
    PN_CHECK_CMPINT (rec.output[0], ==, 3);

    g_clear_object (&rec.last);
    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Topics property                                                    */
/* ------------------------------------------------------------------ */

static void
count_notify (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    (void) obj; (void) pspec;
    (*(guint *) user_data)++;
}

static void
test_set_topic_api (void)
{
    PnTopicDemux *td       = pn_topic_demux_new ();
    guint         notified = 0;
    gchar        *topics   = NULL;

    g_signal_connect (td, "notify::topics", G_CALLBACK (count_notify), &notified);

    pn_topic_demux_set_topic (td, 2, "  x/y  ");
    PN_CHECK_CMPSTR (pn_topic_demux_get_topic (td, 2), ==, "x/y");
    PN_CHECK_CMPSTR (pn_topic_demux_get_topic (td, 0), ==, "");
    PN_CHECK_CMPINT (notified, ==, 1u);

    /* Same value again: no change, no notify. */
    pn_topic_demux_set_topic (td, 2, "x/y");
    PN_CHECK_CMPINT (notified, ==, 1u);

    /* Trailing empty slots are trimmed; leading ones are kept. */
    g_object_get (td, "topics", &topics, NULL);
    PN_CHECK_CMPSTR (topics, ==, "[\"\",\"\",\"x/y\"]");
    g_free (topics);

    pn_topic_demux_set_topic (td, 2, NULL);
    g_object_get (td, "topics", &topics, NULL);
    PN_CHECK_CMPSTR (topics, ==, "[]");
    g_free (topics);

    PN_CHECK_CMPINT (pn_topic_demux_route (td, "x/y"), ==, -1);

    g_object_unref (td);
}

/* Garbage clears every slot; non-string elements clear just theirs. */
static void
test_topics_bad_json (void)
{
    PnTopicDemux *td = PN_TOPIC_DEMUX (make_node (3, "[\"a\",5,\"c\"]", NULL));

    PN_CHECK_CMPSTR (pn_topic_demux_get_topic (td, 0), ==, "a");
    PN_CHECK_CMPSTR (pn_topic_demux_get_topic (td, 1), ==, "");
    PN_CHECK_CMPSTR (pn_topic_demux_get_topic (td, 2), ==, "c");

    g_object_set (td, "topics", "not json", NULL);
    PN_CHECK_CMPSTR (pn_topic_demux_get_topic (td, 0), ==, "");
    PN_CHECK_CMPSTR (pn_topic_demux_get_topic (td, 2), ==, "");

    g_object_unref (td);
}

/* Each output is labelled with its pattern; a long one shows its tail,
 * an empty one the "outN" default. */
static void
test_output_names_follow_topics (void)
{
    PnNode *node = make_node (3,
            "[\"short\",\"tele/tasmota_ABCDEF/SENSOR\"]", NULL);

    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "short");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 1), ==,
                     "\xe2\x80\xa6" "a_ABCDEF/SENSOR");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 2), ==, "out3");

    pn_topic_demux_set_topic (PN_TOPIC_DEMUX (node), 0, "");
    PN_CHECK_CMPSTR (pn_node_get_output_name (node, 0), ==, "out1");

    g_object_unref (node);
}

/* ------------------------------------------------------------------ */
/*  Wires                                                              */
/* ------------------------------------------------------------------ */

static void
count_passed (PnWire *wire, PnMessage *message, gpointer user_data)
{
    (void) wire; (void) message;
    (*(guint *) user_data)++;
}

static void
test_wires_carry_their_output (void)
{
    PnNode *td   = make_node (2, "[\"a\",\"b\"]", NULL);
    PnNode *sink = PN_NODE (pn_topic_demux_new ());
    PnWire *w0   = pn_wire_new_ports (td, 0, sink, 0);
    PnWire *w1   = pn_wire_new_ports (td, 1, sink, 0);
    guint   n0 = 0, n1 = 0;

    g_signal_connect (w0, "message-passed", G_CALLBACK (count_passed), &n0);
    g_signal_connect (w1, "message-passed", G_CALLBACK (count_passed), &n1);

    send (td, "a");
    send (td, "b");
    send (td, "b");
    send (td, "q");
    PN_CHECK_CMPINT (n0, ==, 1u);
    PN_CHECK_CMPINT (n1, ==, 2u);

    g_object_unref (w0);
    g_object_unref (w1);
    g_object_unref (sink);
    g_object_unref (td);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-topic-demux");
    pn_test_add ("match_exact",            test_match_exact);
    pn_test_add ("match_mqtt_plus",        test_match_mqtt_plus);
    pn_test_add ("match_mqtt_hash",        test_match_mqtt_hash);
    pn_test_add ("match_glob",             test_match_glob);
    pn_test_add ("default_ports",          test_default_ports);
    pn_test_add ("routes_to_match",        test_routes_to_matching_output);
    pn_test_add ("unmatched_dropped",      test_unmatched_dropped);
    pn_test_add ("first_match_wins",       test_first_match_wins);
    pn_test_add ("empty_slot_nothing",     test_empty_slot_matches_nothing);
    pn_test_add ("forwarded_unchanged",    test_message_forwarded_unchanged);
    pn_test_add ("shrink_keeps_patterns",  test_shrink_keeps_patterns);
    pn_test_add ("set_topic_api",          test_set_topic_api);
    pn_test_add ("topics_bad_json",        test_topics_bad_json);
    pn_test_add ("output_names",           test_output_names_follow_topics);
    pn_test_add ("wires_carry_output",     test_wires_carry_their_output);
    return pn_test_run ();
}
