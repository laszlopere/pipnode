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

/* ------------------------------------------------------------------ */
/*  Meshtastic enum → label helpers shared by the dialog pages.        */
/*                                                                     */
/*  These tables are direct ports of /usr/bin/pip-mesh's format_       */
/*  hw_model / format_device_role.  Living in libpipnode-core means    */
/*  the Identity page (hw + role for the owner) and the Known Nodes   */
/*  page (hw + role for every NodeInfo) read from a single source --  */
/*  no risk of one page's table drifting from the other's as upstream  */
/*  adds boards.                                                       */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-formats.h"

typedef struct
{
    guint32          id;
    const char      *name;
    PnMeshAmbientLed ambient;   /* RGB LED the Ambient Lighting module drives? */
    const char      *led_note;  /* short indicator descriptor                 */
    PnMeshGps        gps;       /* GPS hardware class                          */
    const char      *gps_note;  /* short GPS descriptor                        */
    const char      *gps_gpio;  /* GPIO wiring that makes GPS work, or NULL    */
} HwEntry;

/* HardwareModel enum.  Sparse: a small linear search is faster than
 * the indexed array would be (and trivially extended).
 *
 * The @ambient / @led_note / @gps* columns are best-effort "internet
 * sources" data, cross-checked against the meshtastic/firmware variant.h
 * files: HAS_NEOPIXEL / RGBLED_* / HAS_NCP5623 / HAS_LP5562 (what the
 * Ambient Lighting thread drives) and GPS_UBLOX / GPS_L76K / GNSS_AIROHA
 * / HAS_GPS + GPS_RX_PIN / GPS_TX_PIN / PIN_GPS_EN (the GPS wiring).
 * As of mid-2026 only three boards here drive an RGB LED: RAK4631 and
 * WISMESH_TAP (NCP5623) and HELTEC_MESH_NODE_T114 (2x WS2812).  GPS pin
 * numbers are the firmware defaults -- the PositionConfig rx/tx/en GPIO
 * override them when non-zero (the page round-trips those).  This data
 * can lag upstream -- the UI frames it "according to internet sources"
 * and warns it may be wrong. */
static const HwEntry HW_MODELS[] = {
    {  1, "TLORA_V2", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_HEADER, "no onboard GPS (UART pads for an external module)", NULL },
    {  2, "TLORA_V1", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_HEADER, "no onboard GPS (UART pads for an external module)", NULL },
    {  3, "TLORA_V2_1_1P6", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_HEADER, "no onboard GPS (UART for an external module)", NULL },
    {  4, "TBEAM", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_BUILTIN, "a built-in u-blox GPS",
         "UART RX 34 / TX 12, powered by the PMU (no enable pin)" },
    {  5, "HELTEC_V2_0", PN_MESH_AMBIENT_LED_NONE, "a single white LED and OLED",
         PN_MESH_GPS_HEADER, "a GPS header (no chip soldered on)",
         "pre-wired UART RX 36 / TX 33" },
    {  6, "TBEAM_V0P7", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_BUILTIN, "a built-in u-blox GPS",
         "UART RX 12 / TX 15, powered by the PMU (no enable pin)" },
    {  7, "T_ECHO", PN_MESH_AMBIENT_LED_NONE, "a single status LED and e-ink display",
         PN_MESH_GPS_BUILTIN, "a built-in Quectel L76K GPS",
         "UART RX 41 / TX 40 (PPS 36)" },
    {  8, "TLORA_V1_1P3", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_HEADER, "no onboard GPS (UART for an external module)", NULL },
    {  9, "RAK4631", PN_MESH_AMBIENT_LED_RGB, "green/blue LEDs plus an NCP5623 RGB driver",
         PN_MESH_GPS_HEADER, "a WisBlock socket for a GPS module (e.g. RAK12500)",
         "UART RX 15 / TX 16 on the WisBlock socket (PPS 17)" },
    { 10, "HELTEC_V2_1", PN_MESH_AMBIENT_LED_NONE, "a single white LED and OLED",
         PN_MESH_GPS_HEADER, "a GPS header (no chip soldered on)",
         "pre-wired UART RX 36 / TX 33, enable GPIO 37" },
    { 11, "HELTEC_V1", PN_MESH_AMBIENT_LED_NONE, "a single white LED and OLED",
         PN_MESH_GPS_HEADER, "a GPS header (no chip soldered on)",
         "pre-wired UART RX 36 / TX 33" },
    { 12, "TBEAM_S3_CORE", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_BUILTIN, "a built-in u-blox GPS", "UART RX 9 / TX 8 (1PPS 6)" },
    { 13, "RAK11200", PN_MESH_AMBIENT_LED_NONE, "a single status LED",
         PN_MESH_GPS_HEADER, "a WisBlock ESP32 core (GPS via a module)", NULL },
    { 14, "NANO_G1", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_BUILTIN, "a built-in u-blox GPS", "UART RX 34 / TX 12" },
    { 15, "TLORA_V2_1_1P8", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_HEADER, "no onboard GPS (UART for an external module)", NULL },
    { 16, "TLORA_T3_S3", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_HEADER, "no onboard GPS (UART for an external module)", NULL },
    { 17, "NANO_G1_EXPLORER", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_BUILTIN, "a built-in u-blox GPS", "UART RX 34 / TX 12" },
    { 18, "NANO_G2_ULTRA", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_BUILTIN, "a built-in Quectel L76K GPS", "UART RX 9 / TX 10" },
    { 25, "STATION_G1", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_HEADER, "a labelled GPS header (module optional)",
         "pre-wired UART RX 34 / TX 12" },
    { 26, "RAK11310", PN_MESH_AMBIENT_LED_NONE, "a single status LED",
         PN_MESH_GPS_HEADER, "a WisBlock RP2040 core (GPS via a module)", NULL },
    { 33, "T_ECHO_PLUS", PN_MESH_AMBIENT_LED_NONE, "a single status LED and e-ink display",
         PN_MESH_GPS_BUILTIN, "a built-in Quectel L76K GPS",
         "UART RX 41 / TX 40 (PPS 36)" },
    { 37, "PORTDUINO", PN_MESH_AMBIENT_LED_NONE, "no hardware LED (native build)",
         PN_MESH_GPS_HEADER, "a Linux build (GPS via a configured serial port)", NULL },
    { 43, "HELTEC_V3", PN_MESH_AMBIENT_LED_NONE, "a single white LED and OLED",
         PN_MESH_GPS_HEADER, "no onboard GPS (UART for an external module)", NULL },
    { 44, "HELTEC_WSL_V3", PN_MESH_AMBIENT_LED_NONE, "a single status LED and no display",
         PN_MESH_GPS_HEADER, "no onboard GPS (UART for an external module)", NULL },
    { 47, "RPI_PICO", PN_MESH_AMBIENT_LED_NONE, "a single onboard LED",
         PN_MESH_GPS_HEADER, "a bare dev board (GPS via UART)", NULL },
    { 48, "HELTEC_WIRELESS_TRACKER", PN_MESH_AMBIENT_LED_NONE, "a single status LED and small TFT",
         PN_MESH_GPS_BUILTIN, "a built-in UC6580 GNSS",
         "UART RX 33 / TX 34 at 115200 baud; powered via VEXT (GPIO 3, active high)" },
    { 49, "HELTEC_WIRELESS_PAPER", PN_MESH_AMBIENT_LED_NONE, "a single status LED and e-ink display",
         PN_MESH_GPS_NONE, "no GPS", NULL },
    { 50, "T_DECK", PN_MESH_AMBIENT_LED_NONE, "a single status LED, TFT and keyboard",
         PN_MESH_GPS_HEADER, "GPS pins routed (module optional)",
         "pre-wired UART RX 44 / TX 43" },
    { 51, "T_WATCH_S3", PN_MESH_AMBIENT_LED_NONE, "a single status LED and touch TFT",
         PN_MESH_GPS_HEADER, "GPS pads (module optional)",
         "pre-wired UART RX 41 / TX 42 at 38400 baud" },
    { 53, "HELTEC_HT62", PN_MESH_AMBIENT_LED_NONE, "a single status LED and no screen",
         PN_MESH_GPS_NONE, "no GPS", NULL },
    { 65, "HELTEC_CAPSULE_SENSOR_V3", PN_MESH_AMBIENT_LED_NONE, "a single status LED and no screen",
         PN_MESH_GPS_BUILTIN, "a built-in GPS",
         "UART RX 5 / TX 4, enable GPIO 21 (reset 3, PPS 1)" },
    { 69, "HELTEC_MESH_NODE_T114", PN_MESH_AMBIENT_LED_RGB, "2x WS2812 NeoPixels and a TFT",
         PN_MESH_GPS_BUILTIN, "a built-in L76K GPS (autodetected; may be unpopulated)",
         "UART RX 37 / TX 39; powered via VEXT (GPIO 21, active high), PPS 36" },
    { 70, "SENSECAP_INDICATOR", PN_MESH_AMBIENT_LED_NONE, "an LCD panel and no user RGB LED",
         PN_MESH_GPS_HEADER, "GPS optional (not present by default)",
         "pre-wired UART RX 20 / TX 19" },
    { 71, "TRACKER_T1000_E", PN_MESH_AMBIENT_LED_NONE, "a single status LED and buzzer",
         PN_MESH_GPS_BUILTIN, "a built-in Airoha AG3335 GNSS",
         "UART RX 14 / TX 13 at 115200 baud; enable GPIO 43 (reset 47)" },
    { 79, "RPI_PICO2", PN_MESH_AMBIENT_LED_NONE, "a single onboard LED",
         PN_MESH_GPS_HEADER, "a bare dev board (GPS via UART)", NULL },
    { 84, "WISMESH_TAP", PN_MESH_AMBIENT_LED_RGB, "green/blue LEDs plus an NCP5623 RGB driver",
         PN_MESH_GPS_HEADER, "a WisBlock socket for a GPS module",
         "UART RX 15 / TX 16 on the WisBlock socket (PPS 17)" },
    { 89, "THINKNODE_M1", PN_MESH_AMBIENT_LED_NONE, "a single status LED and e-ink display",
         PN_MESH_GPS_BUILTIN, "a built-in L76K GPS", "UART RX 41 / TX 40 at 9600 baud" },
    { 91, "T_ETH_ELITE", PN_MESH_AMBIENT_LED_NONE, "a single status LED and optional screen",
         PN_MESH_GPS_BUILTIN, "GPS on the ELITE base board (may be unpopulated)",
         "UART RX 39 / TX 42 at 9600 baud" },
    { 94, "HELTEC_MESH_POCKET", PN_MESH_AMBIENT_LED_NONE, "a single status LED and OLED",
         PN_MESH_GPS_NONE, "no GPS", NULL },
    { 95, "SEEED_SOLAR_NODE", PN_MESH_AMBIENT_LED_NONE, "a single status LED and no screen",
         PN_MESH_GPS_BUILTIN, "a built-in L76K GPS",
         "UART RX 7 / TX 6 at 9600 baud, enable GPIO 18 (reset 17)" },
    {102, "T_DECK_PRO", PN_MESH_AMBIENT_LED_NONE, "a single status LED and e-ink display",
         PN_MESH_GPS_BUILTIN, "a built-in GPS",
         "UART RX 44 / TX 43 at 38400 baud, enable GPIO 15 (PPS 1)" },
    {103, "T_LORA_PAGER", PN_MESH_AMBIENT_LED_NONE, "a single status LED and TFT",
         PN_MESH_GPS_BUILTIN, "a built-in GPS",
         "UART RX 4 / TX 12 at 38400 baud (PPS 13)" },
    {108, "HELTEC_MESH_SOLAR", PN_MESH_AMBIENT_LED_NONE, "a NeoPixel that firmware leaves disabled",
         PN_MESH_GPS_BUILTIN, "a built-in L76K GPS (autodetected; may be unpopulated)",
         "UART RX 37 / TX 39; powered via VEXT (active high), PPS 36" },
    {109, "T_ECHO_LITE", PN_MESH_AMBIENT_LED_NONE, "a single status LED and e-ink display",
         PN_MESH_GPS_BUILTIN, "a built-in L76K GPS",
         "UART RX 29 / TX 42 at 9600 baud, enable GPIO 43 (PPS 47)" },
    {110, "HELTEC_V4", PN_MESH_AMBIENT_LED_NONE, "a single white LED and OLED",
         PN_MESH_GPS_BUILTIN, "a built-in L76K GPS",
         "UART RX 39 / TX 38, enable GPIO 34 (active low), reset 42, PPS 41" },
    {113, "HELTEC_WIRELESS_TRACKER_V2", PN_MESH_AMBIENT_LED_NONE, "a single status LED and TFT",
         PN_MESH_GPS_BUILTIN, "a built-in GNSS (UC6580-class)",
         "UART RX 33 / TX 34 at 115200 baud; powered via VEXT (GPIO 3, active high)" },
    {114, "T_WATCH_ULTRA", PN_MESH_AMBIENT_LED_UNKNOWN, "an AMOLED display; RGB LED undocumented",
         PN_MESH_GPS_UNKNOWN, "no firmware variant; GPS undocumented", NULL },
    {255, "PRIVATE_HW", PN_MESH_AMBIENT_LED_UNKNOWN, "DIY hardware; depends on the build",
         PN_MESH_GPS_UNKNOWN, "DIY hardware; depends on the build", NULL },
};

static const HwEntry *
hw_lookup (guint32 id)
{
    gsize i;
    for (i = 0; i < G_N_ELEMENTS (HW_MODELS); i++)
        if (HW_MODELS[i].id == id)
            return &HW_MODELS[i];
    return NULL;
}

gchar *
pn_mesh_format_hw_model (guint32 id)
{
    const HwEntry *e = hw_lookup (id);
    if (e != NULL)
        return g_strdup_printf ("%s (#%u)", e->name, id);
    return g_strdup_printf ("model #%u", id);
}

PnMeshAmbientLed
pn_mesh_hw_ambient_led (guint32 id)
{
    const HwEntry *e = hw_lookup (id);
    return e != NULL ? e->ambient : PN_MESH_AMBIENT_LED_UNKNOWN;
}

const gchar *
pn_mesh_hw_led_note (guint32 id)
{
    const HwEntry *e = hw_lookup (id);
    return e != NULL ? e->led_note : NULL;
}

PnMeshGps
pn_mesh_hw_gps (guint32 id)
{
    const HwEntry *e = hw_lookup (id);
    return e != NULL ? e->gps : PN_MESH_GPS_UNKNOWN;
}

const gchar *
pn_mesh_hw_gps_note (guint32 id)
{
    const HwEntry *e = hw_lookup (id);
    return e != NULL ? e->gps_note : NULL;
}

const gchar *
pn_mesh_hw_gps_gpio (guint32 id)
{
    const HwEntry *e = hw_lookup (id);
    return e != NULL ? e->gps_gpio : NULL;
}

const gchar *
pn_mesh_format_role (guint32 r)
{
    switch (r)
    {
    case 0:  return "CLIENT";
    case 1:  return "CLIENT_MUTE";
    case 2:  return "ROUTER";
    case 3:  return "ROUTER_CLIENT";   /* deprecated upstream */
    case 4:  return "REPEATER";
    case 5:  return "TRACKER";
    case 6:  return "SENSOR";
    case 7:  return "TAK";
    case 8:  return "CLIENT_HIDDEN";
    case 9:  return "LOST_AND_FOUND";
    case 10: return "TAK_TRACKER";
    case 11: return "ROUTER_LATE";
    default: return NULL;
    }
}
