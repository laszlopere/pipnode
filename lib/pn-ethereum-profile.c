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

#include "pn-ethereum-profile.h"
#include "pn-profile-schema.h"

void
pn_ethereum_register_profile_type (PnNodeFactory *factory)
{
    PnProfileSchema *schema;

    g_return_if_fail (PN_IS_NODE_FACTORY (factory));

    schema = pn_profile_schema_new (PN_PROFILE_TYPE_ETHEREUM_ADDRESS,
                                    "Ethereum Address");

    /* One field.  The profile's own name — the entry at the top of each
     * card in the Credentials dialog — is the other half of the pair, so
     * the type is literally a named address: "Payout wallet" ->
     * 0x1234...  Deliberately NOT a secret: an account address is public,
     * and masking it would only make the address book unreadable.
     *
     * Required, because a nameless-but-empty entry grants nothing: a
     * profile with no address is an incomplete row the dialog flags. */
    pn_profile_schema_field (schema, "address", "Address", PN_FIELD_STRING);
    pn_profile_schema_field_set_required (schema, "address", TRUE);
    pn_profile_schema_field_tooltip (schema, "address",
            "The account address, 0x followed by 40 hexadecimal digits. "
            "Public information — it is the same on every EVM chain "
            "(Ethereum, PulseChain, Base, ...). No private key is stored "
            "here: pipnode cannot sign transactions.");

    pn_profile_schema_set_help_page (schema, "EthereumAddressProfile.html");

    pn_node_factory_register_profile_type (factory, schema);
}
