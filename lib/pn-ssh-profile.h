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

#ifndef PN_SSH_PROFILE_H
#define PN_SSH_PROFILE_H

#include "pn-node-factory.h"
#include "pn-vault.h"

G_BEGIN_DECLS

/* The host-provisioned credential profile type for an SSH login: a
 * username / port / identity-file / passphrase / password / host-key-policy
 * bundle that says HOW to log into a remote host, kept apart from the
 * in-node "host" field that says WHERE.  Two bundled plugins reference it —
 * the shell plugin (Shell Command, df, free, lxc-ls, tmux-monitor) and the
 * host-monitoring plugin (CPU, Load, Memory, …) — so the type lives in
 * core, not behind one plugin's libtool BIND_LOCAL boundary, exactly as the
 * mqtt-broker profile does for the MQTT base class.  A node with no profile
 * keeps today's ambient-key behaviour. */
#define PN_PROFILE_SSH "ssh-login"

/**
 * pn_ssh_register_profile_type:
 * @factory: the process-wide factory
 *
 * Registers the "ssh-login" profile type with @factory.  Called once from
 * the host's built-in registration pass (alongside
 * #pn_mqtt_register_profile_type) so the type is visible to both the shell
 * and host-monitoring plugins before any SSH-using node is constructed.
 * Idempotent: re-registering is a no-op.
 */
void pn_ssh_register_profile_type (PnNodeFactory *factory);

/**
 * pn_ssh_build_argv:
 * @host:            the remote host to reach (passed verbatim as the ssh
 *                   destination; the caller has already decided it is NOT
 *                   the local machine — see pn_shell_host_is_local)
 * @profile:         (nullable): a resolved "ssh-login" #PnProfile whose
 *                   username / port / identity-file / host-key-policy
 *                   fields shape the login, or %NULL to keep today's
 *                   ambient-key behaviour (no -l/-p/-i, accept-new policy)
 * @connect_timeout: ConnectTimeout seconds; 0 falls back to 5
 * @base_argv:       (nullable): the NULL-terminated remote command, taken
 *                   verbatim as the ssh tail (the caller owns its quoting)
 *
 * The one shared SSH argv builder (TODO #47.3).  Turns @host plus the
 * resolved @profile into the exact `ssh` argv every SSH-using node runs:
 *
 *     ssh -o BatchMode=yes -o StrictHostKeyChecking=<policy>
 *         -o ConnectTimeout=<n> [-l user] [-p port] [-i identity]
 *         <host> <base_argv...>
 *
 * The host-key-policy field maps onto OpenSSH's StrictHostKeyChecking:
 * accept-new → accept-new (today's default, trust on first use),
 * strict → yes (refuse an unknown or changed key), off → no (accept
 * anything).  A %NULL @profile, or a profile with the field unset, yields
 * accept-new.  Username, port (when not the default 22) and identity-file
 * are only added when the profile sets them, so a %NULL profile reproduces
 * the fixed argv the nodes hard-coded before this type existed.
 *
 * BatchMode=yes is kept unconditionally: it disables every interactive
 * prompt, so the connection only succeeds via a pre-installed key or the
 * agent.  The profile's passphrase / password secrets are therefore NOT
 * consumed here — feeding them needs a non-interactive path decided in
 * item 47.5; until then they are ignored.
 *
 * There is no local-host short-circuit: this always builds a remote ssh
 * argv.  The caller keeps its own local/remote test and only routes the
 * remote branch through here.
 *
 * Returns: (transfer full): a NULL-terminated argv suitable for
 *   g_spawn_sync() with %G_SPAWN_SEARCH_PATH.  An empty @base_argv yields
 *   an empty (argv[0] == NULL) vector.  Free with g_strfreev().
 */
gchar **pn_ssh_build_argv (const gchar        *host,
                           PnProfile          *profile,
                           guint               connect_timeout,
                           const gchar *const *base_argv);

G_END_DECLS

#endif /* PN_SSH_PROFILE_H */
