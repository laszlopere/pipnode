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

#ifndef PN_TASMOTA_PROBE_H
#define PN_TASMOTA_PROBE_H

#include "pn-node.h"
#include "pn-tasmota-common.h"   /* PnTasmotaStatusKind */

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTasmotaProbe                                                      */
/*                                                                     */
/*  The discovery half of the Tasmota concentrator (project TODO       */
/*  item 22.1).  Sits between an #PnMqttSource and an #PnMqttSink and   */
/*  turns the passive telemetry stream into an active per-device poll.  */
/*                                                                     */
/*  Every inbound message is inspected for a Tasmota device name: the   */
/*  `<device>` segment is parsed out of the envelope topic of the      */
/*  `tele/<device>/...`, `stat/<device>/...` and `cmnd/<device>/...`    */
/*  families.  The node keeps a live set of every device it has seen.   */
/*  When a device is seen for the first time it is probed promptly --   */
/*  the node emits a `cmnd/<device>/Status <n>` command that the        */
/*  downstream #PnMqttSink publishes -- and an already-known device is  */
/*  never re-probed more often than the configurable `interval`.        */
/*                                                                     */
/*  Why per-device and not a single broadcast: a command published to   */
/*  Tasmota's `tasmotas` group topic is acted on by every device but    */
/*  the reply is deliberately suppressed, so a group `Status` query     */
/*  yields zero `stat/+/STATUS` responses.  Reading info back requires  */
/*  addressing each device individually; this node automates that loop  */
/*  with no external scan.  The replies are collected by the companion  */
/*  #PnTasmotaConcentrator (TODO item 22.2).                            */
/*                                                                     */
/*  The node is a pure filter on its output: it emits only probe        */
/*  commands and never forwards the telemetry it observed, so its       */
/*  output wires straight to the Sink without echoing sensor traffic.   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TASMOTA_PROBE (pn_tasmota_probe_get_type ())

G_DECLARE_FINAL_TYPE (PnTasmotaProbe, pn_tasmota_probe,
                      PN, TASMOTA_PROBE, PnNode)

PnTasmotaProbe *pn_tasmota_probe_new (void);

G_END_DECLS

#endif /* PN_TASMOTA_PROBE_H */
