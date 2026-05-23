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

/* ------------------------------------------------------------------ */
/*  PnDial — gui tier.                                                  */
/*                                                                     */
/*  The whole cairo dial-face painter (metallic bezel, glassy face,    */
/*  tick marks + engraved digits, the green/yellow/red zone arcs, the  */
/*  3D-shaded needle, the centre pivot, the glass reflection) and the  */
/*  four-tab settings dialog.  The node's GType, properties, receive() */
/*  logic and the damped-spring needle animation live in the GTK-free  */
/*  core file pn-dial.c; this file installs the drawing + dialog vfunc  */
/*  slots onto that class at editor startup (pn_dial_gui_install),      */
/*  reading the dial's drawing state through the core's GTK-free        */
/*  snapshot seam (pn_dial_get_paint_state).  The headless runtime     */
/*  never loads this file's half, so the Dial logic runs without GTK.  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-dial-gui.h"
#include "pn-dial.h"
#include "pn-node-dialog-helpers.h"

#include <gtk/gtk.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Convert a clock-angle (degrees, clockwise from 12 o'clock) into a
 *  cairo angle (radians, clockwise from +x axis under cairo's default
 *  y-down coordinates).  At clock_angle = 0 the result is -π/2 which
 *  is the top of the dial; the +x axis is the 3 o'clock direction so
 *  a clock_angle of 90° maps to cairo 0 as expected. */
static inline double
clock_to_cairo (double clock_deg)
{
    return (clock_deg - 90.0) * M_PI / 180.0;
}

/** Map a data value to its clock-angle along the scale.  Values are
 *  clamped to [min_value, max_value] so an out-of-range reading parks
 *  the needle at the corresponding endpoint instead of swinging past
 *  the tick marks (which would look broken). */
static double
value_to_angle (const PnDialPaintState *ps, double v)
{
    double span = ps->max_value - ps->min_value;
    double t;

    if (span <= 0.0)
        return ps->start_angle;

    if (v < ps->min_value) v = ps->min_value;
    if (v > ps->max_value) v = ps->max_value;

    t = (v - ps->min_value) / span;
    return ps->start_angle + t * (ps->end_angle - ps->start_angle);
}

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/*                                                                     */
/*  Every painter below works in a local cairo coordinate system       */
/*  centred on the dial's geometric centre with +y pointing down (the  */
/*  cairo default).  The caller (pn_dial_paint_plot) translates the    */
/*  context once and passes the geometric radius into each helper, so  */
/*  the helpers themselves never have to mention the worksheet's       */
/*  outer (x, y, w, h) rectangle.                                       */
/* ------------------------------------------------------------------ */

/** Draw the metallic outer bezel: a thick ring formed by a linear
 *  gradient from a light grey at the top to a darker grey at the
 *  bottom (the classic "polished steel" reading), with a thin darker
 *  line on the inner edge to separate it from the face.  The
 *  highlight band running across the top sells the metallic look on
 *  its own, even without animation. */
static void
paint_bezel (cairo_t *cr, double radius)
{
    cairo_pattern_t *grad;
    double           outer = radius;
    double           inner = radius * 0.90;   /* bezel is 10% of radius */

    /* Outer disc: light-to-dark vertical gradient. */
    grad = cairo_pattern_create_linear (0.0, -outer, 0.0, outer);
    cairo_pattern_add_color_stop_rgb (grad, 0.00, 0.92, 0.92, 0.94);
    cairo_pattern_add_color_stop_rgb (grad, 0.45, 0.45, 0.45, 0.48);
    cairo_pattern_add_color_stop_rgb (grad, 0.55, 0.30, 0.30, 0.32);
    cairo_pattern_add_color_stop_rgb (grad, 1.00, 0.78, 0.78, 0.80);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, outer, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    /* Inner rim: a second, narrower gradient ring sitting just inside
     * the outer one.  Reversing the direction makes the bezel read as
     * a beveled lip catching light from the opposite side, which is
     * what gives the bezel its sense of thickness. */
    grad = cairo_pattern_create_linear (0.0, -inner, 0.0, inner);
    cairo_pattern_add_color_stop_rgb (grad, 0.00, 0.55, 0.55, 0.57);
    cairo_pattern_add_color_stop_rgb (grad, 0.50, 0.85, 0.85, 0.87);
    cairo_pattern_add_color_stop_rgb (grad, 1.00, 0.40, 0.40, 0.42);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, inner * 1.03, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    /* Dark hairline between bezel and face -- without it the bezel
     * gradient bleeds into the face fill on machines whose Cairo
     * builds antialias rings slightly inward. */
    cairo_set_source_rgba (cr, 0.10, 0.10, 0.10, 0.55);
    cairo_set_line_width (cr, 0.8);
    cairo_arc (cr, 0.0, 0.0, inner * 0.985, 0.0, 2.0 * M_PI);
    cairo_stroke (cr);
}

/** Paint the dial face: a radial gradient from a slightly brighter
 *  centre to the user-chosen face_color at the rim.  The brightness
 *  bump in the middle gives the face a hint of curvature so the glass
 *  highlight painted later has something to sit on. */
static void
paint_face (cairo_t *cr, double radius, const PnColor *face)
{
    cairo_pattern_t *grad;
    double           r = radius * 0.88;
    double           cr_r, cg, cb;

    /* Brighter centre = face_color blended toward white. */
    cr_r = face->red   * 0.85 + 0.15;
    cg   = face->green * 0.85 + 0.15;
    cb   = face->blue  * 0.85 + 0.15;

    grad = cairo_pattern_create_radial (0.0, -r * 0.20, r * 0.20,
                                        0.0,  0.0,      r);
    cairo_pattern_add_color_stop_rgba (grad, 0.0, cr_r, cg, cb, 1.0);
    cairo_pattern_add_color_stop_rgba (grad, 1.0,
                                       face->red, face->green, face->blue,
                                       1.0);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);
}

/** Paint a single coloured zone arc just inside the tick band.  The
 *  zone is drawn as a stroked thick arc rather than a filled wedge so
 *  it sits visually on the scale, not on the face -- the difference
 *  is what makes the dial in the reference image read as "scale
 *  decoration" rather than "pie chart slice".  No-op when start ≥ end
 *  (the user-disabled case). */
static void
paint_zone (
        cairo_t                *cr,
        const PnDialPaintState *ps,
        double                  radius,
        double                  band_radius,
        double                  band_width,
        double                  zone_start,
        double                  zone_end,
        const PnColor          *color)
{
    double a0, a1;

    (void) radius;

    if (zone_start >= zone_end)
        return;

    a0 = clock_to_cairo (value_to_angle (ps, zone_start));
    a1 = clock_to_cairo (value_to_angle (ps, zone_end));

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_set_line_width (cr, band_width);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_BUTT);
    cairo_new_sub_path (cr);
    cairo_arc (cr, 0.0, 0.0, band_radius, a0, a1);
    cairo_stroke (cr);
}

/** Pick a "nice" tick step that yields at most @target_ticks marks
 *  across a range of @range, snapping to one of {1, 2, 5} × 10^n.
 *  This is the same heuristic matplotlib / gnuplot use to keep axis
 *  labels at human-readable values (10, 20, 30 …) rather than at
 *  evenly-divided fractions of the range (10, 13.33, 16.67 …) -- a
 *  user reading a 0-16 dial with 13 requested ticks naturally
 *  expects to see "0, 2, 4, …, 16" and to be able to enter a zone
 *  bound of 9 and have it land at the value 9 on the face, not at
 *  ceil(9 / 1.333) * 1.333 = 9.333 the way evenly-subdivided ticks
 *  would have it. */
static double
pick_nice_step (double range, guint target_ticks)
{
    double raw, mag, norm, nice;

    if (target_ticks < 2 || !(range > 0.0))
        return 0.0;

    raw  = range / (double) (target_ticks - 1);
    mag  = pow (10.0, floor (log10 (raw)));
    norm = raw / mag;

    /* Round norm UP to the nearest of {1, 2, 5, 10} so the chosen
     * step yields at most target_ticks marks across the range -- the
     * dial respects the user's tick-count as an upper bound rather
     * than as a hard count. */
    if      (norm <= 1.0) nice =  1.0;
    else if (norm <= 2.0) nice =  2.0;
    else if (norm <= 5.0) nice =  5.0;
    else                  nice = 10.0;

    return nice * mag;
}

/** Format a tick value at @step's precision: integer when the step is
 *  an integer >= 1 (the common case for 0-100, 0-120, 0-16 dials),
 *  otherwise the minimum number of decimals that keep the printed
 *  values distinct from each other.  Uses %g for the fractional case
 *  so trailing zeros are dropped ("0.5" not "0.50"). */
static void
format_tick_label (gchar *buf, gsize sz, double val, double step)
{
    int decimals;

    /* Snap microscopic floating-point noise (e.g. 0.30000000000000004)
     * to a clean zero so a tick at "0" never prints as "3e-17". */
    if (fabs (val) < step * 1e-9)
        val = 0.0;

    if (step >= 1.0 && fabs (step - round (step)) < 1e-9)
    {
        g_snprintf (buf, sz, "%d", (int) round (val));
        return;
    }

    /* Pick enough decimals to make the step value distinguishable --
     * step 0.5 → 1 decimal, step 0.05 → 2 decimals, etc. */
    decimals = (int) ceil (-log10 (step));
    if (decimals < 1) decimals = 1;
    if (decimals > 6) decimals = 6;
    g_snprintf (buf, sz, "%.*f", decimals, val);
}

/** Paint the tick marks and digit labels.  Major ticks are longer and
 *  carry a digit; minor ticks are short and unlabelled.  Both use the
 *  configurable scale_color (the reference image uses a deep red but
 *  black or any other colour works the same).  Digits live just
 *  inside the tick ring -- placing them outside would push them onto
 *  the bezel where they would disappear into the metallic shading.
 *
 *  Major ticks are placed at "nice" value positions ({1, 2, 5} × 10^n
 *  apart) derived from the user's `major-ticks` target, so the
 *  printed digit at each tick really equals the underlying value at
 *  that angle and a zone bound entered as "9" lands at the "9" tick
 *  mark rather than at the awkward fraction (9.333…) an evenly-
 *  subdivided arc would have placed value 9 at.  Minor ticks fill
 *  evenly between consecutive major ticks. */
static void
paint_ticks (
        cairo_t                *cr,
        const PnDialPaintState *ps,
        double                  radius)
{
    double      r_outer = radius * 0.84;    /* outer end of major ticks */
    double      r_minor = radius * 0.79;    /* inner end of minor ticks */
    double      r_major = radius * 0.74;    /* inner end of major ticks */
    double      r_digit = radius * 0.62;    /* digit centre radius */
    PangoLayout *layout;
    PangoFontDescription *fd;
    gchar        buf[32];
    double       step;
    double       first;
    double       v;
    double       prev_v;
    gboolean     have_prev;
    guint        minors;
    guint        j;

    if (ps->major_ticks < 2)
        return;
    if (!(ps->max_value > ps->min_value))
        return;

    step   = pick_nice_step (ps->max_value - ps->min_value,
                             ps->major_ticks);
    if (!(step > 0.0))
        return;

    minors = ps->minor_ticks_per_major;

    /* First major tick is the smallest multiple of step that is not
     * less than min_value -- so a min_value of 0 lines up at the
     * start_angle, and a min_value of 3.5 with step=1 starts at 4
     * (the unmarked gap from 3.5 to 4 is the natural visual cue that
     * the scale starts mid-step). */
    first = ceil (ps->min_value / step - 1e-9) * step;

    cairo_set_source_rgba (cr,
                           ps->scale_color.red,
                           ps->scale_color.green,
                           ps->scale_color.blue,
                           ps->scale_color.alpha);

    /* Minor ticks: between each pair of consecutive major-tick value
     * positions (including the implicit boundary at min_value / first
     * and at last / max_value).  Drawn first so the bolder major
     * ticks overpaint any antialias seam.  Capped at the scale ends
     * so a value slightly past max never gets a tick. */
    cairo_set_line_width (cr, 0.8);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    have_prev = FALSE;
    prev_v    = ps->min_value;
    for (v = first; v <= ps->max_value + step * 1e-9; v += step)
    {
        double seg_lo = have_prev ? prev_v : ps->min_value;
        double seg_hi = v;

        for (j = 1; j < minors; j++)
        {
            double t   = (double) j / (double) minors;
            double mv  = seg_lo + t * (seg_hi - seg_lo);
            double ang;

            if (mv <= ps->min_value || mv >= ps->max_value)
                continue;
            ang = clock_to_cairo (value_to_angle (ps, mv));
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_minor, sin (ang) * r_minor);
        }
        prev_v    = v;
        have_prev = TRUE;
    }
    /* Trailing minors between the last major tick and max_value, when
     * the scale extends past the last nice-step mark. */
    if (have_prev && prev_v < ps->max_value)
    {
        double seg_lo = prev_v;
        double seg_hi = ps->max_value;
        for (j = 1; j < minors; j++)
        {
            double t   = (double) j / (double) minors;
            double mv  = seg_lo + t * (seg_hi - seg_lo);
            double ang;

            if (mv <= ps->min_value || mv >= ps->max_value)
                continue;
            ang = clock_to_cairo (value_to_angle (ps, mv));
            cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
            cairo_line_to (cr, cos (ang) * r_minor, sin (ang) * r_minor);
        }
    }
    cairo_stroke (cr);

    /* Major ticks at the nice-step value positions. */
    cairo_set_line_width (cr, 1.6);
    for (v = first; v <= ps->max_value + step * 1e-9; v += step)
    {
        double ang = clock_to_cairo (value_to_angle (ps, v));
        cairo_move_to (cr, cos (ang) * r_outer, sin (ang) * r_outer);
        cairo_line_to (cr, cos (ang) * r_major, sin (ang) * r_major);
    }
    cairo_stroke (cr);

    /* Digit labels.  A single Pango layout is rebuilt per tick with a
     * fresh value formatted into the same scratch buffer; that keeps
     * the per-paint allocation count to one layout (Pango reuses its
     * internal glyph cache for every set_text on the same layout). */
    layout = pango_cairo_create_layout (cr);
    fd     = pango_font_description_from_string ("Sans 9");
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);

    for (v = first; v <= ps->max_value + step * 1e-9; v += step)
    {
        double ang = clock_to_cairo (value_to_angle (ps, v));
        double cx  = cos (ang) * r_digit;
        double cy  = sin (ang) * r_digit;
        int    pw, ph;

        format_tick_label (buf, sizeof (buf), v, step);
        pango_layout_set_text (layout, buf, -1);
        pango_layout_get_pixel_size (layout, &pw, &ph);

        cairo_save (cr);
        cairo_move_to (cr, cx - pw * 0.5, cy - ph * 0.5);
        pango_cairo_show_layout (cr, layout);
        cairo_restore (cr);
    }

    g_object_unref (layout);
}

/** Paint the "glass" reflection: a single arched line splitting the
 *  face into two translucent overlays painted on top of every other
 *  face element (ticks, digits, zones, needle, pivot) so the whole
 *  face reads as if it were under a curved glass cover.  No gradient
 *  anywhere -- one solid alpha for the upper "catch-light" overlay
 *  (translucent white) and one for the lower "shaded" overlay
 *  (translucent dark grey).  The dividing line bulges upward at its
 *  midpoint and is slanted so the left endpoint sits below the right
 *  endpoint; the off-axis bend reads as a reflection of an overhead
 *  source that is itself off-centre, which is what gives the face
 *  its sense of curved glass rather than a flat painted disc.       */
static void
paint_glass (cairo_t *cr, double radius)
{
    double r = radius * 0.88;
    /* Alpha kept low so the glass tints rather than obscures the    */
    /* readout underneath -- 10 % white and 10 % dark grey is enough */
    /* to read as a glass cover on a careful look without washing    */
    /* out the tick digits.                                           */
    const double glass_alpha = 0.10;

    cairo_save (cr);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_clip (cr);

    /* Upper-region path -- the visible boundary of this fill is a   */
    /* single cubic bezier from the left endpoint to the right       */
    /* endpoint; the rest of the path goes off-screen to the top so  */
    /* the upper face arc fills in completely.                       */
    /*   left endpoint:  (-r*1.05, +r*0.18)   below the equator      */
    /*   right endpoint: (+r*1.05, -r*0.15)   above the equator      */
    /*   midpoint:       (≈0, -r*0.45)        above both endpoints,  */
    /*                                       achieved via control    */
    /*                                       points pulled up        */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, glass_alpha);
    cairo_move_to    (cr, -r * 1.50, -r * 1.50);
    cairo_line_to    (cr, -r * 1.05,  r * 0.18);
    cairo_curve_to   (cr,
                      -r * 0.40, -r * 0.55,    /* control 1 */
                      +r * 0.40, -r * 0.55,    /* control 2 */
                      +r * 1.05, -r * 0.15);   /* right endpoint */
    cairo_line_to    (cr, +r * 1.50, -r * 1.50);
    cairo_close_path (cr);
    cairo_fill (cr);

    /* Lower-region path -- the same boundary curve flipped to enclose
     * the area below the line instead of above it. */
    cairo_set_source_rgba (cr, 0.10, 0.10, 0.12, glass_alpha);
    cairo_move_to    (cr, -r * 1.50,  r * 1.50);
    cairo_line_to    (cr, -r * 1.05,  r * 0.18);
    cairo_curve_to   (cr,
                      -r * 0.40, -r * 0.55,
                      +r * 0.40, -r * 0.55,
                      +r * 1.05, -r * 0.15);
    cairo_line_to    (cr, +r * 1.50,  r * 1.50);
    cairo_close_path (cr);
    cairo_fill (cr);

    cairo_restore (cr);
}

/** Paint one line of label text horizontally centred on the face at
 *  the given baseline y offset (measured from the dial centre, in
 *  cairo pixels), using @font_desc.  No-op when @text is empty so a
 *  worksheet that wants only one of the two label lines can leave the
 *  other one unset. */
static void
paint_text_line (
        cairo_t        *cr,
        const gchar    *text,
        const gchar    *font_desc,
        const PnColor  *color,
        double          y_center)
{
    PangoLayout          *layout;
    PangoFontDescription *fd;
    int                   pw, ph;

    if (text == NULL || *text == '\0')
        return;

    layout = pango_cairo_create_layout (cr);
    fd     = pango_font_description_from_string (font_desc);
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    cairo_set_source_rgba (cr, color->red, color->green, color->blue,
                           color->alpha);
    cairo_move_to (cr, -pw * 0.5, y_center - ph * 0.5);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
}

/** Paint the descriptive label + the unit, stacked.  Both live in
 *  the empty wedge below the centre pivot that the default 240°
 *  scale sweep leaves open at 6 o'clock.  The descriptive label
 *  sits closer to the centre in a small font; the unit sits below
 *  it in a much bigger font (the unit is typically one or two
 *  characters, so the bigger size still fits comfortably and reads
 *  as the primary identifier of the value at a single glance).
 *
 *  Both lines are clipped to the bottom-wedge pie slice between the
 *  scale's two endpoint rays (with a 5° angular inset on each side)
 *  so that an over-long label or unit gets cut off at the wedge
 *  boundary rather than bleeding into the bottom-corner tick digits. */
static void
paint_label (cairo_t *cr, const PnDialPaintState *ps, double radius)
{
    /* The two y baselines were picked so the unit's bigger glyphs
     * have headroom under the centre pivot and tail-room above the
     * inner bottom tick mark, with about 4 px of clear space
     * between the two lines on the canonical 220 px face. */
    double y_label = radius * 0.32;
    double y_unit  = radius * 0.58;
    double a0;
    double a1;

    cairo_save (cr);

    /* Pie-slice clip: apex at the dial centre, two straight edges
     * along the scale's start/end rays (inset 5° toward the gap so
     * the clip stops short of the bottom-corner digit text), and a
     * curved edge at the face radius so the labels stay inside the
     * face circle.  When the scale spans a full 360° the two rays
     * collapse to one and nothing renders -- which is the right
     * outcome, since a full-circle scale has no empty wedge for the
     * labels to live in. */
    a0 = clock_to_cairo (ps->end_angle   +   5.0);
    a1 = clock_to_cairo (ps->start_angle + 360.0 - 5.0);
    cairo_move_to    (cr, 0.0, 0.0);
    cairo_arc        (cr, 0.0, 0.0, radius * 0.88, a0, a1);
    cairo_close_path (cr);
    cairo_clip (cr);

    paint_text_line (cr, ps->label, "Sans 10",
                     &ps->label_color, y_label);
    paint_text_line (cr, ps->unit,  "Sans 22",
                     &ps->label_color, y_unit);

    cairo_restore (cr);
}

/** Paint the needle: a tapered triangle pointing outward from the
 *  centre with a thin counterweight on the opposite side.  The 3D
 *  effect comes from drawing the shape twice -- once with a darker
 *  shadow offset down-right, once with the user colour on top --
 *  which reads as the needle catching light from the upper-left like
 *  the rest of the dial. */
static void
paint_needle (cairo_t *cr, const PnDialPaintState *ps, double radius)
{
    double angle;

    /* Hide the needle entirely until the first message arrives -- a
     * needle parked at min_value looks identical to a needle that has
     * received a real reading of min_value, and the user wants the
     * two states visually distinguishable.  On the first message
     * has_value flips to TRUE while display_value is still at
     * min_value (start_anim has not yet run any tick), so the needle
     * pops into view at the leftmost position and the spring then
     * sweeps it to the received value -- exactly the "needle springs
     * in from the parked position" animation the user described. */
    if (!ps->has_value)
        return;

    angle = clock_to_cairo (value_to_angle (ps, ps->display_value));
    double tip_r = radius * 0.78;      /* needle tip near outer ticks */
    double back_r = radius * 0.18;     /* short counterweight */
    double half_w = radius * 0.045;    /* needle half-width at base */
    double tx = cos (angle) * tip_r;
    double ty = sin (angle) * tip_r;
    double bx = cos (angle + M_PI) * back_r;
    double by = sin (angle + M_PI) * back_r;
    /* Perpendicular unit vector for the base of the triangle. */
    double px = -sin (angle);
    double py =  cos (angle);

    /* Shadow pass: same shape, offset 1.5 px to the lower-right, in
     * translucent black so it reads as a cast shadow on the dial face
     * regardless of the needle's user colour. */
    cairo_save (cr);
    cairo_translate (cr, 1.5, 1.5);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.35);
    cairo_move_to (cr, tx, ty);
    cairo_line_to (cr, px * half_w, py * half_w);
    cairo_line_to (cr, bx, by);
    cairo_line_to (cr, -px * half_w, -py * half_w);
    cairo_close_path (cr);
    cairo_fill (cr);
    cairo_restore (cr);

    /* Needle body: user colour. */
    cairo_set_source_rgba (cr,
                           ps->needle_color.red,
                           ps->needle_color.green,
                           ps->needle_color.blue,
                           ps->needle_color.alpha);
    cairo_move_to (cr, tx, ty);
    cairo_line_to (cr, px * half_w, py * half_w);
    cairo_line_to (cr, bx, by);
    cairo_line_to (cr, -px * half_w, -py * half_w);
    cairo_close_path (cr);
    cairo_fill_preserve (cr);

    /* Highlight stroke along one edge of the needle.  Stroking the
     * whole closed path with a thin lighter colour catches the upper
     * edge in the rendered output (the bottom edge gets the dark
     * shadow pass underneath) which is what gives the needle its 3D
     * read at this size. */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.40);
    cairo_set_line_width (cr, 0.8);
    cairo_stroke (cr);
}

/** Paint the centre pivot: a small metallic cap covering the base of
 *  the needle.  Two stacked radial gradients give the cap its
 *  three-tone bevel: a darker outer rim, a mid-tone body, and a
 *  bright catch-light in the upper-left where the bezel highlight is
 *  brightest, so the pivot reads as a polished metal dome rather
 *  than a flat disc. */
static void
paint_centre (cairo_t *cr, double radius)
{
    cairo_pattern_t *grad;
    double           r = radius * 0.11;

    /* Outer rim: dark grey ring under the dome. */
    cairo_set_source_rgb (cr, 0.20, 0.20, 0.22);
    cairo_arc (cr, 0.0, 0.0, r * 1.15, 0.0, 2.0 * M_PI);
    cairo_fill (cr);

    /* Body: bright-to-dark radial gradient. */
    grad = cairo_pattern_create_radial (-r * 0.35, -r * 0.35, r * 0.10,
                                        0.0,        0.0,      r);
    cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.95, 0.95, 0.97);
    cairo_pattern_add_color_stop_rgb (grad, 0.5, 0.60, 0.60, 0.62);
    cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.35, 0.35, 0.37);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    /* Catch-light: small bright dot in the upper-left of the dome. */
    grad = cairo_pattern_create_radial (-r * 0.30, -r * 0.30, 0.0,
                                        -r * 0.30, -r * 0.30, r * 0.45);
    cairo_pattern_add_color_stop_rgba (grad, 0.0, 1.0, 1.0, 1.0, 0.85);
    cairo_pattern_add_color_stop_rgba (grad, 1.0, 1.0, 1.0, 1.0, 0.00);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);
}

static void
pn_dial_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnDial          *self   = PN_DIAL (node);
    double           side   = (w < h) ? w : h;
    double           radius = side * 0.5 - 4.0;   /* 4 px clear of edge */
    double           cx     = x + w * 0.5;
    double           cy     = y + h * 0.5;
    double           band_radius;
    double           band_width;
    PnDialPaintState ps;

    pn_dial_get_paint_state (self, &ps);

    /* Move the origin to the dial centre once.  Every helper after
     * this point draws relative to that origin, which keeps the
     * geometry maths in each helper readable. */
    cairo_save (cr);
    cairo_translate (cr, cx, cy);

    paint_bezel (cr, radius);
    paint_face  (cr, radius, &ps.face_color);

    /* Zone arcs sit on a band that lines up with the inner edge of
     * the major ticks; band_width is the visible thickness of the
     * coloured arc. */
    band_radius = radius * 0.76;
    band_width  = radius * 0.05;

    paint_zone (cr, &ps, radius, band_radius, band_width,
                ps.green_start,  ps.green_end,  &ps.green_color);
    paint_zone (cr, &ps, radius, band_radius, band_width,
                ps.yellow_start, ps.yellow_end, &ps.yellow_color);
    paint_zone (cr, &ps, radius, band_radius, band_width,
                ps.red_start,    ps.red_end,    &ps.red_color);

    paint_ticks  (cr, &ps, radius);
    paint_label  (cr, &ps, radius);
    paint_needle (cr, &ps, radius);
    paint_centre (cr, radius);

    /* Glass goes last -- the two translucent overlays tint everything
     * underneath (digits, needle, pivot), which is how a real glass
     * cover would catch the reflection. */
    paint_glass (cr, radius);

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_class_tabs override             */
/*                                                                     */
/*  The default introspection-driven tab would lay out one row per     */
/*  writable property in a single grid -- with the dial's 21           */
/*  properties that is tall enough that the settings dialog no longer  */
/*  fits on a 1080 px screen.  Contribute four sibling pages           */
/*  (Data, Scale, Zones, Colours) directly to the dialog's top-level   */
/*  notebook via the multi-tab hook so the dialog reads as             */
/*                                                                     */
/*      Class | Node | Data | Scale | Zones | Colours                  */
/*                                                                     */
/*  with each page only as tall as its own row set.  Editors are      */
/*  built through pn_node_dialog_default_editor() so they match the   */
/*  look of every other auto-generated property tab.                  */
/* ------------------------------------------------------------------ */

/** Build a property grid containing one row per element of @names,
 *  looking each name up on @target's class.  Names that do not
 *  resolve to a writable property are silently skipped -- which lets
 *  the four category arrays below be authored as plain lists of
 *  property names without having to keep a parallel "this row is
 *  visible" flag in sync with the property table. */
static GtkWidget *
build_dial_page (
        GObject     *target,
        const gchar *const *names)
{
    GtkWidget   *grid  = pn_node_dialog_new_property_grid ();
    GObjectClass *klass = G_OBJECT_GET_CLASS (target);
    guint        row   = 0;
    guint        i;

    for (i = 0; names[i] != NULL; i++)
    {
        GParamSpec *pspec  = g_object_class_find_property (klass, names[i]);
        GtkWidget  *editor;
        const gchar *nick;

        if (pspec == NULL || (pspec->flags & G_PARAM_WRITABLE) == 0)
            continue;

        editor = pn_node_dialog_default_editor (target, pspec);
        nick   = g_param_spec_get_nick (pspec);
        pn_node_dialog_attach_row (GTK_GRID (grid), row,
                                   nick != NULL ? nick : names[i],
                                   editor);
        row += 1;
    }

    return grid;
}

static void
pn_dial_build_class_tabs (
        PnNode      *self,
        GtkNotebook *notebook,
        GtkWindow   *parent G_GNUC_UNUSED)
{
    GObject *target = G_OBJECT (self);

    /* The four category lists.  Order within each list is the order  */
    /* the rows appear on the page -- chosen to read top-down the way */
    /* a user would fill the dial in (binding first, then the         */
    /* geometry of the scale, then the optional zones, then the paint */
    /* colours that finish the look).                                  */
    static const gchar *const data_props[] = {
        "key", "label", "unit", NULL
    };
    static const gchar *const scale_props[] = {
        "min-value", "max-value",
        "start-angle", "end-angle",
        "major-ticks", "minor-ticks-per-major",
        NULL
    };
    static const gchar *const zone_props[] = {
        "green-start",  "green-end",  "green-color",
        "yellow-start", "yellow-end", "yellow-color",
        "red-start",    "red-end",    "red-color",
        NULL
    };
    static const gchar *const color_props[] = {
        "face-color", "scale-color",
        "needle-color", "label-color",
        NULL
    };

    pn_node_dialog_append_page (notebook,
                                build_dial_page (target, data_props),
                                "Data");
    pn_node_dialog_append_page (notebook,
                                build_dial_page (target, scale_props),
                                "Scale");
    pn_node_dialog_append_page (notebook,
                                build_dial_page (target, zone_props),
                                "Zones");
    pn_node_dialog_append_page (notebook,
                                build_dial_page (target, color_props),
                                "Colours");
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_dial_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_DIAL));

    node_class->paint_plot       = pn_dial_paint_plot;
    node_class->build_class_tabs = pn_dial_build_class_tabs;
    /* The dial face is circular -- the standard rectangular drop shadow
     * leaks out of the corners and reads as a rectangular ghost around
     * the otherwise-round dial. */
    node_class->paint_plot_skip_shadow = TRUE;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
