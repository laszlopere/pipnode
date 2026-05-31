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
/*  PnSunPath — gui tier.                                               */
/*                                                                     */
/*  A small orthographic 3D engine drawn with cairo.  The scene — a     */
/*  translucent ground disc carrying the N/S/E/W compass marks, one     */
/*  reference house at the centre, the Sun's full-day track arc and a    */
/*  Sun glyph riding it — is projected through an orbit camera (yaw +    */
/*  pitch) and painted back-to-front with a depth-sorted primitive list  */
/*  so the house occludes the part of the arc behind it.  The node's     */
/*  GType, properties, receive() with the NOAA arc recomputation and the */
/*  serialised view angles live in the GTK-free core file pn-sun-path.c; */
/*  this file installs the paint_plot vfunc onto that class at editor    */
/*  startup (pn_sun_path_gui_install), reading the reading + display     */
/*  config through the core's GTK-free snapshot accessor.  The headless  */
/*  runtime never loads this half, so the Sun Path logic runs without    */
/*  GTK.                                                                */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-sun-path-gui.h"
#include "pn-sun-path.h"

#include <gtk/gtk.h>
#include <pango/pangocairo.h>
#include <math.h>
#include <stdlib.h>

#define DEG (G_PI / 180.0)

#define FONT_SANS   "Sans"
#define FONT_BOLD   "Sans Bold"
#define FONT_FA     "FontAwesome"

#define ICON_MARKER "\xef\x81\x81"  /* fa-map-marker U+F041 */
#define ICON_SUN    "\xef\x86\x85"  /* fa-sun-o      U+F185 */

#define GLYPH_DEGREE   "\xc2\xb0"      /* °  */
#define GLYPH_ELLIPSIS "\xe2\x80\xa6"  /* …  */

/* ------------------------------------------------------------------ */
/*  Scene constants (world units; the dome is the unit hemisphere)     */
/* ------------------------------------------------------------------ */

#define R_GROUND     1.0    /* ground disc / horizon-circle radius       */
#define R_DOME       1.0    /* the arc rides the unit sphere             */
#define R_LABEL      1.16   /* compass labels sit just beyond the rim    */

#define HOUSE_HALF   0.22   /* footprint half-width (square)             */
#define HOUSE_WALL   0.26   /* wall height                               */
#define HOUSE_RIDGE  0.20   /* roof apex above the wall top              */

/* ------------------------------------------------------------------ */
/*  3D plumbing                                                        */
/* ------------------------------------------------------------------ */

typedef struct { double x, y, z; } V3;

typedef struct
{
    double cx, cy;     /* screen centre (device coords)        */
    double scale;      /* world unit -> pixels                 */
    double cyaw, syaw; /* cos/sin of yaw                       */
    double cpitch, spitch;
} Cam;

/* World point -> camera space.  Yaw about the up (z) axis, then pitch
 * about the (rotated) east (x) axis so the dome tips toward the viewer.
 * In camera space x is screen-right, z is screen-up, and y runs into the
 * screen (larger y = farther away → the painter's-algorithm depth key). */
static V3
to_cam (const Cam *c, V3 p)
{
    double x1 = p.x * c->cyaw - p.y * c->syaw;
    double y1 = p.x * c->syaw + p.y * c->cyaw;
    double z1 = p.z;

    V3 out;
    out.x = x1;
    out.y = y1 * c->cpitch - z1 * c->spitch;
    out.z = y1 * c->spitch + z1 * c->cpitch;
    return out;
}

static void
project (const Cam *c, V3 p, double *sx, double *sy, double *depth)
{
    V3 cm = to_cam (c, p);
    if (sx)    *sx    = c->cx + c->scale * cm.x;
    if (sy)    *sy    = c->cy - c->scale * cm.z;
    if (depth) *depth = cm.y;
}

/* Unit vector toward the Sun (or any az/alt direction), scaled by @r.
 * Azimuth is degrees clockwise from north; east is +x, north is +y. */
static V3
sky_vec (double az_deg, double alt_deg, double r)
{
    double a  = az_deg  * DEG;
    double al = alt_deg * DEG;
    double ca = cos (al);
    V3 v = { r * ca * sin (a), r * ca * cos (a), r * sin (al) };
    return v;
}

/* ------------------------------------------------------------------ */
/*  Depth-sorted primitive list                                        */
/* ------------------------------------------------------------------ */

typedef enum { K_FACE, K_SEG, K_SUN, K_LABEL } PrimKind;

typedef struct
{
    PrimKind kind;
    double   depth;          /* sort key: larger = farther = drawn first */
    double   sx[5], sy[5];   /* screen polygon / segment endpoints       */
    int      n;
    PnColor  color;
    gboolean dashed;
    double   size;           /* sun radius / label font size             */
    const gchar *text;       /* label text (borrowed)                    */
} Prim;

static int
prim_cmp (gconstpointer a, gconstpointer b)
{
    const Prim *pa = a;
    const Prim *pb = b;
    if (pa->depth < pb->depth) return  1;   /* nearer last */
    if (pa->depth > pb->depth) return -1;   /* farther first */
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Cairo / Pango text                                                 */
/* ------------------------------------------------------------------ */

static PangoLayout *
build_layout (cairo_t *cr, const gchar *font, double size, const gchar *text)
{
    PangoLayout          *l = pango_cairo_create_layout (cr);
    PangoFontDescription *d = pango_font_description_from_string (font);

    pango_font_description_set_absolute_size (d, size * PANGO_SCALE);
    pango_layout_set_font_description (l, d);
    pango_font_description_free (d);
    pango_layout_set_text (l, text, -1);
    return l;
}

static void
draw_text (cairo_t *cr, double x, double y, const gchar *font, double size,
           const gchar *text, const PnColor *c)
{
    PangoLayout *l = build_layout (cr, font, size, text);

    cairo_set_source_rgba (cr, c->red, c->green, c->blue, c->alpha);
    cairo_move_to (cr, x, y);
    pango_cairo_show_layout (cr, l);
    g_object_unref (l);
}

static void
draw_text_centered (cairo_t *cr, double cx, double cy, const gchar *font,
                    double size, const gchar *text, const PnColor *c)
{
    PangoLayout *l = build_layout (cr, font, size, text);
    int          pw, ph;

    pango_layout_get_pixel_size (l, &pw, &ph);
    cairo_set_source_rgba (cr, c->red, c->green, c->blue, c->alpha);
    cairo_move_to (cr, cx - pw / 2.0, cy - ph / 2.0);
    pango_cairo_show_layout (cr, l);
    g_object_unref (l);
}

/* Centred icon + message, used for the "no data yet" and "no position"
 * states. */
static void
paint_notice (cairo_t *cr, double w, double h,
              const gchar *line1, const gchar *line2, const PnColor *ink)
{
    PnColor faint = { ink->red, ink->green, ink->blue, 0.85 };

    draw_text_centered (cr, w / 2.0, h * 0.40, FONT_FA, h * 0.18,
                        ICON_SUN, &faint);
    draw_text_centered (cr, w / 2.0, h * 0.62, FONT_SANS, h * 0.07,
                        line1, ink);
    if (line2 != NULL)
        draw_text_centered (cr, w / 2.0, h * 0.72, FONT_SANS, h * 0.055,
                            line2, &faint);
}

/* ------------------------------------------------------------------ */
/*  Sky background                                                     */
/* ------------------------------------------------------------------ */

static void
set_sky_source (const PnSunPathSnapshot *s, cairo_t *cr,
                double x, double y, double w, double h)
{
    const PnColor *a = &s->sky_color;
    const PnColor *b = &s->sky_color2;
    cairo_pattern_t *grad;
    double x2 = x, y2 = y;

    if (s->sky_gradient == PN_SP_GRADIENT_NONE)
    {
        cairo_set_source_rgba (cr, a->red, a->green, a->blue, a->alpha);
        return;
    }

    switch (s->sky_gradient)
    {
    case PN_SP_GRADIENT_HORIZONTAL: x2 = x + w;             break;
    case PN_SP_GRADIENT_DIAGONAL:   x2 = x + w; y2 = y + h; break;
    case PN_SP_GRADIENT_VERTICAL:
    default:                        y2 = y + h;             break;
    }

    grad = cairo_pattern_create_linear (x, y, x2, y2);
    cairo_pattern_add_color_stop_rgba (grad, 0.0,
                                       a->red, a->green, a->blue, a->alpha);
    cairo_pattern_add_color_stop_rgba (grad, 1.0,
                                       b->red, b->green, b->blue, b->alpha);
    cairo_set_source (cr, grad);
    cairo_pattern_destroy (grad);
}

/* ------------------------------------------------------------------ */
/*  Ground disc + compass cross                                        */
/* ------------------------------------------------------------------ */

/* Lay down the projected outline of the ground disc as the current path
 * (no fill/stroke), so callers can fill it, stroke it, or clip to it. */
static void
ground_disc_path (const Cam *c, cairo_t *cr)
{
    const int K = 72;
    int       i;

    cairo_new_path (cr);
    for (i = 0; i <= K; i++)
    {
        double a = 2.0 * G_PI * i / K;
        V3     p = { R_GROUND * cos (a), R_GROUND * sin (a), 0.0 };
        double sx, sy;
        project (c, p, &sx, &sy, NULL);
        if (i == 0) cairo_move_to (cr, sx, sy);
        else        cairo_line_to (cr, sx, sy);
    }
    cairo_close_path (cr);
}

static void
paint_ground (const Cam *c, const PnSunPathSnapshot *s, cairo_t *cr)
{
    const PnColor *g = &s->ground_color;

    /* Filled disc. */
    ground_disc_path (c, cr);
    cairo_set_source_rgba (cr, g->red, g->green, g->blue, g->alpha);
    cairo_fill_preserve (cr);
    cairo_set_source_rgba (cr, g->red * 0.6, g->green * 0.6, g->blue * 0.6,
                           MIN (1.0, g->alpha + 0.30));
    cairo_set_line_width (cr, 1.4);
    cairo_stroke (cr);

    /* S–N and W–E axis lines through the centre. */
    {
        double sx, sy;
        cairo_set_line_width (cr, 1.2);
        cairo_set_source_rgba (cr, g->red * 0.5, g->green * 0.5, g->blue * 0.5,
                               MIN (1.0, g->alpha + 0.25));

        project (c, (V3){ 0.0,  R_GROUND, 0.0 }, &sx, &sy, NULL);
        cairo_move_to (cr, sx, sy);
        project (c, (V3){ 0.0, -R_GROUND, 0.0 }, &sx, &sy, NULL);
        cairo_line_to (cr, sx, sy);
        project (c, (V3){  R_GROUND, 0.0, 0.0 }, &sx, &sy, NULL);
        cairo_move_to (cr, sx, sy);
        project (c, (V3){ -R_GROUND, 0.0, 0.0 }, &sx, &sy, NULL);
        cairo_line_to (cr, sx, sy);
        cairo_stroke (cr);
    }
}

/* ------------------------------------------------------------------ */
/*  Below-horizon arc (faint dashed, drawn behind everything)          */
/* ------------------------------------------------------------------ */

static void
paint_underground_arc (const Cam *c, const PnSunPathSnapshot *s, cairo_t *cr)
{
    const PnColor *p = &s->path_color;
    guint          i;
    const double   dash[] = { 4.0, 4.0 };

    if (s->path == NULL || s->n_path < 2)
        return;

    cairo_save (cr);
    cairo_set_source_rgba (cr, p->red, p->green, p->blue, 0.30);
    cairo_set_line_width (cr, 1.4);
    cairo_set_dash (cr, dash, 2, 0.0);

    for (i = 0; i + 1 < s->n_path; i++)
    {
        double a0 = s->path[i].altitude;
        double a1 = s->path[i + 1].altitude;
        double sx0, sy0, sx1, sy1;

        if (a0 >= 0.0 && a1 >= 0.0)
            continue;   /* fully above: belongs to the foreground arc */

        project (c, sky_vec (s->path[i].azimuth,     a0, R_DOME),
                 &sx0, &sy0, NULL);
        project (c, sky_vec (s->path[i + 1].azimuth, a1, R_DOME),
                 &sx1, &sy1, NULL);
        cairo_move_to (cr, sx0, sy0);
        cairo_line_to (cr, sx1, sy1);
    }
    cairo_stroke (cr);
    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  House faces (appended to the depth-sorted primitive list)          */
/* ------------------------------------------------------------------ */

static void
add_face (GArray *prims, const Cam *c, const PnColor *base,
          const V3 *world, int n)
{
    Prim   prim;
    V3     cm[5];
    double depth = 0.0;
    V3     e1, e2, nrm;
    double len, lit;
    int    i;
    /* Light in camera space: upper-right, toward the viewer. */
    static const V3 L = { -0.35, -0.55, 0.74 };

    g_assert (n >= 3 && n <= 5);

    for (i = 0; i < n; i++)
    {
        cm[i] = to_cam (c, world[i]);
        prim.sx[i] = c->cx + c->scale * cm[i].x;
        prim.sy[i] = c->cy - c->scale * cm[i].z;
        depth += cm[i].y;
    }
    depth /= n;

    /* Flat-shade from the camera-space normal, oriented toward the
     * viewer so the lit side is always the one we see. */
    e1.x = cm[1].x - cm[0].x; e1.y = cm[1].y - cm[0].y; e1.z = cm[1].z - cm[0].z;
    e2.x = cm[2].x - cm[0].x; e2.y = cm[2].y - cm[0].y; e2.z = cm[2].z - cm[0].z;
    nrm.x = e1.y * e2.z - e1.z * e2.y;
    nrm.y = e1.z * e2.x - e1.x * e2.z;
    nrm.z = e1.x * e2.y - e1.y * e2.x;
    len = sqrt (nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
    if (len < 1e-9) len = 1.0;
    nrm.x /= len; nrm.y /= len; nrm.z /= len;
    if (nrm.y > 0.0) { nrm.x = -nrm.x; nrm.y = -nrm.y; nrm.z = -nrm.z; }

    lit = nrm.x * L.x + nrm.y * L.y + nrm.z * L.z;
    lit = 0.45 + 0.55 * CLAMP (lit, 0.0, 1.0);

    prim.kind   = K_FACE;
    prim.depth  = depth;
    prim.n      = n;
    prim.dashed = FALSE;
    prim.size   = 0.0;
    prim.text   = NULL;
    prim.color  = (PnColor){ base->red * lit, base->green * lit,
                             base->blue * lit, 1.0 };
    g_array_append_val (prims, prim);
}

/* Rigid quarter-turn of the house about the vertical axis: (x,y) ->
 * (-y, x).  Applied to every vertex so the ridge runs east-west. */
static V3
rot_z90 (V3 p)
{
    V3 o = { -p.y, p.x, p.z };
    return o;
}

/* The ten house vertices — b0..b3 base (z=0), w0..w3 wall top (z=hw),
 * r0/r1 ridge ends — turned 90°.  Shared by the house painter and the
 * shadow projector so the two always describe the same solid. */
static void
house_vertices (V3 v[10])
{
    const double s  = HOUSE_HALF;
    const double hw = HOUSE_WALL;
    const double hr = HOUSE_WALL + HOUSE_RIDGE;
    const V3 base[10] = {
        { -s, -s, 0 }, {  s, -s, 0 }, {  s,  s, 0 }, { -s,  s, 0 },
        { -s, -s, hw}, {  s, -s, hw}, {  s,  s, hw}, { -s,  s, hw},
        {  0, -s, hr}, {  0,  s, hr},
    };
    int i;

    for (i = 0; i < 10; i++)
        v[i] = rot_z90 (base[i]);
}

static void
add_house (GArray *prims, const Cam *c)
{
    /* Wall white and a slightly warmer roof so the planes read apart
     * even before the shading. */
    const PnColor wall = { 0.96, 0.96, 0.97, 1.0 };
    const PnColor roof = { 0.78, 0.42, 0.34, 1.0 };
    V3  v[10];
    V3  b0, b1, b2, b3, w0, w1, w2, w3, r0, r1;

    house_vertices (v);
    b0 = v[0]; b1 = v[1]; b2 = v[2]; b3 = v[3];
    w0 = v[4]; w1 = v[5]; w2 = v[6]; w3 = v[7];
    r0 = v[8]; r1 = v[9];

    /* Two plain walls and two gable-end pentagons (up to the ridge),
     * then the two roof slopes.  The face topology is the un-rotated
     * house's; the quarter-turn is baked into the vertices. */
    add_face (prims, c, &wall, (V3[]){ b1, b2, w2, w1 }, 4);
    add_face (prims, c, &wall, (V3[]){ b3, b0, w0, w3 }, 4);
    add_face (prims, c, &wall, (V3[]){ b0, b1, w1, r0, w0 }, 5);
    add_face (prims, c, &wall, (V3[]){ b2, b3, w3, r1, w2 }, 5);
    add_face (prims, c, &roof, (V3[]){ w1, w2, r1, r0 }, 4);
    add_face (prims, c, &roof, (V3[]){ w3, w0, r0, r1 }, 4);
}

/* ------------------------------------------------------------------ */
/*  House shadow (projected onto the ground along the Sun's rays)      */
/* ------------------------------------------------------------------ */

typedef struct { double x, y; } P2;

static int
p2_cmp (const void *a, const void *b)
{
    const P2 *p = a;
    const P2 *q = b;
    if (p->x < q->x) return -1;
    if (p->x > q->x) return  1;
    if (p->y < q->y) return -1;
    if (p->y > q->y) return  1;
    return 0;
}

static double
cross2 (P2 o, P2 a, P2 b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

/* Andrew's monotone-chain convex hull of @n points into @out (capacity
 * >= 2*@n + 1); returns the hull vertex count, ordered around the
 * boundary.  @pts is reordered in place. */
static int
convex_hull (P2 *pts, int n, P2 *out)
{
    int k = 0, i, lower;

    if (n < 3)
    {
        for (i = 0; i < n; i++) out[i] = pts[i];
        return n;
    }

    qsort (pts, n, sizeof (P2), p2_cmp);

    for (i = 0; i < n; i++)
    {
        while (k >= 2 && cross2 (out[k - 2], out[k - 1], pts[i]) <= 0) k--;
        out[k++] = pts[i];
    }
    lower = k + 1;
    for (i = n - 2; i >= 0; i--)
    {
        while (k >= lower && cross2 (out[k - 2], out[k - 1], pts[i]) <= 0) k--;
        out[k++] = pts[i];
    }
    return k - 1;   /* last point repeats the first */
}

/* Slide every house vertex away from the Sun, onto the ground plane, and
 * fill the resulting silhouette (the convex hull of the projected
 * vertices, the house being convex) as a soft shadow clipped to the
 * ground disc.  Skipped when the Sun is below — or barely above — the
 * horizon, where the shadow is undefined or runs off to infinity. */
static void
paint_shadow (const Cam *c, const PnSunPathSnapshot *s, cairo_t *cr)
{
    V3     v[10];
    P2     gp[10];
    P2     hull[21];
    int    nh, i;
    double alt = s->sun_altitude * DEG;
    double az  = s->sun_azimuth  * DEG;
    double cot;

    if (!s->show_house || !s->have_data || !s->success || !s->sun_up)
        return;
    if (s->sun_altitude < 3.0)
        return;

    cot = cos (alt) / sin (alt);

    house_vertices (v);
    for (i = 0; i < 10; i++)
    {
        gp[i].x = v[i].x - v[i].z * cot * sin (az);
        gp[i].y = v[i].y - v[i].z * cot * cos (az);
    }

    nh = convex_hull (gp, 10, hull);
    if (nh < 3)
        return;

    cairo_save (cr);
    ground_disc_path (c, cr);
    cairo_clip (cr);

    cairo_new_path (cr);
    for (i = 0; i < nh; i++)
    {
        double sx, sy;
        V3     p = { hull[i].x, hull[i].y, 0.0 };
        project (c, p, &sx, &sy, NULL);
        if (i == 0) cairo_move_to (cr, sx, sy);
        else        cairo_line_to (cr, sx, sy);
    }
    cairo_close_path (cr);
    cairo_set_source_rgba (cr, 0.08, 0.11, 0.10, 0.30);
    cairo_fill (cr);
    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Foreground (above-horizon) arc segments + Sun + labels             */
/* ------------------------------------------------------------------ */

static void
add_arc (GArray *prims, const Cam *c, const PnSunPathSnapshot *s)
{
    guint i;

    if (s->path == NULL)
        return;

    for (i = 0; i + 1 < s->n_path; i++)
    {
        double a0 = s->path[i].altitude;
        double a1 = s->path[i + 1].altitude;
        Prim   prim;
        double d0, d1;

        if (a0 < 0.0 && a1 < 0.0)
            continue;   /* fully below: drawn by paint_underground_arc */

        project (c, sky_vec (s->path[i].azimuth,     a0, R_DOME),
                 &prim.sx[0], &prim.sy[0], &d0);
        project (c, sky_vec (s->path[i + 1].azimuth, a1, R_DOME),
                 &prim.sx[1], &prim.sy[1], &d1);

        prim.kind   = K_SEG;
        prim.depth  = (d0 + d1) * 0.5;
        prim.n      = 2;
        prim.dashed = (a0 < 0.0 || a1 < 0.0);
        prim.size   = 0.0;
        prim.text   = NULL;
        prim.color  = s->path_color;
        g_array_append_val (prims, prim);
    }
}

static void
add_sun (GArray *prims, const Cam *c, const PnSunPathSnapshot *s)
{
    Prim   prim;
    double depth, r;
    V3     v = sky_vec (s->sun_azimuth, s->sun_altitude, R_DOME);

    project (c, v, &prim.sx[0], &prim.sy[0], &depth);
    r = MAX (4.0, c->scale * 0.075);

    prim.kind   = K_SUN;
    /* Bias the Sun slightly nearer so it is never hidden by the arc
     * segment it sits on. */
    prim.depth  = depth - 0.02;
    prim.n      = 1;
    prim.dashed = !s->sun_up;
    prim.size   = r;
    prim.text   = NULL;
    prim.color  = s->sun_color;
    g_array_append_val (prims, prim);
}

static void
add_labels (GArray *prims, const Cam *c, const PnSunPathSnapshot *s)
{
    static const struct { double az; const gchar *t; } marks[] = {
        {   0.0, "N" }, {  90.0, "E" }, { 180.0, "S" }, { 270.0, "W" },
    };
    int i;

    for (i = 0; i < 4; i++)
    {
        Prim   prim;
        double depth;
        V3     p = sky_vec (marks[i].az, 0.0, R_LABEL);

        project (c, p, &prim.sx[0], &prim.sy[0], &depth);
        prim.kind   = K_LABEL;
        prim.depth  = depth;
        prim.n      = 1;
        prim.dashed = FALSE;
        prim.size   = 0.0;
        prim.text   = marks[i].t;
        prim.color  = s->text_color;
        g_array_append_val (prims, prim);
    }
}

/* ------------------------------------------------------------------ */
/*  Primitive rendering                                                */
/* ------------------------------------------------------------------ */

static void
draw_sun_glyph (cairo_t *cr, double cx, double cy, double r,
                const PnColor *col, gboolean dim)
{
    double a;
    double alpha = dim ? 0.45 : 1.0;
    int    k;

    cairo_save (cr);
    cairo_set_source_rgba (cr, col->red, col->green, col->blue, alpha);
    cairo_set_line_width (cr, MAX (1.2, r * 0.18));
    cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
    for (k = 0; k < 8; k++)
    {
        a = k * G_PI / 4.0;
        cairo_move_to (cr, cx + cos (a) * r * 1.35, cy + sin (a) * r * 1.35);
        cairo_line_to (cr, cx + cos (a) * r * 1.85, cy + sin (a) * r * 1.85);
    }
    cairo_stroke (cr);

    cairo_arc (cr, cx, cy, r, 0.0, 2.0 * G_PI);
    cairo_set_source_rgba (cr, col->red, col->green, col->blue, alpha);
    cairo_fill_preserve (cr);
    cairo_set_source_rgba (cr, col->red * 0.7, col->green * 0.7,
                           col->blue * 0.5, alpha);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);
}

static void
draw_prim (cairo_t *cr, const Prim *p)
{
    int i;

    switch (p->kind)
    {
    case K_FACE:
        cairo_new_path (cr);
        cairo_move_to (cr, p->sx[0], p->sy[0]);
        for (i = 1; i < p->n; i++)
            cairo_line_to (cr, p->sx[i], p->sy[i]);
        cairo_close_path (cr);
        cairo_set_source_rgba (cr, p->color.red, p->color.green,
                               p->color.blue, p->color.alpha);
        cairo_fill_preserve (cr);
        cairo_set_source_rgba (cr, p->color.red * 0.55, p->color.green * 0.55,
                               p->color.blue * 0.55, 1.0);
        cairo_set_line_width (cr, 1.0);
        cairo_stroke (cr);
        break;

    case K_SEG:
        cairo_save (cr);
        cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
        if (p->dashed)
        {
            double dash[] = { 4.0, 4.0 };
            cairo_set_dash (cr, dash, 2, 0.0);
            cairo_set_line_width (cr, 1.6);
            cairo_set_source_rgba (cr, p->color.red, p->color.green,
                                   p->color.blue, 0.55);
        }
        else
        {
            cairo_set_line_width (cr, 2.6);
            cairo_set_source_rgba (cr, p->color.red, p->color.green,
                                   p->color.blue, p->color.alpha);
        }
        cairo_move_to (cr, p->sx[0], p->sy[0]);
        cairo_line_to (cr, p->sx[1], p->sy[1]);
        cairo_stroke (cr);
        cairo_restore (cr);
        break;

    case K_SUN:
        draw_sun_glyph (cr, p->sx[0], p->sy[0], p->size, &p->color, p->dashed);
        break;

    case K_LABEL:
        draw_text_centered (cr, p->sx[0], p->sy[0], FONT_BOLD,
                            14.0, p->text, &p->color);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Camera fit                                                         */
/* ------------------------------------------------------------------ */

static void
setup_camera (Cam *c, const PnSunPathSnapshot *s,
              double x, double y, double w, double h)
{
    double pitch = s->pitch * DEG;
    double cp    = cos (pitch);
    double sp    = sin (pitch);
    double top, bottom, sx, sy;

    c->cyaw   = cos (s->yaw * DEG);
    c->syaw   = sin (s->yaw * DEG);
    c->cpitch = cp;
    c->spitch = sp;

    /* Vertical extents from the scene origin, in world units.  The disc
     * (and the compass labels just beyond its rim, at R_LABEL) projects
     * to an ellipse whose half-height is R_LABEL*sp: its far rim sits
     * R_LABEL*sp above the origin, its near rim the same below.  The arc
     * peaks at the zenith, cp above.  Looking down (sp -> 1) the disc
     * fills the height, so the rim term — not the shrinking zenith — is
     * what must drive the fit, otherwise the circle overflows the frame.
     *   - top:    higher of the zenith (cp) and the far rim (R_LABEL*sp)
     *   - bottom: the near rim (R_LABEL*sp) */
    top    = fmax (cp, R_LABEL * sp);
    bottom = R_LABEL * sp;

    /* Horizontal half-extent is R_LABEL regardless of pitch (the camera
     * only tips about the east axis, leaving the x span unchanged). */
    sx = (w * 0.5 * 0.90) / R_LABEL;
    sy = (h * 0.90) / (top + bottom + 0.12);
    c->scale = MIN (sx, sy);

    /* Centre the figure vertically: the origin projects to (cx, cy), the
     * top edge to cy − scale*top and the bottom edge to cy + scale*bottom. */
    c->cx = x + w * 0.5;
    c->cy = y + h * 0.5
          + c->scale * (top - bottom) * 0.5;
}

/* ------------------------------------------------------------------ */
/*  The scene                                                          */
/* ------------------------------------------------------------------ */

static void
paint_scene (const PnSunPathSnapshot *s, cairo_t *cr,
             double x, double y, double w, double h)
{
    Cam     c;
    GArray *prims;
    guint   i;

    setup_camera (&c, s, x, y, w, h);

    /* Back-to-front floor layers: the underground arc, the ground disc,
     * then the house shadow cast onto it. */
    paint_underground_arc (&c, s, cr);
    paint_ground (&c, s, cr);
    paint_shadow (&c, s, cr);

    /* Everything with real depth goes through the sorted list. */
    prims = g_array_new (FALSE, FALSE, sizeof (Prim));
    add_arc (prims, &c, s);
    if (s->show_house)
        add_house (prims, &c);
    add_labels (prims, &c, s);
    if (s->have_data && s->success)
        add_sun (prims, &c, s);

    g_array_sort (prims, prim_cmp);
    for (i = 0; i < prims->len; i++)
        draw_prim (cr, &g_array_index (prims, Prim, i));

    g_array_free (prims, TRUE);

    /* 2D overlays: place name (top-left) and the live alt/az readout
     * (bottom-left). */
    if (s->have_data && s->success)
    {
        const PnColor *ink = &s->text_color;

        if (s->city != NULL && *s->city != '\0')
        {
            gchar *place = (s->country != NULL && *s->country != '\0')
                    ? g_strdup_printf ("%s, %s", s->city, s->country)
                    : g_strdup (s->city);
            double mw;
            PangoLayout *l = build_layout (cr, FONT_FA, h * 0.055, ICON_MARKER);
            int pw, ph;
            pango_layout_get_pixel_size (l, &pw, &ph);
            mw = pw;
            draw_text (cr, w * 0.04, h * 0.05, FONT_FA, h * 0.055,
                       ICON_MARKER, ink);
            draw_text (cr, w * 0.04 + mw + w * 0.012, h * 0.045,
                       FONT_BOLD, h * 0.062, place, ink);
            g_object_unref (l);
            g_free (place);
        }

        {
            gchar *read = g_strdup_printf (
                    "alt %.0f" GLYPH_DEGREE "   az %.0f" GLYPH_DEGREE,
                    s->sun_altitude, s->sun_azimuth);
            PnColor faint = { ink->red, ink->green, ink->blue, 0.85 };
            draw_text (cr, w * 0.04, h - h * 0.105, FONT_SANS, h * 0.052,
                       read, &faint);
            g_free (read);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  paint_plot vfunc                                                   */
/* ------------------------------------------------------------------ */

static void
pn_sun_path_paint_plot (PnNode  *node,
                        cairo_t *cr,
                        double   x,
                        double   y,
                        double   w,
                        double   h)
{
    PnSunPath        *self = PN_SUN_PATH (node);
    PnSunPathSnapshot s;

    pn_sun_path_get_snapshot (self, &s);

    /* Sky background + hairline frame, always drawn so an empty card
     * reads as a deliberate surface rather than a hole in the canvas. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    set_sky_source (&s, cr, x, y, w, h);
    cairo_fill_preserve (cr);
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.18);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);
    cairo_translate (cr, x, y);

    if (!s.have_data)
    {
        paint_notice (cr, w, h, "Waiting for sun position" GLYPH_ELLIPSIS,
                      NULL, &s.text_color);
    }
    else if (!s.success)
    {
        paint_notice (cr, w, h, "No position", s.output, &s.text_color);
    }
    else
    {
        /* paint_scene works in card-local coords thanks to the translate. */
        paint_scene (&s, cr, 0.0, 0.0, w, h);
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_sun_path_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_SUN_PATH));

    node_class->paint_plot = pn_sun_path_paint_plot;
    /* The dome is a fixed-aspect scene, so the centred zoom overlay
     * should fit it while keeping the 280:173 aspect ratio rather than
     * stretching it. */
    node_class->paint_plot_zoom_keep_aspect = TRUE;

    /* The class ref is intentionally held for the process lifetime — the
     * same lifetime the factory keeps it alive for — so the slot we just
     * wrote stays valid.  (One leaked ref on a singleton class, mirroring
     * pn_node_factory_register.) */
}
