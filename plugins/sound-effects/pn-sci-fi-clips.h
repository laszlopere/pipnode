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

#ifndef PN_SCI_FI_CLIPS_H
#define PN_SCI_FI_CLIPS_H

#include <glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Shared, GTK-free clip-storage + playback helpers.                  */
/*                                                                     */
/*  Compiled into BOTH the logic module (pipnode_sound_effects.so) and */
/*  the companion GUI module (pipnode_sound_effects-gui.so).  The      */
/*  editor loads the companion with G_MODULE_BIND_LOCAL, so a symbol   */
/*  exported by the logic .so is invisible across the barrier (TODO    */
/*  #23, Phase 8); rather than thread these stateless, node-           */
/*  independent helpers through a property seam, each module carries    */
/*  its own copy.  Single-sourcing the path-traversal-safe resolver    */
/*  (pn_sci_fi_resolve_path) — security-sensitive — is the main reason */
/*  this is a shared file rather than duplicated code.                  */
/* ------------------------------------------------------------------ */

/** Absolute path of the per-user clip cache,
 *  $XDG_DATA_HOME/pipnode/sound-effects/startrek/.  Free with g_free(). */
gchar  *pn_sci_fi_cache_dir    (void);

/** Resolve a "<Pack>/<basename>" clip identifier to its absolute cache
 *  path, rejecting path-traversal forms ("..", a leading slash, a
 *  backslash) so a hand-edited worksheet cannot aim the player at an
 *  arbitrary file.  Returns %NULL on an empty or unsafe identifier.
 *  Free with g_free(). */
gchar  *pn_sci_fi_resolve_path (const gchar *clip);

/** First media player found on $PATH (mpv, ffplay, gst-play-1.0,
 *  mpg123, paplay), or %NULL if none.  Free with g_free(). */
gchar  *pn_sci_fi_find_player  (void);

/** Build a %NULL-terminated argv that plays @file as quietly and
 *  headlessly as the detected @player allows.  Free with g_strfreev(). */
gchar **pn_sci_fi_build_argv   (const gchar *player, const gchar *file);

G_END_DECLS

#endif /* PN_SCI_FI_CLIPS_H */
