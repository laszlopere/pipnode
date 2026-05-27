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
/*  PnMeshPb — protobuf primitives for the Meshtastic admin subset.    */
/*                                                                     */
/*  Port of /usr/bin/pip-mesh's pb_decode_* / pb_encode_* helpers      */
/*  (lines 789-986).  The bash version operates on a single global     */
/*  HEXDATA string with a PB_POS cursor; this C version puts every     */
/*  read/write through a reader/writer struct so each per-device       */
/*  connection owns its own decode state (TODO #29 hard rule: no       */
/*  global protobuf state).                                            */
/*                                                                     */
/*  Scope: just the primitives the Meshtastic admin protocol and the   */
/*  channel-share QR URL actually need -- varint, tag (field number    */
/*  + wire type), fixed32, length-delimited / embedded messages.  No   */
/*  signed-varint (sint32/64 zig-zag), no group wire types (3/4,       */
/*  deprecated), no descriptor machinery.                              */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-pb.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Reader                                                              */
/* ------------------------------------------------------------------ */

void
pn_mesh_pb_reader_init (PnMeshPbReader *r, const guint8 *data, gsize size)
{
    r->data  = data;
    r->size  = size;
    r->pos   = 0;
    r->error = FALSE;
}

gboolean
pn_mesh_pb_reader_eof (const PnMeshPbReader *r)
{
    return r->pos >= r->size;
}

gboolean
pn_mesh_pb_read_varint (PnMeshPbReader *r, guint64 *out)
{
    guint64 value = 0;
    guint   shift = 0;
    gsize   i;

    if (r->error)
        return FALSE;

    /* Protobuf varints are at most 10 bytes (a 64-bit value, 7 bits
     * per byte, ceil(64/7) = 10).  Bail past that with the error flag
     * sticky so the caller does not consume the rest of the buffer
     * trying to recover. */
    for (i = 0; i < 10; i++)
    {
        guint8 byte;

        if (r->pos >= r->size)
        {
            r->error = TRUE;
            return FALSE;
        }
        byte = r->data[r->pos++];
        value |= ((guint64) (byte & 0x7F)) << shift;
        if ((byte & 0x80) == 0)
        {
            *out = value;
            return TRUE;
        }
        shift += 7;
    }

    r->error = TRUE;
    return FALSE;
}

gboolean
pn_mesh_pb_read_tag (PnMeshPbReader *r,
                     guint32        *field_number,
                     guint32        *wire_type)
{
    guint64 tag;

    /* EOF without a partial read is not an error -- it is the "no
     * more fields" signal callers loop against. */
    if (r->error || r->pos >= r->size)
        return FALSE;

    if (!pn_mesh_pb_read_varint (r, &tag))
        return FALSE;

    *field_number = (guint32) (tag >> 3);
    *wire_type    = (guint32) (tag & 0x07);
    return TRUE;
}

gboolean
pn_mesh_pb_read_fixed32 (PnMeshPbReader *r, guint32 *out)
{
    if (r->error)
        return FALSE;
    if (r->pos + 4 > r->size)
    {
        r->error = TRUE;
        return FALSE;
    }
    /* Little-endian on the wire (protobuf spec). */
    *out =  (guint32)  r->data[r->pos]
         | ((guint32)  r->data[r->pos + 1] << 8)
         | ((guint32)  r->data[r->pos + 2] << 16)
         | ((guint32)  r->data[r->pos + 3] << 24);
    r->pos += 4;
    return TRUE;
}

gboolean
pn_mesh_pb_read_fixed64 (PnMeshPbReader *r, guint64 *out)
{
    if (r->error)
        return FALSE;
    if (r->pos + 8 > r->size)
    {
        r->error = TRUE;
        return FALSE;
    }
    *out =  (guint64)  r->data[r->pos]
         | ((guint64)  r->data[r->pos + 1] << 8)
         | ((guint64)  r->data[r->pos + 2] << 16)
         | ((guint64)  r->data[r->pos + 3] << 24)
         | ((guint64)  r->data[r->pos + 4] << 32)
         | ((guint64)  r->data[r->pos + 5] << 40)
         | ((guint64)  r->data[r->pos + 6] << 48)
         | ((guint64)  r->data[r->pos + 7] << 56);
    r->pos += 8;
    return TRUE;
}

gboolean
pn_mesh_pb_read_float (PnMeshPbReader *r, gfloat *out)
{
    guint32 bits;

    if (!pn_mesh_pb_read_fixed32 (r, &bits))
        return FALSE;
    /* Type-punning via memcpy (no strict-aliasing UB).  Wire encoding
     * is IEEE-754 single-precision, little-endian -- which matches
     * every architecture pipnode runs on; if that ever changes we
     * will need a byte-swap here. */
    memcpy (out, &bits, 4);
    return TRUE;
}

gboolean
pn_mesh_pb_read_length (PnMeshPbReader *r,
                        const guint8  **data,
                        gsize          *size)
{
    guint64 len;

    if (!pn_mesh_pb_read_varint (r, &len))
        return FALSE;

    /* Reject a length that runs past the buffer rather than silently
     * truncating; otherwise a malformed message could hand the caller
     * an oversized "valid" slice. */
    if (len > r->size - r->pos)
    {
        r->error = TRUE;
        return FALSE;
    }

    *data = r->data + r->pos;
    *size = (gsize) len;
    r->pos += (gsize) len;
    return TRUE;
}

gboolean
pn_mesh_pb_skip_field (PnMeshPbReader *r, guint32 wire_type)
{
    switch (wire_type)
    {
    case PN_MESH_PB_WIRE_VARINT:
    {
        guint64 dummy;
        return pn_mesh_pb_read_varint (r, &dummy);
    }
    case PN_MESH_PB_WIRE_FIXED64:
        if (r->pos + 8 > r->size) { r->error = TRUE; return FALSE; }
        r->pos += 8;
        return TRUE;
    case PN_MESH_PB_WIRE_LEN:
    {
        const guint8 *d;
        gsize         s;
        return pn_mesh_pb_read_length (r, &d, &s);
    }
    case PN_MESH_PB_WIRE_FIXED32:
        if (r->pos + 4 > r->size) { r->error = TRUE; return FALSE; }
        r->pos += 4;
        return TRUE;
    default:
        /* Wire types 3/4 are deprecated start/end-group; 6/7 are
         * undefined.  Any of these is a malformed frame -- mark
         * sticky-error and bail. */
        r->error = TRUE;
        return FALSE;
    }
}

/* ------------------------------------------------------------------ */
/*  Writer                                                              */
/* ------------------------------------------------------------------ */

void
pn_mesh_pb_writer_init (PnMeshPbWriter *w)
{
    w->buf = g_byte_array_new ();
}

void
pn_mesh_pb_writer_clear (PnMeshPbWriter *w)
{
    if (w->buf != NULL)
    {
        g_byte_array_unref (w->buf);
        w->buf = NULL;
    }
}

GBytes *
pn_mesh_pb_writer_take_bytes (PnMeshPbWriter *w)
{
    GBytes     *out;
    GByteArray *buf = w->buf;

    /* g_byte_array_free_to_bytes consumes the array -- the writer is
     * left with a fresh empty buffer ready for the next message, so
     * the caller can reuse the writer without explicitly clearing. */
    w->buf = g_byte_array_new ();
    out = g_byte_array_free_to_bytes (buf);
    return out;
}

void
pn_mesh_pb_write_varint (PnMeshPbWriter *w, guint64 value)
{
    while (value > 0x7F)
    {
        guint8 byte = (guint8) ((value & 0x7F) | 0x80);
        g_byte_array_append (w->buf, &byte, 1);
        value >>= 7;
    }
    {
        guint8 byte = (guint8) value;
        g_byte_array_append (w->buf, &byte, 1);
    }
}

void
pn_mesh_pb_write_tag (PnMeshPbWriter *w,
                      guint32 field_number, guint32 wire_type)
{
    pn_mesh_pb_write_varint (w,
                             ((guint64) field_number << 3) | (wire_type & 0x07));
}

void
pn_mesh_pb_write_fixed32 (PnMeshPbWriter *w, guint32 value)
{
    guint8 bytes[4];

    bytes[0] = (guint8)  (value        & 0xFF);
    bytes[1] = (guint8) ((value >> 8)  & 0xFF);
    bytes[2] = (guint8) ((value >> 16) & 0xFF);
    bytes[3] = (guint8) ((value >> 24) & 0xFF);
    g_byte_array_append (w->buf, bytes, 4);
}

void
pn_mesh_pb_write_fixed64 (PnMeshPbWriter *w, guint64 value)
{
    guint8 bytes[8];
    int    i;

    for (i = 0; i < 8; i++)
        bytes[i] = (guint8) ((value >> (8 * i)) & 0xFF);
    g_byte_array_append (w->buf, bytes, 8);
}

void
pn_mesh_pb_write_varint_field (PnMeshPbWriter *w,
                               guint32 field_number, guint64 value)
{
    pn_mesh_pb_write_tag    (w, field_number, PN_MESH_PB_WIRE_VARINT);
    pn_mesh_pb_write_varint (w, value);
}

void
pn_mesh_pb_write_fixed32_field (PnMeshPbWriter *w,
                                guint32 field_number, guint32 value)
{
    pn_mesh_pb_write_tag     (w, field_number, PN_MESH_PB_WIRE_FIXED32);
    pn_mesh_pb_write_fixed32 (w, value);
}

void
pn_mesh_pb_write_bytes_field (PnMeshPbWriter *w,
                              guint32 field_number,
                              const guint8 *data, gsize size)
{
    pn_mesh_pb_write_tag    (w, field_number, PN_MESH_PB_WIRE_LEN);
    pn_mesh_pb_write_varint (w, (guint64) size);
    if (size > 0 && data != NULL)
        g_byte_array_append (w->buf, data, size);
}

void
pn_mesh_pb_write_string_field (PnMeshPbWriter *w,
                               guint32 field_number, const gchar *s)
{
    if (s == NULL)
        s = "";
    pn_mesh_pb_write_bytes_field (w, field_number,
                                  (const guint8 *) s, strlen (s));
}

void
pn_mesh_pb_write_embedded_field (PnMeshPbWriter *w,
                                 guint32        field_number,
                                 const guint8  *data,
                                 gsize          size)
{
    /* Identical wire shape to bytes_field; kept as a separate name so
     * the caller's intent (an embedded message vs raw bytes) is
     * visible at the call site -- pip-mesh uses the same separation. */
    pn_mesh_pb_write_bytes_field (w, field_number, data, size);
}
