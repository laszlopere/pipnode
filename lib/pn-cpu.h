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

#ifndef PN_CPU_H
#define PN_CPU_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnCpu                                                              */
/*                                                                     */
/*  Auto-trigger node that samples /proc/stat every                    */
/*  #PnAutoTrigger:period seconds and emits the CPU busy percentage   */
/*  as data.value (0..100), computed as the per-tick delta of the     */
/*  kernel's cumulative jiffy counters between two samples.  Unlike   */
/*  #PnLoad (which reports the runqueue-length load average -- a      */
/*  proxy that does not distinguish "100% of one core" from           */
/*  "saturated 16 cores"), this node reports the actual fraction of   */
/*  wall-clock time the CPU spent in non-idle states.                  */
/*                                                                     */
/*  When #PnCpu:hostname is empty or "localhost" the sample is read    */
/*  directly from /proc/stat.  Any other value is treated as a        */
/*  remote host and the sample is fetched via passwordless SSH        */
/*  exactly like #PnLoad / #PnDiskIo / #PnNetIo do.                    */
/*                                                                     */
/*  #PnCpu:core picks which row of /proc/stat to measure: an empty    */
/*  string (the default) reads the aggregate "cpu" line (a sum        */
/*  across every core that the kernel maintains for us, so the        */
/*  emitted percentage represents the whole machine).  A numeric      */
/*  value ("0", "1", ...) selects the matching per-core row           */
/*  ("cpu0", "cpu1", ...) so a worksheet can graph a single core in   */
/*  isolation.                                                         */
/* ------------------------------------------------------------------ */

#define PN_TYPE_CPU (pn_cpu_get_type ())

G_DECLARE_FINAL_TYPE (PnCpu, pn_cpu, PN, CPU, PnAutoTrigger)

PnCpu *pn_cpu_new (void);

G_END_DECLS

#endif /* PN_CPU_H */
