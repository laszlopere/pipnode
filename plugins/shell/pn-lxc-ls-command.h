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

#ifndef PN_LXC_LS_COMMAND_H
#define PN_LXC_LS_COMMAND_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnLxcLsCommand                                                     */
/*                                                                     */
/*  Auto-trigger node that runs `sudo lxc-ls -f` once per              */
/*  #PnAutoTrigger:period seconds and emits a message carrying         */
/*    data.success - whether the command exited with status 0,         */
/*    data.output  - the combined stdout/stderr verbatim, and          */
/*    data.table   - the parsed table in PnTableModel's shape so a     */
/*                   downstream PnTableView renders it without an      */
/*                   intermediate filter.                              */
/*                                                                     */
/*  `lxc-ls -f` produces fixed-width whitespace-padded output with     */
/*  single-token headers (default: NAME, STATE, AUTOSTART, GROUPS,     */
/*  IPV4, IPV6, UNPRIVILEGED) so the parser is a plain whitespace      */
/*  split per line, with the column count derived dynamically from     */
/*  the header so a future `lxc-ls` version that adds or removes a    */
/*  column needs no code change.  Trailing tokens past the last        */
/*  header column are folded into the last cell so a wider-than-       */
/*  expected last column doesn't spawn a phantom extra column.         */
/*                                                                     */
/*  Each cell carries an extra `name` member alongside `text`, shaped  */
/*  <row>.<column> -- the column id comes from the sanitised header    */
/*  word ("IPV4" -> "ipv4") and the row id from the sanitised first-  */
/*  column value (the container NAME, "dns2" -> "dns2"); the header    */
/*  row's row id is the literal "header".                              */
/*                                                                     */
/*  Note: `sudo` is invoked verbatim, so the host user must have       */
/*  passwordless sudo configured for `lxc-ls` (e.g. via a             */
/*  /etc/sudoers.d entry) -- otherwise the spawn succeeds but sudo    */
/*  exits non-zero and the emitted data.success is false.              */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LXC_LS_COMMAND (pn_lxc_ls_command_get_type ())

G_DECLARE_FINAL_TYPE (PnLxcLsCommand, pn_lxc_ls_command,
                      PN, LXC_LS_COMMAND, PnAutoTrigger)

PnLxcLsCommand *pn_lxc_ls_command_new (void);

G_END_DECLS

#endif /* PN_LXC_LS_COMMAND_H */
