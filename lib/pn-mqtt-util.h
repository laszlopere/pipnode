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

#ifndef PN_MQTT_UTIL_H
#define PN_MQTT_UTIL_H

#include <glib.h>

#include "pn-message.h"
#include "pn-node.h"
#include "pn-flow.h"
#include "pn-vault.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  pn-mqtt-util                                                        */
/*                                                                     */
/*  The broker-free, GTK-free pure helpers shared by the MQTT Source   */
/*  (#PnMqtt) and Sink (#PnMqttSink): URL parsing, inbound-message     */
/*  construction, outbound-payload construction, and the offline       */
/*  publish-queue policy.  Kept out of the node .c files so they can be */
/*  unit-tested directly without a broker — and so the two nodes share  */
/*  one copy instead of each carrying its own.  Nothing here links      */
/*  libmosquitto or GTK; the nodes wrap these with the I/O.            */
/* ------------------------------------------------------------------ */

/**
 * pn_mqtt_parse_url:
 * @url:      a broker URL
 * @out_host: (out): newly allocated host (caller frees) on success
 * @out_port: (out): resolved port on success
 * @out_tls:  (out): whether the scheme selects TLS, on success
 *
 * Accepts <code>tcp://host[:port]</code> for plain MQTT (default port
 * 1883) and <code>ssl://host[:port]</code> / <code>mqtts://host[:port]</code>
 * for MQTT-over-TLS (default port 8883).  A bare <code>host[:port]</code>
 * with no scheme is accepted and treated as plain TCP.
 *
 * Returns: %TRUE on success (and fills the out-params), %FALSE without
 *   touching the out-params on a malformed URL (NULL/empty, empty host,
 *   or a port that is non-numeric or outside 1..65535).
 */
gboolean pn_mqtt_parse_url (const gchar *url,
                            gchar      **out_host,
                            int         *out_port,
                            gboolean    *out_tls);

/* Default MQTT keep-alive (seconds): the broker disconnects a client that
 * goes silent for 1.5x this, so 60 s gives brisk dead-peer detection without
 * flooding an idle link with pings.  Used when a profile does not pin one. */
#define PN_MQTT_DEFAULT_KEEPALIVE_SECONDS 60

/**
 * pn_mqtt_keepalive_seconds:
 * @configured: a profile's resolved keep-alive (0 when unset / inline mode)
 *
 * Normalises a configured keep-alive into the value handed to
 * mosquitto_connect: @configured when it is a sane positive number, the
 * %PN_MQTT_DEFAULT_KEEPALIVE_SECONDS default when it is 0 or negative, and the
 * 16-bit protocol ceiling (65535) when it is larger.  Pure, so the nodes share
 * one clamp instead of each open-coding the default.
 *
 * Returns: a keep-alive in the inclusive range 1..65535.
 */
int pn_mqtt_keepalive_seconds (gint configured);

/**
 * PnMqttConnOpts:
 * @ca_file:      (transfer full): trusted CA bundle for TLS; "" → the system
 *                trust store (lets a private / self-signed CA broker verify).
 * @client_cert:  (transfer full): client certificate for mutual TLS; "" → none.
 * @client_key:   (transfer full): client private key for mutual TLS; "" → none.
 * @tls_insecure: skip certificate / hostname verification (dev only).
 * @keepalive:    keep-alive seconds; 0 → the node default (see
 *                #pn_mqtt_keepalive_seconds).
 *
 * The connection options beyond the broker identity (url/user/pass/cid): the
 * TLS trust material and the keep-alive interval.  These come only from a
 * provisioned profile — "Custom settings" inline mode has no UI for them, so
 * an inline resolve yields the unset defaults ("" / %FALSE / 0).  The owned
 * strings are released by #pn_mqtt_conn_opts_clear.
 */
typedef struct
{
    gchar    *ca_file;
    gchar    *client_cert;
    gchar    *client_key;
    gboolean  tls_insecure;
    gint      keepalive;
} PnMqttConnOpts;

/**
 * pn_mqtt_conn_opts_clear:
 * @opts: (transfer none): a #PnMqttConnOpts filled by pn_mqtt_resolve_connection()
 *
 * Frees the owned strings and resets every field, leaving @opts safe to reuse
 * or discard.  Does not free @opts itself (it is normally stack-allocated).
 */
void pn_mqtt_conn_opts_clear (PnMqttConnOpts *opts);

/**
 * pn_mqtt_resolve_connection:
 * @profile:     (nullable): the resolved "mqtt-broker" vault profile, or
 *               %NULL to use the inline fallback values
 * @inline_url:  (nullable): the node's inline url property
 * @inline_user: (nullable): the node's inline username property
 * @inline_pass: (nullable): the node's inline password property
 * @inline_cid:  (nullable): the node's inline client-id property
 * @out_url:     (out) (optional): resolved url (caller frees)
 * @out_user:    (out) (optional): resolved username (caller frees)
 * @out_pass:    (out) (optional): resolved password (caller frees)
 * @out_cid:     (out) (optional): resolved client id (caller frees)
 * @out_opts:    (out caller-allocates) (optional): TLS / keep-alive options;
 *               %NULL to skip.  Clear with #pn_mqtt_conn_opts_clear.
 *
 * Resolves the connection identity: when @profile is non-%NULL every field
 * comes from it (the password via #pn_profile_get_secret, so it follows the
 * vault's secret path); otherwise the inline values are used (a %NULL inline
 * value yields "").  The choice of which is made by the caller: a node passes
 * the profile from pn_node_get_profile(), which is %NULL when the picker is on
 * "Custom settings" (#PN_PROFILE_REF_CUSTOM) so the inline fields take effect.
 * Every requested out string is freshly allocated and non-%NULL.  @out_opts,
 * when requested, carries the TLS trust material and keep-alive — profile-only
 * settings, so an inline (%NULL @profile) resolve leaves them at their unset
 * defaults ("" / %FALSE / 0).
 */
void pn_mqtt_resolve_connection (PnProfile      *profile,
                                 const gchar    *inline_url,
                                 const gchar    *inline_user,
                                 const gchar    *inline_pass,
                                 const gchar    *inline_cid,
                                 gchar         **out_url,
                                 gchar         **out_user,
                                 gchar         **out_pass,
                                 gchar         **out_cid,
                                 PnMqttConnOpts *out_opts);

/**
 * pn_mqtt_build_message:
 * @source:      the node the message originates from (for #pn_message_new)
 * @topic:       the MQTT topic (may be %NULL → treated as "")
 * @payload:     (nullable): the raw, NOT-NUL-terminated payload bytes
 * @payload_len: number of bytes in @payload
 * @qos:         QoS the publish arrived at
 * @retained:    whether the broker delivered it from retained storage
 *
 * Builds the inbound #PnMessage from a received MQTT message's decoded
 * fields.  The data bag carries: the envelope topic verbatim; a
 * <code>payload</code> member that is a nested JSON node when the bytes
 * parse as JSON, otherwise the UTF-8 string (binary → ""); a
 * <code>value</code> mirror for a bare JSON number or a numeric text
 * payload; and <code>qos</code> / <code>retained</code> / <code>output</code>
 * / <code>success</code>.  Pure apart from constructing the message.
 *
 * Returns: (transfer full): a new #PnMessage.
 */
PnMessage *pn_mqtt_build_message (PnNode       *source,
                                  const gchar  *topic,
                                  const guchar *payload,
                                  gsize         payload_len,
                                  int           qos,
                                  gboolean      retained);

/**
 * pn_mqtt_build_payload:
 * @payload_template: (nullable): the Sink's payload template; when
 *                    non-empty its <code>${path}</code> placeholders are
 *                    expanded against @message and @flow
 * @message:          the inbound message to publish
 * @flow:             (nullable): the document flow, for global resolution
 * @out_bytes:        (out): the payload bytes (caller frees) on success
 * @out_len:          (out): the payload length on success
 *
 * Builds the outbound payload bytes.  With a non-empty template, expands
 * it.  Otherwise falls back to <code>data.payload</code>: a string member
 * goes out as raw bytes, a structured member is re-serialised to JSON.
 *
 * Returns: %TRUE on success (out-params filled), %FALSE when no usable
 *   payload could be derived (no template and no <code>data.payload</code>),
 *   so the caller drops the message rather than publishing an empty string.
 */
gboolean pn_mqtt_build_payload (const gchar *payload_template,
                                PnMessage   *message,
                                PnFlow      *flow,
                                gchar      **out_bytes,
                                gsize       *out_len);

/* ------------------------------------------------------------------ */
/*  Offline publish queue                                              */
/*                                                                     */
/*  The Sink buffers publishes that arrive before the broker session   */
/*  is up (the common case at document load) in a bounded, timestamped  */
/*  backlog and delivers them on connect.  The policy — eviction when   */
/*  full, discard when stale — is pure and lives here; the node keeps   */
/*  the mosquitto_publish.  The clock is injected so the policy is       */
/*  deterministically testable.                                         */
/* ------------------------------------------------------------------ */

/* Most entries the backlog holds before the oldest is evicted. */
#define PN_MQTT_SINK_QUEUE_MAX_LEN     64u
/* How long a buffered publish stays meaningful (microseconds). */
#define PN_MQTT_SINK_QUEUE_MAX_AGE_US  (30 * G_TIME_SPAN_SECOND)

/* The largest MQTT application-message payload: the spec's 4-byte
 * Remaining Length field caps a message at 268,435,455 bytes (~256 MB).
 * libmosquitto's publish takes an int payloadlen, so a gsize past this both
 * breaks the protocol and risks a gsize->int truncation to a bogus (often
 * negative) length; the Sink screens against it before the cast. */
#define PN_MQTT_MAX_PAYLOAD_BYTES      268435455u

/**
 * pn_mqtt_payload_within_limit:
 * @payload_len: a candidate publish payload length
 *
 * Whether @payload_len may be published as-is, i.e. it fits both the MQTT
 * application-message ceiling (#PN_MQTT_MAX_PAYLOAD_BYTES) and the int
 * payloadlen parameter libmosquitto's publish takes.  The Sink checks this
 * before narrowing gsize->int so an oversize payload is rejected cleanly
 * (a "payload too large" error, message dropped) instead of wrapping.
 *
 * Returns: %TRUE when @payload_len is publishable.
 */
gboolean pn_mqtt_payload_within_limit (gsize payload_len);

/* One buffered publish.  @topic / @payload are owned by the entry. */
typedef struct
{
    gchar   *topic;
    gchar   *payload;
    gsize    payload_len;
    int      qos;
    gboolean retain;
    gint64   received_us;
} PnMqttPending;

/**
 * pn_mqtt_pending_free:
 * @entry: (transfer full): a #PnMqttPending
 *
 * Frees the entry and its owned strings.  Signature matches #GDestroyNotify
 * so it can be a #GQueue / #GPtrArray element-free func.
 */
void pn_mqtt_pending_free (gpointer entry);

/**
 * pn_mqtt_pending_enqueue:
 * @queue:       the backlog (head = oldest)
 * @topic:       the resolved publish topic (copied)
 * @payload:     (transfer full): the payload bytes (ownership taken)
 * @payload_len: length of @payload
 * @qos:         publish QoS
 * @retain:      publish retain flag
 * @now_us:      the enqueue timestamp (g_get_monotonic_time() in the node)
 *
 * Appends an entry, first evicting the oldest while the queue is at
 * %PN_MQTT_SINK_QUEUE_MAX_LEN, so a prolonged outage keeps the most
 * recent intents rather than growing without bound.
 */
void pn_mqtt_pending_enqueue (GQueue      *queue,
                              const gchar *topic,
                              gchar       *payload,
                              gsize        payload_len,
                              int          qos,
                              gboolean     retain,
                              gint64       now_us);

/**
 * pn_mqtt_pending_retarget:
 * @queue:        the backlog
 * @old_identity: (nullable): the broker identity the backlog was buffered
 *                for, or %NULL when no broker has been resolved yet (the
 *                first connect after construction / load)
 * @new_identity: the broker identity about to be (re)connected to
 *
 * The retarget policy for the offline backlog (review M2): a publish buffered
 * for one broker must not be misdelivered to another.  When @old_identity is
 * non-%NULL and differs from @new_identity, the whole backlog is freed — every
 * entry was meant for a broker the node has retargeted away from.  A %NULL
 * @old_identity (first resolve, when load-time commands are legitimately
 * waiting for this broker's first connect) or an unchanged identity (a
 * transient reconnect / vault touch to the same broker) leaves the backlog
 * intact.  The identity is the resolved broker URL string; the caller records
 * it across restarts and compares the previous against the new one.
 *
 * Returns: the number of entries discarded (0 when kept).
 */
guint pn_mqtt_pending_retarget (GQueue      *queue,
                                const gchar *old_identity,
                                const gchar *new_identity);

/**
 * pn_mqtt_pending_collect_fresh:
 * @queue:      the backlog (drained by this call)
 * @now_us:     the current time (g_get_monotonic_time() in the node)
 * @max_age_us: the freshness window
 *
 * Pops every entry, oldest first; returns those still younger than
 * @max_age_us (the caller publishes them, in order) and frees the rest.
 *
 * Returns: (transfer full) (element-type PnMqttPending): the fresh entries
 *   in FIFO order.  The array's free func is #pn_mqtt_pending_free, so
 *   g_ptr_array_unref() frees the entries once the caller is done.
 */
GPtrArray *pn_mqtt_pending_collect_fresh (GQueue *queue,
                                          gint64  now_us,
                                          gint64  max_age_us);

G_END_DECLS

#endif /* PN_MQTT_UTIL_H */
