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

#ifndef PN_MESH_PAGE_FIRMWARE_H
#define PN_MESH_PAGE_FIRMWARE_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"

G_BEGIN_DECLS

/* Firmware page of the Meshtastic dialog.
 *
 * Meshtastic devices are flashed by the official browser-based flasher
 * (flasher.meshtastic.org), which drives the device over WebSerial in a
 * Chromium browser (Chrome / Edge).  WebSerial needs exclusive access to
 * the serial port, and pipnode holds that port open for the whole
 * configuring session -- so this page does NOT flash itself.  It hands
 * off: it shows the device's reported hardware model and firmware version
 * as guidance, then on the flasher link it (1) opens the flasher URL in
 * the user's browser and (2) asks the dialog to CLOSE itself, which tears
 * down the live serial connection (closing our fd frees the port for the
 * browser's WebSerial).
 *
 * Holding no state of its own, the page only needs the read-only
 * PnMeshState for the hardware / firmware read-out; a NULL state means
 * "no device connected" and clears the read-out. */
GtkWidget *pn_mesh_page_firmware_new (void);

void       pn_mesh_page_firmware_set_state (GtkWidget         *page,
                                            const PnMeshState *state);

/* Invoked once the user confirms the hand-off, AFTER the flasher URL has
 * been opened: the dialog should close itself so its serial fd is freed
 * for the browser.  @user_data is the pointer passed to
 * _set_close_callback.  This destroys the dialog (and with it this page),
 * so the page touches nothing after calling it. */
typedef void (*PnMeshFirmwareCloseFunc) (gpointer user_data);

void       pn_mesh_page_firmware_set_close_callback (
        GtkWidget                  *page,
        PnMeshFirmwareCloseFunc     callback,
        gpointer                    user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_FIRMWARE_H */
