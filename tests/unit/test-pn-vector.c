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

/* Unit tests for PnVector — the refcounted, immutable contiguous double
 * buffer that backs a "$pnvector" marker (TODO #43.1).  Pins:
 *
 *   1. new_take ADOPTS a malloc'd buffer (no copy): len + values match.
 *   2. new_copy holds an INDEPENDENT copy: mutating the source afterwards
 *      does not change the vector.
 *   3. An empty vector (len 0) normalises its buffer to NULL — get_data is
 *      NULL iff the length is 0 — via both the take and copy constructors.
 *   4. The buffer is shared by reference: a g_object_ref'd alias sees the
 *      very same backing pointer, and the data outlives the original ref. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-vector.h"

static void
test_new_take_adopts (void)
{
    gdouble *buf = g_new (gdouble, 4);
    PnVector *v;
    const gdouble *data;

    buf[0] = 1.0; buf[1] = 2.5; buf[2] = -3.0; buf[3] = 1e300;

    v = pn_vector_new_take (buf, 4);          /* adopts buf, no copy */
    PN_CHECK_CMPINT (pn_vector_get_len (v), ==, 4);

    data = pn_vector_get_data (v);
    PN_CHECK (data == buf);                   /* same pointer: truly adopted */
    PN_CHECK_NEAR (data[0],  1.0,   0.0);
    PN_CHECK_NEAR (data[1],  2.5,   0.0);
    PN_CHECK_NEAR (data[2], -3.0,   0.0);
    PN_CHECK_NEAR (data[3],  1e300, 0.0);

    g_object_unref (v);                       /* frees buf via g_free */
}

static void
test_new_copy_is_independent (void)
{
    gdouble  src[3] = { 10.0, 20.0, 30.0 };
    PnVector *v     = pn_vector_new_copy (src, 3);
    const gdouble *data;

    PN_CHECK_CMPINT (pn_vector_get_len (v), ==, 3);

    data = pn_vector_get_data (v);
    PN_CHECK (data != src);                   /* a private copy, not aliased */

    /* Mutating the source after the copy must not reach into the vector. */
    src[1] = 999.0;
    PN_CHECK_NEAR (data[0], 10.0, 0.0);
    PN_CHECK_NEAR (data[1], 20.0, 0.0);       /* still the copied value */
    PN_CHECK_NEAR (data[2], 30.0, 0.0);

    g_object_unref (v);
}

static void
test_empty_take (void)
{
    PnVector *v = pn_vector_new_take (NULL, 0);

    PN_CHECK_CMPINT (pn_vector_get_len (v), ==, 0);
    PN_CHECK (pn_vector_get_data (v) == NULL);   /* NULL iff empty */

    g_object_unref (v);
}

static void
test_empty_copy (void)
{
    PnVector *v = pn_vector_new_copy (NULL, 0);

    PN_CHECK_CMPINT (pn_vector_get_len (v), ==, 0);
    PN_CHECK (pn_vector_get_data (v) == NULL);

    g_object_unref (v);
}

static void
test_ref_shares_buffer (void)
{
    gdouble *buf = g_new (gdouble, 2);
    PnVector *v, *alias;
    const gdouble *data;

    buf[0] = 7.0; buf[1] = 8.0;
    v = pn_vector_new_take (buf, 2);

    alias = g_object_ref (v);                 /* fan-out share: no copy */
    PN_CHECK (pn_vector_get_data (alias) == pn_vector_get_data (v));
    PN_CHECK_CMPINT (pn_vector_get_len (alias), ==, 2);

    /* Drop the original; the buffer must outlive it through the alias. */
    g_object_unref (v);
    data = pn_vector_get_data (alias);
    PN_CHECK_NEAR (data[0], 7.0, 0.0);
    PN_CHECK_NEAR (data[1], 8.0, 0.0);

    g_object_unref (alias);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-vector");

    pn_test_add ("new_take_adopts",        test_new_take_adopts);
    pn_test_add ("new_copy_independent",   test_new_copy_is_independent);
    pn_test_add ("empty_take",             test_empty_take);
    pn_test_add ("empty_copy",             test_empty_copy);
    pn_test_add ("ref_shares_buffer",      test_ref_shares_buffer);

    return pn_test_run ();
}
