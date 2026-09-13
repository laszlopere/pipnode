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

#ifndef PN_PIPE_READER_H
#define PN_PIPE_READER_H

#include "pn-node.h"
#include "pn-pipe-common.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnPipeReader                                                       */
/*                                                                     */
/*  Source node that reads a named pipe (FIFO) and emits one message   */
/*  per newline-terminated line.  "pipe-path" names the FIFO; it is    */
/*  created when missing, and anything that exists there but is not a */
/*  FIFO is refused.  "format" decides what a line holds:              */
/*                                                                     */
/*    - Output text: the line becomes data.output; data.value is the   */
/*      line's number when it is one, 0 otherwise; success is TRUE.    */
/*    - JSON message: the line is a message envelope (as the Pipe      */
/*      Writer / Debug Print produce) or a bare data object; it is     */
/*      re-emitted with this node as its source.  A line that is not   */
/*      valid JSON emits a success=FALSE message saying so.            */
/*                                                                     */
/*  Nothing ever blocks: the FIFO is opened O_NONBLOCK and read from a */
/*  main-loop fd watch.  The node also holds a write end of its own    */
/*  pipe open, so writers may come and go without the reader seeing    */
/*  end-of-file (which would otherwise spin the watch).                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PIPE_READER (pn_pipe_reader_get_type ())

G_DECLARE_FINAL_TYPE (PnPipeReader, pn_pipe_reader,
                      PN, PIPE_READER, PnNode)

PnPipeReader *pn_pipe_reader_new     (void);

/**
 * pn_pipe_reader_is_open:
 *
 * Returns: whether the FIFO is currently open and being watched.  The
 * open happens from an idle callback after construction or a path
 * change, so this is %FALSE until the main loop has run.
 */
gboolean      pn_pipe_reader_is_open (PnPipeReader *self);

G_END_DECLS

#endif /* PN_PIPE_READER_H */
