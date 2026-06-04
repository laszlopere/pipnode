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

/* Derive the locale key a voice belongs to, used to group voices by
 * language in the settings dialog and to constrain the per-source voice
 * picker.  Only the "piper" engine encodes a locale in its voice ids:
 * the "<lang>_<COUNTRY>-" filename prefix is returned verbatim (e.g.
 * "en_US-amy-medium.onnx" -> "en_US", "hu_HU-imre-medium.onnx" ->
 * "hu_HU").  Returns %NULL for piper ids that lack the prefix and for
 * every other engine (whose voice ids are not locale-tagged).  Caller
 * frees a non-NULL result with g_free. */
gchar *pn_tts_derive_voice_language (const gchar *engine_id, const gchar *id);

/* Human-readable name for a locale key returned by
 * pn_tts_derive_voice_language (e.g. "en_US" -> "English (United
 * States)", "hu_HU" -> "Hungarian").  Returns a static string owned by
 * the lookup table, or %NULL when the key is not in the table — callers
 * fall back to showing the raw key.  Do not free. */
const gchar *pn_tts_language_label (const gchar *code);

/* Deterministically map a sender label to one of @n_voices slots, so a
 * given source always speaks in the same voice.  Returns 0 when
 * @n_voices is 0. */
guint  pn_tts_voice_index_for    (const gchar *who, guint n_voices);

/* ------------------------------------------------------------------ */
/*  Engine-table seam (GTK-free)                                        */
/*                                                                     */
/*  The engine table is core data the receive/speak path needs; these  */
/*  read-only accessors let the gui-tier settings dialog enumerate it  */
/*  to build the engine/voice combos without reaching into the core    */
/*  instance's private struct or the file-static table.                */
/* ------------------------------------------------------------------ */

/* Number of built-in TTS engines. */
guint        pn_tts_n_engines        (void);

/* Stable id (e.g. "piper") and human label (e.g. "Piper") for engine
 * @index in [0, pn_tts_n_engines()).  The returned strings are owned by
 * the engine table and must not be freed.  Out-of-range returns NULL. */
const gchar *pn_tts_engine_id        (guint index);
const gchar *pn_tts_engine_label     (guint index);

/* TRUE when the engine at @index has its CLI binary on $PATH. */
gboolean     pn_tts_engine_installed (guint index);

/* Human label for the engine identified by @id, or NULL when @id is not
 * a known engine.  Owned by the engine table; do not free. */
const gchar *pn_tts_engine_label_for_id (const gchar *id);

/* Voice ids for the engine identified by @id, or NULL when the engine
 * is unknown or has no enumerable voice selector.  Caller owns the
 * array (free with g_strfreev). */
gchar      **pn_tts_engine_list_voices  (const gchar *id);

/* Distinct locale keys present among the engine identified by @id's
 * installed voices, sorted and %NULL-terminated (e.g. {"en_US", "hu_HU",
 * %NULL}).  Returns an empty array (still %NULL-terminated) when the
 * engine is unknown, has no enumerable voices, or none of its voices
 * carry a locale — which is every engine except piper.  Caller owns the
 * array (free with g_strfreev). */
gchar      **pn_tts_engine_list_languages (const gchar *id);

/* Speak @text on @self using the configured engine/voice (or
 * @voice_override when non-NULL/non-empty for this call only).  Used by
 * the settings dialog's audio preview; the production speak path is the
 * node's own receive(). */
void         pn_tts_speak (PnTts       *self,
                           const gchar *text,
                           const gchar *voice_override);

G_END_DECLS

#endif /* PN_TTS_H */
