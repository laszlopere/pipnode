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
} HwEntry;

/* HardwareModel enum.  Sparse: a small linear search is faster than
 * the indexed array would be (and trivially extended).
 *
 * The @ambient / @led_note columns are best-effort "internet sources"
 * data, cross-checked against the meshtastic/firmware variant.h files
 * (HAS_NEOPIXEL / RGBLED_* / HAS_NCP5623 / HAS_LP5562 are what the
 * Ambient Lighting thread actually drives).  As of mid-2026 only three
 * boards here carry such a define: RAK4631 and WISMESH_TAP (NCP5623),
 * and HELTEC_MESH_NODE_T114 (2x WS2812).  Everything else has only a
 * monochrome LED or none.  This data can lag upstream -- the UI frames
 * it as "according to internet sources" and warns it may be wrong. */
static const HwEntry HW_MODELS[] = {
    {  1, "TLORA_V2",                 PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    {  2, "TLORA_V1",                 PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    {  3, "TLORA_V2_1_1P6",           PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    {  4, "TBEAM",                    PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    {  5, "HELTEC_V2_0",              PN_MESH_AMBIENT_LED_NONE,    "a single white LED and OLED" },
    {  6, "TBEAM_V0P7",               PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    {  7, "T_ECHO",                   PN_MESH_AMBIENT_LED_NONE,    "a single status LED and e-ink display" },
    {  8, "TLORA_V1_1P3",             PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    {  9, "RAK4631",                  PN_MESH_AMBIENT_LED_RGB,     "green/blue LEDs plus an NCP5623 RGB driver" },
    { 10, "HELTEC_V2_1",              PN_MESH_AMBIENT_LED_NONE,    "a single white LED and OLED" },
    { 11, "HELTEC_V1",                PN_MESH_AMBIENT_LED_NONE,    "a single white LED and OLED" },
    { 12, "TBEAM_S3_CORE",            PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    { 13, "RAK11200",                 PN_MESH_AMBIENT_LED_NONE,    "a single status LED" },
    { 14, "NANO_G1",                  PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    { 15, "TLORA_V2_1_1P8",           PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    { 16, "TLORA_T3_S3",              PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    { 17, "NANO_G1_EXPLORER",         PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    { 18, "NANO_G2_ULTRA",            PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    { 25, "STATION_G1",               PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    { 26, "RAK11310",                 PN_MESH_AMBIENT_LED_NONE,    "a single status LED" },
    { 33, "T_ECHO_PLUS",              PN_MESH_AMBIENT_LED_NONE,    "a single status LED and e-ink display" },
    { 37, "PORTDUINO",                PN_MESH_AMBIENT_LED_NONE,    "no hardware LED (native build)" },
    { 43, "HELTEC_V3",                PN_MESH_AMBIENT_LED_NONE,    "a single white LED and OLED" },
    { 44, "HELTEC_WSL_V3",            PN_MESH_AMBIENT_LED_NONE,    "a single status LED and no display" },
    { 47, "RPI_PICO",                 PN_MESH_AMBIENT_LED_NONE,    "a single onboard LED" },
    { 48, "HELTEC_WIRELESS_TRACKER",  PN_MESH_AMBIENT_LED_NONE,    "a single status LED and small TFT" },
    { 49, "HELTEC_WIRELESS_PAPER",    PN_MESH_AMBIENT_LED_NONE,    "a single status LED and e-ink display" },
    { 50, "T_DECK",                   PN_MESH_AMBIENT_LED_NONE,    "a single status LED, TFT and keyboard" },
    { 51, "T_WATCH_S3",               PN_MESH_AMBIENT_LED_NONE,    "a single status LED and touch TFT" },
    { 53, "HELTEC_HT62",              PN_MESH_AMBIENT_LED_NONE,    "a single status LED and no screen" },
    { 65, "HELTEC_CAPSULE_SENSOR_V3", PN_MESH_AMBIENT_LED_NONE,    "a single status LED and no screen" },
    { 69, "HELTEC_MESH_NODE_T114",    PN_MESH_AMBIENT_LED_RGB,     "2x WS2812 NeoPixels and a TFT" },
    { 70, "SENSECAP_INDICATOR",       PN_MESH_AMBIENT_LED_NONE,    "an LCD panel and no user RGB LED" },
    { 71, "TRACKER_T1000_E",          PN_MESH_AMBIENT_LED_NONE,    "a single status LED and buzzer" },
    { 79, "RPI_PICO2",                PN_MESH_AMBIENT_LED_NONE,    "a single onboard LED" },
    { 84, "WISMESH_TAP",              PN_MESH_AMBIENT_LED_RGB,     "green/blue LEDs plus an NCP5623 RGB driver" },
    { 89, "THINKNODE_M1",             PN_MESH_AMBIENT_LED_NONE,    "a single status LED and e-ink display" },
    { 91, "T_ETH_ELITE",              PN_MESH_AMBIENT_LED_NONE,    "a single status LED and optional screen" },
    { 94, "HELTEC_MESH_POCKET",       PN_MESH_AMBIENT_LED_NONE,    "a single status LED and OLED" },
    { 95, "SEEED_SOLAR_NODE",         PN_MESH_AMBIENT_LED_NONE,    "a single status LED and no screen" },
    {102, "T_DECK_PRO",               PN_MESH_AMBIENT_LED_NONE,    "a single status LED and e-ink display" },
    {103, "T_LORA_PAGER",             PN_MESH_AMBIENT_LED_NONE,    "a single status LED and TFT" },
    {108, "HELTEC_MESH_SOLAR",        PN_MESH_AMBIENT_LED_NONE,    "a NeoPixel that firmware leaves disabled" },
    {109, "T_ECHO_LITE",              PN_MESH_AMBIENT_LED_NONE,    "a single status LED and e-ink display" },
    {110, "HELTEC_V4",                PN_MESH_AMBIENT_LED_NONE,    "a single white LED and OLED" },
    {113, "HELTEC_WIRELESS_TRACKER_V2", PN_MESH_AMBIENT_LED_NONE,  "a single status LED and TFT" },
    {114, "T_WATCH_ULTRA",            PN_MESH_AMBIENT_LED_UNKNOWN, "an AMOLED display; RGB LED undocumented" },
    {255, "PRIVATE_HW",               PN_MESH_AMBIENT_LED_UNKNOWN, "DIY hardware; depends on the build" },
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
