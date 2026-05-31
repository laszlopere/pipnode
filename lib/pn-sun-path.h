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

#ifndef PN_SUN_PATH_H
#define PN_SUN_PATH_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSunPath                                                          */
/*                                                                     */
/*  Sink node that renders the message emitted by a #PnAstronomical    */
/*  node as a small, rotatable 3D sky dome.  A translucent ground disc */
/*  carries the N/S/E/W compass marks, a single house sits at the      */
/*  centre, and the Sun's full-day track is drawn as an arc that arcs  */
/*  over the dome (solid above the horizon, dashed below); a Sun glyph */
/*  rides the arc at the live position the message reports.            */
/*                                                                     */
/*  The card geometry is deliberately identical to #PnWeatherReport /  */
/*  #PnGraph (a 40 px header, a small gap, then a 280 x 173 card) so    */
/*  the three sink types are visually interchangeable on the canvas.   */
/*                                                                     */
/*  Like the other reading cards the scene painter lives in the gui    */
/*  tier (pn-sun-path-gui.c, installed at editor startup); the core    */
/*  carries no GTK/cairo.  The scene is genuinely 3D: clicking the card */
/*  lifts it into the zoom overlay where a click-drag orbits the        */
/*  camera (horizontal = yaw / full circle, vertical = pitch / tilt).  */
/*  The chosen view (yaw + pitch) is a serialised property so it        */
/*  survives a save/load.                                              */
/*                                                                     */
/*  It reads the named members the Astronomical node promotes          */
/*  (latitude, longitude, sun_azimuth, sun_altitude, sun_up, city,     */
/*  country, success, output); the full-day arc is recomputed in C from */
/*  the resolved coordinates via pn_astronomical_compute(), so it needs */
/*  no extra data beyond a single message.                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SUN_PATH (pn_sun_path_get_type ())

G_DECLARE_FINAL_TYPE (PnSunPath, pn_sun_path, PN, SUN_PATH, PnNode)

PnSunPath *pn_sun_path_new (void);

/* ------------------------------------------------------------------ */
/*  Sky-background gradient direction (shared with the gui painter)    */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_SP_GRADIENT_NONE,
    PN_SP_GRADIENT_VERTICAL,
    PN_SP_GRADIENT_HORIZONTAL,
    PN_SP_GRADIENT_DIAGONAL,
} PnSpGradient;

/* ------------------------------------------------------------------ */
/*  Interaction (GTK-free)                                             */
/*                                                                     */
/*  Driven from the worksheet's zoom-overlay drag handler.  @dyaw and  */
/*  @dpitch are deltas in degrees; yaw wraps, pitch is clamped to a     */
/*  sensible tilt range so the dome never goes edge-on or fully         */
/*  top-down.  Repaints and notifies the matching properties.          */
/* ------------------------------------------------------------------ */

void pn_sun_path_rotate (PnSunPath *self, gdouble dyaw, gdouble dpitch);

/* Spin the house about its own vertical axis by @ddeg degrees (wraps).
 * Driven by the mouse wheel while the card is in the zoom overlay. */
void pn_sun_path_spin_house (PnSunPath *self, gdouble ddeg);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The cairo scene painter lives in pn-sun-path-gui.c but reads the    */
/*  same reading snapshot + display config the core fills.  One sample  */
/*  of the Sun's track; @path is a borrowed array owned by the node,    */
/*  valid for the duration of one paint call.                          */
/* ------------------------------------------------------------------ */

typedef struct
{
    gdouble azimuth;   /* degrees, 0 = N, clockwise            */
    gdouble altitude;  /* degrees above the horizon (may be <0) */
} PnSunPathPoint;

typedef struct
{
    /* Orbit camera. */
    gdouble  yaw;       /* degrees, 0..360 */
    gdouble  pitch;     /* degrees, tilt above the horizon plane */

    /* House heading: the direction the ridge faces, degrees, 0..360. */
    gdouble  house_heading;

    /* Latest reading.  When @have_data is %FALSE the painter shows the
     * "waiting" notice; when @success is %FALSE it shows @output. */
    gboolean have_data;
    gboolean success;
    gboolean sun_up;
    gdouble  sun_azimuth;
    gdouble  sun_altitude;

    const PnSunPathPoint *path;   /* borrowed; %NULL when n_path == 0 */
    guint                 n_path;

    const gchar *city;            /* borrowed, may be %NULL */
    const gchar *country;         /* borrowed, may be %NULL */
    const gchar *output;          /* borrowed failure text, may be %NULL */

    /* Display config. */
    PnColor      sky_color;
    PnColor      sky_color2;
    PnSpGradient sky_gradient;
    PnColor      ground_color;
    PnColor      path_color;
    PnColor      sun_color;
    PnColor      text_color;
    gboolean     show_house;
} PnSunPathSnapshot;

/**
 * pn_sun_path_get_snapshot:
 * @self: sun-path instance
 * @out:  (out): caller-provided snapshot filled with the current view,
 *        reading and display configuration.
 */
void pn_sun_path_get_snapshot (PnSunPath *self, PnSunPathSnapshot *out);

G_END_DECLS

#endif /* PN_SUN_PATH_H */
