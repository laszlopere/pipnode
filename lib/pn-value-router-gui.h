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

#ifndef PN_VALUE_ROUTER_GUI_H
#define PN_VALUE_ROUTER_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/* pn_value_router_gui_install:
 *
 * Install the gui-tier settings-dialog vfunc slot (the output count, the
 * tested path, and one rule row per output) onto the already-registered
 * #PnValueRouter class.  Called once at editor startup via
 * pn_gui_install_builtin_nodes(); the headless runtime never calls it. */
void pn_value_router_gui_install (void);

G_END_DECLS

#endif /* PN_VALUE_ROUTER_GUI_H */
