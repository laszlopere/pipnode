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

/* Characterization test for the "ethereum-address" profile schema.  The
 * type is declared in core but consumed by an out-of-tree plugin, which
 * resolves the address by looking the exact field name up in a provisioned
 * profile — so the type id, the field name, and the fact that there is
 * exactly ONE field (no key material: pipnode cannot hold an Ethereum
 * private key) are a load-bearing contract this test pins down. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ethereum-profile.h"
#include "pn-node-factory.h"
#include "pn-profile-schema.h"

static PnProfileSchema *
ethereum_schema (void)
{
    PnNodeFactory *factory = pn_node_factory_get_default ();

    /* The default factory already registers this in its built-in pass;
     * registering again is an idempotent no-op (the duplicate ref is
     * dropped), and keeps the test self-contained regardless of that
     * ordering. */
    pn_ethereum_register_profile_type (factory);

    return pn_node_factory_lookup_profile_type (factory,
                                                PN_PROFILE_TYPE_ETHEREUM_ADDRESS);
}

static void
test_registered (void)
{
    PnProfileSchema *s = ethereum_schema ();

    PN_CHECK (s != NULL);
    if (s == NULL)
        return;

    PN_CHECK_CMPSTR (pn_profile_schema_get_type_id (s), ==, "ethereum-address");
    PN_CHECK_CMPSTR (pn_profile_schema_get_display_name (s),
                     ==, "Ethereum Address");
}

/* One field, a plain string: the profile's own name is the other half of
 * the name/address pair the user asked for. */
static void
test_fields (void)
{
    PnProfileSchema *s = ethereum_schema ();

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    PN_CHECK_CMPINT (pn_profile_schema_get_n_fields (s), ==, 1);
    PN_CHECK_CMPSTR (pn_profile_schema_field_name (s, 0), ==, "address");
    PN_CHECK_CMPSTR (pn_profile_schema_field_get_label (s, 0), ==, "Address");
    PN_CHECK (pn_profile_schema_field_get_kind (s, 0) == PN_FIELD_STRING);
    PN_CHECK (pn_profile_schema_field_get_required (s, 0));
}

/* The point of the type in this release: it stores public data only.  No
 * field may be a secret — pipnode has no Ethereum key support yet, so a
 * private-key field would promise storage it cannot honour.  Flip this
 * test when (and only when) key handling lands. */
static void
test_no_secret_fields (void)
{
    PnProfileSchema *s = ethereum_schema ();
    guint            i, n;

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    n = pn_profile_schema_get_n_fields (s);
    for (i = 0; i < n; i++)
        PN_CHECK (pn_profile_schema_field_get_kind (s, i) != PN_FIELD_SECRET);
}

/* Every field carries hover help: the credentials manager applies it as the
 * label + editor tooltip, so a missing one leaves a widget unexplained. */
static void
test_field_tooltips (void)
{
    PnProfileSchema *s = ethereum_schema ();
    guint            i, n;

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    n = pn_profile_schema_get_n_fields (s);
    for (i = 0; i < n; i++)
    {
        const gchar *tip = pn_profile_schema_field_get_tooltip (s, i);
        PN_CHECK (tip != NULL && *tip != '\0');
    }
}

/* The Help button on the Credentials dialog's Ethereum page is wired to
 * this page name. */
static void
test_help_page (void)
{
    PnProfileSchema *s = ethereum_schema ();

    if (s == NULL)
    {
        PN_CHECK (s != NULL);
        return;
    }

    PN_CHECK_CMPSTR (pn_profile_schema_get_help_page (s),
                     ==, "EthereumAddressProfile.html");
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-ethereum-profile");
    pn_test_add ("registered",       test_registered);
    pn_test_add ("fields",           test_fields);
    pn_test_add ("no_secret_fields", test_no_secret_fields);
    pn_test_add ("field_tooltips",   test_field_tooltips);
    pn_test_add ("help_page",        test_help_page);
    return pn_test_run ();
}
