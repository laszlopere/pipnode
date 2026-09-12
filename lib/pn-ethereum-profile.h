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

#ifndef PN_ETHEREUM_PROFILE_H
#define PN_ETHEREUM_PROFILE_H

#include "pn-node-factory.h"

G_BEGIN_DECLS

/* The credential profile type for a named Ethereum (EVM) account address:
 * a "unique name -> 0x address" pair, and nothing else.  There is NO key
 * material here — pipnode cannot hold or use an Ethereum private key yet,
 * so this type is a public address book, not a wallet.  Everything it
 * stores is public information; it lives in the vault only so a worksheet
 * carries a reference instead of a hard-coded address, exactly as the
 * broker and login types do.
 *
 * The type is declared in core rather than in a plugin because the
 * Ethereum nodes ship as a SEPARATE, out-of-tree plugin: a core-owned id
 * gives that plugin (and any other EVM plugin) one stable schema to point
 * a profile-ref property at, the same reason "mqtt-broker" and
 * "ssh-login" live here. */
#define PN_PROFILE_TYPE_ETHEREUM_ADDRESS "ethereum-address"

/**
 * pn_ethereum_register_profile_type:
 * @factory: the process-wide factory
 *
 * Registers the "ethereum-address" profile type with @factory.  Called
 * once from the host's built-in registration pass, alongside
 * #pn_mqtt_register_profile_type and #pn_ssh_register_profile_type, so
 * the type appears in the Credentials dialog's type list whether or not
 * any Ethereum plugin is installed.  Idempotent: re-registering is a
 * no-op.
 */
void pn_ethereum_register_profile_type (PnNodeFactory *factory);

G_END_DECLS

#endif /* PN_ETHEREUM_PROFILE_H */
