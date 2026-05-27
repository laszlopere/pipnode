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
    guint32     id;
    const char *name;
} HwEntry;

/* HardwareModel enum.  Sparse: a small linear search is faster than
 * the indexed array would be (and trivially extended). */
static const HwEntry HW_MODELS[] = {
    {  1, "TLORA_V2" },
    {  2, "TLORA_V1" },
    {  3, "TLORA_V2_1_1P6" },
    {  4, "TBEAM" },
    {  5, "HELTEC_V2_0" },
    {  6, "TBEAM_V0P7" },
    {  7, "T_ECHO" },
    {  8, "TLORA_V1_1P3" },
    {  9, "RAK4631" },
    { 10, "HELTEC_V2_1" },
    { 11, "HELTEC_V1" },
    { 12, "TBEAM_S3_CORE" },
    { 13, "RAK11200" },
    { 14, "NANO_G1" },
    { 15, "TLORA_V2_1_1P8" },
    { 16, "TLORA_T3_S3" },
    { 17, "NANO_G1_EXPLORER" },
    { 18, "NANO_G2_ULTRA" },
    { 25, "STATION_G1" },
    { 26, "RAK11310" },
    { 33, "T_ECHO_PLUS" },
    { 37, "PORTDUINO" },
    { 43, "HELTEC_V3" },
    { 44, "HELTEC_WSL_V3" },
    { 47, "RPI_PICO" },
    { 48, "HELTEC_WIRELESS_TRACKER" },
    { 49, "HELTEC_WIRELESS_PAPER" },
    { 50, "T_DECK" },
    { 51, "T_WATCH_S3" },
    { 53, "HELTEC_HT62" },
    { 65, "HELTEC_CAPSULE_SENSOR_V3" },
    { 69, "HELTEC_MESH_NODE_T114" },
    { 70, "SENSECAP_INDICATOR" },
    { 71, "TRACKER_T1000_E" },
    { 79, "RPI_PICO2" },
    { 84, "WISMESH_TAP" },
    { 89, "THINKNODE_M1" },
    { 91, "T_ETH_ELITE" },
    { 94, "HELTEC_MESH_POCKET" },
    { 95, "SEEED_SOLAR_NODE" },
    {102, "T_DECK_PRO" },
    {103, "T_LORA_PAGER" },
    {108, "HELTEC_MESH_SOLAR" },
    {109, "T_ECHO_LITE" },
    {110, "HELTEC_V4" },
    {113, "HELTEC_WIRELESS_TRACKER_V2" },
    {114, "T_WATCH_ULTRA" },
    {255, "PRIVATE_HW" },
};

gchar *
pn_mesh_format_hw_model (guint32 id)
{
    gsize i;
    for (i = 0; i < G_N_ELEMENTS (HW_MODELS); i++)
        if (HW_MODELS[i].id == id)
            return g_strdup_printf ("%s (#%u)", HW_MODELS[i].name, id);
    return g_strdup_printf ("model #%u", id);
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
