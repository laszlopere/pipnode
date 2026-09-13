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

/* Unit tests for PnPipeReader, the named-pipe source.  Each test works
 * on a FIFO in a throwaway temp directory, plays the writing process
 * itself with a plain O_WRONLY descriptor, and spins the default main
 * context (bounded) so the node's idle open and fd watch run.  No GUI. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-pipe-reader.h"

#include <glib/gstdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- helpers ------------------------------------------------------- */

typedef struct
{
    GPtrArray *messages;   /* PnMessage*, owned */
} Recorder;

static void
record_cb (PnNode *node, PnMessage *message, gpointer user_data)
{
    Recorder *rec = user_data;

    (void) node;
    g_ptr_array_add (rec->messages, g_object_ref (message));
}

/* Spin the main context until @done says so or ~2 s pass. */
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
reader_open_cb (gpointer data)
{
    return pn_pipe_reader_is_open (PN_PIPE_READER (data));
}

typedef struct { Recorder *rec; guint want; } WantCount;

static gboolean
count_cb (gpointer data)
{
    WantCount *w = data;
    return w->rec->messages->len >= w->want;
}

static gboolean
wait_messages (Recorder *rec, guint want)
{
    WantCount w = { rec, want };
    return spin_until (count_cb, &w);
}

/* Spin a fixed short while, for "nothing should happen" checks. */
static void
spin_briefly (void)
{
    gint i;

    for (i = 0; i < 50; i++)
    {
        g_main_context_iteration (NULL, FALSE);
        g_usleep (1000);
    }
}

static PnMessage *
nth (Recorder *rec, guint i)
{
    return i < rec->messages->len ? g_ptr_array_index (rec->messages, i)
                                  : NULL;
}

static void
write_all (gint fd, const gchar *text)
{
    gssize n = write (fd, text, strlen (text));
    PN_CHECK_CMPINT (n, ==, (gssize) strlen (text));
}

typedef struct
{
    gchar        *dir;
    gchar        *path;
    PnNode       *node;
    Recorder      rec;
} Fixture;

static void
fixture_setup (Fixture *f, PnPipeFormat format)
{
    f->dir  = g_dir_make_tmp ("pn-pipe-reader-XXXXXX", NULL);
    f->path = g_build_filename (f->dir, "in.fifo", NULL);
    f->rec.messages = g_ptr_array_new_with_free_func (g_object_unref);
    f->node = g_object_new (PN_TYPE_PIPE_READER,
                            "pipe-path", f->path,
                            "format",    format,
                            NULL);
    g_signal_connect (f->node, "message", G_CALLBACK (record_cb), &f->rec);
}

static void
fixture_teardown (Fixture *f)
{
    g_object_unref (f->node);
    g_ptr_array_unref (f->rec.messages);
    g_unlink (f->path);
    g_rmdir (f->dir);
    g_free (f->path);
    g_free (f->dir);
}

/* ---- tests --------------------------------------------------------- */

static void
test_creates_fifo (void)
{
    Fixture  f;
    GStatBuf st;

    fixture_setup (&f, PN_PIPE_FORMAT_OUTPUT);

    PN_CHECK (spin_until (reader_open_cb, f.node));
    PN_CHECK (g_stat (f.path, &st) == 0 && S_ISFIFO (st.st_mode));
    PN_CHECK_FALSE (pn_node_get_has_error (f.node));
    /* A successful open is silent. */
    PN_CHECK_CMPINT (f.rec.messages->len, ==, 0);

    fixture_teardown (&f);
}

static void
test_output_lines (void)
{
    Fixture f;
    gint    fd;

    fixture_setup (&f, PN_PIPE_FORMAT_OUTPUT);
    PN_CHECK (spin_until (reader_open_cb, f.node));

    fd = open (f.path, O_WRONLY | O_NONBLOCK);
    PN_CHECK (fd >= 0);

    write_all (fd, "hello world\n 42.5 \r\npart");
    PN_CHECK (wait_messages (&f.rec, 2));
    PN_CHECK_CMPSTR (pn_test_str (nth (&f.rec, 0), "output"), ==, "hello world");
    PN_CHECK_NEAR (pn_test_num (nth (&f.rec, 0), "value"), 0.0, 1e-9);
    PN_CHECK (pn_test_bool (nth (&f.rec, 0), "success"));
    /* CR stripped; a numeric line carries its number as value. */
    PN_CHECK_CMPSTR (pn_test_str (nth (&f.rec, 1), "output"), ==, " 42.5 ");
    PN_CHECK_NEAR (pn_test_num (nth (&f.rec, 1), "value"), 42.5, 1e-9);
    PN_CHECK (pn_message_get_source (nth (&f.rec, 1)) == f.node);

    /* An unterminated tail waits for its newline. */
    spin_briefly ();
    PN_CHECK_CMPINT (f.rec.messages->len, ==, 2);
    write_all (fd, "ial\n");
    PN_CHECK (wait_messages (&f.rec, 3));
    PN_CHECK_CMPSTR (pn_test_str (nth (&f.rec, 2), "output"), ==, "partial");

    close (fd);
    fixture_teardown (&f);
}

static void
test_writers_come_and_go (void)
{
    Fixture f;
    gint    fd;

    fixture_setup (&f, PN_PIPE_FORMAT_OUTPUT);
    PN_CHECK (spin_until (reader_open_cb, f.node));

    fd = open (f.path, O_WRONLY | O_NONBLOCK);
    write_all (fd, "first\n");
    close (fd);
    PN_CHECK (wait_messages (&f.rec, 1));

    /* The writer's close is not an end-of-file for the node. */
    spin_briefly ();
    PN_CHECK (pn_pipe_reader_is_open (PN_PIPE_READER (f.node)));
    PN_CHECK_FALSE (pn_node_get_has_error (f.node));
    PN_CHECK_CMPINT (f.rec.messages->len, ==, 1);

    fd = open (f.path, O_WRONLY | O_NONBLOCK);
    PN_CHECK (fd >= 0);
    write_all (fd, "second\n");
    close (fd);
    PN_CHECK (wait_messages (&f.rec, 2));
    PN_CHECK_CMPSTR (pn_test_str (nth (&f.rec, 1), "output"), ==, "second");

    fixture_teardown (&f);
}

static void
test_json_messages (void)
{
    Fixture    f;
    PnMessage *src;
    gchar     *json;
    gchar     *text;
    gint       fd;

    fixture_setup (&f, PN_PIPE_FORMAT_JSON);
    PN_CHECK (spin_until (reader_open_cb, f.node));

    src = pn_message_new (NULL, "home/kitchen");
    pn_message_set_double  (src, "value",   21.5);
    pn_message_set_string  (src, "output",  "21.5 C");
    pn_message_set_boolean (src, "success", TRUE);
    pn_message_set_string  (src, "room",    "kitchen");
    json = pn_message_serialize (src, TRUE);

    text = g_strconcat (json, "\n\n{\"output\":\"bare\"}\nnot json\n", NULL);
    fd = open (f.path, O_WRONLY | O_NONBLOCK);
    write_all (fd, text);
    close (fd);

    /* Blank line skipped: envelope, bare object, failure. */
    PN_CHECK (wait_messages (&f.rec, 3));
    spin_briefly ();
    PN_CHECK_CMPINT (f.rec.messages->len, ==, 3);

    PN_CHECK_CMPSTR (pn_message_get_topic (nth (&f.rec, 0)), ==, "home/kitchen");
    PN_CHECK_CMPSTR (pn_message_get_id (nth (&f.rec, 0)), ==,
                     pn_message_get_id (src));
    PN_CHECK_CMPSTR (pn_test_str (nth (&f.rec, 0), "room"), ==, "kitchen");
    PN_CHECK_NEAR (pn_test_num (nth (&f.rec, 0), "value"), 21.5, 1e-9);
    PN_CHECK (pn_message_get_source (nth (&f.rec, 0)) == f.node);

    PN_CHECK_CMPSTR (pn_test_str (nth (&f.rec, 1), "output"), ==, "bare");
    /* No topic in a bare object: the node's own topic is stamped. */
    PN_CHECK (pn_message_get_topic (nth (&f.rec, 1)) != NULL &&
              *pn_message_get_topic (nth (&f.rec, 1)) != '\0');

    PN_CHECK_FALSE (pn_test_bool (nth (&f.rec, 2), "success"));
    PN_CHECK (strstr (pn_test_str (nth (&f.rec, 2), "output"),
                      "invalid JSON") != NULL);

    g_free (text);
    g_free (json);
    g_object_unref (src);
    fixture_teardown (&f);
}

static void
test_refuses_regular_file (void)
{
    gchar    *dir  = g_dir_make_tmp ("pn-pipe-reader-XXXXXX", NULL);
    gchar    *path = g_build_filename (dir, "plain.txt", NULL);
    Recorder  rec  = { g_ptr_array_new_with_free_func (g_object_unref) };
    PnNode   *node;
    gchar    *body = NULL;

    g_file_set_contents (path, "keep me\n", -1, NULL);

    node = g_object_new (PN_TYPE_PIPE_READER, "pipe-path", path, NULL);
    g_signal_connect (node, "message", G_CALLBACK (record_cb), &rec);

    PN_CHECK (wait_messages (&rec, 1));
    PN_CHECK_FALSE (pn_pipe_reader_is_open (PN_PIPE_READER (node)));
    PN_CHECK (pn_node_get_has_error (node));
    PN_CHECK_FALSE (pn_test_bool (nth (&rec, 0), "success"));
    PN_CHECK (strstr (pn_test_str (nth (&rec, 0), "output"),
                      "not a named pipe") != NULL);

    /* The file is left untouched. */
    PN_CHECK (g_file_get_contents (path, &body, NULL, NULL));
    PN_CHECK_CMPSTR (body, ==, "keep me\n");

    g_free (body);
    g_object_unref (node);
    g_ptr_array_unref (rec.messages);
    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

static void
test_error_state_tracks_path (void)
{
    PnNode *node = g_object_new (PN_TYPE_PIPE_READER, NULL);
    gchar  *dir  = g_dir_make_tmp ("pn-pipe-reader-XXXXXX", NULL);
    gchar  *path = g_build_filename (dir, "p.fifo", NULL);

    PN_CHECK (pn_node_get_has_error (node));

    g_object_set (node, "pipe-path", path, NULL);
    PN_CHECK (spin_until (reader_open_cb, node));
    PN_CHECK_FALSE (pn_node_get_has_error (node));

    g_object_set (node, "pipe-path", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));
    PN_CHECK_FALSE (pn_pipe_reader_is_open (PN_PIPE_READER (node)));

    g_object_unref (node);
    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-pipe-reader");
    pn_test_add ("creates_fifo",            test_creates_fifo);
    pn_test_add ("output_lines",            test_output_lines);
    pn_test_add ("writers_come_and_go",     test_writers_come_and_go);
    pn_test_add ("json_messages",           test_json_messages);
    pn_test_add ("refuses_regular_file",    test_refuses_regular_file);
    pn_test_add ("error_state_tracks_path", test_error_state_tracks_path);
    return pn_test_run ();
}
