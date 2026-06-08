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

#ifndef PN_CONNECTIONS_H
#define PN_CONNECTIONS_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnConnections                                                      */
/*                                                                     */
/*  Auto-trigger node that counts the host's TCP sockets once per      */
/*  #PnAutoTrigger:period seconds and emits the total as data.value    */
/*  (with the v4/v6 split alongside).                                  */
/*                                                                     */
/*  Like #PnLoad, an empty or "localhost" #PnConnections:hostname      */
/*  reads /proc/net/tcp and /proc/net/tcp6 directly.  Any other        */
/*  value is treated as a passwordless-SSH target and the count is     */
/*  fetched via `wc -l /proc/net/tcp /proc/net/tcp6` over ssh on a     */
/*  worker thread, so an unreachable host cannot freeze the GUI.      */
/* ------------------------------------------------------------------ */

#define PN_TYPE_CONNECTIONS (pn_connections_get_type ())

G_DECLARE_FINAL_TYPE (PnConnections, pn_connections,
                      PN, CONNECTIONS, PnAutoTrigger)

PnConnections *pn_connections_new (void);

/* ------------------------------------------------------------------ */
/*  Pure parse seam (no I/O, no GTK)                                    */
/*                                                                     */
/*  Pull the leading non-negative integer out of the counting script's */
/*  stdout, skipping leading whitespace, into @out_value.  Returns     */
/*  FALSE when the buffer holds nothing parseable (NULL, empty, or     */
/*  error chatter with no leading digit).  Exposed (non-static) purely */
/*  so the headless unit tests can drive the parse on canned text      */
/*  without spawning the counting subprocess; the node remains the     */
/*  only production caller.                                            */
/* ------------------------------------------------------------------ */

gboolean pn_connections_parse_count (const gchar *text, guint *out_value);

G_END_DECLS

#endif /* PN_CONNECTIONS_H */
