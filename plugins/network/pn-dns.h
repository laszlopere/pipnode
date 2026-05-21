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

#ifndef PN_DNS_H
#define PN_DNS_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDns                                                              */
/*                                                                     */
/*  Auto-trigger node that asks a DNS server to resolve a list of      */
/*  names once per #PnAutoTrigger:period seconds and emits an          */
/*  aggregate result.  When #PnDns:server is empty the system          */
/*  resolver is queried (i.e. dig with no @server argument), which     */
/*  is what most users want when they just need to verify that         */
/*  outbound DNS works.                                                */
/*                                                                     */
/*  The probe shells out to the system `dig` utility on the worker     */
/*  thread inherited from #PnAutoTrigger so the GUI never blocks on    */
/*  slow or unreachable servers.                                       */
/*                                                                     */
/*  While #PnDns:names is empty the node enters its "configuration     */
/*  required" state: red body, warning glyph, triggers skipped.        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DNS (pn_dns_get_type ())

G_DECLARE_FINAL_TYPE (PnDns, pn_dns, PN, DNS, PnAutoTrigger)

PnDns *pn_dns_new (void);

G_END_DECLS

#endif /* PN_DNS_H */
