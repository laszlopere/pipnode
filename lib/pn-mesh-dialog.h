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

#ifndef PN_MESH_DIALOG_H
#define PN_MESH_DIALOG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The Devices -> Meshtastic configuration dialog.
 *
 * A transient window with the device list on the left (populated only
 * on explicit Scan, never on dialog open) and a stage-based settings
 * stack on the right (Identity / Region / Channels / Share / Test).
 * Per-device sessions are held only while the dialog is open; closing
 * disconnects every transport and discards parsed state.
 *
 * Phase 1 ships the skeleton only: menu entry opens this dialog with
 * the empty layout, a Scan button that yields a stubbed row, and a
 * blank right pane.  Phases 2-7 fill in real discovery, connection,
 * read/write paths, QR share and the test-message console.  See TODO
 * #29 for the phase tracker.
 *
 * Calling present() while the dialog is already open raises the
 * existing instance rather than creating a second one. */
void pn_mesh_dialog_present (GtkWindow *parent_window);

G_END_DECLS

#endif /* PN_MESH_DIALOG_H */
