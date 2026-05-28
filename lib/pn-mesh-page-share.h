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

#ifndef PN_MESH_PAGE_SHARE_H
#define PN_MESH_PAGE_SHARE_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"

G_BEGIN_DECLS

/* Share page of the Meshtastic dialog.
 *
 * Two side-by-side columns built around pip-mesh's --print-qr-channels
 * (additive: receiver keeps its existing setup, adds the selected
 * channels) and --print-qr-config (destructive: receiver replaces its
 * whole channel + LoRa stack).  Each column shows the encoded
 * meshtastic.org/e/# URL with a Copy button and -- when libqrencode is
 * present at build time -- a cairo-rendered QR code the Meshtastic
 * Android / iOS apps decode directly off the screen.  Without
 * libqrencode the URL alone is shown and the missing dependency is
 * called out inline.
 *
 * The page is read-only: it never writes back to the device, so it
 * needs no status-callback bridge.  Every paint runs through the
 * pure pn-mesh-qr URL builder, which is byte-for-byte equivalent to
 * the pip-mesh bash codec (test-pn-mesh-qr pins the wire). */
GtkWidget *pn_mesh_page_share_new (void);

void       pn_mesh_page_share_set_state (GtkWidget         *page,
                                         const PnMeshState *state);

G_END_DECLS

#endif /* PN_MESH_PAGE_SHARE_H */
