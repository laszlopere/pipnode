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

#ifndef PN_FIGURE_GUI_H
#define PN_FIGURE_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pn_figure_gui_install:
 *
 * Installs the cairo/Pango painter (#PnNodeClass.paint_plot) onto the
 * #PnFigure class.  Called once at editor startup from
 * pn_gui_install_builtin_nodes(); the headless runtime never calls it,
 * so the figure language — the parse, the transform and the display
 * list — runs with no GTK, no cairo and no Pango anywhere near it.
 *
 * The painter is deliberately DUMB: it asks pn_figure_render() for a
 * display list already in device units and strokes it.  It evaluates
 * nothing, maps nothing and decides nothing, which is the whole point
 * of the seam 80.4(i) and 80.10(d) asked for — and the reason 80.12's
 * tests can assert every coordinate without a pixel in sight.
 */
void pn_figure_gui_install (void);

G_END_DECLS

#endif /* PN_FIGURE_GUI_H */
