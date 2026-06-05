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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-image-ops.h"
#include "pn-image-message.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/*  Small numeric helpers                                              */
/* ------------------------------------------------------------------ */

static inline gint
clampi (gint v, gint lo, gint hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline guchar
clamp_u8 (gdouble v)
{
    if (v <= 0.0)   return 0;
    if (v >= 255.0) return 255;
    return (guchar) (v + 0.5);
}

/* Rec.601 luma of an RGB triple, the perceptual grey every effect here
 * that "desaturates first" agrees on. */
static inline gdouble
luma_of (guchar r, guchar g, guchar b)
{
    return 0.299 * r + 0.587 * g + 0.114 * b;
}

/* ------------------------------------------------------------------ */
/*  Pixbuf scaffolding                                                 */
/* ------------------------------------------------------------------ */

GdkPixbuf *
pn_image_new_like (GdkPixbuf *src)
{
    return gdk_pixbuf_new (GDK_COLORSPACE_RGB,
                           gdk_pixbuf_get_has_alpha (src),
                           8,
                           gdk_pixbuf_get_width  (src),
                           gdk_pixbuf_get_height (src));
}

/* One luma byte per pixel, row-major w×h.  Caller frees with g_free. */
static guchar *
extract_luma (GdkPixbuf *src)
{
    const gint    w   = gdk_pixbuf_get_width  (src);
    const gint    h   = gdk_pixbuf_get_height (src);
    const gint    nch = gdk_pixbuf_get_n_channels (src);
    const gint    rs  = gdk_pixbuf_get_rowstride  (src);
    const guchar *sp  = gdk_pixbuf_get_pixels     (src);
    guchar       *luma = g_malloc ((gsize) w * h);
    gint          x, y;

    for (y = 0; y < h; y++)
    {
        const guchar *row = sp + (gsize) y * rs;
        for (x = 0; x < w; x++)
        {
            const guchar *p = row + x * nch;
            luma[(gsize) y * w + x] = clamp_u8 (luma_of (p[0], p[1], p[2]));
        }
    }
    return luma;
}

GdkPixbuf *
pn_image_map_point (GdkPixbuf      *src,
                    PnImagePointFn  fn,
                    PnNode         *node)
{
    GdkPixbuf *dst = gdk_pixbuf_copy (src);  /* keeps pixels + alpha */
    gint       w, h, nch, rs, x, y;
    guchar    *base;

    if (dst == NULL)
        return NULL;

    w    = gdk_pixbuf_get_width  (dst);
    h    = gdk_pixbuf_get_height (dst);
    nch  = gdk_pixbuf_get_n_channels (dst);
    rs   = gdk_pixbuf_get_rowstride  (dst);
    base = gdk_pixbuf_get_pixels     (dst);

    for (y = 0; y < h; y++)
    {
        guchar *row = base + (gsize) y * rs;
        for (x = 0; x < w; x++)
            fn (row + x * nch, node);
    }
    return dst;
}

GdkPixbuf *
pn_image_map_point_xy (GdkPixbuf        *src,
                       PnImagePointXYFn  fn,
                       PnNode           *node)
{
    GdkPixbuf *dst = gdk_pixbuf_copy (src);  /* keeps pixels + alpha */
    gint       w, h, nch, rs, x, y;
    guchar    *base;

    if (dst == NULL)
        return NULL;

    w    = gdk_pixbuf_get_width  (dst);
    h    = gdk_pixbuf_get_height (dst);
    nch  = gdk_pixbuf_get_n_channels (dst);
    rs   = gdk_pixbuf_get_rowstride  (dst);
    base = gdk_pixbuf_get_pixels     (dst);

    for (y = 0; y < h; y++)
    {
        guchar *row = base + (gsize) y * rs;
        for (x = 0; x < w; x++)
            fn (row + x * nch, x, y, w, h, node);
    }
    return dst;
}

void
pn_image_rgb_to_hsv (guchar r, guchar g, guchar b,
                     gdouble *h, gdouble *s, gdouble *v)
{
    const gdouble rf  = r / 255.0;
    const gdouble gf  = g / 255.0;
    const gdouble bf  = b / 255.0;
    const gdouble max = MAX (rf, MAX (gf, bf));
    const gdouble min = MIN (rf, MIN (gf, bf));
    const gdouble d   = max - min;
    gdouble       hue = 0.0;

    if (d > 0.0)
    {
        if (max == rf)
            hue = 60.0 * fmod ((gf - bf) / d, 6.0);
        else if (max == gf)
            hue = 60.0 * (((bf - rf) / d) + 2.0);
        else
            hue = 60.0 * (((rf - gf) / d) + 4.0);
    }
    if (hue < 0.0)
        hue += 360.0;

    *h = hue;
    *s = (max <= 0.0) ? 0.0 : d / max;
    *v = max;
}

void
pn_image_hsv_to_rgb (gdouble h, gdouble s, gdouble v,
                     guchar *r, guchar *g, guchar *b)
{
    gdouble rf = v, gf = v, bf = v;

    if (s > 0.0)
    {
        gdouble c, x, m;
        gint    sextant;

        h = fmod (h, 360.0);
        if (h < 0.0)
            h += 360.0;
        h /= 60.0;

        sextant = (gint) h;
        c = v * s;
        x = c * (1.0 - fabs (fmod (h, 2.0) - 1.0));
        m = v - c;

        switch (sextant)
        {
        case 0:  rf = c; gf = x; bf = 0; break;
        case 1:  rf = x; gf = c; bf = 0; break;
        case 2:  rf = 0; gf = c; bf = x; break;
        case 3:  rf = 0; gf = x; bf = c; break;
        case 4:  rf = x; gf = 0; bf = c; break;
        default: rf = c; gf = 0; bf = x; break;
        }
        rf += m; gf += m; bf += m;
    }

    *r = clamp_u8 (rf * 255.0);
    *g = clamp_u8 (gf * 255.0);
    *b = clamp_u8 (bf * 255.0);
}

GdkPixbuf *
pn_image_convolve (GdkPixbuf     *src,
                   const gdouble *kernel,
                   gint           n,
                   gdouble        divisor,
                   gdouble        bias,
                   gboolean       gray_first)
{
    const gint    w    = gdk_pixbuf_get_width  (src);
    const gint    h    = gdk_pixbuf_get_height (src);
    const gint    nch  = gdk_pixbuf_get_n_channels (src);
    const gint    rs_s = gdk_pixbuf_get_rowstride  (src);
    const guchar *sp   = gdk_pixbuf_get_pixels     (src);
    const gint    half = n / 2;
    GdkPixbuf    *dst  = pn_image_new_like (src);
    gint          rs_d, x, y, kx, ky, c;
    guchar       *dp;
    guchar       *luma = NULL;

    if (dst == NULL)
        return NULL;
    if (divisor == 0.0)
        divisor = 1.0;

    rs_d = gdk_pixbuf_get_rowstride (dst);
    dp   = gdk_pixbuf_get_pixels    (dst);

    if (gray_first)
        luma = extract_luma (src);

    for (y = 0; y < h; y++)
    {
        guchar *drow = dp + (gsize) y * rs_d;

        for (x = 0; x < w; x++)
        {
            guchar *dpix = drow + x * nch;

            if (gray_first)
            {
                gdouble acc = 0.0;
                for (ky = -half; ky <= half; ky++)
                {
                    const gint sy = clampi (y + ky, 0, h - 1);
                    for (kx = -half; kx <= half; kx++)
                    {
                        const gint sx = clampi (x + kx, 0, w - 1);
                        acc += kernel[(ky + half) * n + (kx + half)]
                             * luma[(gsize) sy * w + sx];
                    }
                }
                dpix[0] = dpix[1] = dpix[2] = clamp_u8 (acc / divisor + bias);
            }
            else
            {
                for (c = 0; c < 3; c++)
                {
                    gdouble acc = 0.0;
                    for (ky = -half; ky <= half; ky++)
                    {
                        const gint sy = clampi (y + ky, 0, h - 1);
                        const guchar *srow = sp + (gsize) sy * rs_s;
                        for (kx = -half; kx <= half; kx++)
                        {
                            const gint sx = clampi (x + kx, 0, w - 1);
                            acc += kernel[(ky + half) * n + (kx + half)]
                                 * srow[sx * nch + c];
                        }
                    }
                    dpix[c] = clamp_u8 (acc / divisor + bias);
                }
            }

            if (nch == 4)
                dpix[3] = sp[(gsize) y * rs_s + x * nch + 3];
        }
    }

    g_free (luma);
    return dst;
}

GdkPixbuf *
pn_image_gradient (GdkPixbuf     *src,
                   const gdouble *kx_kernel,
                   const gdouble *ky_kernel,
                   gint           n)
{
    const gint    w    = gdk_pixbuf_get_width  (src);
    const gint    h    = gdk_pixbuf_get_height (src);
    const gint    nch  = gdk_pixbuf_get_n_channels (src);
    const gint    rs_s = gdk_pixbuf_get_rowstride  (src);
    const guchar *sp   = gdk_pixbuf_get_pixels     (src);
    const gint    half = n / 2;
    GdkPixbuf    *dst  = pn_image_new_like (src);
    guchar       *luma;
    gint          rs_d, x, y, i, j;
    guchar       *dp;

    if (dst == NULL)
        return NULL;

    luma = extract_luma (src);
    rs_d = gdk_pixbuf_get_rowstride (dst);
    dp   = gdk_pixbuf_get_pixels    (dst);

    for (y = 0; y < h; y++)
    {
        guchar *drow = dp + (gsize) y * rs_d;

        for (x = 0; x < w; x++)
        {
            gdouble gx = 0.0, gy = 0.0, mag;
            guchar *dpix = drow + x * nch;

            for (j = -half; j <= half; j++)
            {
                const gint sy = clampi (y + j, 0, h - 1);
                for (i = -half; i <= half; i++)
                {
                    const gint    sx = clampi (x + i, 0, w - 1);
                    const guchar  l  = luma[(gsize) sy * w + sx];
                    const gint    k  = (j + half) * n + (i + half);
                    gx += kx_kernel[k] * l;
                    gy += ky_kernel[k] * l;
                }
            }

            mag = sqrt (gx * gx + gy * gy);
            dpix[0] = dpix[1] = dpix[2] = clamp_u8 (mag);

            if (nch == 4)
                dpix[3] = sp[(gsize) y * rs_s + x * nch + 3];
        }
    }

    g_free (luma);
    return dst;
}

GdkPixbuf *
pn_image_separable (GdkPixbuf     *src,
                    const gdouble *k1d,
                    gint           n,
                    gdouble        divisor)
{
    const gint    w    = gdk_pixbuf_get_width  (src);
    const gint    h    = gdk_pixbuf_get_height (src);
    const gint    nch  = gdk_pixbuf_get_n_channels (src);
    const gint    rs_s = gdk_pixbuf_get_rowstride  (src);
    const guchar *sp   = gdk_pixbuf_get_pixels     (src);
    const gint    half = n / 2;
    GdkPixbuf    *tmp, *dst;
    gint          rs_t, rs_d, x, y, k, c;
    guchar       *tp, *dp;

    if (divisor == 0.0)
    {
        gint i;
        divisor = 0.0;
        for (i = 0; i < n; i++)
            divisor += k1d[i];
        if (divisor == 0.0)
            divisor = 1.0;
    }

    tmp = pn_image_new_like (src);
    dst = pn_image_new_like (src);
    if (tmp == NULL || dst == NULL)
    {
        g_clear_object (&tmp);
        g_clear_object (&dst);
        return NULL;
    }

    rs_t = gdk_pixbuf_get_rowstride (tmp);
    tp   = gdk_pixbuf_get_pixels    (tmp);
    rs_d = gdk_pixbuf_get_rowstride (dst);
    dp   = gdk_pixbuf_get_pixels    (dst);

    /* Horizontal pass: src -> tmp. */
    for (y = 0; y < h; y++)
    {
        const guchar *srow = sp + (gsize) y * rs_s;
        guchar       *trow = tp + (gsize) y * rs_t;

        for (x = 0; x < w; x++)
        {
            for (c = 0; c < 3; c++)
            {
                gdouble acc = 0.0;
                for (k = 0; k < n; k++)
                {
                    const gint sx = clampi (x + k - half, 0, w - 1);
                    acc += k1d[k] * srow[sx * nch + c];
                }
                trow[x * nch + c] = clamp_u8 (acc / divisor);
            }
            if (nch == 4)
                trow[x * nch + 3] = srow[x * nch + 3];
        }
    }

    /* Vertical pass: tmp -> dst. */
    for (y = 0; y < h; y++)
    {
        guchar *drow = dp + (gsize) y * rs_d;

        for (x = 0; x < w; x++)
        {
            for (c = 0; c < 3; c++)
            {
                gdouble acc = 0.0;
                for (k = 0; k < n; k++)
                {
                    const gint sy = clampi (y + k - half, 0, h - 1);
                    acc += k1d[k] * tp[(gsize) sy * rs_t + x * nch + c];
                }
                drow[x * nch + c] = clamp_u8 (acc / divisor);
            }
            if (nch == 4)
                drow[x * nch + 3] = tp[(gsize) y * rs_t + x * nch + 3];
        }
    }

    g_object_unref (tmp);
    return dst;
}

static int
cmp_u8 (const void *a, const void *b)
{
    return (gint) *(const guchar *) a - (gint) *(const guchar *) b;
}

GdkPixbuf *
pn_image_median (GdkPixbuf *src, gint radius)
{
    const gint    w    = gdk_pixbuf_get_width  (src);
    const gint    h    = gdk_pixbuf_get_height (src);
    const gint    nch  = gdk_pixbuf_get_n_channels (src);
    const gint    rs_s = gdk_pixbuf_get_rowstride  (src);
    const guchar *sp   = gdk_pixbuf_get_pixels     (src);
    const gint    n    = 2 * radius + 1;
    const gint    area = n * n;
    GdkPixbuf    *dst;
    guchar       *dp, *window;
    gint          rs_d, x, y, c, i, j;

    if (radius < 1)
        return gdk_pixbuf_copy (src);

    dst = pn_image_new_like (src);
    if (dst == NULL)
        return NULL;

    rs_d   = gdk_pixbuf_get_rowstride (dst);
    dp     = gdk_pixbuf_get_pixels    (dst);
    window = g_malloc (area);

    for (y = 0; y < h; y++)
    {
        guchar *drow = dp + (gsize) y * rs_d;

        for (x = 0; x < w; x++)
        {
            guchar *dpix = drow + x * nch;

            for (c = 0; c < 3; c++)
            {
                gint k = 0;
                for (j = -radius; j <= radius; j++)
                {
                    const gint    sy   = clampi (y + j, 0, h - 1);
                    const guchar *srow = sp + (gsize) sy * rs_s;
                    for (i = -radius; i <= radius; i++)
                    {
                        const gint sx = clampi (x + i, 0, w - 1);
                        window[k++] = srow[sx * nch + c];
                    }
                }
                qsort (window, area, 1, cmp_u8);
                dpix[c] = window[area / 2];
            }

            if (nch == 4)
                dpix[3] = sp[(gsize) y * rs_s + x * nch + 3];
        }
    }

    g_free (window);
    return dst;
}

GdkPixbuf *
pn_image_compose (GdkPixbuf        *a,
                  GdkPixbuf        *b,
                  gdouble           opacity,
                  PnImageComposeFn  fn)
{
    /* Bigger image (by area) becomes the canvas; the other is composited
     * at the top-left.  A tie keeps input 0 (a) as the base. */
    GdkPixbuf    *base    = a;
    GdkPixbuf    *overlay = b;
    GdkPixbuf    *out;
    gint          ow, oh, o_rs, o_nch, vw, vh, v_rs, v_nch, cw, ch, x, y, c;
    guchar       *o_px;
    const guchar *v_px;

    if ((gint64) gdk_pixbuf_get_width (b) * gdk_pixbuf_get_height (b)
      > (gint64) gdk_pixbuf_get_width (a) * gdk_pixbuf_get_height (a))
    {
        base    = b;
        overlay = a;
    }

    out = gdk_pixbuf_copy (base);   /* base pixels + alpha */
    if (out == NULL)
        return NULL;

    ow    = gdk_pixbuf_get_width      (out);
    oh    = gdk_pixbuf_get_height     (out);
    o_rs  = gdk_pixbuf_get_rowstride  (out);
    o_nch = gdk_pixbuf_get_n_channels (out);
    o_px  = gdk_pixbuf_get_pixels     (out);

    vw    = gdk_pixbuf_get_width      (overlay);
    vh    = gdk_pixbuf_get_height     (overlay);
    v_rs  = gdk_pixbuf_get_rowstride  (overlay);
    v_nch = gdk_pixbuf_get_n_channels (overlay);
    v_px  = gdk_pixbuf_get_pixels     (overlay);

    /* Only the top-left overlap is composited; the rest of the canvas
     * keeps the bigger image untouched. */
    cw = MIN (ow, vw);
    ch = MIN (oh, vh);

    for (y = 0; y < ch; y++)
    {
        guchar       *orow = o_px + (gsize) y * o_rs;
        const guchar *vrow = v_px + (gsize) y * v_rs;

        for (x = 0; x < cw; x++)
        {
            guchar       *o = orow + (gsize) x * o_nch;
            const guchar *v = vrow + (gsize) x * v_nch;

            for (c = 0; c < 3; c++)
            {
                const guchar combined = fn (o[c], v[c]);
                o[c] = clamp_u8 (o[c] * (1.0 - opacity)
                                 + combined * opacity);
            }
            /* o[3] (alpha), if present, is left as the base copy. */
        }
    }

    return out;
}

/* ------------------------------------------------------------------ */
/*  Receive / emit driver                                             */
/* ------------------------------------------------------------------ */

/* Copy a string member from one message's data bag to another's, if
 * present and a string — so the transformed image keeps the originating
 * file's filename / path / mimetype for downstream savers and labels. */
static void
carry_string (PnMessage *from, PnMessage *to, const gchar *name)
{
    JsonObject *data = pn_message_get_data (from);
    JsonNode   *node;

    if (data == NULL || !json_object_has_member (data, name))
        return;

    node = json_object_get_member (data, name);
    if (node != NULL
        && JSON_NODE_HOLDS_VALUE (node)
        && json_node_get_value_type (node) == G_TYPE_STRING)
        pn_message_set_string (to, name, json_node_get_string (node));
}

/* Build the result image message for a finished transform and emit it.
 * Runs on the main thread (the GTask completion callback). */
static void
emit_image_result (PnNode      *node,
                   PnMessage   *source,
                   const gchar *label,
                   GdkPixbuf   *out)
{
    PnImageMessage *msg = pn_image_message_new (node, NULL, out);
    const gint      iw  = gdk_pixbuf_get_width  (out);
    const gint      ih  = gdk_pixbuf_get_height (out);
    gchar          *summary;

    carry_string (source, PN_MESSAGE (msg), "filename");
    carry_string (source, PN_MESSAGE (msg), "path");
    carry_string (source, PN_MESSAGE (msg), "mimetype");

    pn_message_set_boolean (PN_MESSAGE (msg), "image",   TRUE);
    pn_message_set_int     (PN_MESSAGE (msg), "width",   iw);
    pn_message_set_int     (PN_MESSAGE (msg), "height",  ih);
    pn_message_set_boolean (PN_MESSAGE (msg), "success", TRUE);

    summary = g_strdup_printf ("%s (%d×%d)", label, iw, ih);
    pn_message_set_string (PN_MESSAGE (msg), "output", summary);
    g_free (summary);

    pn_node_emit_message (node, PN_MESSAGE (msg));
    g_object_unref (msg);
}

/* ------------------------------------------------------------------ */
/*  Off-thread transform driver                                       */
/*                                                                    */
/*  The pixel work in fn() is O(width·height·kernel) and can take     */
/*  hundreds of milliseconds on a large image — long enough to freeze */
/*  the GTK main loop if run inline in receive().  So each transform  */
/*  runs on a GTask worker thread and its result is emitted back on   */
/*  the main loop in the completion callback.  GTask holds a reference */
/*  on the node for the task's lifetime, so the node cannot be        */
/*  finalised while a transform is in flight.                         */
/*                                                                    */
/*  Only one transform runs per node at a time; an image arriving     */
/*  while one is running is held as the single "pending" job, the     */
/*  latest replacing any earlier one, so a burst (dragging a settings */
/*  slider, say) coalesces to one queued transform instead of piling  */
/*  up worker threads.                                                */
/* ------------------------------------------------------------------ */

#define IMAGE_ASYNC_STATE_KEY "pn-image-async-state"

typedef struct {
    gboolean   busy;       /* a transform is currently running */
    PnMessage *pending;    /* latest image awaiting its turn, or NULL */
} ImageAsyncState;

static void
image_async_state_free (gpointer data)
{
    ImageAsyncState *st = data;
    g_clear_object (&st->pending);
    g_free (st);
}

/* Per-node async bookkeeping, created lazily and freed with the node. */
static ImageAsyncState *
image_async_state (PnNode *node)
{
    ImageAsyncState *st = g_object_get_data (G_OBJECT (node),
                                             IMAGE_ASYNC_STATE_KEY);
    if (st == NULL)
    {
        st = g_new0 (ImageAsyncState, 1);
        g_object_set_data_full (G_OBJECT (node), IMAGE_ASYNC_STATE_KEY,
                                st, image_async_state_free);
    }
    return st;
}

typedef struct {
    PnMessage          *message;   /* ref'd: carries the source pixbuf + strings */
    PnImageTransformFn  fn;
    const gchar        *label;     /* static literal owned by the caller */
} ImageJob;

static void
image_job_free (gpointer data)
{
    ImageJob *job = data;
    g_clear_object (&job->message);
    g_free (job);
}

/* Worker thread: run the transform and hand the new pixbuf back. */
static void
image_job_run (GTask        *task,
               gpointer      source,
               gpointer      task_data,
               GCancellable *cancellable)
{
    ImageJob  *job = task_data;
    GdkPixbuf *src = pn_image_message_get_pixbuf (PN_IMAGE_MESSAGE (job->message));
    GdkPixbuf *out = (src != NULL) ? job->fn (src, PN_NODE (source)) : NULL;

    (void) cancellable;
    g_task_return_pointer (task, out, out != NULL ? g_object_unref : NULL);
}

static void start_image_job (PnNode             *node,
                             PnMessage          *message,
                             const gchar        *label,
                             PnImageTransformFn  fn);

/* Main loop: emit the finished image, then start the pending one if any. */
static void
image_job_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    PnNode          *node = PN_NODE (source);
    ImageAsyncState *st   = image_async_state (node);
    ImageJob        *job  = g_task_get_task_data (G_TASK (result));
    GdkPixbuf       *out  = g_task_propagate_pointer (G_TASK (result), NULL);

    (void) user_data;

    if (out != NULL)
    {
        emit_image_result (node, job->message, job->label, out);
        g_object_unref (out);
    }
    else
    {
        /* Transform failed; don't drop the message, just forward it. */
        pn_node_emit_message (node, job->message);
    }

    if (st->pending != NULL)
    {
        PnMessage *next = g_steal_pointer (&st->pending);
        start_image_job (node, next, job->label, job->fn);   /* stays busy */
        g_object_unref (next);
    }
    else
    {
        st->busy = FALSE;
        /* Queue drained: close the glow opened in pn_image_node_process. */
        pn_node_processing_end (node);
    }
}

static void
start_image_job (PnNode             *node,
                 PnMessage          *message,
                 const gchar        *label,
                 PnImageTransformFn  fn)
{
    ImageJob *job  = g_new0 (ImageJob, 1);
    GTask    *task = g_task_new (node, NULL, image_job_done, NULL);

    job->message = g_object_ref (message);
    job->fn      = fn;
    job->label   = label;

    g_task_set_task_data (task, job, image_job_free);
    g_task_run_in_thread (task, image_job_run);
    g_object_unref (task);
}

void
pn_image_node_process (PnNode             *node,
                       PnMessage          *message,
                       const gchar        *label,
                       PnImageTransformFn  fn)
{
    ImageAsyncState *st;

    /* Not an image (or an empty image message): pass straight through so
     * this stage is transparent to non-image traffic on the wire.  This
     * is cheap, so it stays inline on the main thread. */
    if (!PN_IS_IMAGE_MESSAGE (message)
        || pn_image_message_get_pixbuf (PN_IMAGE_MESSAGE (message)) == NULL)
    {
        pn_node_emit_message (node, message);
        return;
    }

    /* The pixel work is heavy enough to freeze the UI on a large image,
     * so it runs on a worker thread (see the driver comment above). */
    st = image_async_state (node);
    if (st->busy)
    {
        /* Coalesce: keep only the latest image (ref before clearing so a
         * repeat of the same message is handled safely). */
        g_object_ref (message);
        g_clear_object (&st->pending);
        st->pending = message;
        return;
    }

    st->busy = TRUE;

    /* Light the processing glow for the off-thread pixel work; the
     * receive wrap only covers this synchronous dispatch.  Held across a
     * coalesced burst (image_job_done re-arms while busy stays TRUE) and
     * closed when the queue drains in image_job_done. */
    pn_node_processing_begin (node);

    start_image_job (node, message, label, fn);
}

/* ------------------------------------------------------------------ */
/*  Two-input off-thread driver                                       */
/*                                                                    */
/*  A two-input node (Blend) cannot act until it has an image on both */
/*  ports, and the two images arrive in separate messages at separate */
/*  times.  So this driver latches the most recent pixbuf per input   */
/*  and only schedules work once both are present, then runs fn() —    */
/*  which receives both images — on a worker thread exactly like the   */
/*  single-input path.  Bursts coalesce through a "dirty" flag: an     */
/*  image arriving mid-transform marks the node dirty and the latest   */
/*  pair is re-run when the current job finishes, so dragging a slider */
/*  never piles up worker threads.                                     */
/* ------------------------------------------------------------------ */

#define IMAGE_ASYNC_STATE2_KEY "pn-image-async-state2"

typedef struct {
    GdkPixbuf *latest[2];  /* most recent image per input, ref'd, or NULL */
    PnMessage *msg;        /* most recent triggering message (for metadata) */
    gboolean   busy;       /* a blend is currently running */
    gboolean   dirty;      /* a new input arrived while busy */
} ImageAsyncState2;

static void
image_async_state2_free (gpointer data)
{
    ImageAsyncState2 *st = data;
    g_clear_object (&st->latest[0]);
    g_clear_object (&st->latest[1]);
    g_clear_object (&st->msg);
    g_free (st);
}

static ImageAsyncState2 *
image_async_state2 (PnNode *node)
{
    ImageAsyncState2 *st = g_object_get_data (G_OBJECT (node),
                                              IMAGE_ASYNC_STATE2_KEY);
    if (st == NULL)
    {
        st = g_new0 (ImageAsyncState2, 1);
        g_object_set_data_full (G_OBJECT (node), IMAGE_ASYNC_STATE2_KEY,
                                st, image_async_state2_free);
    }
    return st;
}

typedef struct {
    GdkPixbuf      *a;        /* ref'd snapshot of input 0 */
    GdkPixbuf      *b;        /* ref'd snapshot of input 1 */
    PnMessage      *message;  /* ref'd: source of the carried-over metadata */
    PnImageBlendFn  fn;
    const gchar    *label;    /* static literal owned by the caller */
} ImageJob2;

static void
image_job2_free (gpointer data)
{
    ImageJob2 *job = data;
    g_clear_object (&job->a);
    g_clear_object (&job->b);
    g_clear_object (&job->message);
    g_free (job);
}

/* Worker thread: run the two-input transform and hand the result back. */
static void
image_job2_run (GTask        *task,
                gpointer      source,
                gpointer      task_data,
                GCancellable *cancellable)
{
    ImageJob2 *job = task_data;
    GdkPixbuf *out = (job->a != NULL && job->b != NULL)
                       ? job->fn (job->a, job->b, PN_NODE (source))
                       : NULL;

    (void) cancellable;
    g_task_return_pointer (task, out, out != NULL ? g_object_unref : NULL);
}

/* Snapshot the current input pair and launch a worker for it. */
static void
start_image_job2 (PnNode             *node,
                  ImageAsyncState2   *st,
                  const gchar        *label,
                  PnImageBlendFn      fn);

/* Main loop: emit the finished image, then re-run if marked dirty. */
static void
image_job2_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    PnNode           *node = PN_NODE (source);
    ImageAsyncState2 *st   = image_async_state2 (node);
    ImageJob2        *job  = g_task_get_task_data (G_TASK (result));
    GdkPixbuf        *out  = g_task_propagate_pointer (G_TASK (result), NULL);

    (void) user_data;

    if (out != NULL)
    {
        emit_image_result (node, job->message, job->label, out);
        g_object_unref (out);
    }
    else
    {
        /* Blend failed; don't drop the message, just forward it. */
        pn_node_emit_message (node, job->message);
    }

    /* A new image (on either port) arrived while we were busy: re-run on
     * the freshest pair, which both ports still hold. */
    if (st->dirty && st->latest[0] != NULL && st->latest[1] != NULL)
    {
        st->dirty = FALSE;
        start_image_job2 (node, st, job->label, job->fn);   /* stays busy */
    }
    else
    {
        st->busy = FALSE;
        /* Work settled: close the glow opened in pn_image_node_process2. */
        pn_node_processing_end (node);
    }
}

static void
start_image_job2 (PnNode             *node,
                  ImageAsyncState2   *st,
                  const gchar        *label,
                  PnImageBlendFn      fn)
{
    ImageJob2 *job  = g_new0 (ImageJob2, 1);
    GTask     *task = g_task_new (node, NULL, image_job2_done, NULL);

    job->a       = st->latest[0] != NULL ? g_object_ref (st->latest[0]) : NULL;
    job->b       = st->latest[1] != NULL ? g_object_ref (st->latest[1]) : NULL;
    job->message = st->msg       != NULL ? g_object_ref (st->msg)       : NULL;
    job->fn      = fn;
    job->label   = label;

    g_task_set_task_data (task, job, image_job2_free);
    g_task_run_in_thread (task, image_job2_run);
    g_object_unref (task);
}

void
pn_image_node_process2 (PnNode         *node,
                        PnMessage      *message,
                        gint            input,
                        const gchar    *label,
                        PnImageBlendFn  fn)
{
    ImageAsyncState2 *st;
    GdkPixbuf        *pixbuf;

    /* Not an image (or an empty image message): pass straight through so
     * this stage is transparent to non-image traffic, without disturbing
     * the latched inputs. */
    if (!PN_IS_IMAGE_MESSAGE (message)
        || pn_image_message_get_pixbuf (PN_IMAGE_MESSAGE (message)) == NULL)
    {
        pn_node_emit_message (node, message);
        return;
    }

    /* This driver wires exactly two ports; ignore anything outside. */
    if (input < 0 || input > 1)
        return;

    st     = image_async_state2 (node);
    pixbuf = pn_image_message_get_pixbuf (PN_IMAGE_MESSAGE (message));

    /* Latch the newest image for this port and remember the message it
     * came on so the result keeps a sensible filename / path / mimetype. */
    g_object_ref (pixbuf);
    g_clear_object (&st->latest[input]);
    st->latest[input] = pixbuf;

    g_object_ref (message);
    g_clear_object (&st->msg);
    st->msg = message;

    /* Need both ports before there is anything to blend. */
    if (st->latest[0] == NULL || st->latest[1] == NULL)
        return;

    if (st->busy)
    {
        st->dirty = TRUE;
        return;
    }

    st->busy = TRUE;

    /* Light the processing glow for the off-thread blend; the receive
     * wrap only covers this synchronous dispatch.  Held across a
     * coalesced burst (image_job2_done re-arms while busy stays TRUE) and
     * closed when the work settles in image_job2_done. */
    pn_node_processing_begin (node);

    start_image_job2 (node, st, label, fn);
}
