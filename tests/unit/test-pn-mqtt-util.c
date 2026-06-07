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

/* Unit tests for the broker-free MQTT helpers shared by the Source and
 * Sink (pn-mqtt-util.c).  These are pure functions of their arguments, so
 * they run in process with no broker, no socket, and no GObject lifecycle.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-mqtt-util.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  pn_mqtt_parse_url                                                   */
/* ------------------------------------------------------------------ */

/* One row of the URL table: @url should parse to @host/@port/@tls. */
static void
check_url_ok (const gchar *url, const gchar *host, int port, gboolean tls)
{
    gchar    *got_host = NULL;
    int       got_port = -1;
    gboolean  got_tls  = !tls;     /* seed with the wrong value */

    PN_CHECK (pn_mqtt_parse_url (url, &got_host, &got_port, &got_tls));
    PN_CHECK_CMPSTR (got_host, ==, host);
    PN_CHECK_CMPINT (got_port, ==, port);
    PN_CHECK (got_tls == tls);

    g_free (got_host);
}

/* @url should be rejected, and the out-params left untouched. */
static void
check_url_bad (const gchar *url)
{
    gchar    *host = NULL;
    int       port = -12345;
    gboolean  tls  = FALSE;

    PN_CHECK_FALSE (pn_mqtt_parse_url (url, &host, &port, &tls));
    /* On failure the parser must not allocate or touch the outputs. */
    PN_CHECK (host == NULL);
    PN_CHECK_CMPINT (port, ==, -12345);
}

static void
test_url_schemes (void)
{
    /* Plain MQTT -> 1883, no TLS. */
    check_url_ok ("tcp://broker.example",        "broker.example", 1883, FALSE);
    check_url_ok ("mqtt://broker.example",       "broker.example", 1883, FALSE);
    /* TLS schemes -> 8883, TLS. */
    check_url_ok ("ssl://broker.example",        "broker.example", 8883, TRUE);
    check_url_ok ("mqtts://broker.example",      "broker.example", 8883, TRUE);
    /* Bare host with no scheme is treated as plain TCP. */
    check_url_ok ("broker.example",              "broker.example", 1883, FALSE);
}

static void
test_url_explicit_port (void)
{
    /* An explicit port overrides the scheme default, on both sides. */
    check_url_ok ("tcp://broker.example:1884",   "broker.example", 1884, FALSE);
    check_url_ok ("ssl://broker.example:9001",   "broker.example", 9001, TRUE);
    check_url_ok ("mqtt://10.0.0.5:1883",        "10.0.0.5",       1883, FALSE);
    check_url_ok ("host.local:8883",             "host.local",     8883, FALSE);
    /* Boundary ports. */
    check_url_ok ("tcp://h:1",                    "h",             1,    FALSE);
    check_url_ok ("tcp://h:65535",                "h",             65535, FALSE);
}

/* Bracketed IPv6 literals: the host is everything between the brackets, so
 * the ':' separators inside the address are not mistaken for the port
 * delimiter (review finding N4).  An optional ":port" may follow the ']'. */
static void
test_url_ipv6 (void)
{
    /* No port -> scheme default; host has the brackets stripped. */
    check_url_ok ("tcp://[::1]",                   "::1",            1883, FALSE);
    check_url_ok ("[::1]",                          "::1",           1883, FALSE);
    check_url_ok ("mqtts://[2001:db8::1]",         "2001:db8::1",    8883, TRUE);
    /* Explicit port after the closing bracket. */
    check_url_ok ("tcp://[::1]:1884",              "::1",            1884, FALSE);
    check_url_ok ("ssl://[2001:db8::1]:9001",      "2001:db8::1",    9001, TRUE);

    /* Malformed bracket forms are rejected. */
    check_url_bad ("tcp://[::1");      /* unterminated */
    check_url_bad ("tcp://[]");        /* empty host    */
    check_url_bad ("tcp://[]:1883");   /* empty host    */
    check_url_bad ("tcp://[::1]x");    /* junk after ]  */
    check_url_bad ("tcp://[::1]:");    /* empty port    */
    check_url_bad ("tcp://[::1]:abc"); /* bad port      */
    check_url_bad ("tcp://[::1]:0");   /* port range    */
}

static void
test_url_rejected (void)
{
    check_url_bad (NULL);
    check_url_bad ("");
    /* Empty host after a scheme. */
    check_url_bad ("tcp://");
    check_url_bad ("ssl://");
    /* Leading ':' or '/' is not a host. */
    check_url_bad ("tcp://:1883");
    check_url_bad ("tcp:///path");
    /* Non-numeric and out-of-range ports. */
    check_url_bad ("tcp://h:");
    check_url_bad ("tcp://h:abc");
    check_url_bad ("tcp://h:0");
    check_url_bad ("tcp://h:65536");
    check_url_bad ("tcp://h:-5");
}

/* ------------------------------------------------------------------ */
/*  pn_mqtt_build_message                                               */
/* ------------------------------------------------------------------ */

/* Build from a NUL-terminated text payload (source node may be NULL). */
static PnMessage *
build_text (const gchar *topic, const gchar *payload, int qos, gboolean ret)
{
    return pn_mqtt_build_message (NULL, topic,
                                  (const guchar *) payload,
                                  payload != NULL ? strlen (payload) : 0,
                                  qos, ret);
}

/* A JSON object payload lands on data.payload as a nested object (not a
 * string-escaped blob), and does NOT set data.value (it is not a scalar). */
static void
test_message_json_object (void)
{
    PnMessage *m = build_text ("tele/dev/SENSOR",
                               "{\"Time\":\"x\",\"AM2301\":{\"Temperature\":23.5}}",
                               0, FALSE);
    JsonNode  *payload = pn_message_get_member (m, "payload");

    PN_CHECK (payload != NULL);
    PN_CHECK (payload != NULL && JSON_NODE_HOLDS_OBJECT (payload));
    PN_CHECK_FALSE (pn_test_has (m, "value"));

    g_object_unref (m);
}

/* A bare JSON number is mirrored onto data.value so a Graph just works. */
static void
test_message_json_number (void)
{
    PnMessage *m = build_text ("t", "23.5", 0, FALSE);
    JsonNode  *payload = pn_message_get_member (m, "payload");

    PN_CHECK (payload != NULL);
    PN_CHECK (payload != NULL && JSON_NODE_HOLDS_VALUE (payload));
    PN_CHECK (pn_test_has (m, "value"));
    PN_CHECK_NEAR (pn_test_num (m, "value"), 23.5, 1e-9);

    g_object_unref (m);
}

/* A JSON boolean lands on data.payload as a boolean and — per the M1 fix —
 * is mirrored onto data.value following the 0.0/1.0 boolean convention, so a
 * `true`/`false` payload drives a downstream gate like a numeric 1/0 would. */
static void
test_message_json_bool_value (void)
{
    PnMessage *m = build_text ("t", "true", 0, FALSE);
    JsonNode  *payload = pn_message_get_member (m, "payload");

    PN_CHECK (payload != NULL);
    PN_CHECK (payload != NULL && JSON_NODE_HOLDS_VALUE (payload));
    PN_CHECK (payload != NULL &&
              json_node_get_value_type (payload) == G_TYPE_BOOLEAN);
    PN_CHECK (pn_test_has (m, "value"));
    PN_CHECK_NEAR (pn_test_num (m, "value"), 1.0, 1e-9);

    g_object_unref (m);

    /* false → 0.0 */
    m = build_text ("t", "false", 0, FALSE);
    PN_CHECK (pn_test_has (m, "value"));
    PN_CHECK_NEAR (pn_test_num (m, "value"), 0.0, 1e-9);

    g_object_unref (m);
}

/* Non-JSON but numeric text is best-effort coerced onto data.value, while
 * data.payload stays the raw string.  "+23.5" is not valid JSON (leading
 * '+'), so it exercises the text branch, yet g_ascii_strtod consumes it. */
static void
test_message_text_numeric (void)
{
    PnMessage *m = build_text ("t", "+23.5", 0, FALSE);

    /* payload kept as the raw string -> confirms the non-JSON branch. */
    PN_CHECK_CMPSTR (pn_test_str (m, "payload"), ==, "+23.5");
    PN_CHECK (pn_test_has (m, "value"));
    PN_CHECK_NEAR (pn_test_num (m, "value"), 23.5, 1e-9);

    g_object_unref (m);
}

/* Plain non-numeric text: payload is the string, no value mirror. */
static void
test_message_text_plain (void)
{
    PnMessage *m = build_text ("t", "hello world", 0, FALSE);

    PN_CHECK_CMPSTR (pn_test_str (m, "payload"), ==, "hello world");
    PN_CHECK_FALSE (pn_test_has (m, "value"));

    g_object_unref (m);
}

/* Binary (non-UTF-8) payload: data.payload is "" and no value mirror. */
static void
test_message_binary (void)
{
    const guchar bytes[] = { 0xff, 0xfe, 0xff };
    PnMessage *m = pn_mqtt_build_message (NULL, "t", bytes, sizeof bytes,
                                          0, FALSE);

    PN_CHECK_CMPSTR (pn_test_str (m, "payload"), ==, "");
    PN_CHECK_FALSE (pn_test_has (m, "value"));

    g_object_unref (m);
}

/* Empty payload: "" string, no value, success still TRUE. */
static void
test_message_empty (void)
{
    PnMessage *m = pn_mqtt_build_message (NULL, "t", NULL, 0, 0, FALSE);

    PN_CHECK_CMPSTR (pn_test_str (m, "payload"), ==, "");
    PN_CHECK_FALSE (pn_test_has (m, "value"));
    PN_CHECK (pn_test_bool (m, "success"));

    g_object_unref (m);
}

/* Envelope: topic verbatim, qos/retained mirrored, output one-liner names
 * the topic, success TRUE. */
static void
test_message_envelope (void)
{
    PnMessage *m = build_text ("tele/dev/STATE", "x", 2, TRUE);
    JsonNode  *qos = pn_message_get_member (m, "qos");

    PN_CHECK_CMPSTR (pn_message_get_topic (m), ==, "tele/dev/STATE");
    PN_CHECK (qos != NULL && json_node_get_int (qos) == 2);
    PN_CHECK (pn_test_bool (m, "retained"));
    PN_CHECK (pn_test_bool (m, "success"));
    PN_CHECK_CMPSTR (pn_test_str (m, "output"), ==,
                     "MQTT message on tele/dev/STATE");

    g_object_unref (m);
}

/* ------------------------------------------------------------------ */
/*  pn_mqtt_build_payload                                               */
/* ------------------------------------------------------------------ */

/* A non-empty template is expanded against the message's data bag. */
static void
test_payload_template (void)
{
    PnMessage *m = pn_message_new (NULL, "t");
    gchar     *out = NULL;
    gsize      len = 0;

    pn_message_set_string (m, "device", "lamp");

    PN_CHECK (pn_mqtt_build_payload ("cmnd/${data/device}/POWER",
                                     m, NULL, &out, &len));
    PN_CHECK_CMPSTR (out, ==, "cmnd/lamp/POWER");
    PN_CHECK_CMPINT ((int) len, ==, (int) strlen ("cmnd/lamp/POWER"));

    g_free (out);
    g_object_unref (m);
}

/* Empty template -> a string data.payload goes out as raw bytes. */
static void
test_payload_fallback_string (void)
{
    PnMessage *m = pn_message_new (NULL, "t");
    gchar     *out = NULL;
    gsize      len = 0;

    pn_message_set_string (m, "payload", "ON");

    PN_CHECK (pn_mqtt_build_payload ("", m, NULL, &out, &len));
    PN_CHECK_CMPSTR (out, ==, "ON");
    PN_CHECK_CMPINT ((int) len, ==, 2);

    g_free (out);
    g_object_unref (m);
}

/* Empty template -> a structured data.payload is re-serialised to JSON so
 * a Source->Sink relay round-trips the original shape. */
static void
test_payload_fallback_structured (void)
{
    PnMessage  *m = pn_message_new (NULL, "t");
    JsonObject *o = json_object_new ();
    JsonNode   *n = json_node_new (JSON_NODE_OBJECT);
    gchar      *out = NULL;
    gsize       len = 0;

    json_object_set_int_member (o, "a", 1);
    json_node_take_object (n, o);
    pn_message_set_member (m, "payload", n);   /* takes ownership */

    PN_CHECK (pn_mqtt_build_payload (NULL, m, NULL, &out, &len));
    PN_CHECK_CMPSTR (out, ==, "{\"a\":1}");
    PN_CHECK (len > 0);

    g_free (out);
    g_object_unref (m);
}

/* A template path that resolves to nothing expands to "" (PN_SUBST_MISS_EMPTY)
 * -- still a successful build, just an empty payload. */
static void
test_payload_template_missing_path (void)
{
    PnMessage *m = pn_message_new (NULL, "t");
    gchar     *out = NULL;
    gsize      len = 999;

    PN_CHECK (pn_mqtt_build_payload ("${data/nope}", m, NULL, &out, &len));
    PN_CHECK_CMPSTR (out, ==, "");
    PN_CHECK_CMPINT ((int) len, ==, 0);

    g_free (out);
    g_object_unref (m);
}

/* Empty template and no data.payload -> FALSE, so the caller drops the
 * message rather than publishing a misleading empty string. */
static void
test_payload_missing_drops (void)
{
    PnMessage *m = pn_message_new (NULL, "t");
    gchar     *out = (gchar *) 0x1;   /* sentinel: must stay untouched */
    gsize      len = 999;

    PN_CHECK_FALSE (pn_mqtt_build_payload ("", m, NULL, &out, &len));

    g_object_unref (m);
}

/* ------------------------------------------------------------------ */
/*  Offline publish queue                                              */
/* ------------------------------------------------------------------ */

/* A full backlog evicts the oldest to admit the newest; length is capped. */
static void
test_queue_cap_eviction (void)
{
    GQueue    *q = g_queue_new ();
    GPtrArray *fresh;
    guint      i;

    for (i = 0; i < PN_MQTT_SINK_QUEUE_MAX_LEN + 5; i++)
    {
        gchar *topic = g_strdup_printf ("t%u", i);
        pn_mqtt_pending_enqueue (q, topic, g_strdup ("x"), 1, 0, FALSE,
                                 (gint64) i);
        g_free (topic);   /* enqueue copies the topic */
    }

    /* Capped, not unbounded. */
    PN_CHECK_CMPINT ((int) g_queue_get_length (q),
                     ==, (int) PN_MQTT_SINK_QUEUE_MAX_LEN);

    /* Drain (all fresh): the first 5 were evicted, so head == "t5" and the
     * last-enqueued entry survives at the tail. */
    fresh = pn_mqtt_pending_collect_fresh (q, 1000000, G_MAXINT64);
    PN_CHECK_CMPINT ((int) fresh->len, ==, (int) PN_MQTT_SINK_QUEUE_MAX_LEN);
    if (fresh->len == PN_MQTT_SINK_QUEUE_MAX_LEN)
    {
        PnMqttPending *head = g_ptr_array_index (fresh, 0);
        PnMqttPending *tail = g_ptr_array_index (fresh, fresh->len - 1);
        gchar         *last = g_strdup_printf ("t%u",
                                  PN_MQTT_SINK_QUEUE_MAX_LEN + 4);

        PN_CHECK_CMPSTR (head->topic, ==, "t5");
        PN_CHECK_CMPSTR (tail->topic, ==, last);
        g_free (last);
    }

    g_ptr_array_unref (fresh);
    g_queue_free (q);
}

/* collect_fresh on an empty queue returns a valid, empty array. */
static void
test_queue_collect_empty (void)
{
    GQueue    *q = g_queue_new ();
    GPtrArray *fresh = pn_mqtt_pending_collect_fresh (q, 1000, G_MAXINT64);

    PN_CHECK (fresh != NULL);
    PN_CHECK_CMPINT ((int) fresh->len, ==, 0);

    g_ptr_array_unref (fresh);
    g_queue_free (q);
}

/* collect_fresh keeps entries within the age window (FIFO) and discards the
 * stale ones, draining the queue. */
static void
test_queue_age_filter (void)
{
    GQueue    *q = g_queue_new ();
    GPtrArray *fresh;

    pn_mqtt_pending_enqueue (q, "old", g_strdup ("x"), 1, 0, FALSE, 1000);
    pn_mqtt_pending_enqueue (q, "mid", g_strdup ("x"), 1, 0, FALSE, 2000);
    pn_mqtt_pending_enqueue (q, "new", g_strdup ("x"), 1, 0, FALSE, 3000);

    /* now=3500, window=1000 -> ages 2500/1500/500; only "new" survives. */
    fresh = pn_mqtt_pending_collect_fresh (q, 3500, 1000);
    PN_CHECK_CMPINT ((int) fresh->len, ==, 1);
    if (fresh->len == 1)
    {
        PnMqttPending *p = g_ptr_array_index (fresh, 0);
        PN_CHECK_CMPSTR (p->topic, ==, "new");
    }
    /* The queue is fully drained either way. */
    PN_CHECK_CMPINT ((int) g_queue_get_length (q), ==, 0);

    g_ptr_array_unref (fresh);
    g_queue_free (q);
}

/* All-fresh entries come back oldest-first, with payload ownership intact. */
static void
test_queue_fifo_order (void)
{
    GQueue    *q = g_queue_new ();
    GPtrArray *fresh;

    pn_mqtt_pending_enqueue (q, "a", g_strdup ("1"), 1, 0, FALSE, 100);
    pn_mqtt_pending_enqueue (q, "b", g_strdup ("2"), 1, 0, FALSE, 100);
    pn_mqtt_pending_enqueue (q, "c", g_strdup ("3"), 1, 0, FALSE, 100);

    fresh = pn_mqtt_pending_collect_fresh (q, 100, G_MAXINT64);
    PN_CHECK_CMPINT ((int) fresh->len, ==, 3);
    if (fresh->len == 3)
    {
        PnMqttPending *a = g_ptr_array_index (fresh, 0);
        PnMqttPending *b = g_ptr_array_index (fresh, 1);
        PnMqttPending *c = g_ptr_array_index (fresh, 2);

        PN_CHECK_CMPSTR (a->topic, ==, "a");
        PN_CHECK_CMPSTR (b->topic, ==, "b");
        PN_CHECK_CMPSTR (c->topic, ==, "c");
        PN_CHECK_CMPSTR (a->payload, ==, "1");   /* transfer kept the bytes */
    }

    g_ptr_array_unref (fresh);
    g_queue_free (q);
}

/* retarget (review M2): a backlog buffered for broker A must not flush to a
 * different broker B.  The first resolve (old identity NULL) and a reconnect
 * to the same broker keep the backlog; a genuine broker change clears it. */
static void
test_queue_retarget (void)
{
    GQueue *q = g_queue_new ();
    guint   dropped;

    pn_mqtt_pending_enqueue (q, "a", g_strdup ("1"), 1, 0, FALSE, 100);
    pn_mqtt_pending_enqueue (q, "b", g_strdup ("2"), 1, 0, FALSE, 100);

    /* First resolve: NULL prior identity → keep (load-time commands wait
     * for this broker's first connect). */
    dropped = pn_mqtt_pending_retarget (q, NULL, "tcp://a:1883");
    PN_CHECK_CMPINT ((int) dropped, ==, 0);
    PN_CHECK_CMPINT ((int) g_queue_get_length (q), ==, 2);

    /* Reconnect to the same broker → keep (the queue's whole purpose). */
    dropped = pn_mqtt_pending_retarget (q, "tcp://a:1883", "tcp://a:1883");
    PN_CHECK_CMPINT ((int) dropped, ==, 0);
    PN_CHECK_CMPINT ((int) g_queue_get_length (q), ==, 2);

    /* Broker change → drop the whole backlog (it was for broker A). */
    dropped = pn_mqtt_pending_retarget (q, "tcp://a:1883", "tcp://b:1883");
    PN_CHECK_CMPINT ((int) dropped, ==, 2);
    PN_CHECK_CMPINT ((int) g_queue_get_length (q), ==, 0);

    /* Removing the broker (URL → "") is a distinct identity, so it clears
     * too — a backlog with no broker to flush to is dropped. */
    pn_mqtt_pending_enqueue (q, "c", g_strdup ("3"), 1, 0, FALSE, 100);
    dropped = pn_mqtt_pending_retarget (q, "tcp://b:1883", "");
    PN_CHECK_CMPINT ((int) dropped, ==, 1);
    PN_CHECK_CMPINT ((int) g_queue_get_length (q), ==, 0);

    g_queue_free (q);
}

/* payload limit (review M4): the gsize->int narrowing mosquitto_publish
 * needs is guarded by this predicate.  Anything up to the MQTT 256MB ceiling
 * is publishable; one byte past it (and anything larger) is rejected before
 * the cast can wrap to a bogus length. */
static void
test_payload_within_limit (void)
{
    PN_CHECK (pn_mqtt_payload_within_limit (0) == TRUE);
    PN_CHECK (pn_mqtt_payload_within_limit (1) == TRUE);
    PN_CHECK (pn_mqtt_payload_within_limit (PN_MQTT_MAX_PAYLOAD_BYTES) == TRUE);
    PN_CHECK (pn_mqtt_payload_within_limit (
                  (gsize) PN_MQTT_MAX_PAYLOAD_BYTES + 1) == FALSE);
}

/* keep-alive normalisation (review M3): a configured value passes through,
 * 0 / negative falls back to the default, and anything past the 16-bit
 * protocol ceiling is clamped to 65535. */
static void
test_keepalive_seconds (void)
{
    PN_CHECK_CMPINT (pn_mqtt_keepalive_seconds (30),  ==, 30);
    PN_CHECK_CMPINT (pn_mqtt_keepalive_seconds (1),   ==, 1);
    PN_CHECK_CMPINT (pn_mqtt_keepalive_seconds (0),   ==,
                     PN_MQTT_DEFAULT_KEEPALIVE_SECONDS);
    PN_CHECK_CMPINT (pn_mqtt_keepalive_seconds (-5),  ==,
                     PN_MQTT_DEFAULT_KEEPALIVE_SECONDS);
    PN_CHECK_CMPINT (pn_mqtt_keepalive_seconds (65535), ==, 65535);
    PN_CHECK_CMPINT (pn_mqtt_keepalive_seconds (70000), ==, 65535);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mqtt-util");
    pn_test_add ("url_schemes",          test_url_schemes);
    pn_test_add ("url_explicit_port",    test_url_explicit_port);
    pn_test_add ("url_ipv6",             test_url_ipv6);
    pn_test_add ("url_rejected",         test_url_rejected);
    pn_test_add ("message_json_object",  test_message_json_object);
    pn_test_add ("message_json_number",  test_message_json_number);
    pn_test_add ("message_json_bool_value", test_message_json_bool_value);
    pn_test_add ("message_text_numeric", test_message_text_numeric);
    pn_test_add ("message_text_plain",   test_message_text_plain);
    pn_test_add ("message_binary",       test_message_binary);
    pn_test_add ("message_empty",        test_message_empty);
    pn_test_add ("message_envelope",     test_message_envelope);
    pn_test_add ("payload_template",     test_payload_template);
    pn_test_add ("payload_fallback_string", test_payload_fallback_string);
    pn_test_add ("payload_fallback_structured", test_payload_fallback_structured);
    pn_test_add ("payload_template_missing_path", test_payload_template_missing_path);
    pn_test_add ("payload_missing_drops", test_payload_missing_drops);
    pn_test_add ("queue_collect_empty",  test_queue_collect_empty);
    pn_test_add ("queue_cap_eviction",   test_queue_cap_eviction);
    pn_test_add ("queue_age_filter",     test_queue_age_filter);
    pn_test_add ("queue_fifo_order",     test_queue_fifo_order);
    pn_test_add ("queue_retarget",       test_queue_retarget);
    pn_test_add ("payload_within_limit", test_payload_within_limit);
    pn_test_add ("keepalive_seconds",    test_keepalive_seconds);
    return pn_test_run ();
}
