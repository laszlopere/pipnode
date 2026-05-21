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

#ifndef PN_TEMP_H
#define PN_TEMP_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTempAggregation                                                  */
/*                                                                     */
/*  How #PnTemp collapses the per-sensor readings collected from a     */
/*  host's CPU temperature sources into the single number emitted on   */
/*  data.value.  AVERAGE divides the sum by the count (giving a feel   */
/*  for whole-package thermals); MAXIMUM reports the hottest sensor    */
/*  (better for spotting a runaway core / a stuck fan).                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TEMP_AGGREGATION (pn_temp_aggregation_get_type ())

typedef enum
{
    PN_TEMP_AVERAGE,
    PN_TEMP_MAXIMUM,
} PnTempAggregation;

GType pn_temp_aggregation_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnTemp                                                             */
/*                                                                     */
/*  Auto-trigger node that samples CPU temperature once per            */
/*  #PnAutoTrigger:period seconds and emits the aggregated value (in   */
/*  degrees Celsius) as data.value.  Sources are scanned across the    */
/*  host's /sys/class/hwmon entries whose `name` matches a known CPU   */
/*  driver (coretemp, k10temp, k8temp, zenpower, cpu_thermal); if no   */
/*  hwmon source is found the node falls back to                       */
/*  /sys/class/thermal/thermal_zone* entries whose `type` looks like a */
/*  CPU thermal zone.  Each input sensor contributes one reading, the  */
/*  #PnTemp:aggregation property selects whether data.value is their   */
/*  arithmetic mean or their maximum.                                  */
/*                                                                     */
/*  When #PnTemp:hostname is empty or "localhost" the sample is read   */
/*  directly off the local filesystem.  Any other value is treated as  */
/*  a remote host and the sample is fetched via passwordless SSH;      */
/*  the worker shells out to `ssh -o BatchMode=yes ...` and runs a     */
/*  small POSIX-sh snippet on the far end that emits one               */
/*  `name|label|millidegrees` line per sensor for the C side to parse. */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TEMP (pn_temp_get_type ())

G_DECLARE_FINAL_TYPE (PnTemp, pn_temp, PN, TEMP, PnAutoTrigger)

PnTemp *pn_temp_new (void);

G_END_DECLS

#endif /* PN_TEMP_H */
