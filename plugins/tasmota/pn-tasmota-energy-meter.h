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

#ifndef PN_TASMOTA_ENERGY_METER_H
#define PN_TASMOTA_ENERGY_METER_H

#include "pn-analog-meter.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTasmotaEnergyMeter                                               */
/*                                                                     */
/*  Abstract derivable subclass of #PnAnalogMeter that shares the      */
/*  Tasmota-specific plumbing every concrete energy-channel meter      */
/*  (voltage, current, power, ...) needs: a "switch-name" property to  */
/*  filter incoming messages down to a single Tasmota device, the      */
/*  topic gate that restricts traffic to the                           */
/*  `<prefix>/tele/<device>/SENSOR` family, and the chain-up to        */
/*  #PnAnalogMeter::receive that runs the inherited `key`-based JSON   */
/*  path resolver against the (now-filtered) message.                  */
/*                                                                     */
/*  Concrete leaves set the inherited `key` property in their `_init`  */
/*  to the actual payload path their quantity lives at -- typically    */
/*  `data/payload/ENERGY/<Name>` -- alongside the meter's cosmetic     */
/*  defaults (unit string, min/max range, AC/DC/None glyph mode).      */
/*  Surfacing the path through the standard `key` property means the   */
/*  settings dialog's Data tab shows the user where the reading comes  */
/*  from and lets them retarget the meter at a non-standard Tasmota    */
/*  template without writing C.                                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TASMOTA_ENERGY_METER (pn_tasmota_energy_meter_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnTasmotaEnergyMeter, pn_tasmota_energy_meter,
                          PN, TASMOTA_ENERGY_METER, PnAnalogMeter)

struct _PnTasmotaEnergyMeterClass
{
    PnAnalogMeterClass parent_class;
};

G_END_DECLS

#endif /* PN_TASMOTA_ENERGY_METER_H */
