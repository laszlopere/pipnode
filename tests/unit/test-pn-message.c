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
#include "pn-vector.h"

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

/* ---- large numeric vectors (TODO #43) --------------------------------
 *
 * A vector lives out-of-band as a #PnVector, registered on the message and
 * referenced from the data bag by a self-describing "$pnvector" marker.
 * These cases pin the registry (set/resolve), the zero-copy fan-out on
 * clone, and the serialize/deserialize boundary in both modes. */

static PnVector *
make_ramp (gsize len)
{
    gdouble *buf = g_new (gdouble, len);
    gsize    i;
    for (i = 0; i < len; i++)
        buf[i] = (gdouble) i * 0.5 - 1.0;       /* -1.0, -0.5, 0.0, ... */
    return pn_vector_new_take (buf, len);
}

static void
test_set_writes_marker (void)
{
    g_autoptr (PnMessage)  m   = pn_message_new (NULL, "test");
    g_autoptr (PnVector)   vec = make_ramp (8);
    guint64                handle;
    JsonNode              *marker;
    JsonObject            *obj;

    handle = pn_message_set_vector (m, "samples", vec);
    PN_CHECK_CMPINT (handle, >=, 1);             /* 0 is the no-handle sentinel */

    /* The data bag now carries a { "$pnvector":h, "len":N, "dtype":"f64" }
     * marker object — a sane descriptor even to a vector-blind consumer. */
    marker = pn_message_get_member (m, "samples");
    PN_CHECK (marker != NULL && JSON_NODE_HOLDS_OBJECT (marker));
    obj = json_node_get_object (marker);
    PN_CHECK (json_object_has_member (obj, PN_MESSAGE_VECTOR_MARKER));
    PN_CHECK_CMPINT (json_object_get_int_member (obj, PN_MESSAGE_VECTOR_MARKER),
                     ==, (gint64) handle);
    PN_CHECK_CMPINT (json_object_get_int_member (obj, "len"), ==, 8);
    PN_CHECK_CMPSTR (json_object_get_string_member (obj, "dtype"), ==, "f64");
}

static void
test_resolve_identity (void)
{
    g_autoptr (PnMessage)  m   = pn_message_new (NULL, "test");
    g_autoptr (PnVector)   vec = make_ramp (4);
    PnVector              *got;

    pn_message_set_vector (m, "samples", vec);

    /* resolve_vector returns the very same buffer object we registered. */
    got = pn_message_resolve_vector (m, pn_message_get_member (m, "samples"));
    PN_CHECK (got == vec);
    PN_CHECK_CMPINT (pn_vector_get_len (got), ==, 4);

    /* A non-marker member resolves to NULL, never crashes. */
    pn_message_set_double (m, "scalar", 3.0);
    PN_CHECK (pn_message_resolve_vector (
                  m, pn_message_get_member (m, "scalar")) == NULL);
    PN_CHECK (pn_message_resolve_vector (m, NULL) == NULL);
}

static void
test_clone_shares_buffer (void)
{
    g_autoptr (PnMessage)  m     = pn_message_new (NULL, "test");
    g_autoptr (PnVector)   vec   = make_ramp (6);
    g_autoptr (PnMessage)  clone = NULL;
    PnVector              *a, *b;

    pn_message_set_vector (m, "samples", vec);
    clone = pn_message_clone (m);

    /* The clone resolves the SAME PnVector — fan-out is a g_object_ref,
     * not a megabyte memcpy (the whole point of the out-of-band buffer). */
    a = pn_message_resolve_vector (m,     pn_message_get_member (m,     "samples"));
    b = pn_message_resolve_vector (clone, pn_message_get_member (clone, "samples"));
    PN_CHECK (a == vec);
    PN_CHECK (b == a);
    PN_CHECK (pn_vector_get_data (b) == pn_vector_get_data (a));
}

static void
test_serialize_descriptor_only (void)
{
    g_autoptr (PnMessage)  m   = pn_message_new (NULL, "test");
    g_autoptr (PnVector)   vec = make_ramp (16);
    g_autofree gchar      *json = NULL;
    g_autoptr (PnMessage)  back = NULL;
    g_autoptr (GError)     err  = NULL;

    pn_message_set_vector (m, "samples", vec);

    /* include_blobs = FALSE: the marker survives as a descriptor but the
     * payload bytes are NOT externalised (no "blobs" section). */
    json = pn_message_serialize (m, FALSE);
    PN_CHECK (json != NULL);
    PN_CHECK (strstr (json, PN_MESSAGE_VECTOR_MARKER) != NULL);
    PN_CHECK (strstr (json, "\"blobs\"") == NULL);

    /* Rehydrating a descriptor-only envelope leaves a dangling marker:
     * the member is there, but resolve finds no registered buffer. */
    back = pn_message_deserialize (json, &err);
    PN_CHECK (back != NULL);
    PN_CHECK (err == NULL);
    PN_CHECK (pn_message_get_member (back, "samples") != NULL);
    PN_CHECK (pn_message_resolve_vector (
                  back, pn_message_get_member (back, "samples")) == NULL);
}

static void
test_serialize_full_roundtrip (void)
{
    g_autoptr (PnMessage)  m    = pn_message_new (NULL, "waves");
    g_autoptr (PnVector)   vec  = make_ramp (32);
    g_autofree gchar      *json = NULL;
    g_autoptr (PnMessage)  back = NULL;
    g_autoptr (GError)     err  = NULL;
    PnVector              *got;
    const gdouble         *src, *dst;
    gsize                  i;

    pn_message_set_id (m, "abc-123");
    pn_message_set_vector (m, "samples", vec);

    /* include_blobs = TRUE: the buffer is base64'd into a "blobs" sibling
     * so deserialize can rebuild it byte-identically. */
    json = pn_message_serialize (m, TRUE);
    PN_CHECK (strstr (json, "\"blobs\"") != NULL);

    back = pn_message_deserialize (json, &err);
    PN_CHECK (back != NULL);
    PN_CHECK (err == NULL);
    PN_CHECK_CMPSTR (pn_message_get_topic (back), ==, "waves");
    PN_CHECK_CMPSTR (pn_message_get_id (back),    ==, "abc-123");

    got = pn_message_resolve_vector (
              back, pn_message_get_member (back, "samples"));
    PN_CHECK (got != NULL);
    PN_CHECK_CMPINT (pn_vector_get_len (got), ==, 32);

    src = pn_vector_get_data (vec);
    dst = pn_vector_get_data (got);
    for (i = 0; i < 32; i++)
        PN_CHECK_NEAR (dst[i], src[i], 0.0);     /* bit-exact, not approx */
}

static void
test_deserialize_truncated_blob_fails (void)
{
    /* A blob whose decoded byte count does not match len*8 must fail
     * loudly (PN_MESSAGE_ERROR), never silently hand back a short buffer. */
    const gchar *bad =
        "{ \"data\": { \"samples\": "
        "{ \"$pnvector\": 1, \"len\": 4, \"dtype\": \"f64\" } }, "
        "\"blobs\": { \"1\": "
        "{ \"dtype\": \"f64\", \"len\": 4, \"b64\": \"AAAA\" } } }";
    g_autoptr (PnMessage) back = NULL;
    g_autoptr (GError)    err  = NULL;

    back = pn_message_deserialize (bad, &err);
    PN_CHECK (back == NULL);
    PN_CHECK (err != NULL);
    PN_CHECK_CMPINT (err->domain, ==, PN_MESSAGE_ERROR);
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

    pn_test_add ("vec_set_writes_marker",     test_set_writes_marker);
    pn_test_add ("vec_resolve_identity",      test_resolve_identity);
    pn_test_add ("vec_clone_shares_buffer",   test_clone_shares_buffer);
    pn_test_add ("vec_serialize_descriptor",  test_serialize_descriptor_only);
    pn_test_add ("vec_serialize_roundtrip",   test_serialize_full_roundtrip);
    pn_test_add ("vec_truncated_blob_fails",  test_deserialize_truncated_blob_fails);

    return pn_test_run ();
}
