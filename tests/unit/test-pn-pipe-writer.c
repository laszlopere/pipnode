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

/* Unit tests for PnPipeWriter, the named-pipe sink.  Each test works on
 * a FIFO in a throwaway temp directory and plays the reading process
 * itself with a plain O_RDONLY|O_NONBLOCK descriptor (or a Pipe Reader
 * node for the round trip).  Every receive must return at once, reader
 * or not; the main context is spun (bounded) only where queued bytes
 * are expected to drain.  No GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-pipe-writer.h"
#include "pn-pipe-reader.h"

#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- helpers ------------------------------------------------------- */

static PnMessage *
make_message (const gchar *output)
{
    PnMessage *msg = pn_message_new (NULL, "pipe/test");

    pn_message_set_double  (msg, "value",   7.0);
    pn_message_set_string  (msg, "output",  output);
    pn_message_set_boolean (msg, "success", TRUE);
    return msg;
}

static void
send_text (PnNode *node, const gchar *output)
{
    PnMessage *msg = make_message (output);

    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

/* Everything currently readable from @fd, without blocking. */
static gchar *
drain_fd (gint fd)
{
    GString *out = g_string_new (NULL);
    gchar    buf[65536];
    gssize   n;

    while ((n = read (fd, buf, sizeof buf)) > 0)
        g_string_append_len (out, buf, n);
    return g_string_free (out, FALSE);
}

typedef struct
{
    gchar  *dir;
    gchar  *path;
    PnNode *node;
} Fixture;

static void
fixture_setup (Fixture *f, PnPipeFormat format)
{
    f->dir  = g_dir_make_tmp ("pn-pipe-writer-XXXXXX", NULL);
    f->path = g_build_filename (f->dir, "out.fifo", NULL);
    f->node = g_object_new (PN_TYPE_PIPE_WRITER,
                            "pipe-path", f->path,
                            "format",    format,
                            NULL);
}

static void
fixture_teardown (Fixture *f)
{
    g_object_unref (f->node);
    g_unlink (f->path);
    g_rmdir (f->dir);
    g_free (f->path);
    g_free (f->dir);
}

static gboolean
spin_until (gboolean (*done) (gpointer), gpointer data)
{
    gint64 deadline = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;

    while (!done (data))
    {
        if (g_get_monotonic_time () > deadline)
            return FALSE;
        g_main_context_iteration (NULL, FALSE);
        g_usleep (1000);
    }
    return TRUE;
}

static gboolean
is_fifo_cb (gpointer data)
{
    GStatBuf st;
    return g_stat (data, &st) == 0 && S_ISFIFO (st.st_mode);
}

/* ---- tests --------------------------------------------------------- */

static void
test_no_reader_drops (void)
{
    Fixture f;
    gint64  t0;

    fixture_setup (&f, PN_PIPE_FORMAT_OUTPUT);

    /* The FIFO is created up front, for an outside reader to open. */
    PN_CHECK (spin_until (is_fifo_cb, f.path));

    t0 = g_get_monotonic_time ();
    send_text (f.node, "nobody listens");
    send_text (f.node, "still nobody");
    /* Returned immediately: nothing waited for a reader. */
    PN_CHECK_CMPINT (g_get_monotonic_time () - t0, <, G_USEC_PER_SEC / 10);

    PN_CHECK_FALSE (pn_pipe_writer_is_open (PN_PIPE_WRITER (f.node)));
    PN_CHECK_CMPINT (pn_pipe_writer_get_dropped (PN_PIPE_WRITER (f.node)), ==, 2);
    /* No reader is a normal condition, not an error. */
    PN_CHECK_FALSE (pn_node_get_has_error (f.node));

    fixture_teardown (&f);
}

static void
test_output_lines (void)
{
    Fixture    f;
    PnMessage *no_output = pn_message_new (NULL, NULL);
    gint       rd;
    gchar     *got;

    fixture_setup (&f, PN_PIPE_FORMAT_OUTPUT);
    PN_CHECK (spin_until (is_fifo_cb, f.path));

    rd = open (f.path, O_RDONLY | O_NONBLOCK);
    PN_CHECK (rd >= 0);

    send_text (f.node, "alpha");
    send_text (f.node, "beta");
    pn_message_set_double (no_output, "value", 1.0);
    pn_node_receive_message (f.node, no_output);

    got = drain_fd (rd);
    PN_CHECK_CMPSTR (got, ==, "alpha\nbeta\n\n");
    PN_CHECK (pn_pipe_writer_is_open (PN_PIPE_WRITER (f.node)));
    PN_CHECK_CMPINT (pn_pipe_writer_get_dropped (PN_PIPE_WRITER (f.node)), ==, 0);

    g_free (got);
    close (rd);
    g_object_unref (no_output);
    fixture_teardown (&f);
}

static void
test_json_lines (void)
{
    Fixture    f;
    PnMessage *msg = make_message ("hello");
    PnMessage *back;
    gchar     *got;
    gchar    **lines;
    gint       rd;

    fixture_setup (&f, PN_PIPE_FORMAT_JSON);
    PN_CHECK (spin_until (is_fifo_cb, f.path));
    rd = open (f.path, O_RDONLY | O_NONBLOCK);

    pn_node_receive_message (f.node, msg);

    got   = drain_fd (rd);
    lines = g_strsplit (got, "\n", -1);
    /* Exactly one line, then the terminator. */
    PN_CHECK_CMPINT (g_strv_length (lines), ==, 2);
    PN_CHECK_CMPSTR (lines[1], ==, "");

    back = pn_message_deserialize (lines[0], NULL);
    PN_CHECK (back != NULL);
    if (back != NULL)
    {
        PN_CHECK_CMPSTR (pn_message_get_topic (back), ==, "pipe/test");
        PN_CHECK_CMPSTR (pn_test_str (back, "output"), ==, "hello");
        PN_CHECK_NEAR (pn_test_num (back, "value"), 7.0, 1e-9);
        g_object_unref (back);
    }

    g_strfreev (lines);
    g_free (got);
    close (rd);
    g_object_unref (msg);
    fixture_teardown (&f);
}

static void
test_reader_reconnects (void)
{
    Fixture f;
    gint    rd;
    gchar  *got;

    fixture_setup (&f, PN_PIPE_FORMAT_OUTPUT);
    PN_CHECK (spin_until (is_fifo_cb, f.path));

    rd = open (f.path, O_RDONLY | O_NONBLOCK);
    send_text (f.node, "one");
    got = drain_fd (rd);
    PN_CHECK_CMPSTR (got, ==, "one\n");
    g_free (got);
    close (rd);

    /* A new reader arrives after the old one left: the next message
     * must reach it (and must not kill the process with SIGPIPE). */
    rd = open (f.path, O_RDONLY | O_NONBLOCK);
    send_text (f.node, "two");
    got = drain_fd (rd);
    PN_CHECK_CMPSTR (got, ==, "two\n");
    g_free (got);
    close (rd);

    /* Reader gone and none back: dropped, still alive, not an error. */
    send_text (f.node, "three");
    PN_CHECK_FALSE (pn_node_get_has_error (f.node));
    PN_CHECK_FALSE (pn_pipe_writer_is_open (PN_PIPE_WRITER (f.node)));

    fixture_teardown (&f);
}

static gboolean
queue_empty_cb (gpointer data)
{
    return pn_pipe_writer_get_queued (PN_PIPE_WRITER (data)) == 0;
}

static void
test_slow_reader_queues_and_drains (void)
{
    Fixture  f;
    gchar   *big = g_strnfill (100000, 'x');
    GString *all = g_string_new (NULL);
    gint64   t0;
    guint    sent = 0;
    gint     rd;
    gint64   deadline;

    fixture_setup (&f, PN_PIPE_FORMAT_OUTPUT);
    PN_CHECK (spin_until (is_fifo_cb, f.path));
    rd = open (f.path, O_RDONLY | O_NONBLOCK);

    /* Nobody reads: the kernel buffer fills, then the in-memory queue,
     * then whole messages are dropped.  None of it may block. */
    t0 = g_get_monotonic_time ();
    while (pn_pipe_writer_get_dropped (PN_PIPE_WRITER (f.node)) == 0 &&
           sent < 100)
    {
        send_text (f.node, big);
        sent++;
    }
    PN_CHECK_CMPINT (g_get_monotonic_time () - t0, <, G_USEC_PER_SEC);
    PN_CHECK_CMPINT (pn_pipe_writer_get_dropped (PN_PIPE_WRITER (f.node)), >, 0);
    PN_CHECK_CMPINT (pn_pipe_writer_get_queued (PN_PIPE_WRITER (f.node)), >, 0);
    PN_CHECK_CMPINT (pn_pipe_writer_get_queued (PN_PIPE_WRITER (f.node)), <=,
                     PN_PIPE_MAX_BUFFER);

    /* Now read: the fd watch drains the queue. */
    deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
    while (!queue_empty_cb (f.node) && g_get_monotonic_time () < deadline)
    {
        gchar *chunk = drain_fd (rd);

        g_string_append (all, chunk);
        g_free (chunk);
        g_main_context_iteration (NULL, FALSE);
    }
    {
        gchar *chunk = drain_fd (rd);
        g_string_append (all, chunk);
        g_free (chunk);
    }
    PN_CHECK (queue_empty_cb (f.node));

    /* Only whole lines came through. */
    PN_CHECK_CMPINT (all->len % 100001, ==, 0);
    PN_CHECK_CMPINT (all->len / 100001, ==,
                     sent - pn_pipe_writer_get_dropped (PN_PIPE_WRITER (f.node)));

    g_string_free (all, TRUE);
    g_free (big);
    close (rd);
    fixture_teardown (&f);
}

static void
test_refuses_regular_file (void)
{
    gchar  *dir  = g_dir_make_tmp ("pn-pipe-writer-XXXXXX", NULL);
    gchar  *path = g_build_filename (dir, "plain.txt", NULL);
    PnNode *node;
    gchar  *body = NULL;

    g_file_set_contents (path, "keep me\n", -1, NULL);
    node = g_object_new (PN_TYPE_PIPE_WRITER, "pipe-path", path, NULL);

    send_text (node, "must not land in the file");
    PN_CHECK (pn_node_get_has_error (node));
    PN_CHECK_FALSE (pn_pipe_writer_is_open (PN_PIPE_WRITER (node)));
    PN_CHECK (g_file_get_contents (path, &body, NULL, NULL));
    PN_CHECK_CMPSTR (body, ==, "keep me\n");

    g_free (body);
    g_object_unref (node);
    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

static void
test_error_state_tracks_path (void)
{
    PnNode *node = g_object_new (PN_TYPE_PIPE_WRITER, NULL);

    PN_CHECK (pn_node_get_has_error (node));
    g_object_set (node, "pipe-path", "/tmp/whatever.fifo", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (node));
    g_object_set (node, "pipe-path", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    /* Unconfigured receive is a quiet no-op. */
    send_text (node, "ignored");

    g_object_unref (node);
}

/* Writer node -> FIFO -> Reader node, in JSON mode: the message arrives
 * with its topic, id and data intact. */

typedef struct { PnMessage *got; } Catch;

static void
catch_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Catch *c = user_data;

    (void) node;
    if (c->got == NULL)
        c->got = g_object_ref (message);
}

static gboolean
reader_open_cb (gpointer data)
{
    return pn_pipe_reader_is_open (PN_PIPE_READER (data));
}

static gboolean
caught_cb (gpointer data)
{
    return ((Catch *) data)->got != NULL;
}

static void
test_round_trip_json (void)
{
    gchar     *dir    = g_dir_make_tmp ("pn-pipe-writer-XXXXXX", NULL);
    gchar     *path   = g_build_filename (dir, "rt.fifo", NULL);
    PnNode    *reader = g_object_new (PN_TYPE_PIPE_READER,
                                      "pipe-path", path,
                                      "format",    PN_PIPE_FORMAT_JSON,
                                      NULL);
    PnNode    *writer = g_object_new (PN_TYPE_PIPE_WRITER,
                                      "pipe-path", path,
                                      "format",    PN_PIPE_FORMAT_JSON,
                                      NULL);
    PnMessage *msg    = make_message ("across the pipe");
    Catch      c      = { NULL };

    g_signal_connect (reader, "message", G_CALLBACK (catch_cb), &c);
    PN_CHECK (spin_until (reader_open_cb, reader));

    pn_message_set_string (msg, "extra", "kept");
    pn_node_receive_message (writer, msg);

    PN_CHECK (spin_until (caught_cb, &c));
    if (c.got != NULL)
    {
        PN_CHECK_CMPSTR (pn_message_get_topic (c.got), ==, "pipe/test");
        PN_CHECK_CMPSTR (pn_message_get_id (c.got), ==, pn_message_get_id (msg));
        PN_CHECK_CMPSTR (pn_test_str (c.got, "output"), ==, "across the pipe");
        PN_CHECK_CMPSTR (pn_test_str (c.got, "extra"), ==, "kept");
        g_object_unref (c.got);
    }

    g_object_unref (msg);
    g_object_unref (writer);
    g_object_unref (reader);
    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-pipe-writer");
    pn_test_add ("no_reader_drops",         test_no_reader_drops);
    pn_test_add ("output_lines",            test_output_lines);
    pn_test_add ("json_lines",              test_json_lines);
    pn_test_add ("reader_reconnects",       test_reader_reconnects);
    pn_test_add ("slow_reader_queues",      test_slow_reader_queues_and_drains);
    pn_test_add ("refuses_regular_file",    test_refuses_regular_file);
    pn_test_add ("error_state_tracks_path", test_error_state_tracks_path);
    pn_test_add ("round_trip_json",         test_round_trip_json);
    return pn_test_run ();
}
