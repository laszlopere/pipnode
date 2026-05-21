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

#include "pn-tasmota-reactive-power.h"

struct _PnTasmotaReactivePower
{
    PnTasmotaEnergyMeter parent_instance;
};

G_DEFINE_TYPE (PnTasmotaReactivePower, pn_tasmota_reactive_power,
               PN_TYPE_TASMOTA_ENERGY_METER)

static void
pn_tasmota_reactive_power_class_init (PnTasmotaReactivePowerClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->class_name = "Tasmota Reactive Power";
    node_class->category   = "Tasmota";
}

static void
pn_tasmota_reactive_power_init (PnTasmotaReactivePower *self)
{
    /* Reactive power Q = V * I * sinφ -- the "var" SI unit (volt-
     * amperes reactive).  Scale shared with the real / apparent
     * meters (0 - 5000) so the three power readings line up
     * visually; Q ≤ S in magnitude so the shared upper bound is
     * never the limiting factor.  No AC / DC glyph -- reactive
     * power has no meaningful DC equivalent. */
    g_object_set (self,
                  "key",       "data/payload/ENERGY/ReactivePower",
                  "min-value", 0.0,
                  "max-value", 5000.0,
                  "unit",      "var",
                  "mode",      PN_ANALOG_METER_MODE_NONE,
                  NULL);

    pn_node_set_class_name (PN_NODE (self), "Tasmota Reactive Power");
}
