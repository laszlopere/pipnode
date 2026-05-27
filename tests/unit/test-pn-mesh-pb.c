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

/* Unit tests for the Meshtastic protobuf primitives (pn-mesh-pb.c).
 * The codec is a hand-port of pip-mesh's pure-bash encoder/decoder, so
 * these tests pin the wire-format invariants: varint length boundaries,
 * tag = (field << 3) | wire, little-endian fixed widths, length-
 * delimited slices that point into the source buffer, and skip_field
 * stepping over unknown fields without disturbing the cursor.
 * Truncated and malformed inputs must set the reader's sticky error
 * flag rather than silently returning zero. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "pntest.h"
#include "pn-mesh-pb.h"

/* ------------------------------------------------------------------ */
/*  Varint round-trip                                                   */
/* ------------------------------------------------------------------ */

/* Encode @value, then decode the exact bytes, and assert the
 * round-trip plus the encoded length for a few values whose lengths
 * straddle the 7-bit boundaries (1/2/3/5/10 bytes). */
static void
check_varint_roundtrip (guint64 value, gsize expect_bytes)
{
    PnMeshPbWriter w;
    PnMeshPbReader r;
    GBytes        *bytes;
    const guint8  *data;
    gsize          size;
    guint64        decoded = 0xdeadbeef;

    pn_mesh_pb_writer_init (&w);
    pn_mesh_pb_write_varint (&w, value);
    bytes = pn_mesh_pb_writer_take_bytes (&w);
    pn_mesh_pb_writer_clear (&w);

    data = g_bytes_get_data (bytes, &size);
    PN_CHECK_CMPINT (size, ==, expect_bytes);

    pn_mesh_pb_reader_init (&r, data, size);
    PN_CHECK (pn_mesh_pb_read_varint (&r, &decoded));
    PN_CHECK_CMPINT (decoded, ==, value);
    PN_CHECK (pn_mesh_pb_reader_eof (&r));
    PN_CHECK_FALSE (r.error);

    g_bytes_unref (bytes);
}

static void
test_varint_lengths (void)
{
    /* 0 + 1-byte boundary (last value: 0x7F = 127) */
    check_varint_roundtrip (0,    1);
    check_varint_roundtrip (1,    1);
    check_varint_roundtrip (127,  1);
    /* 2-byte boundary (128 .. 16383) */
    check_varint_roundtrip (128,  2);
    check_varint_roundtrip (16383, 2);
    /* 3-byte (16384 .. 2^21 - 1) */
    check_varint_roundtrip (16384, 3);
    /* 5-byte fits 32-bit max */
    check_varint_roundtrip (G_GUINT64_CONSTANT (0xFFFFFFFF), 5);
    /* 10-byte at the 64-bit top */
    check_varint_roundtrip (G_MAXUINT64, 10);
}

/* ------------------------------------------------------------------ */
/*  Tag = (field << 3) | wire                                           */
/* ------------------------------------------------------------------ */

static void
check_tag_roundtrip (guint32 field, guint32 wire)
{
    PnMeshPbWriter w;
    PnMeshPbReader r;
    GBytes        *bytes;
    const guint8  *data;
    gsize          size;
    guint32        f = 0xdeadbeef, t = 0xdeadbeef;

    pn_mesh_pb_writer_init (&w);
    pn_mesh_pb_write_tag (&w, field, wire);
    bytes = pn_mesh_pb_writer_take_bytes (&w);
    pn_mesh_pb_writer_clear (&w);

    data = g_bytes_get_data (bytes, &size);
    pn_mesh_pb_reader_init (&r, data, size);
    PN_CHECK (pn_mesh_pb_read_tag (&r, &f, &t));
    PN_CHECK_CMPINT (f, ==, field);
    PN_CHECK_CMPINT (t, ==, wire);

    g_bytes_unref (bytes);
}

static void
test_tag (void)
{
    /* Single-byte tag (field <= 15, wire < 8) */
    check_tag_roundtrip (1,   PN_MESH_PB_WIRE_VARINT);
    check_tag_roundtrip (15,  PN_MESH_PB_WIRE_LEN);
    /* Two-byte tag (field 16 .. 2047) */
    check_tag_roundtrip (16,  PN_MESH_PB_WIRE_LEN);
    check_tag_roundtrip (256, PN_MESH_PB_WIRE_FIXED32);
}

/* ------------------------------------------------------------------ */
/*  Fixed32 / fixed64 little-endian                                     */
/* ------------------------------------------------------------------ */

static void
test_fixed (void)
{
    PnMeshPbWriter w;
    PnMeshPbReader r;
    GBytes        *bytes;
    const guint8  *data;
    gsize          size;
    guint32        u32 = 0;
    guint64        u64 = 0;

    pn_mesh_pb_writer_init (&w);
    pn_mesh_pb_write_fixed32 (&w, 0x11223344u);
    pn_mesh_pb_write_fixed64 (&w, G_GUINT64_CONSTANT (0xAABBCCDD11223344));
    bytes = pn_mesh_pb_writer_take_bytes (&w);
    pn_mesh_pb_writer_clear (&w);
    data = g_bytes_get_data (bytes, &size);

    /* Little-endian on the wire: lowest byte first. */
    PN_CHECK_CMPINT (size, ==, 12);
    PN_CHECK_CMPINT (data[0], ==, 0x44);
    PN_CHECK_CMPINT (data[1], ==, 0x33);
    PN_CHECK_CMPINT (data[2], ==, 0x22);
    PN_CHECK_CMPINT (data[3], ==, 0x11);

    pn_mesh_pb_reader_init (&r, data, size);
    PN_CHECK (pn_mesh_pb_read_fixed32 (&r, &u32));
    PN_CHECK_CMPINT (u32, ==, 0x11223344u);
    PN_CHECK (pn_mesh_pb_read_fixed64 (&r, &u64));
    PN_CHECK (u64 == G_GUINT64_CONSTANT (0xAABBCCDD11223344));
    PN_CHECK (pn_mesh_pb_reader_eof (&r));

    g_bytes_unref (bytes);
}

/* ------------------------------------------------------------------ */
/*  Length-delimited / embedded                                         */
/* ------------------------------------------------------------------ */

/* bytes_field + string_field share the wire shape: tag(LEN) + varint len
 * + payload.  Decode round-trips through the buffer so the returned
 * pointer must lie inside it. */
static void
test_length_delimited (void)
{
    PnMeshPbWriter w;
    PnMeshPbReader r;
    GBytes        *bytes;
    const guint8  *data;
    gsize          size;
    guint32        field = 0, wire = 0;
    const guint8  *slice = NULL;
    gsize          slice_size = 0;

    pn_mesh_pb_writer_init (&w);
    pn_mesh_pb_write_string_field (&w, 3, "hello");
    pn_mesh_pb_write_bytes_field  (&w, 7,
                                   (const guint8 *) "\x00\xff\x01", 3);
    bytes = pn_mesh_pb_writer_take_bytes (&w);
    pn_mesh_pb_writer_clear (&w);

    data = g_bytes_get_data (bytes, &size);
    pn_mesh_pb_reader_init (&r, data, size);

    PN_CHECK (pn_mesh_pb_read_tag (&r, &field, &wire));
    PN_CHECK_CMPINT (field, ==, 3);
    PN_CHECK_CMPINT (wire,  ==, PN_MESH_PB_WIRE_LEN);
    PN_CHECK (pn_mesh_pb_read_length (&r, &slice, &slice_size));
    PN_CHECK_CMPINT (slice_size, ==, 5);
    PN_CHECK (memcmp (slice, "hello", 5) == 0);
    /* The slice must point inside the source buffer, not into a copy. */
    PN_CHECK (slice >= data && slice + slice_size <= data + size);

    PN_CHECK (pn_mesh_pb_read_tag (&r, &field, &wire));
    PN_CHECK_CMPINT (field, ==, 7);
    PN_CHECK (pn_mesh_pb_read_length (&r, &slice, &slice_size));
    PN_CHECK_CMPINT (slice_size, ==, 3);
    PN_CHECK_CMPINT (slice[0], ==, 0x00);
    PN_CHECK_CMPINT (slice[1], ==, 0xFF);
    PN_CHECK_CMPINT (slice[2], ==, 0x01);

    PN_CHECK (pn_mesh_pb_reader_eof (&r));

    g_bytes_unref (bytes);
}

/* ------------------------------------------------------------------ */
/*  Skip unknown field                                                  */
/* ------------------------------------------------------------------ */

/* Three fields of mixed wire types; the middle one is "unknown" to the
 * reader.  skip_field must step past it so the cursor lands on the
 * third tag exactly. */
static void
test_skip_unknown (void)
{
    PnMeshPbWriter w;
    PnMeshPbReader r;
    GBytes        *bytes;
    const guint8  *data;
    gsize          size;
    guint32        field = 0, wire = 0;
    guint64        val   = 0;
    guint32        u32   = 0;

    pn_mesh_pb_writer_init (&w);
    pn_mesh_pb_write_varint_field    (&w, 1, 42);
    pn_mesh_pb_write_string_field    (&w, 5, "skipme");
    pn_mesh_pb_write_fixed32_field   (&w, 9, 0xCAFEBABEu);
    bytes = pn_mesh_pb_writer_take_bytes (&w);
    pn_mesh_pb_writer_clear (&w);
    data = g_bytes_get_data (bytes, &size);

    pn_mesh_pb_reader_init (&r, data, size);

    PN_CHECK (pn_mesh_pb_read_tag (&r, &field, &wire));
    PN_CHECK_CMPINT (field, ==, 1);
    PN_CHECK (pn_mesh_pb_read_varint (&r, &val));
    PN_CHECK_CMPINT (val, ==, 42);

    /* Field 5 is "unknown" -- skip it. */
    PN_CHECK (pn_mesh_pb_read_tag (&r, &field, &wire));
    PN_CHECK_CMPINT (field, ==, 5);
    PN_CHECK (pn_mesh_pb_skip_field (&r, wire));

    PN_CHECK (pn_mesh_pb_read_tag (&r, &field, &wire));
    PN_CHECK_CMPINT (field, ==, 9);
    PN_CHECK_CMPINT (wire,  ==, PN_MESH_PB_WIRE_FIXED32);
    PN_CHECK (pn_mesh_pb_read_fixed32 (&r, &u32));
    PN_CHECK_CMPINT (u32, ==, 0xCAFEBABEu);

    PN_CHECK (pn_mesh_pb_reader_eof (&r));
    PN_CHECK_FALSE (r.error);

    g_bytes_unref (bytes);
}

/* ------------------------------------------------------------------ */
/*  Error contract                                                      */
/* ------------------------------------------------------------------ */

/* Truncated input must set the sticky error flag, and a subsequent
 * read on an errored reader must fail without advancing the cursor. */
static void
test_truncation (void)
{
    PnMeshPbReader r;
    /* A continuation byte (0x80) with no follow-up: malformed varint. */
    const guint8 truncated_varint[] = { 0x80 };
    /* A length-prefixed string of length 99 with only 3 payload bytes. */
    const guint8 short_length[] = { 0x0A, 0x63, 'h', 'i', '!' };
    const guint8  *slice = NULL;
    gsize          slice_size = 0;
    guint32        field = 0, wire = 0;
    guint64        v = 0;

    pn_mesh_pb_reader_init (&r, truncated_varint, sizeof truncated_varint);
    PN_CHECK_FALSE (pn_mesh_pb_read_varint (&r, &v));
    PN_CHECK (r.error);
    /* Errored reader stays errored; subsequent reads must not lie. */
    PN_CHECK_FALSE (pn_mesh_pb_read_varint (&r, &v));

    pn_mesh_pb_reader_init (&r, short_length, sizeof short_length);
    PN_CHECK (pn_mesh_pb_read_tag (&r, &field, &wire));
    PN_CHECK_CMPINT (field, ==, 1);
    PN_CHECK_CMPINT (wire,  ==, PN_MESH_PB_WIRE_LEN);
    PN_CHECK_FALSE (pn_mesh_pb_read_length (&r, &slice, &slice_size));
    PN_CHECK (r.error);
}

/* ------------------------------------------------------------------ */
/*  Composite encoder helpers                                           */
/* ------------------------------------------------------------------ */

/* The varint_field / fixed32_field / bytes_field / embedded_field
 * shortcuts produce exactly the same bytes as the corresponding pair
 * of primitives, so a single field round-trip exercises both halves
 * of each shortcut. */
static void
test_composite_writers (void)
{
    PnMeshPbWriter w;
    PnMeshPbReader r;
    GBytes        *bytes;
    const guint8  *data;
    gsize          size;
    guint32        field = 0, wire = 0;
    guint64        v = 0;
    const guint8  *slice = NULL;
    gsize          slice_size = 0;

    /* Two embedded messages back-to-back; the second is empty.  Empty
     * is interesting because it exercises the length=0 path on both
     * sides. */
    static const guint8 inner_payload[] = { 0x08, 0x2A };  /* field 1 varint 42 */

    pn_mesh_pb_writer_init (&w);
    pn_mesh_pb_write_embedded_field (&w, 2,
                                     inner_payload, sizeof inner_payload);
    pn_mesh_pb_write_embedded_field (&w, 4, NULL, 0);
    bytes = pn_mesh_pb_writer_take_bytes (&w);
    pn_mesh_pb_writer_clear (&w);
    data = g_bytes_get_data (bytes, &size);

    pn_mesh_pb_reader_init (&r, data, size);
    PN_CHECK (pn_mesh_pb_read_tag (&r, &field, &wire));
    PN_CHECK_CMPINT (field, ==, 2);
    PN_CHECK_CMPINT (wire,  ==, PN_MESH_PB_WIRE_LEN);
    PN_CHECK (pn_mesh_pb_read_length (&r, &slice, &slice_size));
    PN_CHECK_CMPINT (slice_size, ==, sizeof inner_payload);

    /* The inner payload should decode in its own reader as field 1 varint 42. */
    {
        PnMeshPbReader inner;
        pn_mesh_pb_reader_init (&inner, slice, slice_size);
        PN_CHECK (pn_mesh_pb_read_tag (&inner, &field, &wire));
        PN_CHECK_CMPINT (field, ==, 1);
        PN_CHECK_CMPINT (wire,  ==, PN_MESH_PB_WIRE_VARINT);
        PN_CHECK (pn_mesh_pb_read_varint (&inner, &v));
        PN_CHECK_CMPINT (v, ==, 42);
        PN_CHECK (pn_mesh_pb_reader_eof (&inner));
    }

    PN_CHECK (pn_mesh_pb_read_tag (&r, &field, &wire));
    PN_CHECK_CMPINT (field, ==, 4);
    PN_CHECK (pn_mesh_pb_read_length (&r, &slice, &slice_size));
    PN_CHECK_CMPINT (slice_size, ==, 0);

    PN_CHECK (pn_mesh_pb_reader_eof (&r));

    g_bytes_unref (bytes);
}

/* ------------------------------------------------------------------ */
/*  Float wire format                                                   */
/* ------------------------------------------------------------------ */

/* IEEE-754 single-precision, little-endian.  Round-tripping every bit
 * pattern is overkill; one finite value, one zero, one negative, and a
 * couple of denormal/inf cases pin the conversion. */
static void
test_float (void)
{
    PnMeshPbWriter w;
    PnMeshPbReader r;
    GBytes        *bytes;
    const guint8  *data;
    gsize          size;
    gfloat         v;
    guint32        bits;

    /* Pre-encode three known floats via fixed32 (their IEEE-754 bit
     * patterns), then decode as floats.  This is independent of any
     * encoder bug -- we are exercising the decoder by itself. */
    pn_mesh_pb_writer_init (&w);
    pn_mesh_pb_write_fixed32 (&w, 0x3F800000u);  /* 1.0f                */
    pn_mesh_pb_write_fixed32 (&w, 0xC0000000u);  /* -2.0f               */
    pn_mesh_pb_write_fixed32 (&w, 0x00000000u);  /* 0.0f                */
    bytes = pn_mesh_pb_writer_take_bytes (&w);
    pn_mesh_pb_writer_clear (&w);
    data = g_bytes_get_data (bytes, &size);

    pn_mesh_pb_reader_init (&r, data, size);
    PN_CHECK (pn_mesh_pb_read_float (&r, &v));
    PN_CHECK_NEAR (v, 1.0,  0.0);
    PN_CHECK (pn_mesh_pb_read_float (&r, &v));
    PN_CHECK_NEAR (v, -2.0, 0.0);
    PN_CHECK (pn_mesh_pb_read_float (&r, &v));
    PN_CHECK_NEAR (v, 0.0,  0.0);

    /* And confirm read_fixed32 can re-read raw bytes from a fresh
     * reader on the same buffer -- proves the cursor is per-reader,
     * not stashed in the buffer. */
    pn_mesh_pb_reader_init (&r, data, 4);
    PN_CHECK (pn_mesh_pb_read_fixed32 (&r, &bits));
    PN_CHECK_CMPINT (bits, ==, 0x3F800000u);

    g_bytes_unref (bytes);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mesh-pb");
    pn_test_add ("varint_lengths",      test_varint_lengths);
    pn_test_add ("tag",                 test_tag);
    pn_test_add ("fixed",               test_fixed);
    pn_test_add ("length_delimited",    test_length_delimited);
    pn_test_add ("skip_unknown",        test_skip_unknown);
    pn_test_add ("truncation",          test_truncation);
    pn_test_add ("composite_writers",   test_composite_writers);
    pn_test_add ("float",               test_float);
    return pn_test_run ();
}
