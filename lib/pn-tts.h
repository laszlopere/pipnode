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

#ifndef PN_TTS_H
#define PN_TTS_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTts                                                              */
/*                                                                     */
/*  Sink node that speaks each incoming message's "output" string by  */
/*  piping it through Piper into the system audio output.  The voice  */
/*  model is configured through #PnTts:model and defaults to the      */
/*  Lessac English voice installed under                               */
/*  ~/.local/share/piper/voices.  Messages whose "output" member is   */
/*  missing or non-string are silently ignored.                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TTS (pn_tts_get_type ())

G_DECLARE_FINAL_TYPE (PnTts, pn_tts, PN, TTS, PnNode)

PnTts *pn_tts_new (void);

/**
 * pn_tts_list_voice_models:
 *
 * Scan ~/.local/share/piper/voices for ".onnx" files and return their
 * full paths as a sorted, %NULL-terminated array.  The caller owns
 * the array and must free it with g_strfreev().  Returns an empty
 * array (still %NULL-terminated) when the directory is missing or
 * holds no models.
 *
 * Returns: (transfer full): array of model paths
 */
gchar **pn_tts_list_voice_models (void);

/**
 * pn_tts_list_audio_sinks:
 *
 * Enumerate the host's PulseAudio sinks by running
 * <literal>pactl list sinks</literal> and parsing the
 * <literal>Name:</literal> and <literal>Description:</literal>
 * lines.  Returns a %NULL-terminated array of paired strings —
 * <literal>{name1, desc1, name2, desc2, …, %NULL}</literal> — so the
 * caller can show each entry's friendly description while persisting
 * the canonical sink name.  Returns an empty array (still
 * %NULL-terminated) when <command>pactl</command> is missing or
 * fails; the dialog falls back to the single &ldquo;Default&rdquo;
 * entry in that case.  The caller owns the array and must free it
 * with g_strfreev().
 *
 * Returns: (transfer full): paired strv of (name, description)
 */
gchar **pn_tts_list_audio_sinks  (void);

/* ------------------------------------------------------------------ */
/*  Pure selection seam (no I/O, no GTK)                                */
/*                                                                     */
/*  Exposed (non-static) so the headless unit tests can drive the      */
/*  voice-selection logic without enumerating installed engines or     */
/*  voices; the node remains the only production caller.                */
/* ------------------------------------------------------------------ */

/* Turn a voice id into a short display label.  For the "piper" engine
 * this strips the directory, the ".onnx" suffix and a leading
 * "<lang>_<COUNTRY>-" prefix (so "en_US-amy-low" -> "amy-low"); for
 * every other engine the id is returned verbatim.  Caller frees. */
gchar *pn_tts_derive_voice_label (const gchar *engine_id, const gchar *id);

/* Deterministically map a sender label to one of @n_voices slots, so a
 * given source always speaks in the same voice.  Returns 0 when
 * @n_voices is 0. */
guint  pn_tts_voice_index_for    (const gchar *who, guint n_voices);

G_END_DECLS

#endif /* PN_TTS_H */
