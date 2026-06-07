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

#ifndef PN_TASMOTA_CONCENTRATOR_H
#define PN_TASMOTA_CONCENTRATOR_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTasmotaConcentrator                                              */
/*                                                                     */
/*  The collector half of the Tasmota concentrator (project TODO       */
/*  item 22.2), companion to #PnTasmotaProbe.  Sits on a branch from   */
/*  an #PnMqttSource and gathers the per-device replies the devices    */
/*  publish on `stat/<device>/STATUS#` (and, optionally, the           */
/*  `tele/<device>/STATE` and `tele/<device>/SENSOR` telemetry).       */
/*                                                                     */
/*  Each inbound payload is shallow-merged, at the top level, into a   */
/*  per-device record keyed by the `<device>` segment of the topic.    */
/*  A device that publishes `STATUS` then `STATUS5` then `STATUS11`     */
/*  thus accumulates a single object carrying `Status`, `StatusNET`     */
/*  and `StatusSTS` together — the firmware splits its report across    */
/*  several MQTT messages and this node stitches them back into one.    */
/*                                                                     */
/*  Emit policy: whenever a Tasmota message arrives AND at least        */
/*  `interval` seconds have elapsed since the previous emit, the node   */
/*  sends ONE combined message whose `data.devices` member is an object */
/*  mapping every known device to its merged record — a single snapshot */
/*  of the whole fleet, ready for a downstream Debug / table / HTTP     */
/*  view.  The min-interval debounces a burst of replies (a Status 0    */
/*  query alone fans out into a dozen `STATUS#` messages) into one      */
/*  downstream update.  An optional `timeout` drops devices that have   */
/*  gone silent for too long so the snapshot reflects only live ones.   */
/*                                                                     */
/*  Why this is a separate node from #PnTasmotaProbe: the probe is the  */
/*  active discover-and-poll half (it emits `Status` queries), the      */
/*  concentrator is the passive collect-and-present half (it consumes   */
/*  the replies).  Keeping them apart lets each be wired independently  */
/*  — the probe to an #PnMqttSink, the concentrator to a view — and     */
/*  composed into the full discover → poll → collect → present loop.    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TASMOTA_CONCENTRATOR (pn_tasmota_concentrator_get_type ())

G_DECLARE_FINAL_TYPE (PnTasmotaConcentrator, pn_tasmota_concentrator,
                      PN, TASMOTA_CONCENTRATOR, PnNode)

PnTasmotaConcentrator *pn_tasmota_concentrator_new (void);

G_END_DECLS

#endif /* PN_TASMOTA_CONCENTRATOR_H */
