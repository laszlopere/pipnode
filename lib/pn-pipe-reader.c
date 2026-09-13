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

#include "pn-pipe-reader.h"
#include "pn-message.h"
#include "pn-path.h"
#include "pn-settings-schema.h"

#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

/* Bytes read per read(2) and reads per wakeup: a writer flooding the
 * pipe still lets the main loop breathe between batches. */
#define PN_PIPE_READ_CHUNK     65536
#define PN_PIPE_READS_PER_WAKE 16

struct _PnPipeReader
{
    PnNode parent_instance;

    gchar        *pipe_path;   /* as typed, saved              */
    gchar        *real_path;   /* pipe_path with ~ expanded    */
    PnPipeFormat  format;

    gint     rd_fd;         /* read end, -1 when closed                 */
    gint     keep_fd;       /* our own write end, keeps EOF away        */
    guint    watch_id;      /* g_unix_fd_add source on rd_fd            */
    guint    open_idle_id;  /* pending (re)open                         */
    GString *line;          /* bytes of the current unterminated line   */
};

G_DEFINE_TYPE (PnPipeReader, pn_pipe_reader, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_PIPE_PATH,
    PROP_FORMAT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Emitting                                                           */
/* ------------------------------------------------------------------ */

static void
emit_failure (PnPipeReader *self, const gchar *text)
{
    PnMessage *msg = pn_message_new (PN_NODE (self), NULL);

    pn_message_set_double  (msg, "value",   0.0);
    pn_message_set_string  (msg, "output",  text);
    pn_message_set_boolean (msg, "success", FALSE);
    pn_node_emit_message (PN_NODE (self), msg);
    g_object_unref (msg);
}

/* The number a whole line spells, surrounding blanks allowed. */
static gdouble
line_value (const gchar *text)
{
    gchar  *copy = g_strstrip (g_strdup (text));
    gchar  *end  = NULL;
    gdouble v    = 0.0;

    if (*copy != '\0')
    {
        v = g_ascii_strtod (copy, &end);
        if (end == NULL || *end != '\0')
            v = 0.0;
    }
    g_free (copy);
    return v;
}

static void
emit_line (PnPipeReader *self, const gchar *text)
{
    PnNode    *node = PN_NODE (self);
    PnMessage *msg;

    if (self->format == PN_PIPE_FORMAT_JSON)
    {
        GError *error = NULL;

        /* Blank lines are just framing noise between JSON records. */
        if (*text == '\0' || strspn (text, " \t") == strlen (text))
            return;

        msg = pn_message_deserialize (text, &error);
        if (msg == NULL)
        {
            gchar *out = g_strdup_printf ("Pipe Reader: invalid JSON "
                                          "message: %s", error->message);
            emit_failure (self, out);
            g_free (out);
            g_error_free (error);
            return;
        }

        pn_message_set_source (msg, node);
        if (pn_message_get_topic (msg) == NULL ||
            *pn_message_get_topic (msg) == '\0')
        {
            gchar *topic = pn_node_resolve_topic (node);
            pn_message_set_topic (msg, topic);
            g_free (topic);
        }
    }
    else
    {
        msg = pn_message_new (node, NULL);
        pn_message_set_double  (msg, "value",   line_value (text));
        pn_message_set_string  (msg, "output",  text);
        pn_message_set_boolean (msg, "success", TRUE);
    }

    pn_node_emit_message (node, msg);
    g_object_unref (msg);
}

/* Emit every complete line in self->line and keep the unterminated tail. */
static void
drain_lines (PnPipeReader *self)
{
    gint   fd = self->rd_fd;
    gchar *last_nl;
    gchar *complete;
    gchar *cursor;
    gsize  i;

    last_nl = NULL;
    for (i = self->line->len; i > 0; i--)
    {
        if (self->line->str[i - 1] == '\n')
        {
            last_nl = self->line->str + i - 1;
            break;
        }
    }
    if (last_nl == NULL)
    {
        /* Never hold an unbounded unterminated line: flush it as is. */
        if (self->line->len >= PN_PIPE_MAX_BUFFER)
        {
            complete = g_string_free (self->line, FALSE);
            self->line = g_string_new (NULL);
            emit_line (self, complete);
            g_free (complete);
        }
        return;
    }

    /* Take the complete lines out of the buffer before emitting, so a
     * downstream handler that closes or reconfigures this node cannot
     * pull the bytes from under the loop. */
    complete = g_strndup (self->line->str, last_nl - self->line->str);
    g_string_erase (self->line, 0, (last_nl - self->line->str) + 1);

    cursor = complete;
    for (;;)
    {
        gchar *nl  = strchr (cursor, '\n');
        gsize  len;

        if (nl != NULL)
            *nl = '\0';
        len = strlen (cursor);
        if (len > 0 && cursor[len - 1] == '\r')
            cursor[len - 1] = '\0';

        emit_line (self, cursor);

        /* Stop when a handler closed or reopened the pipe. */
        if (nl == NULL || self->rd_fd != fd)
            break;
        cursor = nl + 1;
    }

    g_free (complete);
}

/* ------------------------------------------------------------------ */
/*  Open / close                                                       */
/* ------------------------------------------------------------------ */

static void
reader_close (PnPipeReader *self)
{
    if (self->watch_id != 0)
    {
        g_source_remove (self->watch_id);
        self->watch_id = 0;
    }
    if (self->rd_fd >= 0)
    {
        close (self->rd_fd);
        self->rd_fd = -1;
    }
    if (self->keep_fd >= 0)
    {
        close (self->keep_fd);
        self->keep_fd = -1;
    }
    if (self->line != NULL)
        g_string_truncate (self->line, 0);
}

static gboolean
on_readable (gint fd, GIOCondition condition, gpointer user_data)
{
    PnPipeReader *self = PN_PIPE_READER (user_data);
    gchar         buf[PN_PIPE_READ_CHUNK];
    gint          round;

    for (round = 0; round < PN_PIPE_READS_PER_WAKE; round++)
    {
        gssize n = read (fd, buf, sizeof buf);

        if (n > 0)
        {
            gboolean closed;

            g_object_ref (self);
            g_string_append_len (self->line, buf, n);
            drain_lines (self);
            /* A handler closed or reopened the pipe: reader_close()
             * already removed this source (or a new one replaced it). */
            closed = (self->rd_fd != fd);
            g_object_unref (self);
            if (closed)
                return G_SOURCE_REMOVE;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN)
            break;

        /* EOF cannot happen while keep_fd is open; a real error can.
         * Either way stop watching rather than spin. */
        {
            gchar *out = g_strdup_printf ("Pipe Reader: reading '%s' "
                                          "failed: %s", self->real_path,
                                          n < 0 ? g_strerror (errno)
                                                : "end of file");
            self->watch_id = 0;
            reader_close (self);
            pn_node_set_has_error (PN_NODE (self), TRUE);
            emit_failure (self, out);
            g_free (out);
            return G_SOURCE_REMOVE;
        }
    }

    (void) condition;
    return G_SOURCE_CONTINUE;
}

static gboolean
reader_open (PnPipeReader *self, gchar **message)
{
    GError *error = NULL;

    reader_close (self);

    if (self->pipe_path[0] == '\0')
        return FALSE;                     /* unconfigured, not an event */

    if (!pn_pipe_ensure_fifo (self->real_path, &error))
    {
        *message = g_strdup_printf ("Pipe Reader: %s", error->message);
        g_error_free (error);
        return FALSE;
    }

    /* O_NONBLOCK: a read-only open of a FIFO would otherwise wait for a
     * writer.  With a reader already present, the write-only open of
     * keep_fd succeeds immediately too. */
    self->rd_fd = open (self->real_path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (self->rd_fd >= 0)
        self->keep_fd = open (self->real_path,
                              O_WRONLY | O_NONBLOCK | O_CLOEXEC);

    if (self->rd_fd < 0 || self->keep_fd < 0)
    {
        *message = g_strdup_printf ("Pipe Reader: cannot open '%s': %s",
                                    self->real_path, g_strerror (errno));
        reader_close (self);
        return FALSE;
    }

    self->watch_id = g_unix_fd_add (self->rd_fd, G_IO_IN | G_IO_HUP | G_IO_ERR,
                                    on_readable, self);
    return TRUE;
}

static gboolean
open_idle (gpointer user_data)
{
    PnPipeReader *self    = PN_PIPE_READER (user_data);
    gchar        *message = NULL;
    gboolean      ok;

    self->open_idle_id = 0;

    ok = reader_open (self, &message);
    pn_node_set_has_error (PN_NODE (self), !ok);
    if (message != NULL)
    {
        emit_failure (self, message);
        g_free (message);
    }
    return G_SOURCE_REMOVE;
}

/* Open from the main loop rather than inline: properties load after
 * constructed(), and wires attach later still, so a failure message
 * emitted from set_property would reach nobody. */
static void
schedule_open (PnPipeReader *self)
{
    if (self->open_idle_id == 0)
        self->open_idle_id = g_idle_add (open_idle, self);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_pipe_reader_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnPipeReader *self = PN_PIPE_READER (object);

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
pn_pipe_reader_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnPipeReader *self = PN_PIPE_READER (object);

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
                reader_close (self);
                pn_node_set_has_error (PN_NODE (self), *path == '\0');
                if (*path != '\0')
                    schedule_open (self);
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
pn_pipe_reader_dispose (GObject *object)
{
    PnPipeReader *self = PN_PIPE_READER (object);

    if (self->open_idle_id != 0)
    {
        g_source_remove (self->open_idle_id);
        self->open_idle_id = 0;
    }
    reader_close (self);

    G_OBJECT_CLASS (pn_pipe_reader_parent_class)->dispose (object);
}

static void
pn_pipe_reader_finalize (GObject *object)
{
    PnPipeReader *self = PN_PIPE_READER (object);

    g_free (self->pipe_path);
    g_free (self->real_path);
    g_string_free (self->line, TRUE);

    G_OBJECT_CLASS (pn_pipe_reader_parent_class)->finalize (object);
}

static void
pn_pipe_reader_class_init (PnPipeReaderClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_pipe_reader_get_property;
    object_class->set_property = pn_pipe_reader_set_property;
    object_class->dispose      = pn_pipe_reader_dispose;
    object_class->finalize     = pn_pipe_reader_finalize;

    node_class->class_name     = "Pipe Reader";
    node_class->icon           = "\xef\x82\x90";  /* fa-sign-in U+F090 */
    node_class->color          = (PnColor){ 0.33, 0.58, 0.62, 1.0 };
    node_class->category       = "Sources";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row (schema, "pipe-path", PN_EDITOR_FILE);
        pn_settings_schema_row (schema, "format",    PN_EDITOR_AUTO);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_PIPE_PATH] = g_param_spec_string (
            "pipe-path", "Named pipe",
            "Path of the named pipe (FIFO) to read, one message per line. "
            "Created when missing; an existing file that is not a named "
            "pipe is refused",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FORMAT] = g_param_spec_enum (
            "format", "Format",
            "What each line holds: \"Output text\" becomes the message's "
            "output, \"JSON message\" is a whole message envelope in "
            "one-line JSON",
            PN_TYPE_PIPE_FORMAT,
            PN_PIPE_FORMAT_OUTPUT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_pipe_reader_init (PnPipeReader *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.33, 0.58, 0.62, 1.0 };

    self->pipe_path = g_strdup ("");
    self->real_path = g_strdup ("");
    self->format    = PN_PIPE_FORMAT_OUTPUT;
    self->rd_fd     = -1;
    self->keep_fd   = -1;
    self->line      = g_string_new (NULL);

    pn_node_set_class_name (node, "Pipe Reader");
    pn_node_set_icon       (node, "\xef\x82\x90");  /* fa-sign-in U+F090 */
    pn_node_set_color      (node, &teal);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* No pipe configured yet. */
    pn_node_set_has_error  (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnPipeReader *
pn_pipe_reader_new (void)
{
    return g_object_new (PN_TYPE_PIPE_READER, NULL);
}

gboolean
pn_pipe_reader_is_open (PnPipeReader *self)
{
    g_return_val_if_fail (PN_IS_PIPE_READER (self), FALSE);

    return self->rd_fd >= 0;
}
