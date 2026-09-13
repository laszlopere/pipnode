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

#ifndef PN_PIPE_COMMON_H
#define PN_PIPE_COMMON_H

#include <glib-object.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Shared by PnPipeReader and PnPipeWriter (named pipes / FIFOs).     */
/*                                                                     */
/*  Both nodes speak newline-delimited text: one message per line.     */
/*  The format decides what a line holds.                              */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_PIPE_FORMAT_OUTPUT = 0,  /* the message's data.output text      */
    PN_PIPE_FORMAT_JSON   = 1,  /* the whole envelope as one JSON line */
} PnPipeFormat;

GType pn_pipe_format_get_type (void) G_GNUC_CONST;
#define PN_TYPE_PIPE_FORMAT (pn_pipe_format_get_type ())

/* Longest line either node holds in memory.  A reader flushes a longer
 * unterminated line as-is; a writer queues at most this many unsent
 * bytes before dropping new messages. */
#define PN_PIPE_MAX_BUFFER (1024 * 1024)

/**
 * pn_pipe_ensure_fifo:
 * @path:  the named pipe's path
 * @error: (out) (nullable): a G_IO_ERROR on failure
 *
 * Creates a FIFO at @path (mode 0666 minus the umask) when nothing exists
 * there yet.  An existing FIFO is accepted as is; any other kind of file
 * is refused, so a typo never clobbers a regular file.
 *
 * Returns: %TRUE when @path is a FIFO on return.
 */
gboolean pn_pipe_ensure_fifo (const gchar *path, GError **error);

/**
 * pn_pipe_write:
 * @fd:  a non-blocking write descriptor
 * @buf: bytes to write
 * @len: number of bytes
 *
 * write(2) that never raises SIGPIPE: a reader that went away yields -1
 * with errno EPIPE instead of killing the process.
 *
 * Returns: bytes written, or -1 with errno set.
 */
gssize   pn_pipe_write       (gint fd, const gchar *buf, gsize len);

G_END_DECLS

#endif /* PN_PIPE_COMMON_H */
