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

#ifndef PN_DEVICE_COMBO_H
#define PN_DEVICE_COMBO_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A #GtkComboBoxText that ignores the mouse scroll wheel.
 *
 * Drop-in replacement for gtk_combo_box_text_new () in device-config
 * dialogs.  A plain GtkComboBox treats a wheel-tick while the pointer
 * is merely hovering it (popup closed) as a value change, so scrolling
 * a tall dialog can silently flip whichever combo the cursor passes
 * over -- with no visual cue and no undo.  This subclass swallows that
 * by NOT changing its selection on scroll, and lets the wheel event
 * propagate to the enclosing scrolled window so the dialog keeps
 * scrolling.  Selecting with the wheel inside an OPEN popup is GTK menu
 * behaviour and is unaffected.
 *
 * Use it for device-dialog combos so host and plugin dialogs share one
 * consistent, foot-gun-free behaviour. */

#define PN_TYPE_DEVICE_COMBO (pn_device_combo_get_type ())
G_DECLARE_FINAL_TYPE (PnDeviceCombo, pn_device_combo, PN, DEVICE_COMBO,
                      GtkComboBoxText)

/* Create a scroll-safe text combo.  Equivalent to
 * gtk_combo_box_text_new () otherwise: append rows with
 * gtk_combo_box_text_append () and friends as usual. */
GtkWidget *pn_device_combo_new (void);

G_END_DECLS

#endif /* PN_DEVICE_COMBO_H */
