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

#ifndef PN_MESH_PAGE_BUSY_H
#define PN_MESH_PAGE_BUSY_H

#include <glib.h>

G_BEGIN_DECLS

/* Shared dialog-side busy sink used by every Meshtastic page.  The
 * page fires this on each TRUE/FALSE *transition* of its own writing
 * flag so the dialog can float its big spinner over the whole
 * notebook for the duration of any device round-trip -- a per-Apply
 * spinner inside one expander is too easy to miss and leaves the
 * rest of the dialog clickable while the device is mid-write. */
typedef void (*PnMeshPageBusyFunc) (gboolean busy,
                                    gpointer user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_BUSY_H */
