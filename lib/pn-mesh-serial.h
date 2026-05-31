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

#ifndef PN_MESH_SERIAL_H
#define PN_MESH_SERIAL_H

#include <glib.h>

G_BEGIN_DECLS

/* Serial transport for Meshtastic devices.
 *
 * Two cooperating pieces:
 *
 *   PnMeshSerial  — owns the /dev/ttyXXX fd, configures it at 115200
 *   8N1 raw, drains stale bytes, and exposes a frame-oriented write +
 *   raw read.  Always blocking with a millisecond timeout; designed
 *   to be called from a GTask worker thread, not the main loop.
 *
 *   PnMeshFrameReader — a byte-stream accumulator that re-frames the
 *   raw serial bytes back into complete Meshtastic frames.  Scans for
 *   the 0x94 0xC3 magic, reads the 2-byte big-endian length, hands
 *   out one #GBytes per complete frame.  Sliding through stray /
 *   partial data is normal: serial reads return whatever is
 *   available, so the accumulator buffers across reads.
 *
 * The framing here is pip-mesh's; pn-mesh-pb.c handles the
 * protobuf payload inside each frame. */

/* Maximum payload size we'll buffer for a single inbound frame.  The
 * 2-byte length field can claim up to 65535, but real Meshtastic
 * frames are well under 512; we cap to 8 KiB to bound memory on a
 * malformed stream while leaving headroom for any legitimate growth. */
#define PN_MESH_FRAME_MAX_PAYLOAD 8192

/* ------------------------------------------------------------------ */
/*  Frame reader                                                        */
/* ------------------------------------------------------------------ */

typedef struct _PnMeshFrameReader PnMeshFrameReader;

PnMeshFrameReader *pn_mesh_frame_reader_new  (void);
void               pn_mesh_frame_reader_free (PnMeshFrameReader *self);

/* Feed @size raw bytes from the wire into the accumulator.  Cheap
 * to call with single bytes or with whole reads. */
void               pn_mesh_frame_reader_feed (PnMeshFrameReader *self,
                                              const guint8      *data,
                                              gsize              size);

/* Take the next complete frame, or %NULL if no full frame is buffered
 * yet.  Callers loop until %NULL after every feed() to drain any
 * frames that completed.  Returned bytes contain only the protobuf
 * payload (the 4-byte serial header is stripped); g_bytes_unref()
 * when done. */
GBytes            *pn_mesh_frame_reader_take (PnMeshFrameReader *self);

/* ------------------------------------------------------------------ */
/*  Serial transport                                                    */
/* ------------------------------------------------------------------ */

typedef struct _PnMeshSerial PnMeshSerial;

/* Open @path at 115200 8N1 raw, matching pip-mesh's `stty 115200
 * cs8 -cstopb -parenb raw -echo -echoe -echok` configuration.
 * Returns %NULL on failure with @error set. */
PnMeshSerial *pn_mesh_serial_open  (const gchar *path, GError **error);

/* Close the fd and free.  No-op on %NULL. */
void          pn_mesh_serial_close (PnMeshSerial *self);

/* The underlying file descriptor, or -1 if closed.  Exposed so a caller
 * that has to poll() the serial fd alongside other descriptors (the
 * Meshtastic node multiplexes it with a wake pipe in its worker thread)
 * can do so directly; ordinary I/O should still go through the read /
 * write_frame helpers.  The returned fd stays owned by @self -- do not
 * close it. */
int           pn_mesh_serial_get_fd (PnMeshSerial *self);

/* Drain bytes the device may have emitted before we opened (and
 * anything queued by an earlier session).  pip-mesh uses a 200 ms
 * cat-and-discard at the same point in its flow; we do the same.
 * Skipping this corrupts the first read after open. */
void          pn_mesh_serial_drain (PnMeshSerial *self, gint timeout_ms);

/* Close the fd and reopen the same path with the same termios.
 * Some admin writes (set_channel in particular) are only committed
 * to flash by the Meshtastic firmware when the serial session ends
 * -- the DTR-drop on close is the trigger.  pip-mesh gets this for
 * free because it uses `echo > $tty` (a fresh open/close per write);
 * a long-lived fd like ours has to bounce explicitly.  Returns FALSE
 * with @error set if the reopen failed. */
gboolean      pn_mesh_serial_reopen (PnMeshSerial  *self,
                                     GError       **error);

/* Wrap @payload in a 0x94 0xC3 + 2-byte big-endian length frame and
 * write the whole thing as one chunk.  Returns %FALSE on write error
 * with @error set; partial writes are looped internally. */
gboolean      pn_mesh_serial_write_frame (PnMeshSerial  *self,
                                          const guint8  *payload,
                                          gsize          size,
                                          GError       **error);

/* Read up to @size raw bytes into @buf.  Blocks until at least one
 * byte arrives or @timeout_ms elapses with no data.  Returns the
 * number of bytes read; 0 on timeout (no error); -1 on read error
 * (with @error set).  This is the raw byte stream -- the caller is
 * expected to feed it to a PnMeshFrameReader. */
gssize        pn_mesh_serial_read (PnMeshSerial  *self,
                                   guint8        *buf,
                                   gsize          size,
                                   gint           timeout_ms,
                                   GError       **error);

G_END_DECLS

#endif /* PN_MESH_SERIAL_H */
