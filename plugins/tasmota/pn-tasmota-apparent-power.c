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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-tasmota-apparent-power.h"

struct _PnTasmotaApparentPower
{
    PnTasmotaEnergyMeter parent_instance;
};

G_DEFINE_TYPE (PnTasmotaApparentPower, pn_tasmota_apparent_power,
               PN_TYPE_TASMOTA_ENERGY_METER)

static void
pn_tasmota_apparent_power_class_init (PnTasmotaApparentPowerClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->class_name = "Tasmota Apparent Power";
    node_class->category   = "Tasmota";
}

static void
pn_tasmota_apparent_power_init (PnTasmotaApparentPower *self)
{
    /* Apparent power S = V * I -- units are volt-amperes, scale
     * shared with the real / reactive meters (0 - 5000) so the
     * three power readings line up visually on a worksheet.  No
     * AC / DC glyph: S is a derived quantity that only makes sense
     * for AC anyway. */
    g_object_set (self,
                  "key",       "data/payload/ENERGY/ApparentPower",
                  "min-value", 0.0,
                  "max-value", 5000.0,
                  "unit",      "VA",
                  "mode",      PN_ANALOG_METER_MODE_NONE,
                  NULL);

    pn_node_set_class_name (PN_NODE (self), "Tasmota Apparent Power");
}
