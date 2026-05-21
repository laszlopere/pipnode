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

#ifndef PN_SCI_FI_SOUND_H
#define PN_SCI_FI_SOUND_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSciFiSound                                                       */
/*                                                                     */
/*  Sink node that plays a sci-fi sound effect on every incoming       */
/*  message.  Clips are not shipped with the plugin — the settings     */
/*  dialog exposes a "Download Star Trek sounds" button that fetches   */
/*  a curated set of classic effects from a public fan-site mirror     */
/*  into $XDG_DATA_HOME/pipnode/sound-effects/startrek/ the first      */
/*  time the user asks for them.  After the download lands the combo   */
/*  on the same tab lists every clip that ended up on disk and the     */
/*  user picks one as #PnSciFiSound:clip.                              */
/*                                                                     */
/*  Playback semantics mirror #PnSound: overlapping messages are       */
/*  dropped while a clip is still playing, an optional dead period     */
/*  enforces a minimum gap between playbacks.                          */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SCI_FI_SOUND (pn_sci_fi_sound_get_type ())

G_DECLARE_FINAL_TYPE (PnSciFiSound, pn_sci_fi_sound,
                      PN, SCI_FI_SOUND, PnNode)

PnSciFiSound *pn_sci_fi_sound_new (void);

G_END_DECLS

#endif /* PN_SCI_FI_SOUND_H */
