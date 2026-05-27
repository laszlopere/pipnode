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
/*  PnAnalogClock — gui tier.                                          */
/*                                                                     */
/*  The cairo wall-clock face drawing for the Analogue Clock node: an   */
/*  off-white face inside a slim metallic bezel, twelve chunky black    */
/*  hour markers, sixty thin minute pips around the rim, a configurable */
/*  caption below the centre pivot, three drop-shadowed hands (hour /   */
/*  minute / seconds) and a polished metal centre pivot.  The node's     */
/*  GType, properties, receive() and the hours/minutes/seconds          */
/*  breakdown live in the GTK-free core file pn-analog-clock.c; this    */
/*  file installs the paint_plot vfunc slot onto that class at editor   */
/*  startup (pn_analog_clock_gui_install), reading the drawing state    */
/*  through the core's GTK-free snapshot accessor                       */
/*  pn_analog_clock_get_paint_state().  The headless runtime never      */
/*  loads this half, so the Analogue Clock logic runs without GTK.      */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-analog-clock-gui.h"
#include "pn-analog-clock.h"

#include <gtk/gtk.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Convert a clock-angle (degrees, clockwise from 12 o'clock) into a
 * cairo angle (radians, clockwise from +x axis under cairo's default
 * y-down coordinates).  At clock_angle = 0 the result is -π/2 which is
 * the top of the dial. */
static inline double
clock_to_cairo (double clock_deg)
{
    return (clock_deg - 90.0) * M_PI / 180.0;
}

/* ------------------------------------------------------------------ */
/*  Bezel + face                                                       */
/* ------------------------------------------------------------------ */

/* Polished-steel bezel ring framing the dial face.  Same construction
 * as the Dial node — a vertical light-to-dark gradient for the outer
 * disc and a reversed gradient for the inner rim — so the family reads
 * as the same instrument set on a worksheet. */
static void
paint_bezel (cairo_t *cr, double radius)
{
    cairo_pattern_t *grad;
    double           outer = radius;
    double           inner = radius * 0.93;

    grad = cairo_pattern_create_linear (0.0, -outer, 0.0, outer);
    cairo_pattern_add_color_stop_rgb (grad, 0.00, 0.92, 0.92, 0.94);
    cairo_pattern_add_color_stop_rgb (grad, 0.45, 0.45, 0.45, 0.48);
    cairo_pattern_add_color_stop_rgb (grad, 0.55, 0.30, 0.30, 0.32);
    cairo_pattern_add_color_stop_rgb (grad, 1.00, 0.78, 0.78, 0.80);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, outer, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    grad = cairo_pattern_create_linear (0.0, -inner, 0.0, inner);
    cairo_pattern_add_color_stop_rgb (grad, 0.00, 0.55, 0.55, 0.57);
    cairo_pattern_add_color_stop_rgb (grad, 0.50, 0.85, 0.85, 0.87);
    cairo_pattern_add_color_stop_rgb (grad, 1.00, 0.40, 0.40, 0.42);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, inner * 1.03, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);

    cairo_set_source_rgba (cr, 0.10, 0.10, 0.10, 0.55);
    cairo_set_line_width (cr, 0.8);
    cairo_arc (cr, 0.0, 0.0, inner * 0.985, 0.0, 2.0 * M_PI);
    cairo_stroke (cr);
}

/* Off-white dial face with a subtle radial brightness bump in the
 * centre — gives the face just enough curvature to read as glass-
 * covered ceramic instead of a flat painted disc. */
static void
paint_face (cairo_t *cr, double radius, const PnColor *face)
{
    cairo_pattern_t *grad;
    double           r = radius * 0.91;
    double           cr_r, cg, cb;

    cr_r = face->red   * 0.90 + 0.10;
    cg   = face->green * 0.90 + 0.10;
    cb   = face->blue  * 0.90 + 0.10;

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

/* ------------------------------------------------------------------ */
/*  Hour markers + minute pips                                         */
/* ------------------------------------------------------------------ */

/* Twelve rectangular markers at the hour positions.  Drawn by pushing
 * the rotation onto the cairo matrix, painting an upright bar at the
 * right radius, then popping — keeps the geometry trivially symmetric
 * regardless of which clock hour we are at. */
static void
paint_hour_markers (cairo_t *cr, double radius, const PnColor *col)
{
    double r_outer = radius * 0.88;
    double bar_len = radius * 0.10;
    double bar_w   = radius * 0.020;
    int    i;

    cairo_set_source_rgba (cr, col->red, col->green, col->blue, col->alpha);

    for (i = 0; i < 12; i++)
    {
        double ang = clock_to_cairo ((double) i * 30.0);

        cairo_save (cr);
        cairo_rotate (cr, ang + M_PI_2);  /* +π/2 so the bar runs radially */
        cairo_rectangle (cr,
                         r_outer - bar_len, -bar_w * 0.5,
                         bar_len,             bar_w);
        cairo_fill (cr);
        cairo_restore (cr);
    }
}

/* Sixty short minute pips — the four pips between every pair of hour
 * markers are skipped (the hour marker already occupies that slot). */
static void
paint_minute_pips (cairo_t *cr, double radius, const PnColor *col)
{
    double r_outer = radius * 0.88;
    double pip_len = radius * 0.045;
    double pip_w   = radius * 0.012;
    int    i;

    cairo_set_source_rgba (cr, col->red, col->green, col->blue, col->alpha);

    for (i = 0; i < 60; i++)
    {
        double ang;

        if (i % 5 == 0)
            continue;  /* covered by an hour marker */

        ang = clock_to_cairo ((double) i * 6.0);

        cairo_save (cr);
        cairo_rotate (cr, ang + M_PI_2);
        cairo_rectangle (cr,
                         r_outer - pip_len, -pip_w * 0.5,
                         pip_len,             pip_w);
        cairo_fill (cr);
        cairo_restore (cr);
    }
}

/* Twelve Arabic hour numerals (1..12) sitting just inside the marker
 * ring.  Each numeral is centred on its hour angle so the layout reads
 * as the painted print on a wall-clock face — 12 at top, 6 at bottom,
 * 3 and 9 on the equator.  The Pango layout is rebuilt per glyph but
 * the layout object is reused, so the per-paint cost is one set of
 * font metrics. */
static void
paint_hour_numerals (cairo_t *cr, double radius, const PnColor *col)
{
    PangoLayout          *layout;
    PangoFontDescription *fd;
    gchar                *font;
    /* The hour markers run from r_outer (0.88) inward by bar_len
     * (0.10) so their inner edge sits at 0.78; placing the numeral
     * centre at 0.66 leaves a hair of clearance for the widest
     * glyphs ("10", "11", "12") whose bounding boxes extend further
     * radially than the single-digit values. */
    double                r_digit = radius * 0.66;
    int                   font_px = (int) (radius * 0.13);
    int                   i;

    if (font_px < 8) font_px = 8;

    layout = pango_cairo_create_layout (cr);
    font   = g_strdup_printf ("Sans Bold %d", font_px);
    fd     = pango_font_description_from_string (font);
    g_free (font);
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);

    cairo_set_source_rgba (cr, col->red, col->green, col->blue, col->alpha);

    for (i = 1; i <= 12; i++)
    {
        gchar  buf[8];
        double ang = clock_to_cairo ((double) i * 30.0);
        double cx  = cos (ang) * r_digit;
        double cy  = sin (ang) * r_digit;
        int    pw, ph;

        g_snprintf (buf, sizeof (buf), "%d", i);
        pango_layout_set_text (layout, buf, -1);
        pango_layout_get_pixel_size (layout, &pw, &ph);

        cairo_move_to (cr, cx - pw * 0.5, cy - ph * 0.5);
        pango_cairo_show_layout (cr, layout);
    }

    g_object_unref (layout);
}

/* ------------------------------------------------------------------ */
/*  Hands                                                              */
/* ------------------------------------------------------------------ */

/* Trace a rectangular-hand path: a flat-ended bar of constant width
 * running from a short tail behind the pivot to the tip, drawn in a
 * local frame that the caller has already rotated to the hand's clock
 * angle (the hand points along the -y axis after rotation).  The
 * shape is filled by the caller; this routine only lays down the path
 * so the caller can fill it twice (shadow + body). */
static void
trace_hand (cairo_t *cr, double length, double base_w, double tail)
{
    double hw = base_w * 0.5;

    cairo_new_sub_path (cr);
    cairo_rectangle (cr, -hw, -length, base_w, length + tail);
}

/* Paint one hand at the given clock angle, drop-shadow first, then
 * body on top.  The shadow is the same path translated by a small
 * lower-right offset and filled in translucent black, so it reads as
 * cast onto the dial face regardless of the hand's user colour. */
static void
paint_hand (cairo_t       *cr,
            double         clock_deg,
            double         length,
            double         base_w,
            double         tail,
            const PnColor *col)
{
    /* Shadow pass: world-space offset, then rotate into the hand's
     * local frame.  Using a world-space translate keeps the shadow
     * direction (down-right) constant for every hand regardless of the
     * angle the hand points at — what a real cast shadow does. */
    cairo_save (cr);
    cairo_translate (cr, 2.0, 2.5);
    cairo_rotate (cr, clock_to_cairo (clock_deg) + M_PI_2);
    trace_hand (cr, length, base_w, tail);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.32);
    cairo_fill (cr);
    cairo_restore (cr);

    /* Body pass. */
    cairo_save (cr);
    cairo_rotate (cr, clock_to_cairo (clock_deg) + M_PI_2);
    trace_hand (cr, length, base_w, tail);
    cairo_set_source_rgba (cr, col->red, col->green, col->blue, col->alpha);
    cairo_fill_preserve (cr);

    /* Thin highlight stroke along one edge — at this size it reads as
     * a polished bevel on the hand. */
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width (cr, 0.6);
    cairo_stroke (cr);
    cairo_restore (cr);
}

/* Seconds hand: a thin needle with a small counterweight disc near
 * the pivot.  Painted shadow-then-body the same way as the broad
 * hands. */
static void
paint_second_hand (cairo_t       *cr,
                   double         clock_deg,
                   double         length,
                   const PnColor *col)
{
    double tail_len = length * 0.22;

    /* Shadow pass. */
    cairo_save (cr);
    cairo_translate (cr, 2.0, 2.5);
    cairo_rotate (cr, clock_to_cairo (clock_deg) + M_PI_2);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.32);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width (cr, length * 0.025);
    cairo_move_to (cr, 0.0, tail_len);
    cairo_line_to (cr, 0.0, -length);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* Body pass. */
    cairo_save (cr);
    cairo_rotate (cr, clock_to_cairo (clock_deg) + M_PI_2);
    cairo_set_source_rgba (cr, col->red, col->green, col->blue, col->alpha);
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_width (cr, length * 0.025);
    cairo_move_to (cr, 0.0, tail_len);
    cairo_line_to (cr, 0.0, -length);
    cairo_stroke (cr);
    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Centre pivot                                                       */
/* ------------------------------------------------------------------ */

/* Polished metal cap covering the base of the three hands.  Two
 * stacked radial gradients give it a three-tone bevel so it reads as a
 * dome rather than a flat disc; copied from the Dial node so the
 * gauge family stays visually consistent. */
static void
paint_pivot (cairo_t *cr, double radius)
{
    cairo_pattern_t *grad;
    double           r = radius * 0.045;

    cairo_set_source_rgb (cr, 0.12, 0.12, 0.13);
    cairo_arc (cr, 0.0, 0.0, r * 1.30, 0.0, 2.0 * M_PI);
    cairo_fill (cr);

    grad = cairo_pattern_create_radial (-r * 0.35, -r * 0.35, r * 0.10,
                                        0.0,        0.0,      r);
    cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.95, 0.95, 0.97);
    cairo_pattern_add_color_stop_rgb (grad, 0.5, 0.55, 0.55, 0.57);
    cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.25, 0.25, 0.27);
    cairo_set_source (cr, grad);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);
    cairo_pattern_destroy (grad);
}

/* ------------------------------------------------------------------ */
/*  Text caption                                                       */
/* ------------------------------------------------------------------ */

/* The configurable caption sits between the centre pivot and the 6
 * o'clock marker, in the place a wall-clock manufacturer's logo would
 * normally occupy.  Bold to read at a glance; clipped to a horizontal
 * band so an over-long string is cut off rather than overlapping the
 * hour markers. */
static void
paint_text (cairo_t       *cr,
            const gchar   *text,
            double         radius,
            const PnColor *col)
{
    PangoLayout          *layout;
    PangoFontDescription *fd;
    gchar                *font;
    int                   pw, ph;
    double                cy = radius * 0.28;
    double                size = radius * 0.09;

    if (text == NULL || *text == '\0')
        return;
    if (size < 7.0) size = 7.0;

    layout = pango_cairo_create_layout (cr);
    font   = g_strdup_printf ("Sans %d", (int) size);
    fd     = pango_font_description_from_string (font);
    g_free (font);
    pango_layout_set_font_description (layout, fd);
    pango_font_description_free (fd);
    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    cairo_save (cr);
    /* Horizontal-band clip: width = inner-marker band, so a long
     * string is trimmed rather than bleeding past the hour markers. */
    cairo_rectangle (cr, -radius * 0.55, cy - ph,
                     radius * 1.10, ph * 2.0);
    cairo_clip (cr);

    cairo_set_source_rgba (cr, col->red, col->green, col->blue, col->alpha);
    cairo_move_to (cr, -pw * 0.5, cy - ph * 0.5);
    pango_cairo_show_layout (cr, layout);
    cairo_restore (cr);

    g_object_unref (layout);
}

/* ------------------------------------------------------------------ */
/*  Glass                                                              */
/* ------------------------------------------------------------------ */

/* Single arched line splits the face into a translucent catch-light
 * overlay above and a translucent shaded overlay below — sells the
 * curved glass cover.  Copied verbatim from the Dial node's painter
 * so the two displays catch light from the same imaginary source. */
static void
paint_glass (cairo_t *cr, double radius)
{
    double r = radius * 0.91;
    const double glass_alpha = 0.10;

    cairo_save (cr);
    cairo_arc (cr, 0.0, 0.0, r, 0.0, 2.0 * M_PI);
    cairo_clip (cr);

    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, glass_alpha);
    cairo_move_to    (cr, -r * 1.50, -r * 1.50);
    cairo_line_to    (cr, -r * 1.05,  r * 0.18);
    cairo_curve_to   (cr,
                      -r * 0.40, -r * 0.55,
                      +r * 0.40, -r * 0.55,
                      +r * 1.05, -r * 0.15);
    cairo_line_to    (cr, +r * 1.50, -r * 1.50);
    cairo_close_path (cr);
    cairo_fill (cr);

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

/* ------------------------------------------------------------------ */
/*  Paint entry                                                        */
/* ------------------------------------------------------------------ */

static void
pn_analog_clock_paint_plot (PnNode  *node,
                            cairo_t *cr,
                            double   x,
                            double   y,
                            double   w,
                            double   h)
{
    PnAnalogClock           *self   = PN_ANALOG_CLOCK (node);
    double                   side   = (w < h) ? w : h;
    double                   radius = side * 0.5 - 4.0;   /* 4 px clear of edge */
    double                   cx     = x + w * 0.5;
    double                   cy     = y + h * 0.5;
    PnAnalogClockPaintState  ps;
    double                   hour_deg, minute_deg, second_deg;
    int                      hours_12;

    pn_analog_clock_get_paint_state (self, &ps);

    cairo_save (cr);
    cairo_translate (cr, cx, cy);

    paint_bezel        (cr, radius);
    paint_face         (cr, radius, &ps.face_color);
    paint_minute_pips   (cr, radius, &ps.marker_color);
    paint_hour_markers  (cr, radius, &ps.marker_color);
    paint_hour_numerals (cr, radius, &ps.marker_color);
    paint_text          (cr, ps.text, radius, &ps.text_color);

    /* Hand angles: 12-hour face, so a reading of 13h sweeps the hour
     * hand back to the 1 o'clock position; the hour hand moves
     * continuously between markers (mins * 0.5°). */
    hours_12   = ps.hours % 12;
    hour_deg   = (double) hours_12 * 30.0
               + (double) ps.minutes * 0.5
               + (double) ps.seconds * (0.5 / 60.0);
    minute_deg = (double) ps.minutes * 6.0
               + (double) ps.seconds * 0.1;
    second_deg = (double) ps.seconds * 6.0;

    paint_hand (cr, hour_deg,
                radius * 0.52,  radius * 0.038, radius * 0.10,
                &ps.hour_hand_color);
    paint_hand (cr, minute_deg,
                radius * 0.76,  radius * 0.028, radius * 0.10,
                &ps.minute_hand_color);

    if (ps.show_seconds)
        paint_second_hand (cr, second_deg,
                           radius * 0.80,
                           &ps.second_hand_color);

    paint_pivot (cr, radius);
    paint_glass (cr, radius);

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_analog_clock_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_ANALOG_CLOCK));

    node_class->paint_plot       = pn_analog_clock_paint_plot;
    /* The face is circular — the standard rectangular drop shadow leaks
     * out of the corners and reads as a square ghost around the
     * otherwise-round face, exactly the same trade-off the Dial node
     * makes. */
    node_class->paint_plot_skip_shadow = TRUE;

    /* The class ref is intentionally held for the process lifetime so
     * the slots we just wrote stay valid (one leaked ref on a
     * singleton class, mirroring the other gui-tier installers). */
}
