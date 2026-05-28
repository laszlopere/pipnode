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

#ifndef PN_MESH_QR_H
#define PN_MESH_QR_H

#include <glib.h>

#include "pn-mesh-connection.h"

G_BEGIN_DECLS

/* Build a Meshtastic share URL of the form
 *
 *     https://meshtastic.org/e/#<base64url(ChannelSet)>
 *
 * out of the given channel set, optionally including a LoRaConfig sub-
 * message (the receiving device replaces its whole LoRa stack when the
 * URL carries one; without it the channels are merged additively).
 *
 * @channels is a GPtrArray of PnMeshChannel*, typically borrowed from
 * PnMeshState->channels.  Slots with role == 0 (DISABLED) are skipped,
 * matching pip-mesh's behaviour.  When @selected_indices is non-NULL,
 * only channels whose ::index appears in the array are included; pass
 * NULL to include every non-disabled slot.
 *
 * @lora_or_null, when set, attaches a LoRaConfig with the four fields
 * pip-mesh's --print-qr-config publishes (use_preset, modem_preset,
 * region, hop_limit -- the firmware needs no more to reproduce the
 * stack).  Pass NULL for the additive / "share-channels-only" form.
 *
 * Returns a newly-allocated URL string the caller must g_free(), or
 * NULL with @error set when the surviving channel set is empty. */
gchar *pn_mesh_qr_build_share_url (
        const GPtrArray              *channels,
        const guint32                *selected_indices,
        gsize                         n_selected_indices,
        const PnMeshLoraConfigWrite  *lora_or_null,
        GError                      **error);

G_END_DECLS

#endif /* PN_MESH_QR_H */
