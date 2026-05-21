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

#ifndef PN_IMAGE_OPS_H
#define PN_IMAGE_OPS_H

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "pn-node.h"
#include "pn-message.h"

G_BEGIN_DECLS

/* Palette category shared by every node this plugin ships.  The
 * palette treats the string as a slash-separated path, so each node
 * sorts into a subgroup under the top-level "Image Processing" group. */
#define PN_IMAGE_CATEGORY               "Image Processing"
#define PN_IMAGE_CATEGORY_COLOR         "Image Processing/Color"
#define PN_IMAGE_CATEGORY_ADJUST        "Image Processing/Adjust"
#define PN_IMAGE_CATEGORY_BLUR          "Image Processing/Blur"
#define PN_IMAGE_CATEGORY_SHARPEN       "Image Processing/Sharpen"
#define PN_IMAGE_CATEGORY_EDGE          "Image Processing/Edge Detection"
#define PN_IMAGE_CATEGORY_COMPOSITE     "Image Processing/Composite"
#define PN_IMAGE_CATEGORY_GEOMETRY      "Image Processing/Geometry"
#define PN_IMAGE_CATEGORY_STYLIZE       "Image Processing/Stylize"

/* ------------------------------------------------------------------ */
/*  Shared image-op helpers for the pipnode image plugin               */
/*                                                                     */
/*  Every node in this plugin is a pure transform: it receives a       */
/*  #PnImageMessage, runs one of the helpers below over the carried    */
/*  #GdkPixbuf's raw buffer in plain C (no external process), and      */
/*  emits a new #PnImageMessage carrying the result.  This module      */
/*  holds the one copy of the receive/emit driver and the pixel        */
/*  scaffolding so each node file is just its kernel/maths plus the    */
/*  usual GObject boilerplate.                                         */
/*                                                                     */
/*  Conventions shared by all helpers:                                 */
/*    - Output is a freshly allocated 8-bit RGB(A) pixbuf with the     */
/*      same geometry and alpha-ness as the source.                    */
/*    - Any alpha channel is copied through untouched; only the colour */
/*      channels are processed.                                        */
/*    - Sampling outside the image clamps to the nearest edge pixel.   */
/*    - Results are rounded and clamped to [0, 255].                   */
/* ------------------------------------------------------------------ */

/**
 * PnImageTransformFn:
 * @src:  source image (transfer none)
 * @node: the node, so a transform can read its configurable properties
 *
 * Returns: (transfer full) (nullable): a new image, or %NULL on failure
 * (the driver then passes the original message through unchanged).
 */
typedef GdkPixbuf *(*PnImageTransformFn) (GdkPixbuf *src, PnNode *node);

/**
 * PnImagePointFn:
 * @rgb:  pointer to this pixel's R, G, B bytes (modify in place)
 * @node: the node, for property access
 *
 * A per-pixel point operation.  The alpha byte (if any) at rgb[3] is
 * managed by the driver and must not be touched here.
 */
typedef void (*PnImagePointFn) (guchar *rgb, PnNode *node);

/**
 * pn_image_node_process:
 * @node:    the receiving node
 * @message: (transfer none): the incoming message
 * @label:   short effect name used in the emitted `data.output` summary
 * @fn:      the transform to run
 *
 * The shared #PnNodeClass.receive body.  A non-image message (or an
 * image message with no pixbuf) is emitted unchanged on the calling
 * (main) thread.  Otherwise @fn is run on a worker thread so a large
 * image cannot freeze the GTK main loop; when it finishes, a new
 * #PnImageMessage is built carrying the result back on the main loop
 * (the source's filename / path / mimetype carried over and width /
 * height / image / success / output set fresh) and emitted.  At most one
 * transform runs per node at a time: an image arriving mid-transform is
 * coalesced into a single pending slot (latest wins) and run next, so a
 * burst of inputs does not spawn a pile of worker threads.  @fn must
 * therefore be self-contained — it may read @node's transform properties
 * but must not touch GTK / the worksheet.
 */
void pn_image_node_process (PnNode             *node,
                            PnMessage          *message,
                            const gchar        *label,
                            PnImageTransformFn  fn);

/**
 * PnImageBlendFn:
 * @a:    image latched on input 0 (transfer none)
 * @b:    image latched on input 1 (transfer none)
 * @node: the node, so the blend can read its configurable properties
 *
 * A two-input transform.  Receives the most recent image from each of
 * the node's two input ports and returns the combined result.
 *
 * Returns: (transfer full) (nullable): a new image, or %NULL on failure
 * (the driver then forwards the triggering message through unchanged).
 */
typedef GdkPixbuf *(*PnImageBlendFn) (GdkPixbuf *a, GdkPixbuf *b, PnNode *node);

/**
 * pn_image_node_process2:
 * @node:    the receiving node (must declare two inputs)
 * @message: (transfer none): the incoming message
 * @input:   index (0 or 1) of the input port @message arrived on —
 *           typically pn_node_current_input()
 * @label:   short effect name used in the emitted `data.output` summary
 * @fn:      the two-input transform to run
 *
 * The two-input counterpart of pn_image_node_process(): the receive body
 * for a node that combines two images.  A non-image message is forwarded
 * unchanged on the main thread without disturbing the latched inputs.
 * Otherwise the carried pixbuf replaces this port's latched image; once
 * both ports hold an image, @fn is run on a worker thread and its result
 * emitted as a fresh #PnImageMessage on the main loop (metadata carried
 * from the most recent triggering message).  At most one blend runs per
 * node at a time; an image arriving mid-blend re-runs the latest pair
 * when the current one finishes.  @fn must be self-contained — it may
 * read @node's properties but must not touch GTK / the worksheet.
 */
void pn_image_node_process2 (PnNode         *node,
                             PnMessage      *message,
                             gint            input,
                             const gchar    *label,
                             PnImageBlendFn  fn);

/**
 * PnImageComposeFn:
 * @base: a colour-channel byte from the base (canvas) image
 * @over: the matching byte from the overlay image
 *
 * A per-channel two-image combine — the maths of one blend mode, e.g.
 * multiply or screen.  Returns the combined 0..255 byte, before the
 * node's opacity is applied.
 */
typedef guchar (*PnImageComposeFn) (guchar base, guchar over);

/**
 * pn_image_compose:
 * @a:       image latched on input 0 (transfer none)
 * @b:       image latched on input 1 (transfer none)
 * @opacity: 0..1 mix of the combined result back over the base
 * @fn:      the per-channel blend-mode combine
 *
 * Shared body for the plugin's two-input "Composite" nodes (Blend,
 * Multiply, Screen, …).  The bigger image (greater pixel area; a tie
 * keeps @a) becomes the canvas and the smaller is composited into its
 * top-left corner — never scaled — exactly like Blend.  Over the overlap
 * each colour channel becomes base·(1-@opacity) + @fn(base, over)·@opacity;
 * the canvas keeps its own pixels wherever the overlay does not reach,
 * and its alpha channel is preserved.  Because the canvas is the bigger
 * image, an asymmetric @fn (Overlay) treats that image as the base.
 * Returns: (transfer full) (nullable): a new image, or %NULL on failure.
 */
GdkPixbuf *pn_image_compose (GdkPixbuf        *a,
                             GdkPixbuf        *b,
                             gdouble           opacity,
                             PnImageComposeFn  fn);

/* A blank 8-bit RGB(A) pixbuf with @src's geometry and alpha-ness. */
GdkPixbuf *pn_image_new_like (GdkPixbuf *src);

/* Run a per-pixel point op over a copy of @src (alpha preserved). */
GdkPixbuf *pn_image_map_point (GdkPixbuf      *src,
                               PnImagePointFn  fn,
                               PnNode         *node);

/**
 * PnImagePointXYFn:
 * @rgb:  pointer to this pixel's R, G, B bytes (modify in place)
 * @x:    pixel column, 0 .. @w-1
 * @y:    pixel row, 0 .. @h-1
 * @w:    image width
 * @h:    image height
 * @node: the node, for property access
 *
 * Like #PnImagePointFn but the callback also gets the pixel's position
 * and the image size, so position-dependent effects (vignette, gradients)
 * need not re-write the iteration loop.  The alpha byte at rgb[3] (if any)
 * is managed by the driver and must not be touched.
 */
typedef void (*PnImagePointXYFn) (guchar *rgb, gint x, gint y,
                                  gint w, gint h, PnNode *node);

/* Run a coordinate-aware point op over a copy of @src (alpha preserved). */
GdkPixbuf *pn_image_map_point_xy (GdkPixbuf        *src,
                                  PnImagePointXYFn  fn,
                                  PnNode           *node);

/**
 * pn_image_rgb_to_hsv / pn_image_hsv_to_rgb:
 * Convert a single colour between 8-bit RGB and HSV.  Hue is in degrees
 * [0, 360), saturation and value in [0, 1].  Round-trips are stable
 * enough for hue/saturation tweaks (hue is undefined for greys and comes
 * back as 0).  These let the colour-space nodes (Hue Rotate, Vibrance)
 * stay tiny.
 */
void pn_image_rgb_to_hsv (guchar r, guchar g, guchar b,
                          gdouble *h, gdouble *s, gdouble *v);
void pn_image_hsv_to_rgb (gdouble h, gdouble s, gdouble v,
                          guchar *r, guchar *g, guchar *b);

/**
 * pn_image_convolve:
 * @kernel:     row-major @n×@n weights
 * @n:          odd kernel size (3, 5, …)
 * @divisor:    normaliser applied after the weighted sum (0 ⇒ treated as 1)
 * @bias:       constant added after the divide (e.g. 128 for emboss)
 * @gray_first: convert @src to luma first and convolve that single
 *              channel, writing the result to R = G = B (for edge /
 *              relief kernels); otherwise convolve each colour channel
 *              independently.
 *
 * Generic square convolution.  Returns: (transfer full).
 */
GdkPixbuf *pn_image_convolve (GdkPixbuf    *src,
                              const gdouble *kernel,
                              gint           n,
                              gdouble        divisor,
                              gdouble        bias,
                              gboolean       gray_first);

/**
 * pn_image_gradient:
 * @kx: row-major @n×@n horizontal kernel
 * @ky: row-major @n×@n vertical kernel
 * @n:  odd kernel size
 *
 * Edge-magnitude transform: converts @src to luma, applies @kx and @ky,
 * and writes sqrt(gx²+gy²) (clamped) to R = G = B.  Used by the Sobel
 * and Prewitt nodes.  Returns: (transfer full).
 */
GdkPixbuf *pn_image_gradient (GdkPixbuf     *src,
                              const gdouble *kx,
                              const gdouble *ky,
                              gint           n);

/**
 * pn_image_separable:
 * @k1d:     1-D kernel of length @n
 * @n:       odd kernel length
 * @divisor: normaliser (0 ⇒ the sum of @k1d, or 1 if that is 0)
 *
 * Two-pass separable blur (horizontal then vertical) over the colour
 * channels, alpha preserved.  Used by the Gaussian and Box blur nodes,
 * which are separable and so far cheaper than a full 2-D pass.
 * Returns: (transfer full).
 */
GdkPixbuf *pn_image_separable (GdkPixbuf     *src,
                               const gdouble *k1d,
                               gint           n,
                               gdouble        divisor);

/**
 * pn_image_median:
 * @src:    source image (transfer none)
 * @radius: half-width of the square window (window = 2·radius + 1)
 *
 * Median filter: each output channel is the median of its
 * (2·radius+1)² neighbourhood in @src, which removes salt-and-pepper
 * noise while keeping edges crisper than an averaging blur would.  This
 * is a rank filter, not a convolution, so it has its own helper.  A
 * radius below 1 is a no-op copy.  Alpha is preserved.
 * Returns: (transfer full).
 */
GdkPixbuf *pn_image_median (GdkPixbuf *src, gint radius);

G_END_DECLS

#endif /* PN_IMAGE_OPS_H */
