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

/* GPS hardware class for a board, same curated/best-effort caveats as
 * the LED data above (cross-checked against the firmware variant.h GPS
 * defines -- GPS_UBLOX / GPS_L76K / GNSS_AIROHA / HAS_GPS and the
 * GPS_RX_PIN / GPS_TX_PIN / PIN_GPS_EN pins). */
typedef enum
{
    PN_MESH_GPS_UNKNOWN = 0,
    PN_MESH_GPS_NONE,     /* no GPS and no practical GPS header        */
    PN_MESH_GPS_HEADER,   /* no onboard chip, but a UART for an ext one */
    PN_MESH_GPS_BUILTIN,  /* soldered-on GPS module                     */
} PnMeshGps;

PnMeshGps    pn_mesh_hw_gps (guint32 id);

/* A short GPS descriptor, e.g. "a built-in u-blox GPS" or "no GPS".
 * NULL for ids not in the table; static -- do not free. */
const gchar *pn_mesh_hw_gps_note (guint32 id);

/* The GPIO wiring that makes the GPS work on this board (UART RX/TX,
 * the power-enable pin, baud), e.g. "UART RX 34 / TX 12, powered by the
 * PMU (no enable pin)".  NULL when the board has no pre-wired GPS pins
 * (a generic header where the user picks the GPIO) or no GPS at all.
 * Static -- do not free. */
const gchar *pn_mesh_hw_gps_gpio (guint32 id);

/* Whether a board carries a WiFi radio and/or a wired Ethernet port,
 * so the WiFi/Ethernet section can tell the user up front whether the
 * switches there will do anything.  Same curated/best-effort caveats as
 * the LED/GPS data above: derived from the board's MCU family and the
 * firmware variant.h network defines (ESP32 parts have WiFi; nRF52 and
 * RP2040 parts do not; HAS_ETHERNET marks the handful of boards with a
 * soldered-on Ethernet PHY).  HOST is the native (Portduino) build,
 * whose networking belongs to the Linux host rather than the firmware.
 * UNKNOWN covers ids missing from the table and boards that could not
 * be pinned down. */
typedef enum
{
    PN_MESH_NET_UNKNOWN = 0,
    PN_MESH_NET_NO,       /* no such radio/port on this board          */
    PN_MESH_NET_YES,      /* present and firmware-driven                */
    PN_MESH_NET_HOST,     /* native build: provided by the host OS      */
} PnMeshNetCap;

PnMeshNetCap pn_mesh_hw_wifi (guint32 id);
PnMeshNetCap pn_mesh_hw_eth  (guint32 id);

/* Pretty-print a Meshtastic DeviceRole enum value as the upstream
 * symbolic name ("CLIENT", "ROUTER", ...).  Returns NULL for unknown
 * ids; callers fall back to a numeric label of their choice. */
const gchar *pn_mesh_format_role     (guint32 id);

G_END_DECLS

#endif /* PN_MESH_FORMATS_H */
