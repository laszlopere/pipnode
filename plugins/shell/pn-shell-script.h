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

#ifndef PN_SHELL_SCRIPT_H
#define PN_SHELL_SCRIPT_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnShellScript                                                      */
/*                                                                     */
/*  The multi-line sibling of #PnShellCommand: an auto-trigger node    */
/*  that runs a whole shell SCRIPT once per #PnAutoTrigger:period      */
/*  seconds and emits the result on its output port.  The script is    */
/*  fed to `/bin/sh -c` verbatim from the inherited worker thread (or  */
/*  handed to a remote login shell over ssh), so it can span many      */
/*  lines, define functions, loop, and pipe exactly as a saved `.sh`   */
/*  would — the GUI never blocks on a slow script.                     */
/*                                                                     */
/*  Logically identical to #PnShellCommand; the only differences are   */
/*  cosmetic (its own icon / palette name) and that the settings       */
/*  dialog presents the body in a full-width GtkSourceView code        */
/*  editor with `sh` syntax highlighting, declared through the         */
/*  GTK-free #PnSettingsSchema so the node stays headless.             */
/*                                                                     */
/*  Each tick produces one message with                                */
/*    data.success  - whether the script exited with status 0,         */
/*    data.output   - the combined stdout/stderr verbatim.             */
/*                                                                     */
/*  While #PnShellScript:shell-script is empty the node switches to a  */
/*  red body with a warning glyph and skips its triggers entirely.     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SHELL_SCRIPT (pn_shell_script_get_type ())

G_DECLARE_FINAL_TYPE (PnShellScript, pn_shell_script,
                      PN, SHELL_SCRIPT, PnAutoTrigger)

PnShellScript *pn_shell_script_new (void);

G_END_DECLS

#endif /* PN_SHELL_SCRIPT_H */
