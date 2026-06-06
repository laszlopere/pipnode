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

#ifndef PN_OSCILLOSCOPE_GUI_H
#define PN_OSCILLOSCOPE_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pn_oscilloscope_gui_install:
 *
 * Installs the cairo CRT painter (#PnNodeClass.paint_plot) onto the
 * #PnOscilloscope class.  Called once at editor startup from
 * pn_gui_install_builtin_nodes(); the headless runtime never calls it,
 * so the oscilloscope's logic runs without GTK or cairo.
 */
void pn_oscilloscope_gui_install (void);

G_END_DECLS

#endif /* PN_OSCILLOSCOPE_GUI_H */
