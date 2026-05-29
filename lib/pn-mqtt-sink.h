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

#ifndef PN_MQTT_SINK_H
#define PN_MQTT_SINK_H

#include "pn-node.h"
#include "pn-message.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnMqttSink                                                         */
/*                                                                     */
/*  Input-only counterpart of #PnMqtt.  Holds a libmosquitto client    */
/*  open against a configured broker and PUBLISHes one message per      */
/*  incoming #PnMessage.  The publish topic is taken from each          */
/*  message's envelope topic verbatim, so per-message routing is        */
/*  shaped by whichever upstream node sets the topic -- #PnRewrite is   */
/*  the canonical choice for stamping `cmnd/${data/device}/POWER`-style */
/*  topics derived from the data bag.  Designed to close the loop on    */
/*  a Source -> filter -> Sink flow without having to spawn a helper    */
/*  process or shell out through a command node.                        */
/*                                                                     */
/*  Properties:                                                        */
/*    - #PnMqttSink:broker-profile – id of the mqtt-broker vault        */
/*                               profile to publish through; "" follows */
/*                               the primary, "@custom" uses the inline */
/*                               fields                                 */
/*    - #PnMqttSink:url        – inline broker URL ("Custom settings"   */
/*                               mode), same syntax as #PnMqtt:url      */
/*    - #PnMqttSink:topic-template – optional publish-topic template;   */
/*                               when empty the publish topic comes off */
/*                               each inbound message's envelope topic  */
/*    - #PnMqttSink:payload    – payload template; when empty the       */
/*                               inbound `data.payload` is published    */
/*                               verbatim (string -> raw bytes,         */
/*                               object/array/number -> JSON), when     */
/*                               non-empty the template is expanded     */
/*                               against the message and its UTF-8      */
/*                               bytes are published as-is              */
/*    - #PnMqttSink:retain     – publish the message as retained        */
/*                               (broker remembers it for late          */
/*                               subscribers)                           */
/*    - #PnMqttSink:username / #PnMqttSink:password – optional MQTT     */
/*                               auth, same as #PnMqtt                  */
/*    - #PnMqttSink:client-id  – optional MQTT client id                */
/*    - #PnMqttSink:qos        – publish QoS (0..2)                     */
/*                                                                     */
/*  Subclassing:                                                       */
/*                                                                     */
/*    Device-specific sink nodes (e.g. a sink that reshapes every       */
/*    inbound message into the topic / payload a particular relay or    */
/*    bridge expects, instead of relying on an upstream #PnRewrite)      */
/*    subclass PnMqttSink and override the two class vfuncs below.  The  */
/*    connect / publish / offline-queue / reconnect / visual-state       */
/*    machinery stays in the base; the subclass only sees the message    */
/*    on its way out and the errors the base senses.                     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MQTT_SINK (pn_mqtt_sink_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnMqttSink, pn_mqtt_sink, PN, MQTT_SINK, PnNode)

/* ------------------------------------------------------------------ */
/*  PnMqttSinkClass                                                    */
/*                                                                     */
/*  Two extension points let a subclass shape what gets published --   */
/*  and observe every error the base detects -- without touching       */
/*  libmosquitto or the offline-publish queue.                          */
/* ------------------------------------------------------------------ */

struct _PnMqttSinkClass
{
    PnNodeClass parent_class;

    /* Late hook on the inbound message, called on the main thread from
     * the receive path BEFORE the base resolves the publish topic /
     * payload and hands bytes to mosquitto_publish.  Mutate @message in
     * place to reshape what gets published (rewrite the envelope topic,
     * replace data.payload, add bag members, …); return %FALSE to drop
     * the message without publishing.  Default publishes as-is.
     *
     * The base owns @message and does not free it here; it is the same
     * message the node received. */
    gboolean (*process_message) (PnMqttSink *self,
                                 PnMessage  *message);

    /* Error-sensing hook.  The base funnels every error it detects --
     * a rejected publish, a refused / failed connect, an invalid broker
     * URL, a TLS or auth setup failure -- through here so a subclass can
     * sense and react to it.  Always called on the main thread, so
     * pn_node_log_*() and other GObject calls are safe.  @message is a
     * human-readable one-liner describing the failure.
     *
     * The default implementation records @message in the per-node Log
     * dialog at #PN_LOG_LEVEL_ERROR (pn_node_log_error).  A subclass
     * overrides this to report differently (a device-specific note, a
     * different level, its own state); chain to
     * pn_mqtt_sink_real_report_error() to keep the default node-log line
     * in addition to the subclass behaviour. */
    void     (*report_error)    (PnMqttSink  *self,
                                 const gchar *message);

    /* Reserved slots so adding more vfuncs later does not break the
     * ABI of out-of-tree plugins built against this header. */
    gpointer _reserved[6];
};

/* Default vfunc implementations exposed so a subclass override that
 * wants to extend (rather than replace) the base behaviour can chain up
 * to them. */
gboolean pn_mqtt_sink_real_process_message (PnMqttSink  *self,
                                            PnMessage   *message);
void     pn_mqtt_sink_real_report_error    (PnMqttSink  *self,
                                            const gchar *message);

PnMqttSink *pn_mqtt_sink_new (void);

G_END_DECLS

#endif /* PN_MQTT_SINK_H */
