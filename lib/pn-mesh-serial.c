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

/* ------------------------------------------------------------------ */
/*  PnMeshSerial + PnMeshFrameReader                                    */
/*                                                                     */
/*  Serial transport + frame accumulator for the Meshtastic admin      */
/*  protocol.  Ports pip-mesh's "stty 115200 8N1 raw" + 0x94 0xC3      */
/*  framing into C using POSIX termios; blocking with millisecond      */
/*  timeouts via poll().  Designed to be called from a GTask worker    */
/*  thread, not the main loop.                                         */
/*                                                                     */
/*  The accumulator's job is to re-frame the raw byte stream that      */
/*  serial reads return at arbitrary boundaries: scan forward one      */
/*  byte at a time for the 0x94 0xC3 magic (so a partial header at     */
/*  read-1 still lines up at read-2), then a 2-byte big-endian         */
/*  length, then exactly that many payload bytes.  pip-mesh's          */
/*  parse_serial_frames does the same on a hex string with one-char    */
/*  steps; we do it on raw bytes with one-byte steps.                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-serial.h"

#include <gio/gio.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

/* Meshtastic serial frame magic. */
#define FRAME_MAGIC_0 0x94
#define FRAME_MAGIC_1 0xC3

/* ------------------------------------------------------------------ */
/*  PnMeshFrameReader                                                   */
/* ------------------------------------------------------------------ */

struct _PnMeshFrameReader
{
    GByteArray *buf;   /* unparsed bytes (incl. partial header / payload) */
};

PnMeshFrameReader *
pn_mesh_frame_reader_new (void)
{
    PnMeshFrameReader *self = g_slice_new0 (PnMeshFrameReader);
    self->buf = g_byte_array_new ();
    return self;
}

void
pn_mesh_frame_reader_free (PnMeshFrameReader *self)
{
    if (self == NULL)
        return;
    g_byte_array_unref (self->buf);
    g_slice_free (PnMeshFrameReader, self);
}

void
pn_mesh_frame_reader_feed (PnMeshFrameReader *self,
                           const guint8      *data,
                           gsize              size)
{
    if (self == NULL || data == NULL || size == 0)
        return;
    g_byte_array_append (self->buf, data, (guint) size);
}

/* Find the offset of the first 0x94 0xC3 in @buf, or -1 if no two-byte
 * window matches.  A trailing lone 0x94 is treated as "not found yet"
 * because the 0xC3 might land in the next feed(). */
static gssize
find_magic (GByteArray *buf)
{
    guint i;

    if (buf->len < 2)
        return -1;
    for (i = 0; i + 1 < buf->len; i++)
        if (buf->data[i] == FRAME_MAGIC_0 && buf->data[i + 1] == FRAME_MAGIC_1)
            return (gssize) i;
    return -1;
}

GBytes *
pn_mesh_frame_reader_take (PnMeshFrameReader *self)
{
    gssize  magic_at;
    guint16 payload_size;
    GBytes *frame;

    if (self == NULL)
        return NULL;

    for (;;)
    {
        magic_at = find_magic (self->buf);
        if (magic_at < 0)
        {
            /* No header yet.  Hold on to the last byte (it may be a
             * lone 0x94 that will pair with the next read's 0xC3);
             * drop everything before it as junk. */
            if (self->buf->len > 1)
                g_byte_array_remove_range (self->buf, 0,
                                           self->buf->len - 1);
            return NULL;
        }

        /* Drop junk that preceded the header. */
        if (magic_at > 0)
            g_byte_array_remove_range (self->buf, 0, (guint) magic_at);

        /* Need at least 4 bytes (magic + length) before we know if a
         * full frame is buffered. */
        if (self->buf->len < 4)
            return NULL;

        payload_size = ((guint16) self->buf->data[2] << 8)
                     |  (guint16) self->buf->data[3];

        /* Sanity-cap: a wildly oversized claimed length almost
         * certainly means we synced on a coincidental 0x94 0xC3 in
         * the middle of a payload.  Drop the magic and rescan from
         * the next byte. */
        if (payload_size > PN_MESH_FRAME_MAX_PAYLOAD)
        {
            g_byte_array_remove_range (self->buf, 0, 2);
            continue;
        }

        /* Wait for the rest of the payload. */
        if (self->buf->len < (guint) (4 + payload_size))
            return NULL;

        frame = g_bytes_new (self->buf->data + 4, payload_size);
        g_byte_array_remove_range (self->buf, 0, 4 + payload_size);
        return frame;
    }
}

/* ------------------------------------------------------------------ */
/*  PnMeshSerial                                                        */
/* ------------------------------------------------------------------ */

struct _PnMeshSerial
{
    int    fd;
    gchar *path;
};

/* termios setup matching pip-mesh's `stty 115200 cs8 -cstopb -parenb
 * raw -echo -echoe -echok`. */
static gboolean
configure_termios (int fd, GError **error)
{
    struct termios tio;

    if (tcgetattr (fd, &tio) != 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "tcgetattr failed: %s", g_strerror (errno));
        return FALSE;
    }

    /* cfmakeraw is shorthand for the standard "no terminal processing"
     * flag set -- equivalent to `raw -echo -echoe` for our purposes.
     * Lays the foundation; we follow up with the cs8 / no-parity / 1
     * stop bit lines to be explicit. */
    cfmakeraw (&tio);

    tio.c_cflag &= ~(unsigned) (PARENB | CSTOPB | CSIZE);
    tio.c_cflag |= (unsigned) (CS8 | CLOCAL | CREAD);
    tio.c_iflag &= ~(unsigned) (IXON | IXOFF | IXANY);

    /* Blocking read with VMIN=0/VTIME=0 -- we drive timeouts via
     * poll() ourselves rather than the termios VTIME tick.  This
     * gives us millisecond-level control without the 100 ms granularity. */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;

    if (cfsetospeed (&tio, B115200) != 0 || cfsetispeed (&tio, B115200) != 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "cfsetspeed failed: %s", g_strerror (errno));
        return FALSE;
    }

    if (tcsetattr (fd, TCSANOW, &tio) != 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "tcsetattr failed: %s", g_strerror (errno));
        return FALSE;
    }
    return TRUE;
}

/* Open + termios-configure one fd for @path.  Used by both the
 * initial open and reopen so the configuration is in lock-step. */
static int
open_configured (const gchar *path, GError **error)
{
    int fd;

    /* O_NOCTTY: opening a tty shouldn't make it our controlling
     * terminal.  O_NONBLOCK: a USB CDC-ACM device can stall open() on
     * dropped DTR otherwise -- we clear the flag once the fd is up. */
    fd = open (path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "open(%s) failed: %s", path, g_strerror (errno));
        return -1;
    }

    if (!configure_termios (fd, error))
    {
        close (fd);
        return -1;
    }

    /* Switch to blocking mode now that termios is in raw mode; we
     * gate reads on poll() so this is safe. */
    if (fcntl (fd, F_SETFL, 0) < 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "fcntl(F_SETFL,0) failed: %s", g_strerror (errno));
        close (fd);
        return -1;
    }

    return fd;
}

PnMeshSerial *
pn_mesh_serial_open (const gchar *path, GError **error)
{
    PnMeshSerial *self;
    int           fd;

    fd = open_configured (path, error);
    if (fd < 0)
        return NULL;

    self = g_slice_new0 (PnMeshSerial);
    self->fd   = fd;
    self->path = g_strdup (path);
    return self;
}

gboolean
pn_mesh_serial_reopen (PnMeshSerial *self, GError **error)
{
    int new_fd;

    g_return_val_if_fail (self != NULL, FALSE);

    /* Open the new fd first; only swap into self after success, so a
     * failed reopen leaves the existing connection intact. */
    new_fd = open_configured (self->path, error);
    if (new_fd < 0)
        return FALSE;

    if (self->fd >= 0)
        close (self->fd);
    self->fd = new_fd;
    return TRUE;
}

void
pn_mesh_serial_close (PnMeshSerial *self)
{
    if (self == NULL)
        return;
    if (self->fd >= 0)
        close (self->fd);
    g_free (self->path);
    g_slice_free (PnMeshSerial, self);
}

void
pn_mesh_serial_drain (PnMeshSerial *self, gint timeout_ms)
{
    guint8 junk[256];
    gint64 deadline;

    if (self == NULL || self->fd < 0 || timeout_ms <= 0)
        return;

    deadline = g_get_monotonic_time () + (gint64) timeout_ms * 1000;
    for (;;)
    {
        gint64       now      = g_get_monotonic_time ();
        gint64       remain   = deadline - now;
        struct pollfd pfd;
        int          rc;
        ssize_t      r;

        if (remain <= 0)
            return;

        pfd.fd     = self->fd;
        pfd.events = POLLIN;
        rc = poll (&pfd, 1, (int) (remain / 1000));
        if (rc <= 0)
            return;   /* timeout or error -- nothing more to drain */

        r = read (self->fd, junk, sizeof junk);
        if (r <= 0)
            return;
        /* Loop: more bytes may follow within the timeout window. */
    }
}

gboolean
pn_mesh_serial_write_frame (PnMeshSerial  *self,
                            const guint8  *payload,
                            gsize          size,
                            GError       **error)
{
    guint8 *frame;
    gsize   frame_size;
    gsize   written = 0;

    g_return_val_if_fail (self != NULL && self->fd >= 0, FALSE);
    g_return_val_if_fail (size <= PN_MESH_FRAME_MAX_PAYLOAD, FALSE);

    frame_size = 4 + size;
    frame = g_malloc (frame_size);
    frame[0] = FRAME_MAGIC_0;
    frame[1] = FRAME_MAGIC_1;
    frame[2] = (guint8) ((size >> 8) & 0xFF);   /* big-endian length */
    frame[3] = (guint8)  (size       & 0xFF);
    if (size > 0)
        memcpy (frame + 4, payload, size);

    while (written < frame_size)
    {
        ssize_t r = write (self->fd, frame + written, frame_size - written);
        if (r < 0)
        {
            if (errno == EINTR)
                continue;
            g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                         "write(%s) failed: %s",
                         self->path, g_strerror (errno));
            g_free (frame);
            return FALSE;
        }
        written += (gsize) r;
    }
    g_free (frame);
    return TRUE;
}

gssize
pn_mesh_serial_read (PnMeshSerial  *self,
                     guint8        *buf,
                     gsize          size,
                     gint           timeout_ms,
                     GError       **error)
{
    struct pollfd pfd;
    int           rc;
    ssize_t       r;

    g_return_val_if_fail (self != NULL && self->fd >= 0, -1);
    g_return_val_if_fail (buf != NULL && size > 0, -1);

    pfd.fd     = self->fd;
    pfd.events = POLLIN;

    /* poll() handles negative timeouts as "infinite" -- clamp to 0
     * (immediate) for safety; callers pass -1 to mean "block". */
    do {
        rc = poll (&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);

    if (rc < 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "poll(%s) failed: %s", self->path, g_strerror (errno));
        return -1;
    }
    if (rc == 0)
        return 0;   /* timeout, no data */

    do {
        r = read (self->fd, buf, size);
    } while (r < 0 && errno == EINTR);

    if (r < 0)
    {
        g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                     "read(%s) failed: %s", self->path, g_strerror (errno));
        return -1;
    }
    return (gssize) r;
}
