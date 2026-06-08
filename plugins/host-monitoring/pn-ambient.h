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

#ifndef PN_AMBIENT_H
#define PN_AMBIENT_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnAmbientAggregation                                               */
/*                                                                     */
/*  How #PnAmbient collapses the per-sensor readings collected from a  */
/*  host's ambient temperature sources into the single number emitted  */
/*  on data.value.  AVERAGE divides the sum by the count (smooths out  */
/*  short-lived spikes on any one chassis sensor); MAXIMUM reports the */
/*  hottest sensor (useful when one zone of the chassis is warming up  */
/*  faster than the others -- a blocked vent, a sunlit panel).         */
/*                                                                     */
/*  Mirrors #PnTempAggregation deliberately: keeping the enum values   */
/*  string-identical ("average" / "maximum") lets a worksheet share    */
/*  the same dropdown labels and lets a downstream filter that switches*/
/*  on data.aggregation handle messages from either node without       */
/*  caring which one produced them.                                    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_AMBIENT_AGGREGATION (pn_ambient_aggregation_get_type ())

typedef enum
{
    PN_AMBIENT_AVERAGE,
    PN_AMBIENT_MAXIMUM,
} PnAmbientAggregation;

GType pn_ambient_aggregation_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnAmbient                                                          */
/*                                                                     */
/*  Auto-trigger node that samples the host's ambient (chassis /       */
/*  motherboard / chipset) temperature once per #PnAutoTrigger:period  */
/*  seconds and emits the aggregated value (in degrees Celsius) as     */
/*  data.value.  Sibling of #PnTemp: same property shape, same         */
/*  envelope, same aggregation semantics -- the difference is the set  */
/*  of sensors picked up off the host.                                 */
/*                                                                     */
/*  Sources scanned, in order of preference:                           */
/*    1. /sys/class/hwmon/hwmonN entries whose `name` is `acpitz`      */
/*       (ACPI Thermal Zone -- the canonical board sensor on most      */
/*       consumer hardware).                                           */
/*    2. /sys/class/thermal/thermal_zoneN entries whose `type` is      */
/*       `acpitz`, has the `pch_` prefix (Platform Controller Hub --   */
/*       chipset), matches `INT340N Thermal` (Intel DPTF), or is one   */
/*       of `gen_thermal` / `TZ00` / `TZ01` (vendor zones).            */
/*  The thermal-zone fallback is consulted only if hwmon found         */
/*  nothing, mirroring #PnTemp's hwmon-then-thermal layering.          */
/*                                                                     */
/*  When #PnAmbient:hostname is empty or "localhost" the sample is     */
/*  read directly off the local filesystem; any other value is treated */
/*  as a remote host and the sample is fetched via passwordless SSH.   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_AMBIENT (pn_ambient_get_type ())

G_DECLARE_FINAL_TYPE (PnAmbient, pn_ambient, PN, AMBIENT, PnAutoTrigger)

PnAmbient *pn_ambient_new (void);

/* ------------------------------------------------------------------ */
/*  Pure parse/aggregate seam (no I/O, no GTK)                          */
/*                                                                     */
/*  One per-sensor reading, plus the two helpers the node's trigger    */
/*  uses to turn a remote sample into data.value.  They are exposed    */
/*  (non-static) purely so the headless unit tests can drive the parse */
/*  + collapse on canned text without sampling real hardware; the      */
/*  node remains the only production caller.  Mirrors #PnTemp's seam.  */
/* ------------------------------------------------------------------ */

typedef struct
{
    gchar  *label;    /* "<source>/<sensor>", owned */
    gdouble celsius;
} PnAmbientReading;

/* Parse a remote sample (one "name|label|millidegrees" line each) into
 * @readings, a GArray of #PnAmbientReading whose .label is freshly
 * allocated.  Malformed lines (missing either '|') are skipped. */
void    pn_ambient_parse_remote_lines (const gchar *blob,
                                       GArray      *readings);

/* Collapse @readings to a single value per @mode (AVERAGE or MAXIMUM),
 * also reporting the min/max/average and the label of the hottest
 * sensor.  An empty array yields all-zero and a NULL hot label. */
gdouble pn_ambient_aggregate (GArray               *readings,
                              PnAmbientAggregation   mode,
                              gdouble              *out_min,
                              gdouble              *out_max,
                              gdouble              *out_avg,
                              const gchar         **out_hot_label);

G_END_DECLS

#endif /* PN_AMBIENT_H */
