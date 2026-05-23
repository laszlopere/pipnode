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

/* Unit tests for PnTts's pure selection seam.  The node speaks by
 * spawning an external engine and enumerating installed voices, neither
 * reproducible headless, so the testable contract is the two pure
 * helpers underneath that: pn_tts_derive_voice_label (a piper model
 * filename -> short label) and pn_tts_voice_index_for (the deterministic
 * sender -> voice-slot mapping that gives each source a stable voice).
 * These pin the logic half the headless/core split (TODO #23) keeps
 * loadable without GTK. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-tts.h"

static void
test_label_piper_full_path (void)
{
    /* Directory, ".onnx" suffix and the "<lang>_<COUNTRY>-" prefix all
     * get stripped down to "speaker-quality". */
    gchar *l = pn_tts_derive_voice_label (
        "piper", "/usr/share/piper/en_US-amy-low.onnx");
    PN_CHECK_CMPSTR (l, ==, "amy-low");
    g_free (l);

    l = pn_tts_derive_voice_label ("piper", "de_DE-thorsten-medium.onnx");
    PN_CHECK_CMPSTR (l, ==, "thorsten-medium");
    g_free (l);
}

static void
test_label_piper_no_lang_prefix (void)
{
    /* First dash-segment without an underscore is not a lang prefix, so
     * the basename is shown verbatim (minus the .onnx). */
    gchar *l = pn_tts_derive_voice_label ("piper", "voice-name.onnx");
    PN_CHECK_CMPSTR (l, ==, "voice-name");
    g_free (l);

    /* No dash at all: just the de-suffixed basename. */
    l = pn_tts_derive_voice_label ("piper", "simple.onnx");
    PN_CHECK_CMPSTR (l, ==, "simple");
    g_free (l);
}

static void
test_label_non_piper_verbatim (void)
{
    /* Other engines name voices directly, so the id passes through. */
    gchar *l = pn_tts_derive_voice_label ("espeak", "en+f3");
    PN_CHECK_CMPSTR (l, ==, "en+f3");
    g_free (l);

    l = pn_tts_derive_voice_label ("festival", "kal_diphone");
    PN_CHECK_CMPSTR (l, ==, "kal_diphone");
    g_free (l);
}

static void
test_voice_index_deterministic (void)
{
    /* The same sender always lands on the same slot — that is the whole
     * point of the per-source voice feature. */
    PN_CHECK_CMPINT (pn_tts_voice_index_for ("alice", 5),
                     ==, pn_tts_voice_index_for ("alice", 5));
    PN_CHECK_CMPINT (pn_tts_voice_index_for ("bob", 5),
                     ==, pn_tts_voice_index_for ("bob", 5));
}

static void
test_voice_index_in_range (void)
{
    const gchar *who[] = { "alice", "bob", "carol", "node-7", "" };
    guint        i;

    for (i = 0; i < G_N_ELEMENTS (who); i++)
        PN_CHECK (pn_tts_voice_index_for (who[i], 3) < 3u);
}

static void
test_voice_index_edge_cases (void)
{
    /* No voices, or no sender, fall back to the first slot rather than
     * dividing by zero / dereferencing NULL. */
    PN_CHECK_CMPINT (pn_tts_voice_index_for ("alice", 0), ==, 0u);
    PN_CHECK_CMPINT (pn_tts_voice_index_for (NULL, 4),    ==, 0u);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-tts");
    pn_test_add ("label_piper_full_path",     test_label_piper_full_path);
    pn_test_add ("label_piper_no_lang_prefix", test_label_piper_no_lang_prefix);
    pn_test_add ("label_non_piper_verbatim",  test_label_non_piper_verbatim);
    pn_test_add ("voice_index_deterministic", test_voice_index_deterministic);
    pn_test_add ("voice_index_in_range",      test_voice_index_in_range);
    pn_test_add ("voice_index_edge_cases",    test_voice_index_edge_cases);
    return pn_test_run ();
}
