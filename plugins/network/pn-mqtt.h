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

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnMqtt                                                             */
/*                                                                     */
/*  Source-only node that subscribes to an MQTT broker and emits one   */
/*  #PnMessage for every PUBLISH it receives on the configured topic   */
/*  filter.  Designed to drop onto a worksheet, point at a broker, and */
/*  start feeding sensor / pub-sub data into the rest of the pipeline. */
/*                                                                     */
/*  Properties:                                                        */
/*    - #PnMqtt:url     – broker URL, e.g. tcp://mqtt.example:1883     */
/*                        or ssl://mqtt.example:8883                   */
/*    - #PnMqtt:topic   – MQTT topic filter to subscribe to (wildcards */
/*                        + and # honoured); defaults to "#"           */
/*    - #PnMqtt:username / #PnMqtt:password – optional MQTT auth       */
/*    - #PnMqtt:client-id – optional MQTT client id; auto-generated    */
/*                          when left empty                            */
/*    - #PnMqtt:qos     – subscription QoS (0..2)                      */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MQTT (pn_mqtt_get_type ())

G_DECLARE_FINAL_TYPE (PnMqtt, pn_mqtt, PN, MQTT, PnNode)

PnMqtt *pn_mqtt_new (void);

G_END_DECLS

#endif /* PN_MQTT_H */
