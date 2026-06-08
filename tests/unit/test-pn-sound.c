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

/* Unit tests for PnSound.  Actual playback is an outward effect — it
 * decodes a file and streams it to PulseAudio on a worker thread (or
 * spawns paplay) — and cannot be exercised headlessly, so these tests
 * deliberately never cross the playback gate: the node is only ever
 * received-on while UNCONFIGURED (no sound set), which pn_sound_receive()
 * short-circuits before pn_sound_play().  What is covered is the headless
 * contract around that gate: the sound / dead-period properties and their
 * empty-string normalisation, the unconfigured-vs-configured error flag
 * that drives the warning glyph, that it is a pure sink, and that
 * receiving while unconfigured neither emits nor crashes (nor plays). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-sound.h"

static void
test_unconfigured_receive_is_silent_sink (void)
{
    guint      emits = 0;
    PnNode    *node  = PN_NODE (pn_sound_new ());
    PnMessage *msg   = pn_message_new (NULL, NULL);

    g_signal_connect (node, "message",
                      G_CALLBACK (pn_test_count_emits), &emits);

    /* A fresh node has no sound configured, so receive returns before the
     * playback gate: no downstream message, no audio, no crash.  (This is
     * the only receive the headless tests perform — never with a sound
     * set, which would attempt real PulseAudio playback.) */
    pn_message_set_boolean (msg, "success", TRUE);
    pn_node_receive_message (node, msg);
    pn_node_receive_message (node, msg);

    PN_CHECK_CMPINT (emits, ==, 0);
    PN_CHECK_FALSE (pn_node_get_has_output (node));
    PN_CHECK (pn_node_get_has_input (node));

    g_object_unref (msg);
    g_object_unref (node);
}

static void
test_sound_prop_round_trip (void)
{
    PnNode *node  = PN_NODE (pn_sound_new ());
    gchar  *sound = NULL;

    /* Defaults to unconfigured (NULL). */
    g_object_get (node, "sound", &sound, NULL);
    PN_CHECK (sound == NULL);

    /* A freedesktop sound id round-trips verbatim. */
    g_object_set (node, "sound", "bell", NULL);
    g_object_get (node, "sound", &sound, NULL);
    PN_CHECK_CMPSTR (sound, ==, "bell");
    g_free (sound);
    sound = NULL;

    /* So does an absolute path (resolved only at play time, never here). */
    g_object_set (node, "sound", "/usr/share/sounds/x.oga", NULL);
    g_object_get (node, "sound", &sound, NULL);
    PN_CHECK_CMPSTR (sound, ==, "/usr/share/sounds/x.oga");
    g_free (sound);

    g_object_unref (node);
}

static void
test_empty_sound_normalises_to_null (void)
{
    PnNode *node  = PN_NODE (pn_sound_new ());
    gchar  *sound = (gchar *) "sentinel";

    /* The empty string means "not configured" and is stored as NULL, so
     * a blank entry in the dialog clears the node rather than trying to
     * play a zero-length id. */
    g_object_set (node, "sound", "bell", NULL);
    g_object_set (node, "sound", "", NULL);
    g_object_get (node, "sound", &sound, NULL);
    PN_CHECK (sound == NULL);

    g_object_unref (node);
}

static void
test_dead_period_prop (void)
{
    PnNode         *node = PN_NODE (pn_sound_new ());
    GParamSpec     *pspec;
    GParamSpecUInt *uspec;
    guint           dead = 99;

    /* Default 0 (no enforced silence), bounds 0..3600 s. */
    g_object_get (node, "dead-period", &dead, NULL);
    PN_CHECK_CMPINT (dead, ==, 0u);

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node),
                                          "dead-period");
    PN_CHECK (pspec != NULL && G_IS_PARAM_SPEC_UINT (pspec));
    uspec = G_PARAM_SPEC_UINT (pspec);
    PN_CHECK_CMPINT (uspec->minimum,       ==, 0u);
    PN_CHECK_CMPINT (uspec->maximum,       ==, 3600u);
    PN_CHECK_CMPINT (uspec->default_value, ==, 0u);

    /* An in-range value round-trips. */
    g_object_set (node, "dead-period", 30u, NULL);
    g_object_get (node, "dead-period", &dead, NULL);
    PN_CHECK_CMPINT (dead, ==, 30u);

    g_object_unref (node);
}

static void
test_error_flag_tracks_configured (void)
{
    PnNode *node = PN_NODE (pn_sound_new ());

    /* Unconfigured nodes paint the warning glyph (has-error), which the
     * worksheet renders centrally; configuring a sound clears it, and
     * blanking the sound again re-arms it. */
    PN_CHECK (pn_node_get_has_error (node));

    g_object_set (node, "sound", "bell", NULL);
    PN_CHECK_FALSE (pn_node_get_has_error (node));

    g_object_set (node, "sound", "", NULL);
    PN_CHECK (pn_node_get_has_error (node));

    g_object_unref (node);
}

static void
test_backend_description_nonempty (void)
{
    /* The settings dialog shows which playback backend the build carries;
     * it is a static, non-empty string either way. */
    const gchar *desc = pn_sound_backend_description ();

    PN_CHECK (desc != NULL);
    PN_CHECK (desc[0] != '\0');

    /* Class identity. */
    {
        PnNode *node = PN_NODE (pn_sound_new ());
        PN_CHECK_CMPSTR (pn_node_get_class_name (node), ==, "Sound");
        g_object_unref (node);
    }
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-sound");
    pn_test_add ("unconfigured_silent_sink", test_unconfigured_receive_is_silent_sink);
    pn_test_add ("sound_prop_round_trip",    test_sound_prop_round_trip);
    pn_test_add ("empty_sound_to_null",      test_empty_sound_normalises_to_null);
    pn_test_add ("dead_period_prop",         test_dead_period_prop);
    pn_test_add ("error_flag_configured",    test_error_flag_tracks_configured);
    pn_test_add ("backend_description",      test_backend_description_nonempty);
    return pn_test_run ();
}
