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

#ifndef PN_PANEL_GEOMETRY_H
#define PN_PANEL_GEOMETRY_H

/* ------------------------------------------------------------------ */
/*  Panel-applet layout geometry                                       */
/*                                                                     */
/*  Shared, GTK-free constants for the panel-band layout, used by two  */
/*  parties that must agree exactly:                                    */
/*                                                                     */
/*    - the visual layout editor (#PnLayoutEditor in its PANEL kind,   */
/*      lib/pn-layout-editor) draws the band and sticky-snaps widgets   */
/*      to it; and                                                     */
/*    - the headless engine (src/pn-application.c) which decides which  */
/*      widgets are "on the panel" from each node's saved (x, y) and    */
/*      mirrors them to the real XFCE panel applet.                     */
/*                                                                     */
/*  Keeping the numbers here means the engine's band-membership test    */
/*  can never drift from the editor's snap target.                      */
/* ------------------------------------------------------------------ */

/* Pixel height of each readout — a typical XFCE panel icon size, so the
 * editor preview matches what lands on a real panel. */
#define PN_PE_PREVIEW_HEIGHT 36

/* Geometry of the panel band — a horizontal strip standing in for the
 * real XFCE panel.  Its top sits at the cascade margin so the first
 * readout lands inside it, and its depth is exactly the applet-widget
 * height, so a snapped widget fills the band edge to edge. */
#define PN_PE_PANEL_TOP     24
#define PN_PE_PANEL_HEIGHT  PN_PE_PREVIEW_HEIGHT

#endif /* PN_PANEL_GEOMETRY_H */
