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

#ifndef PN_ANALOG_METER_GUI_H
#define PN_ANALOG_METER_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/* pn_analog_meter_gui_install:
 *
 * Install the gui-tier vfunc slots — the cairo panel-meter drawing
 * (paint_plot plus its skip-shadow / skip-zoom flags) and the
 * settings-dialog tabs (build_class_tabs) — onto the already-registered
 * #PnAnalogMeter class.  Called once at editor startup (via
 * pn_gui_install_builtin_nodes()), after the factory has registered the
 * built-in node types.  The headless runtime never calls this, so the
 * Analog-Meter logic runs without GTK while the editor still draws the
 * gauge and offers its dialog.  */
void pn_analog_meter_gui_install (void);

G_END_DECLS

#endif /* PN_ANALOG_METER_GUI_H */
