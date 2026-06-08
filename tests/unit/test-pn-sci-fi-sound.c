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

/* Unit tests for PnSciFiSound and its shared, GTK-free clip helpers
 * (pn-sci-fi-clips.c).  The node spawns a media player on receive, so the
 * tests NEVER feed it a message that resolves to a real file — they cover
 * only headless-safe logic:
 *
 *   - pn_sci_fi_cache_dir() points at the per-user startrek cache;
 *   - pn_sci_fi_resolve_path() joins a "<Pack>/<basename>" id under that
 *     cache and rejects every path-traversal form (NULL/empty, a leading
 *     slash, a backslash, "..");
 *   - pn_sci_fi_build_argv() emits the exact quiet/headless argv for each
 *     known player (mpv/ffplay/mpg123) and a bare [player, file] for an
 *     unrecognised one, honouring a full path's basename;
 *   - the clip / dead-period properties default and round-trip (empty
 *     clip coerces to NULL), the node carries has-error while
 *     unconfigured, and it is a pure sink that emits nothing — verified
 *     with an unset clip so receive returns before any playback.
 *
 * pn_sci_fi_find_player() is intentionally NOT exercised: its result is
 * whatever happens to be installed on the box, and the player is never
 * launched. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "pntest.h"
#include "pn-node.h"
#include "pn-sci-fi-clips.h"
#include "pn-sci-fi-sound.h"

/* ---- cache directory + path resolver ----------------------------- */

static void
test_cache_dir (void)
{
    gchar *dir    = pn_sci_fi_cache_dir ();
    gchar *expect = g_build_filename (g_get_user_data_dir (),
                                      "pipnode", "sound-effects",
                                      "startrek", NULL);

    PN_CHECK_CMPSTR (dir, ==, expect);

    g_free (dir);
    g_free (expect);
}

static void
test_resolve_path_valid (void)
{
    /* A well-formed "<Pack>/<basename>" id joins under the cache dir. */
    gchar *dir    = pn_sci_fi_cache_dir ();
    gchar *expect = g_build_filename (dir, "Romulan",
                                      "romulan_disruptor.mp3", NULL);
    gchar *got    = pn_sci_fi_resolve_path ("Romulan/romulan_disruptor.mp3");

    PN_CHECK_CMPSTR (got, ==, expect);

    g_free (dir);
    g_free (expect);
    g_free (got);
}

static void
test_resolve_path_rejects (void)
{
    /* Empty, absolute, backslash, and parent-traversal forms all refuse
     * (the resolver is the security boundary for a hand-edited
     * worksheet). */
    PN_CHECK (pn_sci_fi_resolve_path (NULL)            == NULL);
    PN_CHECK (pn_sci_fi_resolve_path ("")              == NULL);
    PN_CHECK (pn_sci_fi_resolve_path ("/etc/passwd")   == NULL);
    PN_CHECK (pn_sci_fi_resolve_path ("a\\b.mp3")      == NULL);
    PN_CHECK (pn_sci_fi_resolve_path ("../secret.mp3") == NULL);
    PN_CHECK (pn_sci_fi_resolve_path ("Pack/../x.mp3") == NULL);
}

/* ---- player argv builder ----------------------------------------- */

static void
test_argv_mpv (void)
{
    /* A full path is accepted; mpv gets the silent/headless flag set,
     * then the file last. */
    gchar **argv = pn_sci_fi_build_argv ("/usr/bin/mpv", "clip.mp3");

    PN_CHECK_CMPINT (g_strv_length (argv), ==, 5);
    PN_CHECK_CMPSTR (argv[0], ==, "/usr/bin/mpv");
    PN_CHECK_CMPSTR (argv[1], ==, "--no-video");
    PN_CHECK_CMPSTR (argv[2], ==, "--really-quiet");
    PN_CHECK_CMPSTR (argv[3], ==, "--no-config");
    PN_CHECK_CMPSTR (argv[4], ==, "clip.mp3");
    PN_CHECK (argv[5] == NULL);

    g_strfreev (argv);
}

static void
test_argv_ffplay (void)
{
    gchar **argv = pn_sci_fi_build_argv ("ffplay", "clip.mp3");

    PN_CHECK_CMPINT (g_strv_length (argv), ==, 6);
    PN_CHECK_CMPSTR (argv[0], ==, "ffplay");
    PN_CHECK_CMPSTR (argv[1], ==, "-nodisp");
    PN_CHECK_CMPSTR (argv[2], ==, "-autoexit");
    PN_CHECK_CMPSTR (argv[3], ==, "-loglevel");
    PN_CHECK_CMPSTR (argv[4], ==, "quiet");
    PN_CHECK_CMPSTR (argv[5], ==, "clip.mp3");
    PN_CHECK (argv[6] == NULL);

    g_strfreev (argv);
}

static void
test_argv_mpg123 (void)
{
    gchar **argv = pn_sci_fi_build_argv ("mpg123", "clip.mp3");

    PN_CHECK_CMPINT (g_strv_length (argv), ==, 3);
    PN_CHECK_CMPSTR (argv[0], ==, "mpg123");
    PN_CHECK_CMPSTR (argv[1], ==, "-q");
    PN_CHECK_CMPSTR (argv[2], ==, "clip.mp3");
    PN_CHECK (argv[3] == NULL);

    g_strfreev (argv);
}

static void
test_argv_unknown_player (void)
{
    /* paplay/gst-play-1.0 carry no special flags: just [player, file].
     * The flag set is keyed on the basename, so a full path still
     * matches none here. */
    gchar **argv = pn_sci_fi_build_argv ("/usr/bin/paplay", "clip.mp3");

    PN_CHECK_CMPINT (g_strv_length (argv), ==, 2);
    PN_CHECK_CMPSTR (argv[0], ==, "/usr/bin/paplay");
    PN_CHECK_CMPSTR (argv[1], ==, "clip.mp3");
    PN_CHECK (argv[2] == NULL);

    g_strfreev (argv);
}

/* ---- node properties + sink contract ----------------------------- */

static void
test_clip_round_trip (void)
{
    PnSciFiSound *snd  = pn_sci_fi_sound_new ();
    gchar        *clip = (gchar *) GINT_TO_POINTER (1);

    /* A fresh node has no clip configured. */
    g_object_get (snd, "clip", &clip, NULL);
    PN_CHECK (clip == NULL);
    g_free (clip);

    g_object_set (snd, "clip", "Romulan/disruptor.mp3", NULL);
    g_object_get (snd, "clip", &clip, NULL);
    PN_CHECK_CMPSTR (clip, ==, "Romulan/disruptor.mp3");
    g_free (clip);

    /* An empty string coerces back to NULL (the unconfigured state). */
    g_object_set (snd, "clip", "", NULL);
    clip = (gchar *) GINT_TO_POINTER (1);
    g_object_get (snd, "clip", &clip, NULL);
    PN_CHECK (clip == NULL);
    g_free (clip);

    g_object_unref (snd);
}

static void
test_dead_period (void)
{
    PnSciFiSound   *snd    = pn_sci_fi_sound_new ();
    guint           period = 99;
    GParamSpec     *pspec;
    GParamSpecUInt *uspec;

    /* Default 0 (no enforced silence), bounded [0, 3600]. */
    g_object_get (snd, "dead-period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 0u);

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (snd),
                                          "dead-period");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec));
    uspec = G_PARAM_SPEC_UINT (pspec);
    PN_CHECK_CMPINT (uspec->minimum, ==, 0u);
    PN_CHECK_CMPINT (uspec->maximum, ==, 3600u);

    g_object_set (snd, "dead-period", 120u, NULL);
    g_object_get (snd, "dead-period", &period, NULL);
    PN_CHECK_CMPINT (period, ==, 120u);

    g_object_unref (snd);
}

static void
test_has_error_gate (void)
{
    PnSciFiSound *snd = pn_sci_fi_sound_new ();

    /* No clip → flagged for the worksheet's red/❗ overlay. */
    PN_CHECK (pn_node_get_has_error (PN_NODE (snd)));

    g_object_set (snd, "clip", "Romulan/disruptor.mp3", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (PN_NODE (snd)));

    g_object_set (snd, "clip", "", NULL);
    PN_CHECK (pn_node_get_has_error (PN_NODE (snd)));

    g_object_unref (snd);
}

static void
test_is_a_sink (void)
{
    guint      emits = 0;
    PnSciFiSound *snd = pn_sci_fi_sound_new ();
    PnMessage    *msg = pn_message_new (NULL, NULL);

    /* Contract metadata: input-only sink with no output. */
    PN_CHECK (pn_node_get_has_input (PN_NODE (snd)));
    PN_CHECK_FALSE (pn_node_get_has_output (PN_NODE (snd)));

    g_signal_connect (snd, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* Clip is unset, so receive returns at the configuration gate before
     * touching the filesystem or a player — no message is forwarded and
     * nothing is spawned. */
    pn_message_set_boolean (msg, "success", TRUE);
    pn_node_receive_message (PN_NODE (snd), msg);
    pn_node_receive_message (PN_NODE (snd), msg);

    PN_CHECK_CMPINT (emits, ==, 0);

    g_object_unref (msg);
    g_object_unref (snd);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-sci-fi-sound");
    pn_test_add ("cache_dir",           test_cache_dir);
    pn_test_add ("resolve_path_valid",  test_resolve_path_valid);
    pn_test_add ("resolve_path_reject", test_resolve_path_rejects);
    pn_test_add ("argv_mpv",            test_argv_mpv);
    pn_test_add ("argv_ffplay",         test_argv_ffplay);
    pn_test_add ("argv_mpg123",         test_argv_mpg123);
    pn_test_add ("argv_unknown",        test_argv_unknown_player);
    pn_test_add ("clip_round_trip",     test_clip_round_trip);
    pn_test_add ("dead_period",         test_dead_period);
    pn_test_add ("has_error_gate",      test_has_error_gate);
    pn_test_add ("is_a_sink",           test_is_a_sink);
    return pn_test_run ();
}
