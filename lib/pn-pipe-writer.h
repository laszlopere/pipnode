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

#ifndef PN_PIPE_WRITER_H
#define PN_PIPE_WRITER_H

#include "pn-node.h"
#include "pn-pipe-common.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnPipeWriter                                                       */
/*                                                                     */
/*  Sink node that writes every received message to a named pipe      */
/*  (FIFO) as one newline-terminated line.  "pipe-path" names the      */
/*  FIFO; it is created when missing, and anything that exists there  */
/*  but is not a FIFO is refused.  "format" decides the line:          */
/*                                                                     */
/*    - Output text: the message's data.output ("" when absent).       */
/*    - JSON message: the whole envelope as compact one-line JSON,     */
/*      vector payloads included, which a Pipe Reader in JSON mode     */
/*      turns back into the same message.                              */
/*                                                                     */
/*  Nothing ever blocks.  The FIFO is opened O_NONBLOCK, which fails   */
/*  while no process has it open for reading: messages arriving then   */
/*  are dropped, and the open is retried on the next message.  A      */
/*  reader that is connected but slow fills the kernel pipe buffer;    */
/*  the unsent bytes then queue in memory (up to PN_PIPE_MAX_BUFFER,   */
/*  whole lines only) and drain from a main-loop fd watch.  When the   */
/*  reader goes away the queue is discarded.                           */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PIPE_WRITER (pn_pipe_writer_get_type ())

G_DECLARE_FINAL_TYPE (PnPipeWriter, pn_pipe_writer,
                      PN, PIPE_WRITER, PnNode)

PnPipeWriter *pn_pipe_writer_new         (void);

/**
 * pn_pipe_writer_is_open:
 *
 * Returns: whether a reader is connected (the FIFO is open for writing).
 */
gboolean      pn_pipe_writer_is_open     (PnPipeWriter *self);

/**
 * pn_pipe_writer_get_queued:
 *
 * Returns: bytes accepted but not yet written into the pipe.
 */
gsize         pn_pipe_writer_get_queued  (PnPipeWriter *self);

/**
 * pn_pipe_writer_get_dropped:
 *
 * Returns: messages dropped since construction, because no reader was
 * connected or the queue was full.
 */
guint64       pn_pipe_writer_get_dropped (PnPipeWriter *self);

G_END_DECLS

#endif /* PN_PIPE_WRITER_H */
