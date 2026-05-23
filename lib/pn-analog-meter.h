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

#ifndef PN_ANALOG_METER_H
#define PN_ANALOG_METER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnAnalogMeter                                                      */
/*                                                                     */
/*  Sink node that draws an old-style analogue panel meter: a square   */
/*  white plastic case with a moulded inset bezel, a flat mirror-      */
/*  finish dial face, a single arc of black tick marks with engraved   */
/*  digits, a long thin black needle pivoted at the bottom centre, a   */
/*  small knurled zero-adjust screw at the lower-right and an          */
/*  accuracy-class digit at the lower-left -- the classic AC volt /    */
/*  ampere panel meter look.  The needle reads the latest numeric      */
/*  value extracted from every incoming message via a JSON path key,   */
/*  the same way #PnDial does, but the visual presentation is the      */
/*  flat panel meter rather than #PnDial's round bezelled gauge.       */
/*                                                                     */
/*  Derivable: plugins that want a meter for a fixed quantity (a       */
/*  Tasmota voltmeter, a CPU-load gauge, ...) inherit from this class  */
/*  and override #PnNodeClass.receive to filter the upstream message   */
/*  and push values into the meter via #pn_analog_meter_set_value.     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ANALOG_METER (pn_analog_meter_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnAnalogMeter, pn_analog_meter,
                          PN, ANALOG_METER, PnNode)

struct _PnAnalogMeterClass
{
    PnNodeClass parent_class;
};

/* ------------------------------------------------------------------ */
/*  PnAnalogMeterMode                                                  */
/*                                                                     */
/*  Three-state enum used by the "mode" property to pick which IEC     */
/*  current-type symbol is painted directly below the unit glyph in    */
/*  the upper-left badge area.  PN_ANALOG_METER_MODE_NONE leaves the   */
/*  slot empty, for meters whose unit (eg. "Hz", "°C") does not need a */
/*  current-type indicator at all.                                     */
/* ------------------------------------------------------------------ */

#define PN_TYPE_ANALOG_METER_MODE (pn_analog_meter_mode_get_type ())

typedef enum
{
    PN_ANALOG_METER_MODE_AC = 0,
    PN_ANALOG_METER_MODE_DC = 1,
    PN_ANALOG_METER_MODE_NONE = 2,
} PnAnalogMeterMode;

GType pn_analog_meter_mode_get_type (void) G_GNUC_CONST;

PnAnalogMeter *pn_analog_meter_new (void);

/**
 * pn_analog_meter_set_value:
 * @self:  meter instance
 * @value: new target value the needle should swing to
 *
 * Push a numeric reading into the meter.  Identical to what the
 * built-in JSON-path resolver does when a value lands on the input:
 * stores @value as the new target, kicks off the damped-spring needle
 * animation toward it, and schedules a repaint.  Subclasses that
 * override #PnNodeClass.receive (for example, plugin meters that
 * filter the upstream message by topic and extract a specific JSON
 * field) call this to drive the needle without poking at private
 * state.  The first call also reveals the needle -- before any value
 * has been delivered, the needle is hidden so the parked-at-zero
 * state is visually distinguishable from a real zero reading.
 */
void pn_analog_meter_set_value (PnAnalogMeter *self, gdouble value);

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  Every field the cairo panel-meter painter (pn-analog-meter-gui.c)  */
/*  needs to draw a frame, snapshotted by value through               */
/*  pn_analog_meter_get_paint_state().  Keeping the painter behind a    */
/*  snapshot lets the drawing code live in a separate translation unit  */
/*  without reaching into the node's private instance struct.  The      */
/*  colours are #PnColor (layout-identical to GdkRGBA); @unit and       */
/*  @accuracy_class are borrowed pointers owned by the node, valid for  */
/*  the duration of the paint call.                                     */
/* ------------------------------------------------------------------ */

typedef struct
{
    gdouble            min_value;
    gdouble            max_value;
    gdouble            start_angle;
    gdouble            end_angle;
    guint              major_ticks;
    guint              minor_ticks_per_major;

    const gchar       *unit;
    const gchar       *accuracy_class;
    PnAnalogMeterMode  mode;

    PnColor            frame_color;
    PnColor            face_color;
    PnColor            scale_color;
    PnColor            needle_color;
    PnColor            label_color;

    gdouble            display_value;
    gboolean           has_value;
} PnAnalogMeterPaintState;

/**
 * pn_analog_meter_get_paint_state:
 * @self: meter instance
 * @out:  (out): caller-provided snapshot filled with the current
 *        drawing state
 *
 * Copy the fields the gui-tier painter needs into @out.  GTK-free; the
 * borrowed string pointers in @out remain valid only while @self is
 * alive and unmodified (i.e. for the duration of one paint).
 */
void pn_analog_meter_get_paint_state (PnAnalogMeter           *self,
                                      PnAnalogMeterPaintState *out);

G_END_DECLS

#endif /* PN_ANALOG_METER_H */
