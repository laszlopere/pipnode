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

#ifndef PN_DESKTOP_GEOMETRY_H
#define PN_DESKTOP_GEOMETRY_H

/* ------------------------------------------------------------------ */
/*  Desktop-layout geometry                                            */
/*                                                                     */
/*  Shared, GTK-free constants for the desktop-window layout, the       */
/*  desktop counterpart of pn-panel-geometry.h.  Three parties must     */
/*  agree on these exactly:                                            */
/*                                                                     */
/*    - the visual layout editor (#PnLayoutEditor in the               */
/*      %PN_LAYOUT_EDITOR_DESKTOP kind) draws the window frame and      */
/*      places widgets inside it;                                       */
/*    - #PnFlow, which stores the window size and the window-relative   */
/*      widget placements in the document; and                          */
/*    - the desktop application (still to be written), which opens a    */
/*      plain window of that size and paints the same widgets at the    */
/*      same coordinates.                                              */
/*                                                                     */
/*  Placements are stored WINDOW-RELATIVE: (0, 0) is the top-left of    */
/*  the window's content area, not of the editor canvas, so the app can  */
/*  use each saved (x, y) verbatim.                                     */
/* ------------------------------------------------------------------ */

/* Pixel height of each widget in the desktop window.  Bigger than the
 * panel's PN_PE_PREVIEW_HEIGHT: a window has room to breathe, and the
 * readouts are meant to be legible across a room. */
#define PN_DE_WIDGET_HEIGHT 48

/* Default size of a fresh desktop window, and the range the user may
 * pick from in the editor's size controls. */
#define PN_DE_WINDOW_DEFAULT_WIDTH   640
#define PN_DE_WINDOW_DEFAULT_HEIGHT  400
#define PN_DE_WINDOW_MIN_WIDTH       160
#define PN_DE_WINDOW_MIN_HEIGHT      120
#define PN_DE_WINDOW_MAX_WIDTH      4096
#define PN_DE_WINDOW_MAX_HEIGHT     4096

#endif /* PN_DESKTOP_GEOMETRY_H */
