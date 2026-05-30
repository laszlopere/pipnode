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

#ifndef PN_DEVICE_SPIN_H
#define PN_DEVICE_SPIN_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A #GtkSpinButton that ignores the mouse scroll wheel.
 *
 * The spin-button counterpart of #PnDeviceCombo, for the same reason: a
 * plain GtkSpinButton steps its value on every wheel-tick while the
 * pointer hovers it, so scrolling a tall device dialog can silently
 * bump whichever spinner the cursor passes over.  This subclass leaves
 * its value untouched on scroll and lets the wheel event propagate to
 * the enclosing scrolled window so the dialog keeps scrolling.  The
 * +/- buttons and keyboard stepping are unaffected.
 *
 * Use it for device-dialog numeric fields so host and plugin dialogs
 * share one consistent, foot-gun-free behaviour. */

#define PN_TYPE_DEVICE_SPIN (pn_device_spin_get_type ())
G_DECLARE_FINAL_TYPE (PnDeviceSpin, pn_device_spin, PN, DEVICE_SPIN,
                      GtkSpinButton)

/* Create a scroll-safe spin button.  Equivalent to
 * gtk_spin_button_new_with_range (): the adjustment range, step and
 * displayed digits are derived from @min, @max and @step exactly as
 * GTK does. */
GtkWidget *pn_device_spin_new_with_range (gdouble min,
                                          gdouble max,
                                          gdouble step);

G_END_DECLS

#endif /* PN_DEVICE_SPIN_H */
