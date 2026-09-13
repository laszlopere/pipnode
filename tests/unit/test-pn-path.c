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

/* Unit tests for pn_path_expand, the "~" expansion every file-path
 * setting goes through.  HOME is pointed at a throwaway directory before
 * GLib caches it, so the node checks create their files there and never
 * in the real home.  No GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-path.h"
#include "pn-logger.h"
#include "pn-pipe-writer.h"

#include <glib/gstdio.h>
#include <sys/stat.h>

static gchar *fake_home;

/* ---- tests --------------------------------------------------------- */

static void
expect (const gchar *in, const gchar *want)
{
    gchar *got = pn_path_expand (in);

    PN_CHECK_CMPSTR (got, ==, want);
    g_free (got);
}

static void
test_expands_leading_tilde (void)
{
    gchar *sub   = g_build_filename (fake_home, "pipes", "in.fifo", NULL);
    gchar *slash = g_strconcat (fake_home, "/", NULL);

    expect ("~", fake_home);
    expect ("~/", slash);           /* like a shell: the slash stays */
    expect ("~/pipes/in.fifo", sub);

    g_free (slash);
    g_free (sub);
}

static void
test_leaves_other_paths (void)
{
    expect ("/tmp/a.log",   "/tmp/a.log");
    expect ("relative/x",   "relative/x");
    expect ("~user/x",      "~user/x");
    expect ("/a/~/b",       "/a/~/b");
    expect ("~~",           "~~");
    expect ("",             "");
    expect (NULL,           "");
}

static gboolean
is_fifo (const gchar *path)
{
    GStatBuf st;
    return g_stat (path, &st) == 0 && S_ISFIFO (st.st_mode);
}

static void
test_pipe_writer_uses_home (void)
{
    PnNode *node = g_object_new (PN_TYPE_PIPE_WRITER,
                                 "pipe-path", "~/w.fifo", NULL);
    gchar  *real = g_build_filename (fake_home, "w.fifo", NULL);
    gchar  *kept = NULL;
    gint64  deadline = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;

    while (!is_fifo (real) && g_get_monotonic_time () < deadline)
        g_main_context_iteration (NULL, FALSE);

    PN_CHECK (is_fifo (real));
    PN_CHECK_FALSE (pn_node_get_has_error (node));
    /* The setting keeps the ~ form, so the saved worksheet stays portable. */
    g_object_get (node, "pipe-path", &kept, NULL);
    PN_CHECK_CMPSTR (kept, ==, "~/w.fifo");

    g_free (kept);
    g_object_unref (node);
    g_unlink (real);
    g_free (real);
}

static void
test_logger_uses_home (void)
{
    PnNode    *node = g_object_new (PN_TYPE_LOGGER,
                                    "file-path", "~/app.log",
                                    "flush",     TRUE,
                                    NULL);
    PnMessage *msg  = pn_message_new (NULL, "t");
    gchar     *real = g_build_filename (fake_home, "app.log", NULL);
    gchar     *body = NULL;

    pn_message_set_string (msg, "output", "in the home dir");
    pn_node_receive_message (node, msg);

    PN_CHECK (g_file_get_contents (real, &body, NULL, NULL));
    PN_CHECK (body != NULL && g_strstr_len (body, -1, "in the home dir") != NULL);

    g_free (body);
    g_object_unref (msg);
    g_object_unref (node);
    g_unlink (real);
    g_free (real);
}

int
main (int argc, char **argv)
{
    gint rc;

    /* Before anything asks GLib for the home directory. */
    fake_home = g_dir_make_tmp ("pn-path-home-XXXXXX", NULL);
    g_setenv ("HOME", fake_home, TRUE);

    pn_test_init (&argc, &argv, "pn-path");
    pn_test_add ("expands_leading_tilde", test_expands_leading_tilde);
    pn_test_add ("leaves_other_paths",    test_leaves_other_paths);
    pn_test_add ("pipe_writer_uses_home", test_pipe_writer_uses_home);
    pn_test_add ("logger_uses_home",      test_logger_uses_home);
    rc = pn_test_run ();

    g_rmdir (fake_home);
    g_free (fake_home);
    return rc;
}
