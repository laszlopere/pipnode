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

#ifndef PN_DISK_IO_H
#define PN_DISK_IO_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDiskIoUnit                                                       */
/*                                                                     */
/*  Output unit for #PnDiskIo's emitted throughput.  The four values   */
/*  cover the decimal (SI) byte multiples used by disk vendors and    */
/*  tools like iostat / dd, from a raw byte count up to the GByte/sec */
/*  range modern NVMe devices saturate at:                            */
/*    B/sec        raw bytes per second                                */
/*    KByte/sec    10^3  bytes per second                              */
/*    MByte/sec    10^6  bytes per second                              */
/*    GByte/sec    10^9  bytes per second  (the default)              */
/*  The property value is what the node divides bytes/sec by before    */
/*  emitting data.value; data.unit on the wire carries the matching   */
/*  short string so a downstream PnGraph axis label can pick it up    */
/*  verbatim.                                                         */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DISK_IO_UNIT (pn_disk_io_unit_get_type ())

typedef enum
{
    PN_DISK_IO_UNIT_BYTE,
    PN_DISK_IO_UNIT_KBYTE,
    PN_DISK_IO_UNIT_MBYTE,
    PN_DISK_IO_UNIT_GBYTE,
} PnDiskIoUnit;

GType pn_disk_io_unit_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnDiskIo                                                           */
/*                                                                     */
/*  Auto-trigger node that samples /proc/diskstats every               */
/*  #PnAutoTrigger:period seconds and emits the combined disk-I/O      */
/*  throughput as data.value, expressed in GByte/sec (decimal,         */
/*  1 GB = 1 000 000 000 bytes), computed as the per-second delta      */
/*  of the kernel's cumulative sector counters between two samples.    */
/*                                                                     */
/*  When #PnDiskIo:hostname is empty or "localhost" the sample is      */
/*  read directly from /proc/diskstats.  Any other value is treated    */
/*  as a remote host and the sample is fetched via passwordless SSH    */
/*  exactly like #PnLoad does.                                         */
/*                                                                     */
/*  #PnDiskIo:device picks which row of /proc/diskstats to measure;    */
/*  an empty string (the default) sums across every whole-disk row in  */
/*  the file (partitions, loop, ram, sr, fd and dm-* are skipped so a  */
/*  whole disk is not counted twice through its partitions).           */
/*                                                                     */
/*  #PnDiskIo:unit selects the byte multiple data.value is expressed   */
/*  in (see #PnDiskIoUnit above for the four supported values).        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DISK_IO (pn_disk_io_get_type ())

G_DECLARE_FINAL_TYPE (PnDiskIo, pn_disk_io, PN, DISK_IO, PnAutoTrigger)

PnDiskIo *pn_disk_io_new (void);

G_END_DECLS

#endif /* PN_DISK_IO_H */
