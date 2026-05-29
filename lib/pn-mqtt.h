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

#ifndef PN_MQTT_H
#define PN_MQTT_H

#include "pn-node.h"
#include "pn-message.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnMqtt                                                             */
/*                                                                     */
/*  Source-only node that subscribes to an MQTT broker and emits one   */
/*  #PnMessage for every PUBLISH it receives on the configured topic   */
/*  filter.  Each message's envelope topic equals the MQTT topic       */
/*  verbatim (no prefix), so what the Debug pane shows matches what    */
/*  the broker published.  Designed to drop onto a worksheet, point at */
/*  a broker, and start feeding sensor / pub-sub data into the rest of */
/*  the pipeline.                                                      */
/*                                                                     */
/*  Properties:                                                        */
/*    - #PnMqtt:broker-profile – id of the mqtt-broker vault profile   */
/*                        to connect through; "" follows the primary,  */
/*                        "@custom" uses the inline fields below        */
/*    - #PnMqtt:url     – inline broker URL ("Custom settings" mode),  */
/*                        e.g. tcp://mqtt.example:1883 or              */
/*                        ssl://mqtt.example:8883                       */
/*    - #PnMqtt:subscribe-topic – MQTT topic filter to subscribe to    */
/*                        (wildcards + and # honoured); defaults to "#" */
/*    - #PnMqtt:username / #PnMqtt:password – optional MQTT auth       */
/*    - #PnMqtt:client-id – optional MQTT client id; auto-generated    */
/*                          when left empty                            */
/*    - #PnMqtt:qos     – subscription QoS (0..2)                      */
/*                                                                     */
/*  Subclassing:                                                       */
/*                                                                     */
/*    Device-specific source nodes (e.g. a Zigbee2MQTT bridge that     */
/*    talks the broker but ignores topics outside `zigbee2mqtt/...`,   */
/*    or that wants to collect aggregate state from many topics into   */
/*    one reshaped message) subclass PnMqtt and override the two       */
/*    class vfuncs below.  The connect / subscribe / reconnect /       */
/*    visual-state machinery stays in the base; the subclass only sees */
/*    decoded messages and can drop or reshape them.                   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MQTT (pn_mqtt_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnMqtt, pn_mqtt, PN, MQTT, PnNode)

/* ------------------------------------------------------------------ */
/*  PnMqttClass                                                        */
/*                                                                     */
/*  Two extension points let a subclass observe -- and shape -- the   */
/*  stream of inbound publishes without touching libmosquitto.        */
/* ------------------------------------------------------------------ */

struct _PnMqttClass
{
    PnNodeClass parent_class;

    /* Early topic filter.  Called as soon as a PUBLISH lands, BEFORE
     * the base builds a #PnMessage for it.  Returning %FALSE drops the
     * publish on the spot, skipping the JSON parse / message build /
     * main-thread marshal cost — the right hook for "ignore everything
     * outside topic prefix X" style filtering.
     *
     * THREAD: called on libmosquitto's network thread.  The
     * implementation must not touch GObject state without external
     * synchronisation; reading subclass config that was set once at
     * construction and never mutated is safe.  Default accepts every
     * topic. */
    gboolean (*accept_topic)    (PnMqtt      *self,
                                 const gchar *topic);

    /* Late hook on the fully-built message.  Called on the main thread,
     * after the base has decoded the publish into @message and BEFORE
     * the node emits it downstream.  Mutate @message in place to
     * reshape it (rewrite the envelope topic, add bag members, replace
     * data.value, …); return %FALSE to drop the message without
     * emitting.  Default emits as-is.
     *
     * The base owns @message and frees it after this returns. */
    gboolean (*process_message) (PnMqtt     *self,
                                 PnMessage  *message);

    /* Reserved slots so adding more vfuncs later does not break the
     * ABI of out-of-tree plugins built against this header. */
    gpointer _reserved[6];
};

/* Default vfunc implementations exposed so a subclass override that
 * wants to extend (rather than replace) the base behaviour can call up
 * to them.  Both currently return %TRUE; subclasses chain to these to
 * stay future-proof against the defaults gaining real behaviour. */
gboolean pn_mqtt_real_accept_topic    (PnMqtt      *self,
                                       const gchar *topic);
gboolean pn_mqtt_real_process_message (PnMqtt      *self,
                                       PnMessage   *message);

PnMqtt *pn_mqtt_new (void);

G_END_DECLS

#endif /* PN_MQTT_H */
