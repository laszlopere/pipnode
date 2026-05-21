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

#include "pn-tasmota-power.h"

struct _PnTasmotaPower
{
    PnTasmotaEnergyMeter parent_instance;
};

G_DEFINE_TYPE (PnTasmotaPower, pn_tasmota_power,
               PN_TYPE_TASMOTA_ENERGY_METER)

static void
pn_tasmota_power_class_init (PnTasmotaPowerClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->class_name = "Tasmota Power";
    node_class->category   = "Tasmota";
}

static void
pn_tasmota_power_init (PnTasmotaPower *self)
{
    /* 0 - 5000 W -- comfortably above the 3680 W single-phase
     * ceiling of a 16 A / 230 V relay so a momentary inrush peak
     * still lands inside the dial, and matched to the apparent /
     * reactive meters so the three power readings share the same
     * scale at a glance.  Power is a derived AC quantity
     * (P = V * A * cosφ) so the current-type glyph below the unit
     * is suppressed -- the reading has no meaningful AC / DC sense
     * distinct from the voltage and current the user already meters
     * elsewhere. */
    g_object_set (self,
                  "key",       "data/payload/ENERGY/Power",
                  "min-value", 0.0,
                  "max-value", 5000.0,
                  "unit",      "W",
                  "mode",      PN_ANALOG_METER_MODE_NONE,
                  NULL);

    pn_node_set_class_name (PN_NODE (self), "Tasmota Power");
}
