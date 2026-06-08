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

G_END_DECLS

#endif /* PN_SSH_PROFILE_H */
