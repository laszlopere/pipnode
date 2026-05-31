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

/* Characterization tests for the Meshtastic *node*'s serial stack
 * (TODO #37.1).  The node (pn-meshtastic.c) used to carry its OWN copy
 * of the serial open, the sysfs USB scan, the 0x94 0xC3 frame codec and
 * the protobuf primitives -- divergent from, but parallel to, the
 * Devices-dialog stack (pn-mesh-serial.c / pn-mesh-discover.c, covered
 * by test-pn-mesh-frame / -inuse / -pb).  TODO #37 converged the two
 * onto one shared transport; these tests pinned the node's byte-level
 * and open-time behaviour so each step could be proven a no-op on the
 * live-messaging hot path.
 *
 * What converged, and where each guarantee is pinned now:
 *   #37.2 (open)     -- serial_open() is gone; both callers open through
 *                       pn_mesh_serial_open().  The "Serial open" cases
 *                       below drive that shared function.
 *   #37.3 (framing)  -- frame_wrap()/frame_extract() are gone.  TX builds
 *                       a bare ToRadio PAYLOAD and lets
 *                       pn_mesh_serial_write_frame() add the 0x94 0xC3
 *                       envelope (pinned by the round-trip case here +
 *                       the build_*_payload byte vectors); RX re-frames
 *                       through PnMeshFrameReader (the accumulator
 *                       behaviour now lives in test-pn-mesh-frame).
 *   #37.3 (discovery)-- the node's sysfs walk + known-device table are
 *                       gone; pn_meshtastic_list_devices() delegates to
 *                       pn_mesh_discover_sync() (the canonical scan +
 *                       table are pinned by test-pn-mesh-discover).
 *
 * What remains the node's own code, still pinned here: the protobuf
 * primitives, the outbound ToRadio builders, and the end-to-end
 * list_devices contract.
 *
 * The functions under test are static, so we compile the node's
 * translation unit straight into the test (the shell- and
 * host-monitoring-node tests do the same with their out-of-lib nodes;
 * here libpipnode-core links as a shared object, so the test's own
 * copies of the node's few exported symbols win at link time and we only
 * ever call the static helpers -- the GObject is never instantiated).
 *
 * Everything stays headless: the protobuf cases are pure byte vectors,
 * the open + envelope cases run against an anonymous pseudo-terminal.
 * No real radio, no network, no display. */

/* posix_openpt/grantpt/unlockpt/ptsname need the XSI feature macro, and
 * the build does not set one globally; declare it before any libc header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <glib/gstdio.h>

#include "pntest.h"

/* The node's translation unit, static helpers and all. */
#include "pn-meshtastic.c"

/* The shared serial transport the node's open converged onto (#37.2);
 * its definition comes from the linked libpipnode-core. */
#include "pn-mesh-serial.h"

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

/* build_text_payload encodes ToRadio{ packet=MeshPacket{ to=BCAST,
 * channel=ch, decoded=Data{ portnum=TEXT, payload=text } } } -- the bare
 * ToRadio payload, no serial envelope (that is added at write time by
 * pn_mesh_serial_write_frame; see test_write_frame_envelope).  Pin the
 * exact wire bytes for a known (text, channel). */
static void
test_text_payload_bytes (void)
{
    /* "hi" on channel 0:
     *   Data     = 08 01            (portnum=1)
     *              12 02 68 69      (payload="hi")
     *   MeshPkt  = 15 FF FF FF FF   (to=0xFFFFFFFF, fixed32 LE)
     *              18 00            (channel=0)
     *              22 06 <Data>     (decoded, len 6)
     *   ToRadio  = 0A 0F <MeshPkt>  (packet, len 15) */
    static const guint8 expect[] = {
        0x0A, 0x0F,
        0x15, 0xFF, 0xFF, 0xFF, 0xFF,
        0x18, 0x00,
        0x22, 0x06,
        0x08, 0x01,
        0x12, 0x02, 0x68, 0x69,
    };
    GBytes *payload = build_text_payload ("hi", 0);
    check_frame_bytes (payload, expect, sizeof expect);
    g_bytes_unref (payload);
}

/* A non-zero channel index rides as a plain varint in field 3. */
static void
test_text_payload_channel_index (void)
{
    /* "x" on channel 2 -- only the channel varint and lengths change. */
    static const guint8 expect[] = {
        0x0A, 0x0E,
        0x15, 0xFF, 0xFF, 0xFF, 0xFF,
        0x18, 0x02,
        0x22, 0x05,
        0x08, 0x01,
        0x12, 0x01, 0x78,
    };
    GBytes *payload = build_text_payload ("x", 2);
    check_frame_bytes (payload, expect, sizeof expect);
    g_bytes_unref (payload);
}

/* build_want_config_payload is ToRadio{ want_config_id=1 } -- field 3
 * varint = 1. */
static void
test_want_config_payload_bytes (void)
{
    static const guint8 expect[] = { 0x18, 0x01 };
    GBytes *payload = build_want_config_payload ();
    check_frame_bytes (payload, expect, sizeof expect);
    g_bytes_unref (payload);
}

/* build_heartbeat_payload is ToRadio{ heartbeat=Heartbeat{} } -- field 7,
 * length-delimited, zero payload bytes. */
static void
test_heartbeat_payload_bytes (void)
{
    static const guint8 expect[] = { 0x3A, 0x00 };
    GBytes *payload = build_heartbeat_payload ();
    check_frame_bytes (payload, expect, sizeof expect);
    g_bytes_unref (payload);
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

/* The RX accumulator is the shared PnMeshFrameReader now (#37.3); its
 * clean / split / junk / magic-split / zero-length / oversized-recovery
 * behaviour is pinned by test-pn-mesh-frame, so the node no longer
 * carries its own copy of those cases.  The deliberate reconciliation of
 * the old node-vs-dialog divergence: a zero-length header now yields an
 * empty frame (the reader's behaviour) instead of being silently
 * skipped, which the node's per-frame parsers treat as a harmless no-op. */

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

/* The shared pn_mesh_serial_open (#37.2) configures raw 115200 8N1 and
 * keeps the fd close-on-exec -- the guarantees the node's open carried. */
static void
test_serial_open_termios_cloexec (void)
{
    gchar *path   = NULL;
    int    master = open_pty (&path);
    PN_CHECK (master >= 0);
    if (master < 0)
        return;

    GError       *error  = NULL;
    PnMeshSerial *serial = pn_mesh_serial_open (path, &error);
    PN_CHECK (serial != NULL);
    if (serial != NULL)
    {
        int fd = pn_mesh_serial_get_fd (serial);
        PN_CHECK (fd >= 0);

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

        pn_mesh_serial_close (serial);
    }
    g_clear_error (&error);

    close (master);
    g_free (path);
}

/* A second open on a port we already hold is refused with
 * G_IO_ERROR_BUSY.  pn_mesh_tty_in_use() skips our own pid, so this case
 * specifically exercises the flock(LOCK_EX) #37.2 folded in from the
 * node -- the in-use guard alone would let a same-process re-open
 * through. */
static void
test_serial_open_exclusive (void)
{
    gchar *path   = NULL;
    int    master = open_pty (&path);
    PN_CHECK (master >= 0);
    if (master < 0)
        return;

    GError       *err1 = NULL;
    PnMeshSerial *s1   = pn_mesh_serial_open (path, &err1);
    PN_CHECK (s1 != NULL);
    if (s1 != NULL)
    {
        GError       *err2 = NULL;
        PnMeshSerial *s2   = pn_mesh_serial_open (path, &err2);
        PN_CHECK (s2 == NULL);
        PN_CHECK (g_error_matches (err2, G_IO_ERROR, G_IO_ERROR_BUSY));
        if (s2 != NULL)
            pn_mesh_serial_close (s2);
        g_clear_error (&err2);
        pn_mesh_serial_close (s1);
    }
    g_clear_error (&err1);

    close (master);
    g_free (path);
}

/* A path that does not exist fails cleanly (NULL + error set), not a
 * crash. */
static void
test_serial_open_missing (void)
{
    GError       *error  = NULL;
    PnMeshSerial *serial =
        pn_mesh_serial_open ("/dev/pn-mesh-node-does-not-exist", &error);
    PN_CHECK (serial == NULL);
    PN_CHECK (error != NULL);
    if (serial != NULL)
        pn_mesh_serial_close (serial);
    g_clear_error (&error);
}

/* ================================================================== */
/*  Frame TX envelope: build payload -> write through the shared writer  */
/* ================================================================== */

/* The end of the node's TX path: a builder produces the bare ToRadio
 * payload and pn_mesh_serial_write_frame() (#37.3, shared transport)
 * wraps it in the 0x94 0xC3 + big-endian length envelope on the wire.
 * Open a pty, send a known want_config payload, and read the raw bytes
 * back off the master to pin the exact on-wire framing the node now
 * emits -- this is where the envelope that frame_wrap() used to add is
 * verified after convergence. */
static void
test_write_frame_envelope (void)
{
    gchar *path   = NULL;
    int    master = open_pty (&path);
    PN_CHECK (master >= 0);
    if (master < 0)
        return;

    GError       *error  = NULL;
    PnMeshSerial *serial = pn_mesh_serial_open (path, &error);
    PN_CHECK (serial != NULL);
    if (serial != NULL)
    {
        GBytes       *payload = build_want_config_payload ();
        gsize         plen;
        const guint8 *pdata = g_bytes_get_data (payload, &plen);

        PN_CHECK (pn_mesh_serial_write_frame (serial, pdata, plen, NULL));

        /* ToRadio{ want_config_id=1 } = 18 01, wrapped: 94 C3 00 02 18 01. */
        static const guint8 expect[] = { 0x94, 0xC3, 0x00, 0x02, 0x18, 0x01 };
        guint8  got[sizeof expect];
        gssize  n = read (master, got, sizeof got);
        PN_CHECK_CMPINT (n, ==, (gssize) sizeof expect);
        if (n == (gssize) sizeof expect)
            PN_CHECK (memcmp (got, expect, sizeof expect) == 0);

        g_bytes_unref (payload);
        pn_mesh_serial_close (serial);
    }
    g_clear_error (&error);

    close (master);
    g_free (path);
}

/* ================================================================== */
/*  Discovery: end-to-end list contract                                */
/* ================================================================== */

/* The known-device VID:PID table and the sysfs tty walk are the shared
 * pn-mesh-discover module now (#37.3), pinned by test-pn-mesh-discover.
 * pn_meshtastic_list_devices() delegates to it; what stays the node's
 * contract is the shape it hands the dialog combo.
 *
 * pn_meshtastic_list_devices() reads the real /sys, so its contents are
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

    /* Frame TX: outbound ToRadio payloads */
    pn_test_add ("text_payload_bytes",         test_text_payload_bytes);
    pn_test_add ("text_payload_channel_index", test_text_payload_channel_index);
    pn_test_add ("want_config_payload_bytes",  test_want_config_payload_bytes);
    pn_test_add ("heartbeat_payload_bytes",    test_heartbeat_payload_bytes);

    /* Protobuf primitives */
    pn_test_add ("pb_varint_roundtrip",      test_pb_varint_roundtrip);
    pn_test_add ("pb_fixed32_roundtrip",     test_pb_fixed32_roundtrip);

    /* Serial open */
    pn_test_add ("serial_open_termios",      test_serial_open_termios_cloexec);
    pn_test_add ("serial_open_exclusive",    test_serial_open_exclusive);
    pn_test_add ("serial_open_missing",      test_serial_open_missing);

    /* Frame TX envelope (build payload -> shared writer -> wire) */
    pn_test_add ("write_frame_envelope",     test_write_frame_envelope);

    /* Discovery */
    pn_test_add ("list_devices_contract",    test_list_devices_contract);

    return pn_test_run ();
}
