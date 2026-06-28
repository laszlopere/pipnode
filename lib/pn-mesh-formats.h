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

#ifndef PN_MESH_FORMATS_H
#define PN_MESH_FORMATS_H

#include <glib.h>

G_BEGIN_DECLS

/* Pretty-print a Meshtastic HardwareModel enum value as
 * "HELTEC_V3 (#43)".  Unknown ids fall through to "model #N" so the
 * user always sees something concrete -- the upstream enum gains new
 * boards every release. */
gchar       *pn_mesh_format_hw_model (guint32 id);

/* Whether a board has an RGB LED the Meshtastic Ambient Lighting
 * module can actually drive (addressable WS2812 / NeoPixel, an NCP5623
 * / LP5562 I2C RGB driver, or discrete PWM RGB).  Derived from a
 * curated table keyed on the firmware variant.h defines -- best-effort,
 * "internet sources" data that can lag or be wrong.  UNKNOWN is
 * returned both for ids missing from the table and for boards whose
 * hardware could not be pinned down. */
typedef enum
{
    PN_MESH_AMBIENT_LED_UNKNOWN = 0,
    PN_MESH_AMBIENT_LED_NONE,   /* only a monochrome LED, or none      */
    PN_MESH_AMBIENT_LED_RGB,    /* RGB LED the module can drive         */
} PnMeshAmbientLed;

PnMeshAmbientLed pn_mesh_hw_ambient_led (guint32 id);

/* A short human descriptor of the board's status indicator(s), e.g.
 * "a single white LED and OLED" or "2x WS2812 NeoPixels and a TFT".
 * Returns NULL for ids not in the table.  The string is static -- do
 * not free.  Phrased to slot into "... has <note> ...". */
const gchar *pn_mesh_hw_led_note (guint32 id);

/* Pretty-print a Meshtastic DeviceRole enum value as the upstream
 * symbolic name ("CLIENT", "ROUTER", ...).  Returns NULL for unknown
 * ids; callers fall back to a numeric label of their choice. */
const gchar *pn_mesh_format_role     (guint32 id);

G_END_DECLS

#endif /* PN_MESH_FORMATS_H */
