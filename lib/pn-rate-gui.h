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

#ifndef PN_RATE_GUI_H
#define PN_RATE_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/* pn_rate_gui_install:
 *
 * Install the gui-tier vfunc slot — the settings-dialog editor
 * customisation (read-only label rows for the cached `rate` /
 * `last-update`, and the icon+ticker currency combos for `from` / `to`) —
 * onto the already-registered #PnRate class.  Called once at editor
 * startup (via pn_gui_install_builtin_nodes()), after the factory has
 * registered the built-in node types.  The headless runtime never calls
 * this, so the Rate fetch/convert logic runs without GTK while the editor
 * still offers its dialog.  */
void pn_rate_gui_install (void);

G_END_DECLS

#endif /* PN_RATE_GUI_H */
