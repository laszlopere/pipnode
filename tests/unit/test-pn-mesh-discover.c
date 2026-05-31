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

/* Characterization tests for the shared Meshtastic USB discovery
 * (pn-mesh-discover.c).  TODO #37.3 converged the Meshtastic node's
 * private sysfs walk + known-device table onto this one canonical scan,
 * so the granular coverage the node test used to carry for its now-deleted
 * find_tty_under() / known_devices[] moved here, repointed at the surviving
 * implementation: KNOWN_DEVICES, match_known_device() and find_tty_device().
 *
 * Those are static, so -- exactly as test-pn-mesh-node does for the node
 * TU -- we compile pn-mesh-discover.c straight into the test.
 * libpipnode-core also links as a shared object; the test's own copies of
 * the module's few exported symbols win at link time and we only ever
 * exercise the static helpers.
 *
 * Headless: the tty walk runs against a synthetic /sys-like tree under a
 * tmpdir.  find_tty_device() never touches /dev (it returns the /dev path
 * by name without an existence check), so the leaves are plain dirs. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib/gstdio.h>

#include "pntest.h"

/* The discovery translation unit, static helpers and all. */
#include "pn-mesh-discover.c"

/* ================================================================== */
/*  Known-device table                                                 */
/* ================================================================== */

/* Pin the VID:PID:kind table the dialog and (since #37.3) the node both
 * key off, and that #33's bare-CP2102 / #36's ModemManager-ignore work
 * builds on.  A board added to the pip-mesh prototype but forgotten here
 * silently drops out of discovery, so the exact set is the contract. */
static void
test_known_devices_table (void)
{
    static const struct { const gchar *vid, *pid, *kind; } expect[] = {
        { "10c4", "ea60", "Heltec V3"        },
        { "239a", "0029", "Tracker"          },
        { "239a", "8027", "SenseCAP Tracker" },
        { "239a", "8029", "Tracker"          },
    };

    PN_CHECK_CMPINT (G_N_ELEMENTS (KNOWN_DEVICES), ==, G_N_ELEMENTS (expect));
    for (gsize i = 0; i < G_N_ELEMENTS (expect); i++)
    {
        PN_CHECK_CMPSTR (KNOWN_DEVICES[i].vendor_id,  ==, expect[i].vid);
        PN_CHECK_CMPSTR (KNOWN_DEVICES[i].product_id, ==, expect[i].pid);
        PN_CHECK_CMPSTR (KNOWN_DEVICES[i].kind,       ==, expect[i].kind);
    }
}

/* match_known_device() returns the kind on a hit (case-insensitively, as
 * sysfs may print mixed case on some boards) and NULL on a miss or NULL
 * input. */
static void
test_match_known_device (void)
{
    PN_CHECK_CMPSTR (match_known_device ("10c4", "ea60"), ==, "Heltec V3");
    PN_CHECK_CMPSTR (match_known_device ("239a", "8027"), ==, "SenseCAP Tracker");
    /* Case-insensitive on both fields. */
    PN_CHECK_CMPSTR (match_known_device ("10C4", "EA60"), ==, "Heltec V3");

    PN_CHECK (match_known_device ("1234", "5678") == NULL);  /* unknown */
    PN_CHECK (match_known_device ("10c4", "0000") == NULL);  /* VID-only */
    PN_CHECK (match_known_device (NULL, "ea60")   == NULL);
    PN_CHECK (match_known_device ("10c4", NULL)   == NULL);
}

/* ================================================================== */
/*  tty resolution: the two-deep sysfs walk                            */
/* ================================================================== */

/* Build the directory chain @rel under @root (slash-separated). */
static void
make_dirs (const gchar *root, const gchar *rel)
{
    gchar *full = g_build_filename (root, rel, NULL);
    g_mkdir_with_parents (full, 0755);
    g_free (full);
}

/* Best-effort recursive cleanup of a synthetic tree. */
static void
rmtree (const gchar *root)
{
    gchar *rm = g_strdup_printf ("rm -rf %s", root);
    if (system (rm) != 0) { /* ignore */ }
    g_free (rm);
}

/* find_tty_device() only descends interfaces named "<busnum>:*", where
 * busnum is the basename of the USB device path.  Build that exact name
 * from the tmp dir's basename so the walk engages. */
static gchar *
make_iface_dir (const gchar *root, const gchar *suffix)
{
    gchar *base  = g_path_get_basename (root);
    gchar *iface = g_strconcat (base, ":1.0", suffix, NULL);
    g_free (base);
    return iface;   /* e.g. "abc123:1.0/tty/ttyUSB0" */
}

/* The common one-level layout <dev>/<busnum>:1.0/tty/ttyUSB0 resolves to
 * /dev/ttyUSB0 (find_tty_device returns the /dev path by name -- it never
 * stats /dev, so no real node is needed). */
static void
test_find_tty_one_level (void)
{
    gchar *root = g_dir_make_tmp ("pn-mesh-disco-tty-XXXXXX", NULL);
    PN_CHECK (root != NULL);
    if (root == NULL)
        return;

    gchar *rel = make_iface_dir (root, "/tty/ttyUSB0");
    make_dirs (root, rel);
    g_free (rel);

    gchar *tty = find_tty_device (root);
    PN_CHECK_CMPSTR (tty, ==, "/dev/ttyUSB0");
    g_free (tty);

    rmtree (root);
    g_free (root);
}

/* The one-deeper nesting (drivers like SenseCAP expose
 * <iface>/<subdir>/tty/ttyXXX) is also resolved. */
static void
test_find_tty_nested (void)
{
    gchar *root = g_dir_make_tmp ("pn-mesh-disco-tty2-XXXXXX", NULL);
    PN_CHECK (root != NULL);
    if (root == NULL)
        return;

    gchar *rel = make_iface_dir (root, "/subdev/tty/ttyACM0");
    make_dirs (root, rel);
    g_free (rel);

    gchar *tty = find_tty_device (root);
    PN_CHECK_CMPSTR (tty, ==, "/dev/ttyACM0");
    g_free (tty);

    rmtree (root);
    g_free (root);
}

/* An interface that does NOT match the "<busnum>:" prefix is ignored:
 * a stray sibling directory must not be mistaken for an interface. */
static void
test_find_tty_no_interface (void)
{
    gchar *root = g_dir_make_tmp ("pn-mesh-disco-tty3-XXXXXX", NULL);
    PN_CHECK (root != NULL);
    if (root == NULL)
        return;

    /* power/, driver/ etc. -- present on a real USB device node but not
     * an interface; a tty buried under one of them must not be picked. */
    make_dirs (root, "power/tty/ttyBOGUS");

    gchar *tty = find_tty_device (root);
    PN_CHECK (tty == NULL);
    g_free (tty);

    rmtree (root);
    g_free (root);
}

/* ================================================================== */
/*  End-to-end scan contract                                           */
/* ================================================================== */

/* pn_mesh_discover_sync() reads the real /sys, so its contents are
 * machine-dependent, but the contract is not: never NULL, every row has a
 * tty, and the rich fields are at least internally consistent. */
static void
test_discover_sync_contract (void)
{
    GPtrArray *devices = pn_mesh_discover_sync ();
    PN_CHECK (devices != NULL);
    if (devices == NULL)
        return;

    for (guint i = 0; i < devices->len; i++)
    {
        const PnMeshDevice *dev = g_ptr_array_index (devices, i);
        PN_CHECK (dev->tty != NULL);
        PN_CHECK (dev->kind != NULL);
        if (dev->tty != NULL)
            PN_CHECK (g_str_has_prefix (dev->tty, "/dev/"));
    }

    g_ptr_array_unref (devices);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mesh-discover");

    /* Known-device table */
    pn_test_add ("known_devices_table",   test_known_devices_table);
    pn_test_add ("match_known_device",    test_match_known_device);

    /* tty walk */
    pn_test_add ("find_tty_one_level",    test_find_tty_one_level);
    pn_test_add ("find_tty_nested",       test_find_tty_nested);
    pn_test_add ("find_tty_no_interface", test_find_tty_no_interface);

    /* End-to-end scan */
    pn_test_add ("discover_sync_contract", test_discover_sync_contract);

    return pn_test_run ();
}
