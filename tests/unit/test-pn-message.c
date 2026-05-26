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

/* Unit tests for the PnMessage validation API — the contract documented
 * in data/help/help-index.html that every wire-bound message must carry
 * data.value, data.output and data.success of the right types.  Pins:
 *
 *   1. A fully-populated message passes pn_message_validate().
 *   2. Each of the three mandatory members, when missing, is reported
 *      with PN_MESSAGE_ERROR_MISSING_FIELD and named in the error text.
 *   3. A member of the wrong JSON type is reported with
 *      PN_MESSAGE_ERROR_WRONG_TYPE.
 *   4. pn_message_is_valid() agrees with pn_message_validate(). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-message.h"

static PnMessage *
make_well_formed (void)
{
    PnMessage *m = pn_message_new (NULL, "test");
    pn_message_set_double  (m, "value",   42.0);
    pn_message_set_string  (m, "output",  "ok");
    pn_message_set_boolean (m, "success", TRUE);
    return m;
}

static void
test_well_formed_passes (void)
{
    g_autoptr (PnMessage)  m   = make_well_formed ();
    g_autoptr (GError)     err = NULL;

    PN_CHECK (pn_message_validate (m, &err));
    PN_CHECK (err == NULL);
    PN_CHECK (pn_message_is_valid (m));
}

static void
test_numeric_string_value_passes (void)
{
    /* The help docs explicitly allow data.value to be a numeric string
     * for over-range producers (hex-encoded JSON-RPC quantities,
     * arbitrary-precision integers).  Pin that. */
    g_autoptr (PnMessage)  m   = pn_message_new (NULL, "test");
    g_autoptr (GError)     err = NULL;

    pn_message_set_string  (m, "value",   "0xdeadbeef");
    pn_message_set_string  (m, "output",  "ok");
    pn_message_set_boolean (m, "success", TRUE);

    PN_CHECK (pn_message_validate (m, &err));
    PN_CHECK (err == NULL);
}

static void
check_missing (const gchar *to_remove)
{
    g_autoptr (PnMessage)  m   = make_well_formed ();
    g_autoptr (GError)     err = NULL;
    JsonObject            *bag = pn_message_get_data (m);

    json_object_remove_member (bag, to_remove);

    PN_CHECK_FALSE (pn_message_validate (m, &err));
    PN_CHECK (err != NULL);
    PN_CHECK_CMPINT (err->domain, ==, PN_MESSAGE_ERROR);
    PN_CHECK_CMPINT (err->code,   ==, PN_MESSAGE_ERROR_MISSING_FIELD);
    PN_CHECK (strstr (err->message, to_remove) != NULL);
    PN_CHECK_FALSE (pn_message_is_valid (m));
}

static void test_missing_value   (void) { check_missing ("value");   }
static void test_missing_output  (void) { check_missing ("output");  }
static void test_missing_success (void) { check_missing ("success"); }

static void
test_wrong_type_value (void)
{
    /* data.value as a boolean is not numeric and not a numeric string. */
    g_autoptr (PnMessage)  m   = pn_message_new (NULL, "test");
    g_autoptr (GError)     err = NULL;

    pn_message_set_boolean (m, "value",   TRUE);
    pn_message_set_string  (m, "output",  "ok");
    pn_message_set_boolean (m, "success", TRUE);

    PN_CHECK_FALSE (pn_message_validate (m, &err));
    PN_CHECK (err != NULL);
    PN_CHECK_CMPINT (err->code, ==, PN_MESSAGE_ERROR_WRONG_TYPE);
}

static void
test_wrong_type_output (void)
{
    g_autoptr (PnMessage)  m   = pn_message_new (NULL, "test");
    g_autoptr (GError)     err = NULL;

    pn_message_set_double  (m, "value",   1.0);
    pn_message_set_double  (m, "output",  3.14);
    pn_message_set_boolean (m, "success", TRUE);

    PN_CHECK_FALSE (pn_message_validate (m, &err));
    PN_CHECK (err != NULL);
    PN_CHECK_CMPINT (err->code, ==, PN_MESSAGE_ERROR_WRONG_TYPE);
}

static void
test_wrong_type_success (void)
{
    g_autoptr (PnMessage)  m   = pn_message_new (NULL, "test");
    g_autoptr (GError)     err = NULL;

    pn_message_set_double (m, "value",   1.0);
    pn_message_set_string (m, "output",  "ok");
    pn_message_set_string (m, "success", "true");

    PN_CHECK_FALSE (pn_message_validate (m, &err));
    PN_CHECK (err != NULL);
    PN_CHECK_CMPINT (err->code, ==, PN_MESSAGE_ERROR_WRONG_TYPE);
}

static void
test_has_member (void)
{
    g_autoptr (PnMessage) m = pn_message_new (NULL, "test");

    PN_CHECK_FALSE (pn_message_has_member (m, "value"));
    pn_message_set_double (m, "value", 0.0);
    PN_CHECK (pn_message_has_member (m, "value"));
    PN_CHECK_FALSE (pn_message_has_member (m, "nope"));
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-message");

    pn_test_add ("well_formed_passes",        test_well_formed_passes);
    pn_test_add ("numeric_string_value",      test_numeric_string_value_passes);
    pn_test_add ("missing_value",             test_missing_value);
    pn_test_add ("missing_output",            test_missing_output);
    pn_test_add ("missing_success",           test_missing_success);
    pn_test_add ("wrong_type_value",          test_wrong_type_value);
    pn_test_add ("wrong_type_output",         test_wrong_type_output);
    pn_test_add ("wrong_type_success",        test_wrong_type_success);
    pn_test_add ("has_member",                test_has_member);

    return pn_test_run ();
}
