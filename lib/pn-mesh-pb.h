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

#ifndef PN_MESH_PB_H
#define PN_MESH_PB_H

#include <glib.h>

G_BEGIN_DECLS

/* Minimal protobuf encoder + decoder for the Meshtastic admin/QR
 * subset.  Direct C port of pip-mesh's pure-bash codec: only the
 * primitives we actually need (varint, tag, fixed32, length-delimited
 * / embedded message), no descriptor machinery, no nanopb dependency.
 *
 * The decoder reads from a borrowed buffer; the encoder writes into a
 * GByteArray it owns.  Both are per-call -- no globals (pip-mesh's
 * HEXDATA + PB_POS were singletons; here every connection/session
 * gets its own state).  A reader's @error flag is sticky: once a read
 * runs past the end or hits a malformed varint, every subsequent
 * read returns FALSE without touching @pos. */

/* Wire types per the protobuf spec.  Wire types 3 and 4 (start/end
 * group, deprecated) are not implemented. */
#define PN_MESH_PB_WIRE_VARINT   0
#define PN_MESH_PB_WIRE_FIXED64  1
#define PN_MESH_PB_WIRE_LEN      2
#define PN_MESH_PB_WIRE_FIXED32  5

/* ------------------------------------------------------------------ */
/*  Reader                                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    const guint8 *data;   /* borrowed buffer, must outlive the reader */
    gsize         size;
    gsize         pos;    /* next byte to read                         */
    gboolean      error;  /* sticky: set on malformed/truncated input  */
} PnMeshPbReader;

void     pn_mesh_pb_reader_init (PnMeshPbReader *r,
                                 const guint8   *data,
                                 gsize           size);

gboolean pn_mesh_pb_reader_eof  (const PnMeshPbReader *r);

/* Read a 64-bit varint at the current position.  Returns FALSE on
 * truncation or a varint longer than 10 bytes; sets @r->error in
 * either case. */
gboolean pn_mesh_pb_read_varint (PnMeshPbReader *r, guint64 *out);

/* Read a field tag (varint), splitting into field number and wire
 * type.  Returns FALSE at EOF without setting @r->error -- callers
 * use this as the "no more fields" signal. */
gboolean pn_mesh_pb_read_tag (PnMeshPbReader *r,
                              guint32         *field_number,
                              guint32         *wire_type);

/* Read 4 / 8 little-endian bytes from the wire. */
gboolean pn_mesh_pb_read_fixed32 (PnMeshPbReader *r, guint32 *out);
gboolean pn_mesh_pb_read_fixed64 (PnMeshPbReader *r, guint64 *out);

/* Read a 32-bit IEEE-754 single-precision float (wire type 5). */
gboolean pn_mesh_pb_read_float (PnMeshPbReader *r, gfloat *out);

/* Read a length-delimited field.  Returns a pointer into the reader's
 * underlying buffer plus the length; @data is valid as long as the
 * reader's buffer is. */
gboolean pn_mesh_pb_read_length (PnMeshPbReader *r,
                                 const guint8  **data,
                                 gsize          *size);

/* Skip the value belonging to a tag of @wire_type.  Used to step
 * over fields the caller does not care about while keeping the
 * message-level cursor in sync. */
gboolean pn_mesh_pb_skip_field (PnMeshPbReader *r, guint32 wire_type);

/* ------------------------------------------------------------------ */
/*  Writer                                                              */
/* ------------------------------------------------------------------ */

typedef struct
{
    GByteArray *buf;
} PnMeshPbWriter;

void     pn_mesh_pb_writer_init  (PnMeshPbWriter *w);
void     pn_mesh_pb_writer_clear (PnMeshPbWriter *w);

/* Take ownership of the accumulated bytes; the writer becomes empty
 * and may be cleared or reused for a fresh message. */
GBytes  *pn_mesh_pb_writer_take_bytes (PnMeshPbWriter *w);

/* Primitive writers.  None of these fail -- the GByteArray grows. */
void     pn_mesh_pb_write_varint  (PnMeshPbWriter *w, guint64 value);
void     pn_mesh_pb_write_tag     (PnMeshPbWriter *w,
                                   guint32 field_number, guint32 wire_type);
void     pn_mesh_pb_write_fixed32 (PnMeshPbWriter *w, guint32 value);
void     pn_mesh_pb_write_fixed64 (PnMeshPbWriter *w, guint64 value);

/* Composite writers (tag + value). */
void     pn_mesh_pb_write_varint_field  (PnMeshPbWriter *w,
                                         guint32 field_number, guint64 value);
void     pn_mesh_pb_write_fixed32_field (PnMeshPbWriter *w,
                                         guint32 field_number, guint32 value);
void     pn_mesh_pb_write_bytes_field   (PnMeshPbWriter *w,
                                         guint32        field_number,
                                         const guint8  *data,
                                         gsize          size);
void     pn_mesh_pb_write_string_field  (PnMeshPbWriter *w,
                                         guint32        field_number,
                                         const gchar   *s);

/* Length-delimited embedded message: tag(LEN) + varint(size) + payload. */
void     pn_mesh_pb_write_embedded_field (PnMeshPbWriter *w,
                                          guint32        field_number,
                                          const guint8  *data,
                                          gsize          size);

G_END_DECLS

#endif /* PN_MESH_PB_H */
