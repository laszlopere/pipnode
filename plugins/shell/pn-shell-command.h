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

#ifndef PN_SHELL_COMMAND_H
#define PN_SHELL_COMMAND_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnShellCommand                                                     */
/*                                                                     */
/*  Auto-trigger node that runs a one-line shell command once per      */
/*  #PnAutoTrigger:period seconds and emits the result on its output   */
/*  port.  The command is spawned from the inherited worker thread     */
/*  via /bin/sh -c so the GUI never blocks on slow commands.           */
/*                                                                     */
/*  Each tick produces one message with                                */
/*    data.success  - whether the command exited with status 0,        */
/*    data.output   - the combined stdout/stderr verbatim; JSON's      */
/*                    own string escaping renders embedded newlines    */
/*                    as `\n` when the message is serialised, so the   */
/*                    value survives as one logical string without a   */
/*                    redundant producer-side escape pass.             */
/*                                                                     */
/*  While #PnShellCommand:shell-command is empty the node switches to  */
/*  a red body with a warning glyph and skips its triggers entirely.   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SHELL_COMMAND (pn_shell_command_get_type ())

G_DECLARE_FINAL_TYPE (PnShellCommand, pn_shell_command,
                      PN, SHELL_COMMAND, PnAutoTrigger)

PnShellCommand *pn_shell_command_new (void);

G_END_DECLS

#endif /* PN_SHELL_COMMAND_H */
