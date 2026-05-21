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

#ifndef PN_MEMORY_H
#define PN_MEMORY_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnMemoryUnit                                                       */
/*                                                                     */
/*  Output unit for #PnMemory's emitted figure.  Memory uses *binary*  */
/*  multiples (1 KByte = 1024 bytes, 1 MByte = 1024^2, 1 GByte =      */
/*  1024^3) -- the convention RAM vendors and tools like `free` /     */
/*  `htop` follow, since a "32 GB" stick is universally understood    */
/*  as 32 GiB (2^35 bytes) rather than 32 * 10^9 bytes.  This is a    */
/*  deliberate divergence from #PnDiskIo / #PnNetIo, which use        */
/*  decimal (1000-based) multiples to match the disk/network         */
/*  throughput convention.                                             */
/*                                                                     */
/*  The %  value is the special "report as percentage" mode: data.    */
/*  value is then in 0..100 rather than in bytes.  This is the        */
/*  default since memory pressure as a percentage is the most         */
/*  universally interpretable single-number indicator (the same       */
/*  reason `htop` defaults to a percentage bar).                       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MEMORY_UNIT (pn_memory_unit_get_type ())

typedef enum
{
    PN_MEMORY_UNIT_PERCENT,
    PN_MEMORY_UNIT_BYTE,
    PN_MEMORY_UNIT_KBYTE,
    PN_MEMORY_UNIT_MBYTE,
    PN_MEMORY_UNIT_GBYTE,
} PnMemoryUnit;

GType pn_memory_unit_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnMemory                                                           */
/*                                                                     */
/*  Auto-trigger node that samples /proc/meminfo every                 */
/*  #PnAutoTrigger:period seconds and emits the memory-used figure    */
/*  as data.value -- either a percentage (default) or an absolute     */
/*  byte count in the chosen unit.  "Used" is computed as MemTotal -  */
/*  MemAvailable (the kernel-authoritative "actually unavailable"     */
/*  number, available since Linux 3.14, which accounts for           */
/*  reclaimable caches and slab the way `free -h` and `htop` already  */
/*  do).                                                               */
/*                                                                     */
/*  Unlike the I/O nodes, the figure is an instantaneous snapshot so  */
/*  there is no "warming up" first tick: the very first sample        */
/*  emits a real reading.                                              */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MEMORY (pn_memory_get_type ())

G_DECLARE_FINAL_TYPE (PnMemory, pn_memory, PN, MEMORY, PnAutoTrigger)

PnMemory *pn_memory_new (void);

G_END_DECLS

#endif /* PN_MEMORY_H */
