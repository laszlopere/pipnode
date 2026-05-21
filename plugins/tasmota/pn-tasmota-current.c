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

#include "pn-tasmota-current.h"

struct _PnTasmotaCurrent
{
    PnTasmotaEnergyMeter parent_instance;
};

G_DEFINE_TYPE (PnTasmotaCurrent, pn_tasmota_current,
               PN_TYPE_TASMOTA_ENERGY_METER)

static void
pn_tasmota_current_class_init (PnTasmotaCurrentClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->class_name = "Tasmota Current";
    node_class->category   = "Tasmota";
}

static void
pn_tasmota_current_init (PnTasmotaCurrent *self)
{
    /* 0 - 20 A scale: the typical 16 A Sonoff Pow / Pow R2 rating is
     * comfortably inside the dial without the needle riding the
     * end-stop at the rated maximum, and the headroom covers the
     * higher-current variants (Pow Elite at 20 A) without forcing
     * the user to retune the range.  AC sine-wave glyph below the
     * "A" unit -- a mains ammeter is always AC. */
    g_object_set (self,
                  "key",       "data/payload/ENERGY/Current",
                  "min-value", 0.0,
                  "max-value", 20.0,
                  "unit",      "A",
                  "mode",      PN_ANALOG_METER_MODE_AC,
                  NULL);

    pn_node_set_class_name (PN_NODE (self), "Tasmota Current");
}
