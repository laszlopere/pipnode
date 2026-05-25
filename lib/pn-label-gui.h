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

#ifndef PN_LABEL_GUI_H
#define PN_LABEL_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/* pn_label_gui_install:
 *
 * Install the gui-tier vfunc slots onto the already-registered #PnLabel
 * class: the cairo/Pango text painter (paint_plot, with its skip-shadow /
 * skip-zoom flags) and a Pango-measuring get_size that shrinks the box to
 * the text in flexible mode.  Called once at editor startup (via
 * pn_gui_install_builtin_nodes()), after the factory has registered the
 * built-in node types.  The headless runtime never calls this, so the
 * Label logic runs without GTK while the editor still paints the readout.
 */
void pn_label_gui_install (void);

G_END_DECLS

#endif /* PN_LABEL_GUI_H */
