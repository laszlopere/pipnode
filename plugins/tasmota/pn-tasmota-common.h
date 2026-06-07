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

#ifndef PN_TASMOTA_COMMON_H
#define PN_TASMOTA_COMMON_H

#include <glib.h>
#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Tasmota Status <n> sections                                        */
/*                                                                     */
/*  Shared by every node that speaks Tasmota's `Status` command:       */
/*  #PnTasmotaStatusRequest (the manual single-shot query) and         */
/*  #PnTasmotaProbe (the auto-discovering poller).  The integer value  */
/*  of each member IS the numeric argument sent on the wire, so the    */
/*  payload is simply the stringified enum value.                      */
/*  PN_TASMOTA_STATUS_ALL (0) is Tasmota's "report everything" form.   */
/* ------------------------------------------------------------------ */
typedef enum
{
    PN_TASMOTA_STATUS_ALL              = 0,  /* Status 0  -- everything    */
    PN_TASMOTA_STATUS_PARAMETERS       = 1,  /* Status 1  -- device params */
    PN_TASMOTA_STATUS_FIRMWARE         = 2,  /* Status 2  -- firmware      */
    PN_TASMOTA_STATUS_LOGGING          = 3,  /* Status 3  -- logging/tele  */
    PN_TASMOTA_STATUS_MEMORY           = 4,  /* Status 4  -- memory/flash  */
    PN_TASMOTA_STATUS_NETWORK          = 5,  /* Status 5  -- network       */
    PN_TASMOTA_STATUS_MQTT             = 6,  /* Status 6  -- MQTT          */
    PN_TASMOTA_STATUS_TIME             = 7,  /* Status 7  -- time/location */
    PN_TASMOTA_STATUS_CONNECTED_SENSOR = 8,  /* Status 8  -- sensors       */
    PN_TASMOTA_STATUS_POWER_THRESHOLDS = 9,  /* Status 9  -- power limits  */
    PN_TASMOTA_STATUS_SENSOR_DATA      = 10, /* Status 10 -- sensor data   */
    PN_TASMOTA_STATUS_STATE            = 11, /* Status 11 -- device state  */
} PnTasmotaStatusKind;

/* Registered GEnum for #PnTasmotaStatusKind, used as the property type
 * so the editor offers a combobox of named sections.  Registered once
 * here; every Tasmota node that needs it shares this single GType. */
#define PN_TYPE_TASMOTA_STATUS_KIND (pn_tasmota_status_kind_get_type ())

GType     pn_tasmota_status_kind_get_type (void);

/* Extract the device name from an MQTT envelope topic like
 * "tele/tasmota13/SENSOR" or "tele/tasmota13/STATE".  Tasmota uses
 * the second-to-last segment as the device id regardless of the
 * leading family ("tele", "stat", "cmnd").  Returns a freshly
 * allocated string; %NULL when the topic is empty or has fewer than
 * two segments. */
gchar    *pn_tasmota_device_from_topic (const gchar *topic);

/* Walk @payload looking for a numeric @key, first at the top level
 * and then in each direct child object (Tasmota wraps individual
 * sensor readings in a per-sensor object, e.g. AM2301.Temperature).
 * On a hit, *@out_value receives the number and the function returns
 * %TRUE.  Misses return %FALSE without touching *@out_value. */
gboolean  pn_tasmota_find_number       (JsonObject  *payload,
                                        const gchar *key,
                                        gdouble     *out_value);

/* Look up a string @key on @payload at the top level only.  Returns
 * a borrowed pointer into the payload (valid for as long as
 * @payload is alive) or %NULL when the key is missing or not a
 * string.  Used for unit fields (TempUnit) which Tasmota always
 * places at the top level alongside the per-sensor objects. */
const gchar *pn_tasmota_find_top_string (JsonObject  *payload,
                                         const gchar *key);

G_END_DECLS

#endif /* PN_TASMOTA_COMMON_H */
