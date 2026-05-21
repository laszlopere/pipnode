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

#include "pn-tasmota-power-factor.h"

struct _PnTasmotaPowerFactor
{
    PnTasmotaEnergyMeter parent_instance;
};

G_DEFINE_TYPE (PnTasmotaPowerFactor, pn_tasmota_power_factor,
               PN_TYPE_TASMOTA_ENERGY_METER)

static void
pn_tasmota_power_factor_class_init (PnTasmotaPowerFactorClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->class_name = "Tasmota Power Factor";
    node_class->category   = "Tasmota";
}

static void
pn_tasmota_power_factor_init (PnTasmotaPowerFactor *self)
{
    /* Power factor cos φ -- dimensionless 0..1 ratio of real over
     * apparent power.  No current-type glyph: cosφ describes the
     * phase between voltage and current, so a single AC / DC
     * indicator would be self-referential.  The unit label is the
     * full "cos φ" string -- the analog meter's painter auto-shrinks
     * a wide unit until it fits the badge area, so the recognisable
     * European meter-face notation lands at a readable size without
     * crowding the scale arc. */
    g_object_set (self,
                  "key",       "data/payload/ENERGY/Factor",
                  "min-value", 0.0,
                  "max-value", 1.0,
                  "unit",      "cos \xcf\x86",  /* "cos φ" (U+03C6) */
                  "mode",      PN_ANALOG_METER_MODE_NONE,
                  NULL);

    pn_node_set_class_name (PN_NODE (self), "Tasmota Power Factor");
}
