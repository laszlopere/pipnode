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

#ifndef PN_SHELL_HOST_H
#define PN_SHELL_HOST_H

#include <glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Shared host-routing helper for the Shell-plugin runner nodes.      */
/*                                                                     */
/*  Every node in the Shell category carries a "host" property whose   */
/*  default is the empty string (run on the local machine).  An empty  */
/*  value is left empty rather than coerced to "localhost", and the    */
/*  settings dialog paints the local machine's name as a grey hint so  */
/*  the user can see where a blank field will actually run.  When the  */
/*  user sets it to anything other than empty/localhost, the node      */
/*  executes its command via passwordless ssh on that host instead of  */
/*  spawning it directly.  The two helpers here keep the local/remote  */
/*  branching out of each node's trigger so they stay readable.        */
/* ------------------------------------------------------------------ */

/** Default value of the "host" property — the empty string, meaning
 *  "run on the local machine" (see pn_shell_host_is_local). */
#define PN_SHELL_HOST_DEFAULT ""

/** Return %TRUE when @host refers to the local machine.  Treats %NULL,
 *  the empty string, "localhost", "127.0.0.1" and "::1" as local; any
 *  other value is treated as remote and routed through ssh. */
gboolean pn_shell_host_is_local (const gchar *host);

/** Build a NULL-terminated argv suitable for `g_spawn_sync` that runs
 *  @base_argv either locally or on @host via ssh.
 *
 *  Local: returns `g_strdupv (base_argv)`.
 *
 *  Remote: a thin wrapper over the shared core builder pn_ssh_build_argv()
 *  (TODO #47.3) with no profile, yielding
 *      ["ssh", "-o", "BatchMode=yes",
 *              "-o", "StrictHostKeyChecking=accept-new",
 *              "-o", "ConnectTimeout=5", host,
 *       base_argv[0], base_argv[1], ..., NULL]
 *
 *  `BatchMode=yes` is the load-bearing flag — it disables every
 *  interactive prompt (password, passphrase, host-key confirmation),
 *  so the only way the connection succeeds is via a pre-installed
 *  passwordless key.  This matches the node's stated contract.  When
 *  item 47.4 threads an SSH-login profile through this path, the
 *  username / port / identity / host-key-policy come from there.
 *
 *  Caller owns the result; free with `g_strfreev`. */
gchar **pn_shell_wrap_argv (const gchar        *host,
                            const gchar *const *base_argv);

G_END_DECLS

#endif /* PN_SHELL_HOST_H */
