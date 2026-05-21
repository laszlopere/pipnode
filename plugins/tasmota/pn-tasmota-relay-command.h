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

#ifndef PN_TASMOTA_RELAY_COMMAND_H
#define PN_TASMOTA_RELAY_COMMAND_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTasmotaRelayCommand                                              */
/*                                                                     */
/*  Inverse of #PnTasmotaRelayStatus.  Reads `data.value` off each     */
/*  inbound message, turns it into the Tasmota relay-control message a */
/*  downstream #PnMqttSink can publish, and forwards.  Concretely:     */
/*  value > 0.5 -> `data.payload = "ON"`, value <= 0.5 -> "OFF"; the   */
/*  envelope topic is rewritten to `cmnd/<switch-name>/POWER` so a     */
/*  bare `PnSwitch -> PnTasmotaRelayCommand -> PnMqttSink` chain       */
/*  drives a physical Tasmota relay without intervening Format /       */
/*  Rewrite nodes.                                                     */
/*                                                                     */
/*  The `switch-name` property is mandatory and intentionally has no   */
/*  default -- without an explicit per-instance device name there is   */
/*  no safe behaviour the node could pick on the user's behalf, since  */
/*  silently defaulting would risk flipping the wrong physical relay.  */
/*  An unconfigured node flips its body red and drops every message,   */
/*  mirroring #PnInject's "configuration required" convention.         */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TASMOTA_RELAY_COMMAND (pn_tasmota_relay_command_get_type ())

G_DECLARE_FINAL_TYPE (PnTasmotaRelayCommand, pn_tasmota_relay_command,
                      PN, TASMOTA_RELAY_COMMAND, PnNode)

PnTasmotaRelayCommand *pn_tasmota_relay_command_new (void);

G_END_DECLS

#endif /* PN_TASMOTA_RELAY_COMMAND_H */
