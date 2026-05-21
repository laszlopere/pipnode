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

#ifndef PN_SOUND_H
#define PN_SOUND_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSound                                                            */
/*                                                                     */
/*  Sink node that plays a sound clip whenever a message arrives.      */
/*  The clip is selected through #PnSound:sound, which holds either    */
/*  a freedesktop sound-theme id (e.g. "bell", "complete") or an       */
/*  absolute path to an audio file.  Both forms are dispatched to      */
/*  `canberra-gtk-play` so the same backend handles theme lookups and  */
/*  raw files.                                                         */
/*                                                                     */
/*  After each playback finishes the node enters a configurable        */
/*  insensitive window of #PnSound:dead-period seconds during which    */
/*  further incoming messages are dropped.  A new clip never overlaps  */
/*  one already playing — fresh messages are silently ignored while    */
/*  the subprocess is still running, irrespective of the dead-period.  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SOUND (pn_sound_get_type ())

G_DECLARE_FINAL_TYPE (PnSound, pn_sound, PN, SOUND, PnNode)

PnSound *pn_sound_new (void);

/**
 * pn_sound_list_system_sounds:
 *
 * Scan the freedesktop sound theme for `.oga` clips and return their
 * bare ids (file name with the extension stripped) as a sorted,
 * %NULL-terminated array.  The caller owns the array and must free it
 * with g_strfreev().  Returns an empty array (still %NULL-terminated)
 * when no theme directory is available.
 *
 * Returns: (transfer full): array of sound ids
 */
gchar **pn_sound_list_system_sounds (void);

/**
 * pn_sound_preview:
 * @self: the sound node
 *
 * Play the currently-configured clip immediately, bypassing the
 * dead-period window.  Used by the properties dialog so the user
 * hears the result of picking an entry from the system-sound combo.
 * Has no effect when no sound is configured, or when a clip from a
 * previous preview/message is still playing.
 */
void pn_sound_preview (PnSound *self);

G_END_DECLS

#endif /* PN_SOUND_H */
