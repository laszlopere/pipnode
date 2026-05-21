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

#include "pn-tasmota-voltage.h"

struct _PnTasmotaVoltage
{
    PnTasmotaEnergyMeter parent_instance;
};

G_DEFINE_TYPE (PnTasmotaVoltage, pn_tasmota_voltage,
               PN_TYPE_TASMOTA_ENERGY_METER)

static void
pn_tasmota_voltage_class_init (PnTasmotaVoltageClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->class_name = "Tasmota Voltage";
    node_class->category   = "Tasmota";
}

static void
pn_tasmota_voltage_init (PnTasmotaVoltage *self)
{
    /* AC voltmeter look: 0 - 300 V scale, "V" unit, AC sine-wave
     * glyph below the unit.  300 V upper bound gives a comfortable
     * headroom above the 230 V European mains nominal (and the 240 V
     * upper edge of the UK / IEC tolerance band) so a slightly over-
     * voltage reading still lands well inside the dial rather than
     * pegging the needle at full scale.  `key` points the inherited
     * path resolver at the ENERGY.Voltage member of the Tasmota
     * SENSOR payload. */
    g_object_set (self,
                  "key",       "data/payload/ENERGY/Voltage",
                  "min-value", 0.0,
                  "max-value", 300.0,
                  "unit",      "V",
                  "mode",      PN_ANALOG_METER_MODE_AC,
                  NULL);

    pn_node_set_class_name (PN_NODE (self), "Tasmota Voltage");
}
