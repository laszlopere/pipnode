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

#ifndef PN_NET_IO_H
#define PN_NET_IO_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnNetIoUnit                                                        */
/*                                                                     */
/*  Output unit for #PnNetIo's emitted throughput.  The four values    */
/*  cover the decimal (SI) byte multiples used by network gear and    */
/*  tools like iftop / nload, from a raw byte count up to the         */
/*  GByte/sec range a saturated 10/25/40 GbE link can produce:        */
/*    B/sec        raw bytes per second                                */
/*    KByte/sec    10^3  bytes per second                              */
/*    MByte/sec    10^6  bytes per second  (the default)              */
/*    GByte/sec    10^9  bytes per second                              */
/*  Note that network throughput is conventionally reported in *bits*  */
/*  (Mbps, Gbps) by switches and link-layer tools, while host-side     */
/*  tools that read /proc/net/dev report *bytes*.  This node follows  */
/*  the byte convention to stay symmetric with #PnDiskIo and to       */
/*  match the underlying kernel counter; a downstream filter that    */
/*  prefers bits-per-second can multiply by 8.                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NET_IO_UNIT (pn_net_io_unit_get_type ())

typedef enum
{
    PN_NET_IO_UNIT_BYTE,
    PN_NET_IO_UNIT_KBYTE,
    PN_NET_IO_UNIT_MBYTE,
    PN_NET_IO_UNIT_GBYTE,
} PnNetIoUnit;

GType pn_net_io_unit_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnNetIo                                                            */
/*                                                                     */
/*  Auto-trigger node that samples /proc/net/dev every                 */
/*  #PnAutoTrigger:period seconds and emits the combined network      */
/*  traffic volume (RX + TX bytes per second) as data.value in the    */
/*  byte multiple chosen by #PnNetIo:unit, computed as the per-second */
/*  delta of the kernel's cumulative byte counters between two        */
/*  samples.  This is *volume* (bytes), not packet count.             */
/*                                                                     */
/*  When #PnNetIo:hostname is empty or "localhost" the sample is      */
/*  read directly from /proc/net/dev.  Any other value is treated     */
/*  as a remote host and the sample is fetched via passwordless SSH   */
/*  exactly like #PnLoad / #PnDiskIo do.                              */
/*                                                                     */
/*  #PnNetIo:interface picks which row of /proc/net/dev to measure;   */
/*  an empty string (the default) sums across every non-loopback      */
/*  interface in the file (the lo row is skipped so localhost-loop    */
/*  traffic does not inflate the figure).                              */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NET_IO (pn_net_io_get_type ())

G_DECLARE_FINAL_TYPE (PnNetIo, pn_net_io, PN, NET_IO, PnAutoTrigger)

PnNetIo *pn_net_io_new (void);

G_END_DECLS

#endif /* PN_NET_IO_H */
