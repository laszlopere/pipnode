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

/* Unit tests for the PnMeshtastic *node object* -- its GObject contract,
 * as distinct from the protocol/transport layer pinned by the mesh-*
 * suite (test-pn-mesh-pb / -frame / -node / -discover / -inuse / -qr).
 * Those cover the shared codec, framing, serial open and the node's
 * outbound ToRadio byte builders; none of them instantiate the node, so
 * the node's own GObject behaviour was unpinned.  Here we cover, with no
 * serial/device/GUI/network I/O at all:
 *
 *   - property defaults + the source/sink port shape (input + output,
 *     class name, transient runtime props all start cleared);
 *   - device/channel property round-trips with the worker suppressed
 *     (node disabled), so set_property never spawns a thread or touches
 *     a /dev path;
 *   - the has-error visual gating: unconfigured -> error, a picked
 *     device -> ok, cleared device -> error again;
 *   - the receive() sink guard: when not connected it validates the
 *     `output` member and silently drops, never emitting downstream;
 *   - the FromRadio -> PnMessage bag construction in worker_on_frame:
 *     a TEXT_MESSAGE_APP packet surfaces with the full documented bag
 *     (output / from_node_id / from_long_name / channel_index /
 *     channel_name / packet_id / rx_time / hop_limit / hop_start /
 *     broadcast), sender + channel names resolve from the live session
 *     and fall back to the hex id / absent member when unknown, and a
 *     non-text packet is dropped.
 *
 * worker_on_frame() emits through the default main context (the worker
 * normally runs off-thread), so each receive case drives a bounded
 * g_main_context iteration to deliver the queued emit -- no real radio,
 * no open port, no display.  The static helpers are reached by compiling
 * the node's translation unit straight into the test (the mesh-node and
 * shell-node tests do the same); its few exported symbols win at link
 * time over the libpipnode-core copies. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "pntest.h"

/* The node's translation unit, static helpers and GObject type and all. */
#include "pn-meshtastic.c"

/* ------------------------------------------------------------------ */
/*  Emit capture                                                       */
/* ------------------------------------------------------------------ */

static PnMessage *g_captured;   /* last message seen on "message" (owned) */
static guint      g_emits;      /* count of "message" emissions           */

static void
on_message (PnNode *node, PnMessage *message, gpointer ud)
{
    (void) node;
    (void) ud;
    g_emits++;
    g_clear_object (&g_captured);
    g_captured = g_object_ref (message);
}

static void
reset_capture (void)
{
    g_clear_object (&g_captured);
    g_emits = 0;
}

/* Drain every ready source on the default main context (where
 * worker_on_frame's emit trampoline queues), bounded so a stray source
 * can never hang the test. */
static void
drain_main (void)
{
    for (int i = 0; i < 1000; i++)
        if (!g_main_context_iteration (NULL, FALSE))
            break;
}

/* Build a bare FromRadio payload (the bytes worker_on_frame receives
 * after the frame reader strips the 0x94 0xC3 envelope) carrying one
 * MeshPacket whose decoded Data is portnum @portnum with @text.  Reuses
 * the node's own protobuf encoders so the decoder under test is fed
 * exactly what the encoder produces. */
static GBytes *
make_fromradio_text (guint32      from,
                     guint32      to,
                     guint        channel,
                     guint        portnum,
                     const gchar *text,
                     guint32      packet_id,
                     guint32      rx_time,
                     guint        hop_limit,
                     guint        hop_start)
{
    GByteArray *data = g_byte_array_new ();
    pb_encode_varint_field (data, 1, portnum);
    pb_encode_len_field    (data, 2, (const guint8 *) text, strlen (text));

    GByteArray *pkt = g_byte_array_new ();
    pb_encode_fixed32_field (pkt, 1, from);
    pb_encode_fixed32_field (pkt, 2, to);
    pb_encode_varint_field  (pkt, 3, channel);
    pb_encode_len_field     (pkt, 4, data->data, data->len);
    pb_encode_fixed32_field (pkt, 6, packet_id);
    pb_encode_fixed32_field (pkt, 7, rx_time);
    pb_encode_varint_field  (pkt, 10, hop_limit);
    pb_encode_varint_field  (pkt, 13, hop_start);

    GByteArray *fr = g_byte_array_new ();
    pb_encode_len_field (fr, 2, pkt->data, pkt->len);

    g_byte_array_free (data, TRUE);
    g_byte_array_free (pkt, TRUE);
    return g_byte_array_free_to_bytes (fr);
}

/* Feed @fr to worker_on_frame against @session and pump the main loop so
 * any queued emit is delivered to on_message. */
static void
deliver_frame (PnMeshtastic *self, PnMeshSession *session, GBytes *fr)
{
    PnReadCtx     ctx  = { self, session };
    gsize         len;
    const guint8 *data = g_bytes_get_data (fr, &len);
    worker_on_frame (data, len, &ctx);
    drain_main ();
}

/* ------------------------------------------------------------------ */
/*  Property contract                                                  */
/* ------------------------------------------------------------------ */

/* A fresh node is an input+output "Meshtastic" node with empty,
 * cleared-runtime properties. */
static void
test_property_defaults (void)
{
    PnMeshtastic *self = pn_meshtastic_new ();
    PnNode       *node = PN_NODE (self);

    gchar    *device = NULL, *channel = NULL, *busy_path = NULL, *err = NULL;
    gboolean  busy   = TRUE;
    guint     hw     = 99;

    g_object_get (self,
                  "device",         &device,
                  "channel",        &channel,
                  "busy",           &busy,
                  "hw-model",       &hw,
                  "last-busy-path", &busy_path,
                  "last-error",     &err,
                  NULL);

    PN_CHECK_CMPSTR (device,  ==, "");
    PN_CHECK_CMPSTR (channel, ==, "");
    PN_CHECK_FALSE  (busy);
    PN_CHECK_CMPINT (hw, ==, 0u);
    PN_CHECK (busy_path == NULL);     /* default NULL, runtime-only */
    PN_CHECK (err == NULL);

    /* Bidirectional bridge: both ports present. */
    PN_CHECK (pn_node_get_has_input  (node));
    PN_CHECK (pn_node_get_has_output (node));
    PN_CHECK_CMPSTR (pn_node_get_class_name (node), ==, "Meshtastic");

    g_free (device);
    g_free (channel);
    g_free (busy_path);
    g_free (err);
    g_object_unref (self);
}

/* device + channel round-trip through their properties.  The node is
 * disabled first so set_property's worker_restart never spawns a thread
 * or opens the path. */
static void
test_property_round_trip (void)
{
    PnMeshtastic *self = pn_meshtastic_new ();
    gchar        *out  = NULL;

    pn_node_set_disabled (PN_NODE (self), TRUE);

    g_object_set (self, "device", "/dev/pn-mesh-test-xyz", NULL);
    g_object_get (self, "device", &out, NULL);
    PN_CHECK_CMPSTR (out, ==, "/dev/pn-mesh-test-xyz");
    g_clear_pointer (&out, g_free);

    g_object_set (self, "channel", "LongFast", NULL);
    g_object_get (self, "channel", &out, NULL);
    PN_CHECK_CMPSTR (out, ==, "LongFast");
    g_clear_pointer (&out, g_free);

    drain_main ();   /* flush queued clear-banner closures before unref */
    g_object_unref (self);
}

/* has-error tracks "configured": unconfigured (no device) is an error
 * state, a picked device clears it, clearing the device re-arms it.
 * Disabled throughout so no worker runs. */
static void
test_has_error_gating (void)
{
    PnMeshtastic *self = pn_meshtastic_new ();
    PnNode       *node = PN_NODE (self);

    /* Fresh node has no device -> warning state. */
    PN_CHECK (pn_node_get_has_error (node));

    pn_node_set_disabled (node, TRUE);

    g_object_set (self, "device", "/dev/pn-mesh-test-xyz", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (node));   /* configured -> ok */

    g_object_set (self, "device", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));          /* cleared -> warning */

    drain_main ();
    g_object_unref (self);
}

/* ------------------------------------------------------------------ */
/*  receive() sink guard                                               */
/* ------------------------------------------------------------------ */

/* With no worker (never connected) receive() is a safe sink: it validates
 * the `output` member and drops everything, never emitting downstream. */
static void
test_receive_sink_not_connected (void)
{
    PnMeshtastic *self = pn_meshtastic_new ();
    PnNode       *node = PN_NODE (self);
    PnMessage    *msg  = pn_message_new (NULL, NULL);

    reset_capture ();
    g_signal_connect (self, "message", G_CALLBACK (on_message), NULL);

    /* A well-formed outbound text: dropped because not connected. */
    pn_message_set_string (msg, "output", "hi");
    pn_node_receive_message (node, msg);

    /* No `output` member at all: ignored. */
    g_object_unref (msg);
    msg = pn_message_new (NULL, NULL);
    pn_message_set_boolean (msg, "success", TRUE);
    pn_node_receive_message (node, msg);

    /* Empty `output`: ignored. */
    pn_message_set_string (msg, "output", "");
    pn_node_receive_message (node, msg);

    drain_main ();
    PN_CHECK_CMPINT (g_emits, ==, 0);
    PN_CHECK (g_captured == NULL);

    reset_capture ();
    g_object_unref (msg);
    g_object_unref (self);
}

/* ------------------------------------------------------------------ */
/*  FromRadio -> PnMessage bag (worker_on_frame)                       */
/* ------------------------------------------------------------------ */

/* A TEXT_MESSAGE_APP packet surfaces as one message carrying the full
 * documented bag, with sender + channel names resolved from the live
 * session. */
static void
test_rx_text_message_bag (void)
{
    PnMeshtastic *self = pn_meshtastic_new ();
    PnMeshSession session;

    mesh_session_init (&session);
    /* Channel index 1 is named, and node 0x11223344 has a long name. */
    PnMeshChannel ch = { 1, g_strdup ("LongFast"), 1 };
    g_array_append_val (session.channels, ch);
    g_hash_table_insert (session.node_names,
                         GUINT_TO_POINTER (0x11223344u), g_strdup ("Alice"));

    reset_capture ();
    g_signal_connect (self, "message", G_CALLBACK (on_message), NULL);

    GBytes *fr = make_fromradio_text (0x11223344u, PN_MESH_BROADCAST, 1,
                                      PN_MESH_PORT_TEXT, "hello",
                                      0xBEEFu, 0x1E240u, 3, 5);
    deliver_frame (self, &session, fr);
    g_bytes_unref (fr);

    PN_CHECK_CMPINT (g_emits, ==, 1);
    PN_CHECK (g_captured != NULL);
    if (g_captured != NULL)
    {
        PN_CHECK (pn_test_bool (g_captured, "success"));
        PN_CHECK_CMPSTR (pn_test_str (g_captured, "output"), ==, "hello");
        PN_CHECK_CMPSTR (pn_test_str (g_captured, "from_node_id"),
                         ==, "!11223344");
        PN_CHECK_CMPSTR (pn_test_str (g_captured, "from_long_name"),
                         ==, "Alice");
        PN_CHECK_CMPINT ((gint) pn_test_num (g_captured, "channel_index"),
                         ==, 1);
        PN_CHECK_CMPSTR (pn_test_str (g_captured, "channel_name"),
                         ==, "LongFast");
        PN_CHECK_CMPINT ((gint) pn_test_num (g_captured, "packet_id"),
                         ==, 0xBEEF);
        PN_CHECK_CMPINT ((gint) pn_test_num (g_captured, "rx_time"),
                         ==, 0x1E240);
        PN_CHECK_CMPINT ((gint) pn_test_num (g_captured, "hop_limit"),
                         ==, 3);
        PN_CHECK_CMPINT ((gint) pn_test_num (g_captured, "hop_start"),
                         ==, 5);
        PN_CHECK (pn_test_bool (g_captured, "broadcast"));   /* to == BCAST */
    }

    reset_capture ();
    mesh_session_clear (&session);
    g_object_unref (self);
}

/* When the session knows neither the sender nor the channel, the bag
 * still emits: from_long_name falls back to the hex id and channel_name
 * is simply absent.  A non-broadcast `to` reports broadcast=FALSE. */
static void
test_rx_fallback_no_session (void)
{
    PnMeshtastic *self = pn_meshtastic_new ();
    PnMeshSession session;

    mesh_session_init (&session);   /* empty: no channels, no node names */

    reset_capture ();
    g_signal_connect (self, "message", G_CALLBACK (on_message), NULL);

    GBytes *fr = make_fromradio_text (0xABCDEF01u, 0x00000007u, 0,
                                      PN_MESH_PORT_TEXT, "yo",
                                      1, 2, 0, 0);
    deliver_frame (self, &session, fr);
    g_bytes_unref (fr);

    PN_CHECK_CMPINT (g_emits, ==, 1);
    if (g_captured != NULL)
    {
        PN_CHECK_CMPSTR (pn_test_str (g_captured, "from_node_id"),
                         ==, "!abcdef01");
        /* Unknown sender -> long name mirrors the hex id. */
        PN_CHECK_CMPSTR (pn_test_str (g_captured, "from_long_name"),
                         ==, "!abcdef01");
        /* Unknown channel -> no channel_name member at all. */
        PN_CHECK_FALSE (pn_test_has (g_captured, "channel_name"));
        PN_CHECK_FALSE (pn_test_bool (g_captured, "broadcast"));
    }

    reset_capture ();
    mesh_session_clear (&session);
    g_object_unref (self);
}

/* A non-text packet (portnum != TEXT_MESSAGE_APP) is decoded but never
 * surfaced -- the node only forwards text. */
static void
test_rx_non_text_dropped (void)
{
    PnMeshtastic *self = pn_meshtastic_new ();
    PnMeshSession session;

    mesh_session_init (&session);

    reset_capture ();
    g_signal_connect (self, "message", G_CALLBACK (on_message), NULL);

    /* portnum 3 (POSITION_APP) instead of TEXT. */
    GBytes *fr = make_fromradio_text (0x11223344u, PN_MESH_BROADCAST, 0,
                                      3, "ignored", 1, 1, 1, 1);
    deliver_frame (self, &session, fr);
    g_bytes_unref (fr);

    PN_CHECK_CMPINT (g_emits, ==, 0);
    PN_CHECK (g_captured == NULL);

    reset_capture ();
    mesh_session_clear (&session);
    g_object_unref (self);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-meshtastic");

    /* Property contract */
    pn_test_add ("property_defaults",      test_property_defaults);
    pn_test_add ("property_round_trip",    test_property_round_trip);
    pn_test_add ("has_error_gating",       test_has_error_gating);

    /* receive() sink guard */
    pn_test_add ("receive_sink_not_conn",  test_receive_sink_not_connected);

    /* FromRadio -> PnMessage bag */
    pn_test_add ("rx_text_message_bag",    test_rx_text_message_bag);
    pn_test_add ("rx_fallback_no_session", test_rx_fallback_no_session);
    pn_test_add ("rx_non_text_dropped",    test_rx_non_text_dropped);

    return pn_test_run ();
}
