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

/* Unit tests for the Meshtastic serial frame accumulator.  The frame
 * format on the wire is `0x94 0xC3 LEN_MSB LEN_LSB <payload>` and
 * serial reads return bytes at arbitrary boundaries, so the
 * accumulator has to: scan for the magic byte-by-byte; buffer across
 * feeds when a header or payload is split; survive junk before the
 * first magic; recover from a coincidental 0x94 0xC3 inside a payload
 * by claiming an absurd length.  These tests exercise those paths on
 * synthetic byte streams -- no real serial port, no protobuf, no
 * threading.  Stays purely headless. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "pntest.h"
#include "pn-mesh-serial.h"

/* Build the on-wire bytes for one frame: 0x94 0xC3 + 2-byte BE length
 * + payload.  Returns a freshly malloc'd buffer; caller g_free()s. */
static guint8 *
make_frame (const guint8 *payload, gsize size, gsize *out_total)
{
    guint8 *out = g_malloc (4 + size);
    out[0] = 0x94;
    out[1] = 0xC3;
    out[2] = (guint8) ((size >> 8) & 0xFF);
    out[3] = (guint8)  (size       & 0xFF);
    if (size > 0)
        memcpy (out + 4, payload, size);
    *out_total = 4 + size;
    return out;
}

/* Pull GBytes from the reader and assert its payload matches @expect.
 * Returns the took-frame for further inspection (or NULL if none). */
static GBytes *
take_and_check (PnMeshFrameReader *r,
                const guint8 *expect, gsize expect_size)
{
    GBytes       *frame = pn_mesh_frame_reader_take (r);
    const guint8 *data;
    gsize         size;

    PN_CHECK (frame != NULL);
    if (frame == NULL)
        return NULL;

    data = g_bytes_get_data (frame, &size);
    PN_CHECK_CMPINT (size, ==, expect_size);
    PN_CHECK (memcmp (data, expect, expect_size) == 0);
    return frame;
}

/* ------------------------------------------------------------------ */
/*  Clean back-to-back frames                                          */
/* ------------------------------------------------------------------ */

/* Three full frames fed as one big buffer -- the simplest case.
 * Reader must hand back all three in feed order and report NULL
 * afterwards. */
static void
test_three_clean_frames (void)
{
    PnMeshFrameReader *r = pn_mesh_frame_reader_new ();
    guint8            *f1, *f2, *f3;
    gsize              s1, s2, s3;
    GByteArray        *all = g_byte_array_new ();
    GBytes            *got;

    f1 = make_frame ((const guint8 *) "alpha", 5, &s1);
    f2 = make_frame ((const guint8 *) "be",    2, &s2);
    f3 = make_frame ((const guint8 *) "gamma!", 6, &s3);

    g_byte_array_append (all, f1, (guint) s1);
    g_byte_array_append (all, f2, (guint) s2);
    g_byte_array_append (all, f3, (guint) s3);
    g_free (f1); g_free (f2); g_free (f3);

    pn_mesh_frame_reader_feed (r, all->data, all->len);

    got = take_and_check (r, (const guint8 *) "alpha",  5); g_bytes_unref (got);
    got = take_and_check (r, (const guint8 *) "be",     2); g_bytes_unref (got);
    got = take_and_check (r, (const guint8 *) "gamma!", 6); g_bytes_unref (got);
    PN_CHECK (pn_mesh_frame_reader_take (r) == NULL);

    g_byte_array_unref (all);
    pn_mesh_frame_reader_free (r);
}

/* ------------------------------------------------------------------ */
/*  Split feeds                                                         */
/* ------------------------------------------------------------------ */

/* Real serial reads return bytes at arbitrary boundaries.  Feed one
 * frame byte by byte; the reader must accumulate and yield exactly
 * one frame on the final byte. */
static void
test_byte_by_byte (void)
{
    PnMeshFrameReader *r = pn_mesh_frame_reader_new ();
    guint8            *frame;
    gsize              total;
    gsize              i;
    GBytes            *got;

    frame = make_frame ((const guint8 *) "split-stream", 12, &total);

    /* Every feed except the last must yield NULL; the very last byte
     * completes the frame and the take must succeed. */
    for (i = 0; i < total - 1; i++)
    {
        pn_mesh_frame_reader_feed (r, frame + i, 1);
        PN_CHECK (pn_mesh_frame_reader_take (r) == NULL);
    }
    pn_mesh_frame_reader_feed (r, frame + total - 1, 1);
    got = take_and_check (r, (const guint8 *) "split-stream", 12);
    g_bytes_unref (got);
    PN_CHECK (pn_mesh_frame_reader_take (r) == NULL);

    g_free (frame);
    pn_mesh_frame_reader_free (r);
}

/* ------------------------------------------------------------------ */
/*  Junk before magic                                                   */
/* ------------------------------------------------------------------ */

/* USB CDC-ACM ports sometimes emit stray bytes around the device
 * resetting; the accumulator must skip junk preceding the first
 * 0x94 0xC3 without losing the frame. */
static void
test_junk_before_magic (void)
{
    PnMeshFrameReader *r = pn_mesh_frame_reader_new ();
    static const guint8 junk[] = { 0x00, 0x12, 0x94, 0x42, 0xFF };  /* lone 0x94 in there too */
    guint8            *frame;
    gsize              total;
    GBytes            *got;

    frame = make_frame ((const guint8 *) "after", 5, &total);

    pn_mesh_frame_reader_feed (r, junk, sizeof junk);
    PN_CHECK (pn_mesh_frame_reader_take (r) == NULL);
    pn_mesh_frame_reader_feed (r, frame, total);
    got = take_and_check (r, (const guint8 *) "after", 5);
    g_bytes_unref (got);
    PN_CHECK (pn_mesh_frame_reader_take (r) == NULL);

    g_free (frame);
    pn_mesh_frame_reader_free (r);
}

/* ------------------------------------------------------------------ */
/*  Lone 0x94 at feed boundary                                          */
/* ------------------------------------------------------------------ */

/* Magic is split across two feeds: the 0x94 lands in feed #1 alone,
 * the 0xC3 + length + payload arrives in feed #2.  Reader must hold
 * the lone 0x94 until the 0xC3 completes the magic. */
static void
test_magic_split (void)
{
    PnMeshFrameReader *r = pn_mesh_frame_reader_new ();
    static const guint8 first_part[]  = { 0x94 };
    static const guint8 second_part[] = { 0xC3, 0x00, 0x03, 'X', 'Y', 'Z' };
    GBytes            *got;

    pn_mesh_frame_reader_feed (r, first_part, sizeof first_part);
    PN_CHECK (pn_mesh_frame_reader_take (r) == NULL);
    pn_mesh_frame_reader_feed (r, second_part, sizeof second_part);
    got = take_and_check (r, (const guint8 *) "XYZ", 3);
    g_bytes_unref (got);

    pn_mesh_frame_reader_free (r);
}

/* ------------------------------------------------------------------ */
/*  Empty payload                                                       */
/* ------------------------------------------------------------------ */

/* A header with length=0 is a legitimate (if useless) frame; the
 * reader must yield an empty GBytes, not NULL.  Some ToRadio /
 * FromRadio messages from a fresh device can claim length-0 acks
 * around boot, so the contract has to hold. */
static void
test_zero_length_frame (void)
{
    PnMeshFrameReader *r = pn_mesh_frame_reader_new ();
    static const guint8 hdr[] = { 0x94, 0xC3, 0x00, 0x00 };
    GBytes            *got;
    gsize              size = 99;

    pn_mesh_frame_reader_feed (r, hdr, sizeof hdr);
    got = pn_mesh_frame_reader_take (r);
    PN_CHECK (got != NULL);
    g_bytes_get_data (got, &size);
    PN_CHECK_CMPINT (size, ==, 0);
    g_bytes_unref (got);

    PN_CHECK (pn_mesh_frame_reader_take (r) == NULL);
    pn_mesh_frame_reader_free (r);
}

/* ------------------------------------------------------------------ */
/*  Oversized length recovery                                           */
/* ------------------------------------------------------------------ */

/* A claimed payload size beyond PN_MESH_FRAME_MAX_PAYLOAD almost
 * certainly means we synced on a coincidental 0x94 0xC3 inside a
 * payload (or junk).  The accumulator must drop those two bytes and
 * keep scanning so a real frame that follows is still recovered. */
static void
test_oversized_recovery (void)
{
    PnMeshFrameReader *r = pn_mesh_frame_reader_new ();
    /* Bogus header claims 0xFFFF payload bytes -- way over the cap. */
    static const guint8 bogus[] = { 0x94, 0xC3, 0xFF, 0xFF };
    guint8            *real_frame;
    gsize              total;
    GBytes            *got;

    real_frame = make_frame ((const guint8 *) "recover", 7, &total);

    pn_mesh_frame_reader_feed (r, bogus, sizeof bogus);
    pn_mesh_frame_reader_feed (r, real_frame, total);
    got = take_and_check (r, (const guint8 *) "recover", 7);
    g_bytes_unref (got);

    g_free (real_frame);
    pn_mesh_frame_reader_free (r);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mesh-frame");
    pn_test_add ("three_clean_frames",  test_three_clean_frames);
    pn_test_add ("byte_by_byte",        test_byte_by_byte);
    pn_test_add ("junk_before_magic",   test_junk_before_magic);
    pn_test_add ("magic_split",         test_magic_split);
    pn_test_add ("zero_length_frame",   test_zero_length_frame);
    pn_test_add ("oversized_recovery",  test_oversized_recovery);
    return pn_test_run ();
}
