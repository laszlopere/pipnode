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

#ifndef PN_TMUX_MONITOR_H
#define PN_TMUX_MONITOR_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTmuxMonitor                                                      */
/*                                                                     */
/*  Auto-trigger node that periodically captures the visible content   */
/*  of a tmux session (locally or on a remote host reachable through   */
/*  passwordless ssh) and emits it on its output port.  The shape is   */
/*  intentionally close to the other Shell-plugin runners              */
/*  (#PnFreeCommand, #PnDfCommand, #PnLxcLsCommand): one inheritable   */
/*  worker thread courtesy of #PnAutoTrigger, a `host` property        */
/*  routed through pn_shell_wrap_argv() for the ssh hop, and a         */
/*  combined-stdout/stderr emission per tick.                          */
/*                                                                     */
/*  Settings dialog extras (recipe borrowed from #PnMeshtastic):       */
/*                                                                     */
/*    - The `tmux-session` row is a #GtkComboBoxText populated from    */
/*      the cached `tmux list-sessions` result the node refreshes      */
/*      whenever `host` changes.                                       */
/*                                                                     */
/*    - A read-only boolean `busy` property tells the host dialog to   */
/*      drop its centred spinner overlay on top of the form for as     */
/*      long as the session enumerator thread is running               */
/*      (#PnNodeDialog watches notify::busy on any node that exposes   */
/*      that property — see lib/pn-node-dialog.c::maybe_install_busy_  */
/*      watcher).                                                      */
/*                                                                     */
/*    - A read-only `last-error` string property feeds a red status    */
/*      row in the per-class tab — empty when the last enumeration     */
/*      succeeded, populated with the failure reason otherwise (no     */
/*      tmux server, ssh down, tmux not installed on the remote, …).   */
/*                                                                     */
/*  A successful tick whose buffer is unchanged since the previous     */
/*  one (empty delta) emits nothing — the node stays silent while the  */
/*  monitored screen is idle.  Failures are always emitted.            */
/*                                                                     */
/*  Emitted message (topic `tmux`):                                    */
/*    data.success - whether capture-pane exited with status 0,        */
/*    data.output  - the captured characters verbatim (the visible     */
/*                   pane plus up to :line-limit scrollback lines),    */
/*    data.host    - the host the capture ran on (the local machine's  */
/*                   real name when the host is empty/localhost), and   */
/*    data.session - the session name the capture targeted.            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TMUX_MONITOR (pn_tmux_monitor_get_type ())

G_DECLARE_FINAL_TYPE (PnTmuxMonitor, pn_tmux_monitor,
                      PN, TMUX_MONITOR, PnAutoTrigger)

PnTmuxMonitor *pn_tmux_monitor_new (void);

G_END_DECLS

#endif /* PN_TMUX_MONITOR_H */
