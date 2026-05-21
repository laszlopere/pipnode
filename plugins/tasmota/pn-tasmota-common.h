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
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Extract the device name from an MQTT envelope topic like
 * "mqtt/tele/tasmota13/SENSOR" or "tele/tasmota13/STATE".  Tasmota
 * uses the second-to-last segment as the device id regardless of the
 * leading family ("tele", "stat", "cmnd") and any host-side prefix
 * the MQTT source node may have prepended.  Returns a freshly
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
