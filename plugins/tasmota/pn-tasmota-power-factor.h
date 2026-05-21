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

#ifndef PN_TASMOTA_POWER_FACTOR_H
#define PN_TASMOTA_POWER_FACTOR_H

#include "pn-tasmota-energy-meter.h"

G_BEGIN_DECLS

#define PN_TYPE_TASMOTA_POWER_FACTOR (pn_tasmota_power_factor_get_type ())

G_DECLARE_FINAL_TYPE (PnTasmotaPowerFactor, pn_tasmota_power_factor,
                      PN, TASMOTA_POWER_FACTOR, PnTasmotaEnergyMeter)

G_END_DECLS

#endif /* PN_TASMOTA_POWER_FACTOR_H */
