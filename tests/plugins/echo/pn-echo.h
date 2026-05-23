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

#ifndef PN_ECHO_H
#define PN_ECHO_H

/* Shared between the sample plugin's two halves (TODO #23, Phase 6):
 *
 *   - pn-echo.c       — the GTK-free logic, linked into the core-only
 *                       module pn_echo.so, exporting pn_plugin_init;
 *   - pn-echo-gui.c   — the GTK settings-dialog customisations, linked
 *                       into the companion pn_echo-gui.so, exporting
 *                       pn_plugin_gui_init.
 *
 * The companion needs the #GType so it can g_type_class_ref() the class
 * and write the dialog vfunc slots onto it; everything else about the
 * node (the instance struct, the "device" property, the pass-through
 * receive()) stays private to pn-echo.c. */

#include "pn-node.h"

G_BEGIN_DECLS

#define PN_TYPE_ECHO (pn_echo_get_type ())

G_DECLARE_FINAL_TYPE (PnEcho, pn_echo, PN, ECHO, PnNode)

G_END_DECLS

#endif /* PN_ECHO_H */
