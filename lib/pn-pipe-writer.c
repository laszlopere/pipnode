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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-pipe-writer.h"
#include "pn-message.h"
#include "pn-path.h"
#include "pn-settings-schema.h"

#include <json-glib/json-glib.h>
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

struct _PnPipeWriter
{
    PnNode parent_instance;

    gchar        *pipe_path;   /* as typed, saved              */
    gchar        *real_path;   /* pipe_path with ~ expanded    */
    PnPipeFormat  format;

    gint     fd;            /* write end, -1 while no reader            */
    guint    watch_id;      /* G_IO_OUT watch while bytes are queued    */
    guint    check_idle_id; /* pending FIFO create/validate             */
    GString *queue;         /* accepted, not yet written bytes          */
    guint64  dropped;
};

G_DEFINE_TYPE (PnPipeWriter, pn_pipe_writer, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_PIPE_PATH,
    PROP_FORMAT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Open / close                                                       */
/* ------------------------------------------------------------------ */

static void
writer_close (PnPipeWriter *self)
{
    if (self->watch_id != 0)
    {
        g_source_remove (self->watch_id);
        self->watch_id = 0;
    }
    if (self->fd >= 0)
    {
        close (self->fd);
        self->fd = -1;
    }
    /* A half-written line must not reach the next reader. */
    g_string_truncate (self->queue, 0);
}

/* Create or validate the FIFO; sets the error state to match. */
static gboolean
writer_check_fifo (PnPipeWriter *self)
{
    GError  *error = NULL;
    gboolean ok    = pn_pipe_ensure_fifo (self->real_path, &error);

    /* No terminal to print to and no output port to report on: the red
     * node is the diagnostic (its tooltip/help names the causes). */
    g_clear_error (&error);
    pn_node_set_has_error (PN_NODE (self), !ok);
    return ok;
}

/* Try to connect to a reader.  FALSE when none is there (ENXIO, the
 * normal case, not an error) or the pipe is unusable (error state). */
static gboolean
writer_open (PnPipeWriter *self)
{
    if (self->fd >= 0)
        return TRUE;

    if (!writer_check_fifo (self))
        return FALSE;

    self->fd = open (self->real_path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (self->fd < 0)
    {
        pn_node_set_has_error (PN_NODE (self), errno != ENXIO);
        return FALSE;
    }
    return TRUE;
}

/* A reader that closed its end leaves POLLERR on ours. */
static gboolean
reader_gone (PnPipeWriter *self)
{
    struct pollfd pfd = { self->fd, POLLOUT, 0 };

    return poll (&pfd, 1, 0) > 0 && (pfd.revents & (POLLERR | POLLHUP)) != 0;
}

/* ------------------------------------------------------------------ */
/*  Writing                                                            */
/* ------------------------------------------------------------------ */

static gboolean on_writable (gint fd, GIOCondition condition, gpointer data);

/* Write as much of the queue as the pipe takes right now. */
static void
writer_flush (PnPipeWriter *self)
{
    while (self->fd >= 0 && self->queue->len > 0)
    {
        gssize n = pn_pipe_write (self->fd, self->queue->str,
                                  self->queue->len);

        if (n > 0)
        {
            g_string_erase (self->queue, 0, n);
            continue;
        }
        if (n < 0 && errno == EAGAIN)
        {
            if (self->watch_id == 0)
                self->watch_id = g_unix_fd_add (self->fd,
                                                G_IO_OUT | G_IO_ERR | G_IO_HUP,
                                                on_writable, self);
            return;
        }

        /* EPIPE (reader went away) or a real error: start over on the
         * next message. */
        writer_close (self);
        return;
    }

    if (self->watch_id != 0)
    {
        g_source_remove (self->watch_id);
        self->watch_id = 0;
    }
}

static gboolean
on_writable (gint fd, GIOCondition condition, gpointer user_data)
{
    PnPipeWriter *self = PN_PIPE_WRITER (user_data);

    (void) fd;

    /* Whatever happens below, this source is finished with unless
     * writer_flush() installs a fresh one. */
    self->watch_id = 0;

    if (condition & (G_IO_ERR | G_IO_HUP))
        writer_close (self);
    else
        writer_flush (self);

    return G_SOURCE_REMOVE;
}

/* The line for @message, newline included. */
static gchar *
format_line (PnPipeWriter *self, PnMessage *message)
{
    gchar *body;
    gchar *line;

    if (self->format == PN_PIPE_FORMAT_JSON)
    {
        body = pn_message_serialize (message, TRUE);
    }
    else
    {
        JsonNode *node = pn_message_get_member (message, "output");

        body = g_strdup (node != NULL && JSON_NODE_HOLDS_VALUE (node) &&
                         json_node_get_value_type (node) == G_TYPE_STRING
                         ? json_node_get_string (node) : "");
    }

    line = g_strconcat (body, "\n", NULL);
    g_free (body);
    return line;
}

static void
pn_pipe_writer_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnPipeWriter *self = PN_PIPE_WRITER (node);
    gchar        *line;
    gsize         len;

    if (self->pipe_path[0] == '\0')
        return;

    /* Notice a reader that left while we were idle, so this message
     * goes to the next reader instead of into the EPIPE. */
    if (self->fd >= 0 && self->queue->len == 0 && reader_gone (self))
        writer_close (self);

    if (!writer_open (self))
    {
        self->dropped++;
        return;
    }

    line = format_line (self, message);
    len  = strlen (line);

    if (self->queue->len + len > PN_PIPE_MAX_BUFFER)
    {
        /* The reader is not keeping up: drop whole messages, never
         * block and never grow without bound. */
        self->dropped++;
    }
    else
    {
        g_string_append_len (self->queue, line, len);
        writer_flush (self);
    }

    g_free (line);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static gboolean
check_idle (gpointer user_data)
{
    PnPipeWriter *self = PN_PIPE_WRITER (user_data);

    self->check_idle_id = 0;
    if (self->pipe_path[0] != '\0')
        writer_check_fifo (self);
    return G_SOURCE_REMOVE;
}

static void
pn_pipe_writer_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnPipeWriter *self = PN_PIPE_WRITER (object);

    switch (prop_id)
    {
    case PROP_PIPE_PATH:
        g_value_set_string (value, self->pipe_path);
        break;
    case PROP_FORMAT:
        g_value_set_enum (value, self->format);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_pipe_writer_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnPipeWriter *self = PN_PIPE_WRITER (object);

    switch (prop_id)
    {
    case PROP_PIPE_PATH:
        {
            const gchar *path = g_value_get_string (value);

            if (path == NULL)
                path = "";
            if (strcmp (self->pipe_path, path) != 0)
            {
                g_free (self->pipe_path);
                self->pipe_path = g_strdup (path);
                g_free (self->real_path);
                self->real_path = pn_path_expand (path);
                writer_close (self);
                pn_node_set_has_error (PN_NODE (self), *path == '\0');
                /* Create the FIFO up front, so an outside reader can
                 * open it before the first message is ever sent. */
                if (*path != '\0' && self->check_idle_id == 0)
                    self->check_idle_id = g_idle_add (check_idle, self);
                g_object_notify_by_pspec (object, props[PROP_PIPE_PATH]);
            }
        }
        break;
    case PROP_FORMAT:
        {
            PnPipeFormat f = g_value_get_enum (value);

            if (self->format != f)
            {
                self->format = f;
                g_object_notify_by_pspec (object, props[PROP_FORMAT]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_pipe_writer_dispose (GObject *object)
{
    PnPipeWriter *self = PN_PIPE_WRITER (object);

    if (self->check_idle_id != 0)
    {
        g_source_remove (self->check_idle_id);
        self->check_idle_id = 0;
    }
    writer_close (self);

    G_OBJECT_CLASS (pn_pipe_writer_parent_class)->dispose (object);
}

static void
pn_pipe_writer_finalize (GObject *object)
{
    PnPipeWriter *self = PN_PIPE_WRITER (object);

    g_free (self->pipe_path);
    g_free (self->real_path);
    g_string_free (self->queue, TRUE);

    G_OBJECT_CLASS (pn_pipe_writer_parent_class)->finalize (object);
}

static void
pn_pipe_writer_class_init (PnPipeWriterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_pipe_writer_get_property;
    object_class->set_property = pn_pipe_writer_set_property;
    object_class->dispose      = pn_pipe_writer_dispose;
    object_class->finalize     = pn_pipe_writer_finalize;
    node_class->receive        = pn_pipe_writer_receive;

    node_class->class_name     = "Pipe Writer";
    node_class->icon           = "\xef\x82\x8b";  /* fa-sign-out U+F08B */
    node_class->color          = (PnColor){ 0.33, 0.58, 0.62, 1.0 };
    node_class->category       = "Sinks";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row (schema, "pipe-path", PN_EDITOR_FILE);
        pn_settings_schema_row (schema, "format",    PN_EDITOR_AUTO);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_PIPE_PATH] = g_param_spec_string (
            "pipe-path", "Named pipe",
            "Path of the named pipe (FIFO) messages are written to, one per "
            "line. Created when missing; an existing file that is not a "
            "named pipe is refused. Messages are dropped while no reader "
            "has the pipe open",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FORMAT] = g_param_spec_enum (
            "format", "Format",
            "What each line holds: \"Output text\" writes the message's "
            "output, \"JSON message\" writes the whole message envelope as "
            "one-line JSON",
            PN_TYPE_PIPE_FORMAT,
            PN_PIPE_FORMAT_OUTPUT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_pipe_writer_init (PnPipeWriter *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.33, 0.58, 0.62, 1.0 };

    self->pipe_path = g_strdup ("");
    self->real_path = g_strdup ("");
    self->format    = PN_PIPE_FORMAT_OUTPUT;
    self->fd        = -1;
    self->queue     = g_string_new (NULL);

    pn_node_set_class_name (node, "Pipe Writer");
    pn_node_set_icon       (node, "\xef\x82\x8b");  /* fa-sign-out U+F08B */
    pn_node_set_color      (node, &teal);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    /* No pipe configured yet. */
    pn_node_set_has_error  (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnPipeWriter *
pn_pipe_writer_new (void)
{
    return g_object_new (PN_TYPE_PIPE_WRITER, NULL);
}

gboolean
pn_pipe_writer_is_open (PnPipeWriter *self)
{
    g_return_val_if_fail (PN_IS_PIPE_WRITER (self), FALSE);

    return self->fd >= 0;
}

gsize
pn_pipe_writer_get_queued (PnPipeWriter *self)
{
    g_return_val_if_fail (PN_IS_PIPE_WRITER (self), 0);

    return self->queue->len;
}

guint64
pn_pipe_writer_get_dropped (PnPipeWriter *self)
{
    g_return_val_if_fail (PN_IS_PIPE_WRITER (self), 0);

    return self->dropped;
}
