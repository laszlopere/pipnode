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

/* Unit tests for the "is this serial port already open by another
 * process" guard (pn_mesh_tty_in_use / pn_mesh_path_held_by_pid).  This
 * is what stops the Meshtastic dialog from opening a Zigbee dongle that
 * the generic CP2102 VID:PID match mistook for a Heltec -- TODO #33.
 *
 * The detection walks /proc/<pid>/fd, so the tests exercise it against
 * our OWN open descriptors (deterministic, no fork, no real serial port):
 *   - pn_mesh_path_held_by_pid(getpid(), p) is TRUE for a file we hold
 *     open and FALSE once we close it;
 *   - pn_mesh_tty_in_use() deliberately SKIPS our own pid, so a file only
 *     we hold reads as "not in use", and a bogus path reads the same. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <glib/gstdio.h>
#include <unistd.h>

#include "pntest.h"
#include "pn-mesh-discover.h"

/* A descriptor we hold open is detected against our own pid, and stops
 * being detected the moment we close it. */
static void
test_own_fd_detected (void)
{
    gchar *path = g_strdup ("/tmp/pn-mesh-inuse-XXXXXX");
    int    fd   = g_mkstemp (path);   /* creates AND opens the file */

    PN_CHECK (fd >= 0);
    PN_CHECK (pn_mesh_path_held_by_pid (getpid (), path) == TRUE);

    close (fd);
    PN_CHECK (pn_mesh_path_held_by_pid (getpid (), path) == FALSE);

    g_unlink (path);
    g_free (path);
}

/* A path nobody has open is reported not-in-use, and the holder-out
 * parameter is left NULL. */
static void
test_unheld_path (void)
{
    gchar *holder = (gchar *) 0x1;   /* poison: must be overwritten to NULL */

    PN_CHECK (pn_mesh_tty_in_use ("/dev/pn-mesh-does-not-exist", &holder)
              == FALSE);
    PN_CHECK (holder == NULL);

    /* NULL tty / NULL holder-out are tolerated. */
    PN_CHECK (pn_mesh_tty_in_use (NULL, NULL) == FALSE);
    PN_CHECK (pn_mesh_path_held_by_pid (getpid (), NULL) == FALSE);
}

/* pn_mesh_tty_in_use() must skip OUR pid: a port only we hold is ours to
 * use, not "in use" by someone else.  (This is why the lower-level
 * pn_mesh_path_held_by_pid exists for the own-fd test above.) */
static void
test_self_is_skipped (void)
{
    gchar *path   = g_strdup ("/tmp/pn-mesh-inuse2-XXXXXX");
    int    fd     = g_mkstemp (path);
    gchar *holder = NULL;

    PN_CHECK (fd >= 0);
    /* We hold it, but in_use only counts OTHER processes. */
    PN_CHECK (pn_mesh_tty_in_use (path, &holder) == FALSE);
    PN_CHECK (holder == NULL);

    close (fd);
    g_unlink (path);
    g_free (path);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mesh-inuse");
    pn_test_add ("own_fd_detected", test_own_fd_detected);
    pn_test_add ("unheld_path",     test_unheld_path);
    pn_test_add ("self_is_skipped", test_self_is_skipped);
    return pn_test_run ();
}
