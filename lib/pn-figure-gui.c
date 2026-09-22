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
/*  PnFigure — the painter (TODO #80.25).                              */
/*                                                                     */
/*  The gui half of the figure node: it asks pn_figure_render() for a  */
/*  resolved display list, already in device units with every          */
/*  expression evaluated and every coordinate mapped, and strokes it.  */
/*  It evaluates nothing and maps nothing.  That is not tidiness — it  */
/*  is what lets 80.12's tests assert the whole node headless, and it  */
/*  is what makes "nothing is drawn" honest: by the time the first     */
/*  stroke goes down the core has already decided that the frame is    */
/*  complete (80.10d).                                                 */
/*                                                                     */
/*  Two things the core deliberately left here, and only two:          */
/*                                                                     */
/*  GLYPH EXTENTS (80.7f).  Core cannot measure a glyph without a font */
/*  backend, so a `text` operation carries its anchor point and its    */
/*  alignment enums and this file works out the offset.  It is the one */
/*  place the seam is not a clean "core computes, painter strokes",    */
/*  and it is admitted rather than discovered.                         */
/*                                                                     */
/*  THE DEVICE HAIRLINE (80.5c).  `width 0` resolves to 0.0, which is  */
/*  the PostScript convention for "one device pixel however far the    */
/*  worksheet is zoomed in" — a length only the CTM knows.             */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-figure-gui.h"
#include "pn-figure.h"

#include <gtk/gtk.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <string.h>

/* The error message's size at the node's at-rest width, scaled with the
 * rectangle so the zoom overlay enlarges it like everything else. */
#define PN_FIGURE_ERROR_FONT    11.0
#define PN_FIGURE_ERROR_PADDING  8.0

/* Line advance as a multiple of the font size, for a `text` with `\n`
 * in it (80.7d).  Fixed rather than taken from the font's own metrics,
 * so a four-line bench card sits the same whatever family it is set in. */
#define PN_FIGURE_LINE_SPACING 1.2

/* ------------------------------------------------------------------ */
/*  The pen the painter carries                                        */
/*                                                                     */
/*  Everything here is DEVICE units and comes straight off the display */
/*  list: the painter holds no defaults, because every frame opens     */
/*  with the whole pen state written out (80.5g).  A figure therefore  */
/*  cannot inherit a colour or a width from the frame before it, even  */
/*  by accident.                                                       */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnColor        stroke;
    PnColor        fill;
    gboolean       filling;
    gdouble        width;       /* 0 = a device hairline               */
    gdouble        dashes[4];
    gint           n_dashes;
    gdouble        font;
    PnFigureHAlign halign;
    PnFigureVAlign valign;

    /* From the view: the per-axis scale over the length scale.  Both 1
     * unless `stretch` is on, and what a circle is drawn inside so that
     * it becomes the right ellipse (80.6e). */
    gdouble        scale_x;
    gdouble        scale_y;
} Pen;

static void
pen_init (
        Pen *self)
{
    static const PnColor black = { 0.0, 0.0, 0.0, 1.0 };

    self->stroke   = black;
    self->fill     = black;
    self->filling  = FALSE;
    self->width    = 1.0;
    self->n_dashes = 0;
    self->font     = 1.0;
    self->halign   = PN_FIGURE_HALIGN_CENTRE;
    self->valign   = PN_FIGURE_VALIGN_MIDDLE;
    self->scale_x  = 1.0;
    self->scale_y  = 1.0;
}

static void
set_source (
        cairo_t       *cr,
        const PnColor *color)
{
    cairo_set_source_rgba (cr, color->red, color->green,
                           color->blue, color->alpha);
}

/* The stroke width in user space.  A resolved 0.0 is 80.5(c)'s
 * hairline: one DEVICE pixel, so it stays a hairline however far the
 * worksheet is zoomed in — which is the whole point of the escape, and
 * the reason the CTM has to be asked rather than assumed. */
static gdouble
stroke_width (
        cairo_t   *cr,
        const Pen *pen)
{
    gdouble dx = 1.0;
    gdouble dy = 1.0;

    if (pen->width != 0.0)
        return pen->width;

    cairo_device_to_user_distance (cr, &dx, &dy);
    return MAX (fabs (dx), fabs (dy));
}

/* Arm the context for a stroke: the stroke colour, the width, and the
 * dash pattern the pen is carrying. */
static void
apply_stroke (
        cairo_t   *cr,
        const Pen *pen)
{
    set_source (cr, &pen->stroke);
    cairo_set_line_width (cr, stroke_width (cr, pen));
    cairo_set_dash (cr, pen->dashes, pen->n_dashes, 0.0);
}

/* Fill then stroke (80.5e): a shape drawn with filling on is filled in
 * the fill colour and then outlined in the STROKE colour, which is why
 * the two are separate state and neither sets the other — the fulcrum
 * of a lever is a grey triangle with a dark outline, and the whole
 * style depends on that look.
 *
 * The path is consumed either way. */
static void
fill_and_stroke (
        cairo_t   *cr,
        const Pen *pen)
{
    if (pen->filling)
    {
        set_source (cr, &pen->fill);
        cairo_fill_preserve (cr);
    }

    apply_stroke (cr, pen);
    cairo_stroke (cr);
}

/* ------------------------------------------------------------------ */
/*  Text                                                               */
/* ------------------------------------------------------------------ */

/* A layout for one line at the pen's font size.  The family is a node
 * property rather than a verb, so one figure stays typographically
 * consistent (80.7i); empty means whatever Pango's default is, which is
 * why the family is left UNSET rather than spelled "Sans". */
static PangoLayout *
build_layout (
        cairo_t     *cr,
        const gchar *family,
        gdouble      size,
        const gchar *text)
{
    PangoLayout          *layout = pango_cairo_create_layout (cr);
    PangoFontDescription *desc   = pango_font_description_new ();

    if (family != NULL && *family != '\0')
        pango_font_description_set_family (desc, family);

    /* Absolute, not points (80.7e): the size is already in device units
     * and must scale with the drawing, not with the screen's DPI. */
    pango_font_description_set_absolute_size (desc, size * PANGO_SCALE);
    pango_layout_set_font_description (layout, desc);
    pango_font_description_free (desc);

    pango_layout_set_text (layout, text, -1);
    return layout;
}

/* The left edge at which a line of width @width sits, for an anchor at
 * @x — the horizontal half of the offset core could not compute. */
static gdouble
halign_left (
        PnFigureHAlign align,
        gdouble        x,
        gdouble        width)
{
    switch (align)
    {
    case PN_FIGURE_HALIGN_LEFT:   return x;
    case PN_FIGURE_HALIGN_RIGHT:  return x - width;
    case PN_FIGURE_HALIGN_CENTRE:
    default:                      return x - width / 2.0;
    }
}

/* One `text` operation: the string at its anchor, every line anchored
 * per the alignment and stacked downward at 1.2x the font size (80.7d).
 *
 * Glyphs come out upright for free, because 80.4(a) keeps the y flip
 * out of the CTM — and they are not stretched either, since the font
 * size is the single length scale and a squashed label helps nobody. */
static void
draw_text (
        cairo_t          *cr,
        const Pen        *pen,
        const gchar      *family,
        const PnFigureOp *op)
{
    gchar   **lines;
    guint     n;
    guint     i;
    gdouble   advance = pen->font * PN_FIGURE_LINE_SPACING;
    gdouble   top;
    gdouble   first_h = 0.0;
    gdouble   first_baseline = 0.0;

    if (op->text == NULL || *op->text == '\0')
        return;

    lines = g_strsplit (op->text, "\n", -1);
    n     = g_strv_length (lines);
    if (n == 0)
    {
        g_strfreev (lines);
        return;
    }

    /* The first line's own metrics settle where the block starts; every
     * line after it is one fixed advance further down. */
    {
        PangoLayout *layout = build_layout (cr, family, pen->font, lines[0]);
        gint         pw, ph;

        pango_layout_get_pixel_size (layout, &pw, &ph);
        first_h        = ph;
        first_baseline = pango_layout_get_baseline (layout)
                         / (gdouble) PANGO_SCALE;
        g_object_unref (layout);
    }

    switch (pen->valign)
    {
    case PN_FIGURE_VALIGN_TOP:
        top = op->y;
        break;
    case PN_FIGURE_VALIGN_BOTTOM:
        top = op->y - ((n - 1) * advance + first_h);
        break;
    case PN_FIGURE_VALIGN_BASELINE:
        /* The FIRST line's baseline sits on the anchor, which is what
         * "baseline" means everywhere else in typography. */
        top = op->y - first_baseline;
        break;
    case PN_FIGURE_VALIGN_MIDDLE:
    default:
        top = op->y - ((n - 1) * advance + first_h) / 2.0;
        break;
    }

    set_source (cr, &pen->stroke);

    for (i = 0; i < n; i++)
    {
        PangoLayout *layout = build_layout (cr, family, pen->font, lines[i]);
        gint         pw, ph;

        pango_layout_get_pixel_size (layout, &pw, &ph);
        cairo_move_to (cr, halign_left (pen->halign, op->x, pw),
                       top + i * advance);
        pango_cairo_show_layout (cr, layout);
        g_object_unref (layout);
    }

    g_strfreev (lines);
}

/* ------------------------------------------------------------------ */
/*  Shapes                                                             */
/* ------------------------------------------------------------------ */

/* A circle or an arc, built inside a translate-and-scale so that it
 * becomes the right ELLIPSE when `stretch` is on and cairo_arc's round
 * one is not what was asked for (80.6e).  The scale is undone before
 * the path is painted, so the stroke width stays uniform: cairo has
 * already recorded the path by then.
 *
 * The sweep is the gotcha of 80.6(d), and the arithmetic for it is
 * core's: @a0 and @a1 are DEVICE degrees and @negative says plainly
 * which of the two cairo calls to make, so the y flip is not rediscovered
 * here. */
static void
arc_path (
        cairo_t          *cr,
        const Pen        *pen,
        const PnFigureOp *op,
        gboolean          whole)
{
    cairo_save (cr);
    cairo_translate (cr, op->x, op->y);
    cairo_scale (cr, pen->scale_x, pen->scale_y);

    if (whole)
        cairo_arc (cr, 0.0, 0.0, op->r, 0.0, 2.0 * G_PI);
    else if (op->negative)
        cairo_arc_negative (cr, 0.0, 0.0, op->r,
                            op->a0 * G_PI / 180.0, op->a1 * G_PI / 180.0);
    else
        cairo_arc (cr, 0.0, 0.0, op->r,
                   op->a0 * G_PI / 180.0, op->a1 * G_PI / 180.0);

    cairo_restore (cr);
}

/* The x,y pairs of a LINE, POLY or PATH as a cairo path. */
static gboolean
points_path (
        cairo_t          *cr,
        const PnFigureOp *op,
        gboolean          close)
{
    guint i;

    if (op->points == NULL || op->points->len < 4)
        return FALSE;

    cairo_move_to (cr, g_array_index (op->points, gdouble, 0),
                       g_array_index (op->points, gdouble, 1));
    for (i = 2; i + 1 < op->points->len; i += 2)
        cairo_line_to (cr, g_array_index (op->points, gdouble, i),
                           g_array_index (op->points, gdouble, i + 1));

    if (close)
        cairo_close_path (cr);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  The walk                                                           */
/* ------------------------------------------------------------------ */

static void
paint_ops (
        cairo_t     *cr,
        GPtrArray   *ops,
        const gchar *family)
{
    Pen   pen;
    guint i;

    pen_init (&pen);

    /* Round cap and round join, fixed, no verb (80.5f): cairo defaults
     * to a butt cap and a mitre join, and mitre spikes on the sharp
     * corner of a `poly` are ugly and surprising. */
    cairo_set_line_cap  (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

    for (i = 0; i < ops->len; i++)
    {
        const PnFigureOp *op = g_ptr_array_index (ops, i);

        switch (op->kind)
        {
        case PN_FIGURE_OP_VIEW:
            pen.scale_x = op->scale_x;
            pen.scale_y = op->scale_y;
            break;

        case PN_FIGURE_OP_COLOR:
            pen.stroke = op->color;
            break;

        case PN_FIGURE_OP_FILL:
            pen.fill    = op->color;
            pen.filling = TRUE;
            break;

        case PN_FIGURE_OP_NOFILL:
            pen.filling = FALSE;
            break;

        case PN_FIGURE_OP_WIDTH:
            pen.width = op->value;
            break;

        case PN_FIGURE_OP_DASH:
            pen.n_dashes = op->n_dashes;
            memcpy (pen.dashes, op->dashes, sizeof pen.dashes);
            break;

        case PN_FIGURE_OP_FONT:
            pen.font = op->value;
            break;

        case PN_FIGURE_OP_ALIGN:
            pen.halign = op->halign;
            pen.valign = op->valign;
            break;

        case PN_FIGURE_OP_MOVE:
            /* No ink.  The pen position is core's business: every verb
             * that draws from it already arrived here as two explicit
             * device points. */
            break;

        case PN_FIGURE_OP_LINE:
        case PN_FIGURE_OP_PATH:
            /* An open run of segments only ever strokes — there is no
             * implicit closure for a fill to expose (80.6g). */
            if (points_path (cr, op, FALSE))
            {
                apply_stroke (cr, &pen);
                cairo_stroke (cr);
            }
            break;

        case PN_FIGURE_OP_POLY:
            if (points_path (cr, op, TRUE))
                fill_and_stroke (cr, &pen);
            break;

        case PN_FIGURE_OP_POINT:
            /* A filled disc in the STROKE colour, sized by the current
             * width (80.6h) — so it is not a shape the fill state acts
             * on, and it is not stretched either: it is the pen's own
             * nib, marking a hinge or a mass. */
            cairo_arc (cr, op->x, op->y, op->r, 0.0, 2.0 * G_PI);
            set_source (cr, &pen.stroke);
            cairo_fill (cr);
            break;

        case PN_FIGURE_OP_CIRCLE:
            arc_path (cr, &pen, op, TRUE);
            fill_and_stroke (cr, &pen);
            break;

        case PN_FIGURE_OP_ARC:
            arc_path (cr, &pen, op, FALSE);
            fill_and_stroke (cr, &pen);
            break;

        case PN_FIGURE_OP_RECT:
            cairo_rectangle (cr, op->x, op->y, op->w, op->h);
            fill_and_stroke (cr, &pen);
            break;

        case PN_FIGURE_OP_TEXT:
            draw_text (cr, &pen, family, op);
            break;

        case PN_FIGURE_OP_SKIP:
            /* A statement that was not run, and why.  It is in the list
             * so a dump can say so (80.10b); there is nothing to paint,
             * and above all the node does not go red for it. */
            break;

        default:
            g_warn_if_reached ();
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  The error message                                                  */
/* ------------------------------------------------------------------ */

/* The client area is this node's ONLY channel (80.10): it is a sink, so
 * there is no failure message to emit, and pipnode launches from a
 * desktop launcher with no terminal to warn into.  If the card does not
 * say it, nobody learns it.
 *
 * So the message is drawn in place of the figure, WRAPPED and not
 * truncated (80.10e) — a parse message with a line and a column in it
 * is worth reading in full — in the same red the worksheet paints a
 * broken node's header, so the card reads as one thing. */
static void
paint_error (
        cairo_t     *cr,
        const gchar *text,
        const gchar *family,
        gdouble      x,
        gdouble      y,
        gdouble      w,
        gdouble      h)
{
    static const PnColor red = { 0.86, 0.30, 0.28, 1.0 };

    PangoLayout *layout;
    gdouble      pad  = PN_FIGURE_ERROR_PADDING;
    gdouble      size = PN_FIGURE_ERROR_FONT * (w / PN_FIGURE_WIDTH);
    gint         pw, ph;

    if (w <= 2.0 * pad)
        return;

    layout = build_layout (cr, family, MAX (size, 6.0), text);
    pango_layout_set_width (layout, (gint) ((w - 2.0 * pad) * PANGO_SCALE));
    pango_layout_set_wrap  (layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_get_pixel_size (layout, &pw, &ph);

    set_source (cr, &red);
    cairo_move_to (cr, x + pad, y + MAX ((h - ph) / 2.0, pad));
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
}

/* ------------------------------------------------------------------ */
/*  PnNodeClass::paint_plot                                            */
/* ------------------------------------------------------------------ */

static void
pn_figure_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnFigure    *self = PN_FIGURE (node);
    GPtrArray   *ops;
    PnColor      background;
    const gchar *error;

    cairo_save (cr);

    /* We clip, because the worksheet does not (80.4e) — one stray
     * coordinate would otherwise paint across the whole canvas. */
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);

    /* The background goes down before the program runs (80.4f), so the
     * letterbox bars the fitted window leaves over are simply
     * background and an empty figure still reads as a deliberate area. */
    pn_figure_get_background_color (self, &background);
    set_source (cr, &background);
    cairo_rectangle (cr, x, y, w, h);
    cairo_fill (cr);

    /* Resolve first, then ask what went wrong: rendering is what
     * settles the runtime half of the error state. */
    ops   = pn_figure_render (self, x, y, w, h);
    error = pn_figure_get_error (self);

    if (*error != '\0')
        paint_error (cr, error, pn_figure_get_font_family (self),
                     x, y, w, h);
    else
        paint_ops (cr, ops, pn_figure_get_font_family (self));

    g_ptr_array_unref (ops);
    cairo_restore (cr);
}

void
pn_figure_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_FIGURE));

    node_class->paint_plot = pn_figure_paint_plot;

    /* The class ref is intentionally held for the process lifetime — the
     * same lifetime the factory keeps it alive for — so the slot we just
     * wrote stays valid.  (One leaked ref on a singleton class, mirroring
     * pn_plot_gui_install.) */
}
