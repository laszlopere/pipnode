/*
 * Copyright (C) 2024-2026 Laszlo Pere
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * mgl-graph-preview: standalone styling harness for the MathGL-rendered
 * 3D graphs (the multi-series cases of PnGraph).  It does NOT link the
 * pipnode libs — it just synthesises representative multi-topic data and
 * renders it with MathGL through the exact RGBA->cairo bridge the node
 * will use, then writes a PNG.  Edit, rebuild, look.  Sub-second loop.
 *
 * Build (no automake; MathGL has no .pc file, link -lmgl):
 *   gcc tools/mgl-graph-preview.c -o tools/mgl-graph-preview \
 *       $(pkg-config --cflags --libs cairo glib-2.0) -lmgl -lm
 *
 * Run:
 *   ./tools/mgl-graph-preview [out-prefix]
 * Writes <prefix>-bars.png and <prefix>-lines.png (default prefix: mgl).
 */

#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>

#include <mgl2/mgl_cf.h>

/* ------------------------------------------------------------------ */
/*  MathGL -> cairo bridge                                             */
/*                                                                     */
/*  MathGL hands back a top-down RGBA buffer (byte order R,G,B,A,      */
/*  position of {i,j} at 4*i + 4*W*j).  cairo's ARGB32 is native-      */
/*  endian 0xAARRGGBB which on little-endian is byte order B,G,R,A.    */
/*  We render onto an opaque background, so alpha is 255 everywhere    */
/*  and premultiplication is a no-op; just swap R<->B per pixel.       */
/* ------------------------------------------------------------------ */
static cairo_surface_t *
mgl_to_cairo (HMGL gr)
{
    int                  w   = mgl_get_width  (gr);
    int                  h   = mgl_get_height (gr);
    const unsigned char *src = mgl_get_rgba (gr);
    cairo_surface_t     *surf =
        cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    unsigned char       *dst    = cairo_image_surface_get_data (surf);
    int                  stride = cairo_image_surface_get_stride (surf);
    int                  x, y;

    for (y = 0; y < h; y++)
    {
        const unsigned char *s = src + (size_t) 4 * w * y;
        unsigned char       *d = dst + (size_t) stride * y;

        for (x = 0; x < w; x++, s += 4, d += 4)
        {
            d[0] = s[2];   /* B */
            d[1] = s[1];   /* G */
            d[2] = s[0];   /* R */
            d[3] = s[3];   /* A */
        }
    }

    cairo_surface_mark_dirty (surf);
    return surf;
}

/* ------------------------------------------------------------------ */
/*  Synthetic multi-series data                                        */
/* ------------------------------------------------------------------ */

#define NSERIES   4
#define NBINS_X  12      /* value bins for the histogram view */
#define NPTS     40      /* time samples for the time-series view */

static const char *series_names[NSERIES] = {
    "kitchen", "living", "outdoor", "attic"
};

/* A gaussian-ish count distribution per series, each with a different
 * mean/spread/peak so the ribbon-of-bars reads as four distinct shapes. */
static double
hist_count (int series, int bin)
{
    static const double mean[NSERIES]  = { 3.0, 6.0, 8.5, 5.0 };
    static const double sigma[NSERIES] = { 1.6, 2.2, 1.2, 3.0 };
    static const double peak[NSERIES]  = { 22,  31,  18,  14 };

    double t = ((double) bin - mean[series]) / sigma[series];
    return peak[series] * exp (-0.5 * t * t);
}

/* A smooth wiggling time series per topic. */
static double
ts_value (int series, int pt)
{
    double phase = series * 1.3;
    double base  = 4.0 + 2.0 * series;
    double x     = (double) pt / (double) NPTS * 4.0 * G_PI;
    return base + 3.0 * sin (x + phase) + 0.8 * sin (2.7 * x + phase);
}

/* ------------------------------------------------------------------ */
/*  Renderers                                                          */
/* ------------------------------------------------------------------ */

/* Per-series colour chars walked for the preview palette. */
static const char palette[NSERIES] = { 'b', 'g', 'q', 'm' };

static void
render_bars (const char *out, int W, int H)
{
    HMGL   gr = mgl_create_graph (W, H);
    double zmax = 0.0;
    int    i, j;

    for (j = 0; j < NSERIES; j++)
        for (i = 0; i < NBINS_X; i++)
        {
            double v = hist_count (j, i);
            if (v > zmax) zmax = v;
        }

    mgl_clf_rgb (gr, 1.0, 1.0, 1.0);
    mgl_set_font_size (gr, 2.6);

    mgl_set_ranges (gr, 0, NBINS_X - 1, -0.5, NSERIES - 0.5, 0, zmax * 1.05);
    mgl_rotate (gr, 50, 60, 0);

    /* Solid-looking towers: lit faces + a wide-ish footprint. */
    mgl_set_light (gr, 1);
    mgl_add_light (gr, 0, 1, 0, 3);
    mgl_set_bar_width (gr, 0.7);

    mgl_box (gr);
    mgl_axis (gr, "xyz", "", "");
    mgl_label (gr, 'x', "value", 0, "");
    mgl_label (gr, 'y', "series", 0, "");
    mgl_label (gr, 'z', "count", 0, "");

    /* One bars_xyz call per series so MathGL never connects the last
     * bar of one ribbon to the first of the next (which produced stray
     * slivers when the whole grid was flattened into one array), and so
     * each series gets its own colour. */
    for (j = 0; j < NSERIES; j++)
    {
        HMDT  X = mgl_create_data_size (NBINS_X, 1, 1);
        HMDT  Y = mgl_create_data_size (NBINS_X, 1, 1);
        HMDT  Z = mgl_create_data_size (NBINS_X, 1, 1);
        char  pen[2] = { palette[j], '\0' };

        for (i = 0; i < NBINS_X; i++)
        {
            mgl_data_set_value (X, (double) i, i, 0, 0);
            mgl_data_set_value (Y, (double) j, i, 0, 0);
            mgl_data_set_value (Z, hist_count (j, i), i, 0, 0);
        }

        mgl_bars_xyz (gr, X, Y, Z, pen, "");

        mgl_delete_data (X);
        mgl_delete_data (Y);
        mgl_delete_data (Z);
    }

    {
        cairo_surface_t *s = mgl_to_cairo (gr);
        cairo_surface_write_to_png (s, out);
        cairo_surface_destroy (s);
    }

    mgl_delete_graph (gr);
    g_print ("wrote %s (%dx%d)\n", out, W, H);
}

static void
render_lines (const char *out, int W, int H)
{
    HMGL gr = mgl_create_graph (W, H);
    double zmin = 1e30, zmax = -1e30;
    int    i, j;

    for (j = 0; j < NSERIES; j++)
        for (i = 0; i < NPTS; i++)
        {
            double v = ts_value (j, i);
            if (v < zmin) zmin = v;
            if (v > zmax) zmax = v;
        }

    mgl_clf_rgb (gr, 1.0, 1.0, 1.0);
    mgl_set_font_size (gr, 2.6);

    mgl_set_ranges (gr, 0, NPTS - 1, -0.5, NSERIES - 0.5, zmin, zmax);
    mgl_rotate (gr, 50, 60, 0);

    mgl_box (gr);
    mgl_axis (gr, "xyz", "", "");
    mgl_label (gr, 'x', "time", 0, "");
    mgl_label (gr, 'y', "series", 0, "");

    /* One curve per series at its own y-depth. */
    for (j = 0; j < NSERIES; j++)
    {
        HMDT  X = mgl_create_data_size (NPTS, 1, 1);
        HMDT  Y = mgl_create_data_size (NPTS, 1, 1);
        HMDT  Z = mgl_create_data_size (NPTS, 1, 1);
        char  pen[3] = { palette[j], '2', '\0' };   /* colour + width 2 */

        for (i = 0; i < NPTS; i++)
        {
            mgl_data_set_value (X, (double) i, i, 0, 0);
            mgl_data_set_value (Y, (double) j, i, 0, 0);
            mgl_data_set_value (Z, ts_value (j, i), i, 0, 0);
        }

        mgl_plot_xyz (gr, X, Y, Z, pen, "");

        mgl_delete_data (X);
        mgl_delete_data (Y);
        mgl_delete_data (Z);
    }

    {
        cairo_surface_t *s = mgl_to_cairo (gr);
        cairo_surface_write_to_png (s, out);
        cairo_surface_destroy (s);
    }

    mgl_delete_graph (gr);
    g_print ("wrote %s (%dx%d)\n", out, W, H);
}

int
main (int argc, char **argv)
{
    const char *prefix = (argc > 1) ? argv[1] : "mgl";
    char       *bars   = g_strdup_printf ("%s-bars.png",  prefix);
    char       *lines  = g_strdup_printf ("%s-lines.png", prefix);

    (void) series_names;

    render_bars  (bars,  900, 640);
    render_lines (lines, 900, 640);

    g_free (bars);
    g_free (lines);
    return 0;
}
