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

#ifndef PN_DIAL_H
#define PN_DIAL_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDial                                                             */
/*                                                                     */
/*  Sink node that draws a round analogue dial (the kind a pressure    */
/*  gauge or speedometer uses): metallic outer bezel, glassy face,     */
/*  thin red tick marks with engraved digits, a configurable           */
/*  green/yellow/red zone arc, a 3D-shaded needle, a metallic centre   */
/*  pivot, and a configurable label at the lower centre.  The needle  */
/*  reads the latest numeric value extracted from every incoming       */
/*  message via a JSON path key.                                       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DIAL (pn_dial_get_type ())

G_DECLARE_FINAL_TYPE (PnDial, pn_dial, PN, DIAL, PnNode)

PnDial *pn_dial_new (void);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Every field the cairo dial-face painter (pn-dial-gui.c) needs to   */
/*  draw a frame, snapshotted by value through                         */
/*  pn_dial_get_paint_state().  Keeping the painter behind a snapshot  */
/*  lets the drawing code live in a separate translation unit without  */
/*  reaching into the node's private instance struct.  The colours are */
/*  #PnColor (layout-identical to GdkRGBA); @label and @unit are       */
/*  borrowed pointers owned by the node, valid for the duration of the */
/*  paint call.                                                         */
/* ------------------------------------------------------------------ */

typedef struct
{
    gdouble  min_value;
    gdouble  max_value;
    gdouble  start_angle;
    gdouble  end_angle;
    guint    major_ticks;
    guint    minor_ticks_per_major;

    const gchar *label;
    const gchar *unit;

    gdouble  green_start,  green_end;
    gdouble  yellow_start, yellow_end;
    gdouble  red_start,    red_end;

    PnColor  face_color;
    PnColor  scale_color;
    PnColor  needle_color;
    PnColor  label_color;
    PnColor  green_color;
    PnColor  yellow_color;
    PnColor  red_color;

    gdouble  display_value;
    gboolean has_value;
} PnDialPaintState;

/**
 * pn_dial_get_paint_state:
 * @self: dial instance
 * @out:  (out): caller-provided snapshot filled with the current
 *        drawing state
 *
 * Copy the fields the gui-tier painter needs into @out.  GTK-free; the
 * borrowed string pointers in @out remain valid only while @self is
 * alive and unmodified (i.e. for the duration of one paint).
 */
void pn_dial_get_paint_state (PnDial *self, PnDialPaintState *out);

G_END_DECLS

#endif /* PN_DIAL_H */
