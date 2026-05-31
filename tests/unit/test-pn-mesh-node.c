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

/* Characterization tests for the Meshtastic *node*'s private serial
 * stack (TODO #37.1).  The node (pn-meshtastic.c) carries its OWN copy
 * of the serial open, the sysfs USB scan, the 0x94 0xC3 frame codec and
 * the protobuf primitives -- divergent from, but parallel to, the
 * Devices-dialog stack (pn-mesh-serial.c / pn-mesh-discover.c, already
 * covered by test-pn-mesh-frame / -inuse / -pb).  Before #37.2/#37.3
 * converge the two onto one shared transport, these tests pin the node's
 * CURRENT byte-level and open-time behaviour so the convergence can be
 * proven a no-op on the live-messaging hot path.
 *
 * The functions under test are static, so we compile the node's
 * translation unit straight into the test (the shell- and
 * host-monitoring-node tests do the same with their out-of-lib nodes;
 * here libpipnode-core links as a shared object, so the test's own
 * copies of the node's few exported symbols win at link time and we only
 * ever call the static helpers -- the GObject is never instantiated).
 *
 * Everything stays headless: the frame/protobuf cases are pure byte
 * vectors, the open cases run against an anonymous pseudo-terminal, and
 * the discovery cases use a synthetic /sys-like tree under a tmpdir
 * (resolving to the always-present /dev/null).  No real radio, no
 * network, no display. */

/* posix_openpt/grantpt/unlockpt/ptsname need the XSI feature macro, and
 * the build does not set one globally; declare it before any libc header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "pntest.h"

/* The node's translation unit, static helpers and all. */
#include "pn-meshtastic.c"

/* ================================================================== */
/*  Frame TX: envelope + outbound message construction                 */
/* ================================================================== */

/* Assert that @frame's bytes match @expect exactly. */
static void
check_frame_bytes (GBytes *frame, const guint8 *expect, gsize expect_len)
{
    const guint8 *data;
    gsize         len = 0;

    PN_CHECK (frame != NULL);
    if (frame == NULL)
        return;
    data = g_bytes_get_data (frame, &len);
    PN_CHECK_CMPINT (len, ==, expect_len);
    if (len == expect_len)
        PN_CHECK (memcmp (data, expect, expect_len) == 0);
}

/* frame_wrap prepends 0x94 0xC3 + big-endian 16-bit payload length,
 * then the payload verbatim. */
static void
test_frame_wrap_envelope (void)
{
    static const guint8 payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };
    static const guint8 expect[]  = { 0x94, 0xC3, 0x00, 0x04,
                                      0xDE, 0xAD, 0xBE, 0xEF };
    GBytes *frame = frame_wrap (payload, sizeof payload);
    check_frame_bytes (frame, expect, sizeof expect);
    g_bytes_unref (frame);

    /* A zero-length payload still gets the 4-byte header. */
    static const guint8 expect0[] = { 0x94, 0xC3, 0x00, 0x00 };
    frame = frame_wrap (NULL, 0);
    check_frame_bytes (frame, expect0, sizeof expect0);
    g_bytes_unref (frame);
}

/* build_text_frame encodes ToRadio{ packet=MeshPacket{ to=BCAST,
 * channel=ch, decoded=Data{ portnum=TEXT, payload=text } } } and wraps
 * it.  Pin the exact wire bytes for a known (text, channel). */
static void
test_text_frame_bytes (void)
{
    /* "hi" on channel 0:
     *   Data     = 08 01            (portnum=1)
     *              12 02 68 69      (payload="hi")
     *   MeshPkt  = 15 FF FF FF FF   (to=0xFFFFFFFF, fixed32 LE)
     *              18 00            (channel=0)
     *              22 06 <Data>     (decoded, len 6)
     *   ToRadio  = 0A 0F <MeshPkt>  (packet, len 15)
     *   Frame    = 94 C3 00 11 <ToRadio>   (payload len 17) */
    static const guint8 expect[] = {
        0x94, 0xC3, 0x00, 0x11,
        0x0A, 0x0F,
        0x15, 0xFF, 0xFF, 0xFF, 0xFF,
        0x18, 0x00,
        0x22, 0x06,
        0x08, 0x01,
        0x12, 0x02, 0x68, 0x69,
    };
    GBytes *frame = build_text_frame ("hi", 0);
    check_frame_bytes (frame, expect, sizeof expect);
    g_bytes_unref (frame);
}

/* A non-zero channel index rides as a plain varint in field 3. */
static void
test_text_frame_channel_index (void)
{
    /* "x" on channel 2 -- only the channel varint and lengths change. */
    static const guint8 expect[] = {
        0x94, 0xC3, 0x00, 0x10,
        0x0A, 0x0E,
        0x15, 0xFF, 0xFF, 0xFF, 0xFF,
        0x18, 0x02,
        0x22, 0x05,
        0x08, 0x01,
        0x12, 0x01, 0x78,
    };
    GBytes *frame = build_text_frame ("x", 2);
    check_frame_bytes (frame, expect, sizeof expect);
    g_bytes_unref (frame);
}

/* build_want_config_frame is ToRadio{ want_config_id=1 } -- field 3
 * varint = 1. */
static void
test_want_config_frame_bytes (void)
{
    static const guint8 expect[] = { 0x94, 0xC3, 0x00, 0x02, 0x18, 0x01 };
    GBytes *frame = build_want_config_frame ();
    check_frame_bytes (frame, expect, sizeof expect);
    g_bytes_unref (frame);
}

/* build_heartbeat_frame is ToRadio{ heartbeat=Heartbeat{} } -- field 7,
 * length-delimited, zero payload bytes. */
static void
test_heartbeat_frame_bytes (void)
{
    static const guint8 expect[] = { 0x94, 0xC3, 0x00, 0x02, 0x3A, 0x00 };
    GBytes *frame = build_heartbeat_frame ();
    check_frame_bytes (frame, expect, sizeof expect);
    g_bytes_unref (frame);
}

/* ================================================================== */
/*  Protobuf primitives                                                */
/* ================================================================== */

/* The varint encoder/decoder are the spine of every frame above; pin a
 * multi-byte value both ways. */
static void
test_pb_varint_roundtrip (void)
{
    static const struct { guint64 value; gsize len; guint8 bytes[4]; } vec[] = {
        { 0,    1, { 0x00 } },
        { 1,    1, { 0x01 } },
        { 127,  1, { 0x7F } },
        { 128,  2, { 0x80, 0x01 } },
        { 300,  2, { 0xAC, 0x02 } },
        { 16384,3, { 0x80, 0x80, 0x01 } },
    };

    for (gsize i = 0; i < G_N_ELEMENTS (vec); i++)
    {
        GByteArray *out = g_byte_array_new ();
        pb_encode_varint (out, vec[i].value);
        PN_CHECK_CMPINT (out->len, ==, vec[i].len);
        if (out->len == vec[i].len)
            PN_CHECK (memcmp (out->data, vec[i].bytes, vec[i].len) == 0);

        gsize   pos = 0;
        guint64 got = 0;
        PN_CHECK (pb_decode_varint (out->data, out->len, &pos, &got));
        PN_CHECK (got == vec[i].value);
        PN_CHECK_CMPINT (pos, ==, vec[i].len);

        g_byte_array_free (out, TRUE);
    }
}

/* fixed32 is little-endian; pin the byte order the BCAST address rides
 * on. */
static void
test_pb_fixed32_roundtrip (void)
{
    GByteArray *out = g_byte_array_new ();
    pb_encode_fixed32 (out, 0x01020304u);
    static const guint8 expect[] = { 0x04, 0x03, 0x02, 0x01 };
    PN_CHECK_CMPINT (out->len, ==, 4);
    PN_CHECK (memcmp (out->data, expect, 4) == 0);

    gsize   pos = 0;
    guint32 got = 0;
    PN_CHECK (pb_decode_fixed32 (out->data, out->len, &pos, &got));
    PN_CHECK (got == 0x01020304u);
    PN_CHECK_CMPINT (pos, ==, 4);

    g_byte_array_free (out, TRUE);
}

/* ================================================================== */
/*  Frame RX: the in-place accumulator (frame_extract)                 */
/* ================================================================== */

/* Collector callback: copy each extracted payload into a GPtrArray of
 * GBytes for inspection. */
static void
collect_payload (const guint8 *payload, gsize len, gpointer ud)
{
    GPtrArray *out = ud;
    g_ptr_array_add (out, g_bytes_new (payload, len));
}

/* Append one on-wire frame (header + payload) to @rx. */
static void
append_frame (GByteArray *rx, const guint8 *payload, gsize len)
{
    guint8 hdr[4] = { 0x94, 0xC3,
                      (guint8) ((len >> 8) & 0xFF),
                      (guint8) (len & 0xFF) };
    g_byte_array_append (rx, hdr, 4);
    if (len > 0)
        g_byte_array_append (rx, payload, (guint) len);
}

/* Two back-to-back frames in one buffer come out in feed order, and the
 * buffer is fully drained afterwards. */
static void
test_extract_two_clean (void)
{
    GByteArray *rx  = g_byte_array_new ();
    GPtrArray  *got = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

    append_frame (rx, (const guint8 *) "alpha", 5);
    append_frame (rx, (const guint8 *) "be",    2);

    frame_extract (rx, collect_payload, got);

    PN_CHECK_CMPINT (got->len, ==, 2);
    PN_CHECK_CMPINT (rx->len,  ==, 0);
    if (got->len == 2)
    {
        gsize n = 0;
        const guint8 *p = g_bytes_get_data (g_ptr_array_index (got, 0), &n);
        PN_CHECK (n == 5 && memcmp (p, "alpha", 5) == 0);
        p = g_bytes_get_data (g_ptr_array_index (got, 1), &n);
        PN_CHECK (n == 2 && memcmp (p, "be", 2) == 0);
    }

    g_ptr_array_unref (got);
    g_byte_array_unref (rx);
}

/* A frame split across two feeds: the first feed holds only part of the
 * payload (no complete frame yet), and the partial bytes are preserved
 * in @rx for the next feed, which completes it. */
static void
test_extract_split_payload (void)
{
    GByteArray *rx  = g_byte_array_new ();
    GPtrArray  *got = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

    /* Header + 3 of 6 payload bytes. */
    static const guint8 part1[] = { 0x94, 0xC3, 0x00, 0x06, 'a', 'b', 'c' };
    g_byte_array_append (rx, part1, sizeof part1);
    frame_extract (rx, collect_payload, got);
    PN_CHECK_CMPINT (got->len, ==, 0);   /* nothing complete yet */
    PN_CHECK_CMPINT (rx->len,  ==, sizeof part1);  /* preserved verbatim */

    /* Remaining 3 payload bytes. */
    static const guint8 part2[] = { 'd', 'e', 'f' };
    g_byte_array_append (rx, part2, sizeof part2);
    frame_extract (rx, collect_payload, got);
    PN_CHECK_CMPINT (got->len, ==, 1);
    PN_CHECK_CMPINT (rx->len,  ==, 0);
    if (got->len == 1)
    {
        gsize n = 0;
        const guint8 *p = g_bytes_get_data (g_ptr_array_index (got, 0), &n);
        PN_CHECK (n == 6 && memcmp (p, "abcdef", 6) == 0);
    }

    g_ptr_array_unref (got);
    g_byte_array_unref (rx);
}

/* Junk bytes (including a lone 0x94 not followed by 0xC3) before the
 * first real magic are skipped without losing the following frame. */
static void
test_extract_junk_before_magic (void)
{
    GByteArray *rx  = g_byte_array_new ();
    GPtrArray  *got = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

    static const guint8 junk[] = { 0x00, 0x94, 0x42, 0xFF };  /* lone 0x94 */
    g_byte_array_append (rx, junk, sizeof junk);
    append_frame (rx, (const guint8 *) "after", 5);

    frame_extract (rx, collect_payload, got);

    PN_CHECK_CMPINT (got->len, ==, 1);
    PN_CHECK_CMPINT (rx->len,  ==, 0);
    if (got->len == 1)
    {
        gsize n = 0;
        const guint8 *p = g_bytes_get_data (g_ptr_array_index (got, 0), &n);
        PN_CHECK (n == 5 && memcmp (p, "after", 5) == 0);
    }

    g_ptr_array_unref (got);
    g_byte_array_unref (rx);
}

/* CHARACTERIZATION OF A DIVERGENCE: the node's frame_extract treats a
 * length-zero header as desync and skips it (unlike the dialog reader,
 * which yields an empty frame -- see test-pn-mesh-frame's
 * zero_length_frame).  A real frame following a zero-length header is
 * still recovered.  Pinning this so #37.3 makes the reconciliation a
 * deliberate choice, not an accident. */
static void
test_extract_zero_length_is_skipped (void)
{
    GByteArray *rx  = g_byte_array_new ();
    GPtrArray  *got = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

    static const guint8 zero_hdr[] = { 0x94, 0xC3, 0x00, 0x00 };
    g_byte_array_append (rx, zero_hdr, sizeof zero_hdr);
    append_frame (rx, (const guint8 *) "real", 4);

    frame_extract (rx, collect_payload, got);

    PN_CHECK_CMPINT (got->len, ==, 1);   /* zero-length yields nothing */
    if (got->len == 1)
    {
        gsize n = 0;
        const guint8 *p = g_bytes_get_data (g_ptr_array_index (got, 0), &n);
        PN_CHECK (n == 4 && memcmp (p, "real", 4) == 0);
    }

    g_ptr_array_unref (got);
    g_byte_array_unref (rx);
}

/* A coincidental 0x94 0xC3 that claims a payload larger than
 * PN_MESH_FRAME_MAX is desync: drop the sync byte and keep scanning so a
 * genuine frame that follows is still recovered. */
static void
test_extract_oversized_recovery (void)
{
    GByteArray *rx  = g_byte_array_new ();
    GPtrArray  *got = g_ptr_array_new_with_free_func ((GDestroyNotify) g_bytes_unref);

    static const guint8 bogus[] = { 0x94, 0xC3, 0xFF, 0xFF };  /* claims 65535 */
    g_byte_array_append (rx, bogus, sizeof bogus);
    append_frame (rx, (const guint8 *) "recover", 7);

    frame_extract (rx, collect_payload, got);

    PN_CHECK_CMPINT (got->len, ==, 1);
    if (got->len == 1)
    {
        gsize n = 0;
        const guint8 *p = g_bytes_get_data (g_ptr_array_index (got, 0), &n);
        PN_CHECK (n == 7 && memcmp (p, "recover", 7) == 0);
    }

    g_ptr_array_unref (got);
    g_byte_array_unref (rx);
}

/* ================================================================== */
/*  Serial open: flock + TIOCEXCL exclusivity, O_CLOEXEC, raw 8N1       */
/* ================================================================== */

/* Allocate an anonymous pseudo-terminal and return the master fd, with
 * the slave path in @slave_out (caller g_free()s).  Returns -1 on any
 * failure (in which case the open-related cases self-skip). */
static int
open_pty (gchar **slave_out)
{
    int master = posix_openpt (O_RDWR | O_NOCTTY);
    if (master < 0)
        return -1;
    if (grantpt (master) != 0 || unlockpt (master) != 0)
    {
        close (master);
        return -1;
    }
    *slave_out = g_strdup (ptsname (master));
    return master;
}

/* serial_open configures raw 115200 8N1 and keeps the fd close-on-exec. */
static void
test_serial_open_termios_cloexec (void)
{
    gchar *path   = NULL;
    int    master = open_pty (&path);
    PN_CHECK (master >= 0);
    if (master < 0)
        return;

    int fd = serial_open (path);
    PN_CHECK (fd >= 0);
    if (fd >= 0)
    {
        /* O_CLOEXEC survived to the fd. */
        int flags = fcntl (fd, F_GETFD);
        PN_CHECK (flags >= 0 && (flags & FD_CLOEXEC));

        /* Raw 115200 8N1. */
        struct termios t;
        PN_CHECK (tcgetattr (fd, &t) == 0);
        PN_CHECK (cfgetispeed (&t) == B115200);
        PN_CHECK (cfgetospeed (&t) == B115200);
        PN_CHECK ((t.c_cflag & CSIZE) == CS8);
        PN_CHECK ((t.c_cflag & PARENB) == 0);
        PN_CHECK ((t.c_cflag & CSTOPB) == 0);
        PN_CHECK ((t.c_lflag & (ICANON | ECHO)) == 0);  /* raw */

        close (fd);
    }

    close (master);
    g_free (path);
}

/* A second serial_open on a port the node already holds is refused with
 * EBUSY (flock LOCK_EX + TIOCEXCL).  This guarantee is what #37.2 must
 * preserve while ADDING the dialog's pre-open in-use guard. */
static void
test_serial_open_exclusive (void)
{
    gchar *path   = NULL;
    int    master = open_pty (&path);
    PN_CHECK (master >= 0);
    if (master < 0)
        return;

    int fd1 = serial_open (path);
    PN_CHECK (fd1 >= 0);
    if (fd1 >= 0)
    {
        errno = 0;
        int fd2 = serial_open (path);
        PN_CHECK (fd2 < 0);
        PN_CHECK_CMPINT (errno, ==, EBUSY);
        if (fd2 >= 0)
            close (fd2);
        close (fd1);
    }

    close (master);
    g_free (path);
}

/* A path that does not exist fails cleanly (-1), not a crash. */
static void
test_serial_open_missing (void)
{
    int fd = serial_open ("/dev/pn-mesh-node-does-not-exist");
    PN_CHECK (fd < 0);
    if (fd >= 0)
        close (fd);
}

/* ================================================================== */
/*  Discovery: known-device table + sysfs tty walk + list contract     */
/* ================================================================== */

/* Pin the VID:PID:label table that #37.3 must reconcile with the
 * dialog's list (and that #33's bare-CP2102 / #36's MM-ignore work keys
 * off).  A board added to the pip-mesh prototype but forgotten here
 * silently drops out of the device combo, so the exact set is the
 * contract. */
static void
test_known_devices_table (void)
{
    static const struct { const gchar *vid, *pid, *label; } expect[] = {
        { "10c4", "ea60", "Heltec V3"        },
        { "239a", "0029", "Tracker"          },
        { "239a", "8027", "SenseCAP Tracker" },
        { "239a", "8029", "Tracker"          },
    };

    PN_CHECK_CMPINT (G_N_ELEMENTS (known_devices), ==, G_N_ELEMENTS (expect));
    for (gsize i = 0; i < G_N_ELEMENTS (expect); i++)
    {
        PN_CHECK_CMPSTR (known_devices[i].vendor,  ==, expect[i].vid);
        PN_CHECK_CMPSTR (known_devices[i].product, ==, expect[i].pid);
        PN_CHECK_CMPSTR (known_devices[i].label,   ==, expect[i].label);
    }
}

/* Build the directory chain @rel under @root (slash-separated). */
static void
make_dirs (const gchar *root, const gchar *rel)
{
    gchar *full = g_build_filename (root, rel, NULL);
    g_mkdir_with_parents (full, 0755);
    g_free (full);
}

/* find_tty_under resolves the common one-level layout
 * <dev>/<intf>/tty/<ttyXXX>.  We name the leaf "null" so the
 * g_file_test("/dev/null") existence gate passes deterministically. */
static void
test_find_tty_one_level (void)
{
    gchar *root = g_dir_make_tmp ("pn-mesh-node-tty-XXXXXX", NULL);
    PN_CHECK (root != NULL);
    if (root == NULL)
        return;

    make_dirs (root, "intf0/tty/null");
    gchar *tty = find_tty_under (root);
    PN_CHECK_CMPSTR (tty, ==, "/dev/null");
    g_free (tty);

    gchar *intf = g_build_filename (root, "intf0", NULL);
    gchar *ttyd = g_build_filename (intf, "tty", NULL);
    gchar *leaf = g_build_filename (ttyd, "null", NULL);
    g_rmdir (leaf); g_rmdir (ttyd); g_rmdir (intf); g_rmdir (root);
    g_free (leaf); g_free (ttyd); g_free (intf);
    g_free (root);
}

/* The one-deeper nesting (devices like SenseCAP) is also resolved via
 * the function's recursive descent. */
static void
test_find_tty_nested (void)
{
    gchar *root = g_dir_make_tmp ("pn-mesh-node-tty2-XXXXXX", NULL);
    PN_CHECK (root != NULL);
    if (root == NULL)
        return;

    make_dirs (root, "intf0/sub/iface/tty/null");
    gchar *tty = find_tty_under (root);
    PN_CHECK_CMPSTR (tty, ==, "/dev/null");
    g_free (tty);

    /* Best-effort recursive cleanup of the synthetic tree. */
    gchar *rm = g_strdup_printf ("rm -rf %s", root);
    if (system (rm) != 0) { /* ignore */ }
    g_free (rm);
    g_free (root);
}

/* pn_meshtastic_list_devices() reads the real /sys, so its contents are
 * machine-dependent, but the contract is not: a non-NULL,
 * NULL-terminated, ascending-sorted strv of /dev paths. */
static void
test_list_devices_contract (void)
{
    gchar **devs = pn_meshtastic_list_devices ();
    PN_CHECK (devs != NULL);
    if (devs == NULL)
        return;

    for (gsize i = 0; devs[i] != NULL; i++)
    {
        PN_CHECK (g_str_has_prefix (devs[i], "/dev/"));
        if (i > 0)
            PN_CHECK (g_strcmp0 (devs[i - 1], devs[i]) <= 0);  /* sorted */
    }

    g_strfreev (devs);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-mesh-node");

    /* Frame TX */
    pn_test_add ("frame_wrap_envelope",      test_frame_wrap_envelope);
    pn_test_add ("text_frame_bytes",         test_text_frame_bytes);
    pn_test_add ("text_frame_channel_index", test_text_frame_channel_index);
    pn_test_add ("want_config_frame_bytes",  test_want_config_frame_bytes);
    pn_test_add ("heartbeat_frame_bytes",    test_heartbeat_frame_bytes);

    /* Protobuf primitives */
    pn_test_add ("pb_varint_roundtrip",      test_pb_varint_roundtrip);
    pn_test_add ("pb_fixed32_roundtrip",     test_pb_fixed32_roundtrip);

    /* Frame RX */
    pn_test_add ("extract_two_clean",        test_extract_two_clean);
    pn_test_add ("extract_split_payload",    test_extract_split_payload);
    pn_test_add ("extract_junk_before_magic", test_extract_junk_before_magic);
    pn_test_add ("extract_zero_length_skip", test_extract_zero_length_is_skipped);
    pn_test_add ("extract_oversized_recover", test_extract_oversized_recovery);

    /* Serial open */
    pn_test_add ("serial_open_termios",      test_serial_open_termios_cloexec);
    pn_test_add ("serial_open_exclusive",    test_serial_open_exclusive);
    pn_test_add ("serial_open_missing",      test_serial_open_missing);

    /* Discovery */
    pn_test_add ("known_devices_table",      test_known_devices_table);
    pn_test_add ("find_tty_one_level",       test_find_tty_one_level);
    pn_test_add ("find_tty_nested",          test_find_tty_nested);
    pn_test_add ("list_devices_contract",    test_list_devices_contract);

    return pn_test_run ();
}
