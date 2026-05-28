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
/*    - #PnMqttSink:url        – broker URL, same syntax as #PnMqtt:url */
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
/* ------------------------------------------------------------------ */

#define PN_TYPE_MQTT_SINK (pn_mqtt_sink_get_type ())

G_DECLARE_FINAL_TYPE (PnMqttSink, pn_mqtt_sink, PN, MQTT_SINK, PnNode)

PnMqttSink *pn_mqtt_sink_new (void);

G_END_DECLS

#endif /* PN_MQTT_SINK_H */
