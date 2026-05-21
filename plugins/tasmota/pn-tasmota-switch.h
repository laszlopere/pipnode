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

#ifndef PN_TASMOTA_SWITCH_H
#define PN_TASMOTA_SWITCH_H

#include "pn-switch.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTasmotaSwitch                                                    */
/*                                                                     */
/*  Bidirectional device node for a single Tasmota relay -- merges     */
/*  the canvas-side slide switch (#PnSwitch), the read-side Tasmota    */
/*  POWER status filter (#PnTasmotaRelayStatus) and the write-side     */
/*  Tasmota POWER command builder (#PnTasmotaRelayCommand) into one    */
/*  node so the canonical "show the relay state, let the user flip     */
/*  it" use case wires up as `MQTT Source -> Tasmota Switch -> MQTT    */
/*  Sink` (three nodes) instead of the five-node factored chain.       */
/*                                                                     */
/*  Subclass of #PnSwitch so the slider widget, hit-testing and the    */
/*  worksheet's slider-click routing all work for free via             */
/*  polymorphism (`PN_IS_SWITCH` returns TRUE on this class too).      */
/*                                                                     */
/*  Direction split:                                                   */
/*                                                                     */
/*    * Inbound: any MQTT envelope on `<prefix>/stat/<switch-name>/    */
/*      POWER` (or POWER1..POWER8 multi-relay variants) with a bare    */
/*      "ON" / "OFF" payload syncs the latch                           */
/*      *silently* -- no downstream emit, since the inbound is an      */
/*      authoritative status observation rather than a transit         */
/*      message that should fan out.                                   */
/*                                                                     */
/*    * User click on the slider emits a fresh Tasmota command         */
/*      message: envelope topic = `cmnd/<switch-name>/POWER`,          */
/*      `data.payload = "ON" / "OFF"`, plus the standard               */
/*      `data.value` / `data.success` / `data.device` / `data.output`  */
/*      mirror so a downstream LED / Debug / Graph still reads the     */
/*      same shape it would have seen from the factored chain.         */
/*                                                                     */
/*  Consequence: a node wired downstream of the output only sees       */
/*  *user-driven* state changes, not external ones (a relay flipped    */
/*  via the Tasmota app or the wall button).  To shadow external       */
/*  changes too, wire the observer through a parallel                  */
/*  #PnTasmotaRelayStatus filter off the MQTT source.                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TASMOTA_SWITCH (pn_tasmota_switch_get_type ())

G_DECLARE_FINAL_TYPE (PnTasmotaSwitch, pn_tasmota_switch,
                      PN, TASMOTA_SWITCH, PnSwitch)

PnTasmotaSwitch *pn_tasmota_switch_new (void);

G_END_DECLS

#endif /* PN_TASMOTA_SWITCH_H */
