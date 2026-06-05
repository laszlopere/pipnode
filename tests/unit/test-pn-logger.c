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

/* Unit tests for PnLogger, the file-writing sink.  Each test logs into
 * a throwaway temp directory and reads the resulting file(s) back; the
 * directory and every file in it are removed on the way out.  The node
 * is driven head-less with pn_node_receive_message(); it never opens a
 * GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-logger.h"

#include <glib/gstdio.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------- */

/* A message carrying data.output and data.success. */
static PnMessage *
make_message (const gchar *output, gboolean success)
{
    PnMessage *msg = pn_message_new (NULL, NULL);

    pn_message_set_topic   (msg, "sensors/x");
    pn_message_set_string  (msg, "output", output);
    pn_message_set_boolean (msg, "success", success);
    return msg;
}

/* Read a whole file into a newly-allocated string, or NULL if absent. */
static gchar *
slurp (const gchar *path)
{
    gchar *out = NULL;
    if (!g_file_get_contents (path, &out, NULL, NULL))
        return NULL;
    return out;
}

/* Remove a temp directory and the log + any rotated archives under it. */
static void
cleanup (const gchar *dir, const gchar *path)
{
    gint i;

    g_unlink (path);
    for (i = 1; i <= 8; i++)
    {
        gchar *arch = g_strdup_printf ("%s.%d", path, i);
        g_unlink (arch);
        g_free (arch);
    }
    g_rmdir (dir);
}

/* ---- tests --------------------------------------------------------- */

static void
test_lines_format (void)
{
    gchar     *dir  = g_dir_make_tmp ("pn-logger-XXXXXX", NULL);
    gchar     *path = g_build_filename (dir, "app.log", NULL);
    PnNode    *node = g_object_new (PN_TYPE_LOGGER,
                                    "file-path", path,
                                    "format",    PN_LOGGER_FORMAT_LINES,
                                    "flush",     TRUE,
                                    NULL);
    PnMessage *ok   = make_message ("pump started", TRUE);
    PnMessage *bad  = make_message ("sensor timeout", FALSE);
    gchar     *body;

    pn_node_receive_message (node, ok);
    pn_node_receive_message (node, bad);

    body = slurp (path);
    PN_CHECK (body != NULL);
    if (body != NULL)
    {
        /* The verdict word and the output text land on the line, in the
         * Lines payload (no envelope JSON). */
        PN_CHECK (strstr (body, " SUCCESS pump started\n")  != NULL);
        PN_CHECK (strstr (body, " FAILURE sensor timeout\n") != NULL);
        PN_CHECK (strstr (body, "{") == NULL);
    }

    g_free (body);
    g_object_unref (ok);
    g_object_unref (bad);
    g_object_unref (node);
    cleanup (dir, path);
    g_free (path);
    g_free (dir);
}

static void
test_json_format (void)
{
    gchar     *dir  = g_dir_make_tmp ("pn-logger-XXXXXX", NULL);
    gchar     *path = g_build_filename (dir, "app.log", NULL);
    PnNode    *node = g_object_new (PN_TYPE_LOGGER,
                                    "file-path", path,
                                    "format",    PN_LOGGER_FORMAT_JSON,
                                    "flush",     TRUE,
                                    NULL);
    PnMessage *msg  = make_message ("ok", TRUE);
    gchar     *body;

    pn_node_receive_message (node, msg);

    body = slurp (path);
    PN_CHECK (body != NULL);
    if (body != NULL)
    {
        /* One line: prefix + a compact one-line JSON envelope. */
        PN_CHECK (strstr (body, " SUCCESS {")  != NULL);
        PN_CHECK (strstr (body, "\"data\"")    != NULL);
        PN_CHECK (strstr (body, "\"output\"")  != NULL);
        /* Compact: exactly one newline (the line terminator) and no
         * pretty-printer indentation inside the object. */
        PN_CHECK_CMPINT (g_strv_length (g_strsplit (body, "\n", -1)), ==, 2);
        PN_CHECK (strstr (body, "\n  ") == NULL);
    }

    g_free (body);
    g_object_unref (msg);
    g_object_unref (node);
    cleanup (dir, path);
    g_free (path);
    g_free (dir);
}

static void
test_flush_writes_immediately (void)
{
    gchar     *dir  = g_dir_make_tmp ("pn-logger-XXXXXX", NULL);
    gchar     *path = g_build_filename (dir, "app.log", NULL);
    PnNode    *node = g_object_new (PN_TYPE_LOGGER,
                                    "file-path", path,
                                    "format",    PN_LOGGER_FORMAT_LINES,
                                    "flush",     TRUE,
                                    NULL);
    PnMessage *msg  = make_message ("now", TRUE);
    gchar     *body;

    pn_node_receive_message (node, msg);

    /* With flush on the bytes are on disk before the node is closed. */
    body = slurp (path);
    PN_CHECK (body != NULL && strstr (body, " SUCCESS now\n") != NULL);

    g_free (body);
    g_object_unref (msg);
    g_object_unref (node);
    cleanup (dir, path);
    g_free (path);
    g_free (dir);
}

static void
test_buffered_flushed_on_close (void)
{
    gchar     *dir  = g_dir_make_tmp ("pn-logger-XXXXXX", NULL);
    gchar     *path = g_build_filename (dir, "app.log", NULL);
    PnNode    *node = g_object_new (PN_TYPE_LOGGER,
                                    "file-path", path,
                                    "format",    PN_LOGGER_FORMAT_LINES,
                                    "flush",     FALSE,   /* buffered */
                                    NULL);
    PnMessage *msg  = make_message ("later", TRUE);
    gchar     *body;

    pn_node_receive_message (node, msg);

    /* Disposing the node closes the stream, flushing the buffer. */
    g_object_unref (node);

    body = slurp (path);
    PN_CHECK (body != NULL && strstr (body, " SUCCESS later\n") != NULL);

    g_free (body);
    g_object_unref (msg);
    cleanup (dir, path);
    g_free (path);
    g_free (dir);
}

static void
test_rotation (void)
{
    gchar     *dir   = g_dir_make_tmp ("pn-logger-XXXXXX", NULL);
    gchar     *path  = g_build_filename (dir, "app.log", NULL);
    PnNode    *node  = g_object_new (PN_TYPE_LOGGER,
                                     "file-path",   path,
                                     "format",      PN_LOGGER_FORMAT_LINES,
                                     "flush",       TRUE,
                                     "logrotate",   TRUE,
                                     "max-size-mb", 1,     /* 1 MB */
                                     "max-files",   2,
                                     NULL);
    /* ~20 KB per line, so a few dozen lines top the 1 MB threshold and
     * a couple hundred messages force several rotations. */
    gchar     *big   = g_strnfill (20000, 'x');
    gchar     *arch1 = g_strdup_printf ("%s.1", path);
    gchar     *arch2 = g_strdup_printf ("%s.2", path);
    gchar     *arch3 = g_strdup_printf ("%s.3", path);
    gint       i;

    for (i = 0; i < 200; i++)
    {
        PnMessage *msg = make_message (big, TRUE);
        pn_node_receive_message (node, msg);
        g_object_unref (msg);
    }

    /* Two archives are kept; the third must never appear. */
    PN_CHECK (g_file_test (arch1, G_FILE_TEST_EXISTS));
    PN_CHECK (g_file_test (arch2, G_FILE_TEST_EXISTS));
    PN_CHECK_FALSE (g_file_test (arch3, G_FILE_TEST_EXISTS));

    g_object_unref (node);
    g_unlink (arch1);
    g_unlink (arch2);
    cleanup (dir, path);
    g_free (arch1);
    g_free (arch2);
    g_free (arch3);
    g_free (big);
    g_free (path);
    g_free (dir);
}

static void
test_error_state_tracks_path (void)
{
    PnNode *node = g_object_new (PN_TYPE_LOGGER, NULL);  /* path "" */

    /* Unconfigured: no log file yet, so the node flags the error state. */
    PN_CHECK (pn_node_get_has_error (node));

    /* Setting a path clears it... */
    g_object_set (node, "file-path", "/tmp/whatever.log", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (node));

    /* ...and clearing the path brings it back. */
    g_object_set (node, "file-path", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    g_object_unref (node);
}

static void
test_empty_path_noop (void)
{
    PnNode    *node = g_object_new (PN_TYPE_LOGGER, NULL);  /* path "" */
    PnMessage *msg  = make_message ("ignored", TRUE);

    /* No file to write to: must be a quiet no-op, not a crash. */
    pn_node_receive_message (node, msg);
    PN_CHECK (TRUE);

    g_object_unref (msg);
    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-logger");
    pn_test_add ("lines_format",            test_lines_format);
    pn_test_add ("json_format",             test_json_format);
    pn_test_add ("flush_immediate",         test_flush_writes_immediately);
    pn_test_add ("buffered_on_close",       test_buffered_flushed_on_close);
    pn_test_add ("rotation",                test_rotation);
    pn_test_add ("error_state_tracks_path", test_error_state_tracks_path);
    pn_test_add ("empty_path_noop",         test_empty_path_noop);
    return pn_test_run ();
}
