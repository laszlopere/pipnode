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
/*  Pipnode image-processing plugin                                    */
/*                                                                     */
/*  Ships a family of pure image-transform nodes.  Each one receives a */
/*  #PnImageMessage, processes the carried #GdkPixbuf in plain C (no   */
/*  external process), and emits a new #PnImageMessage carrying the    */
/*  result, so an effect can sit between a FileDrop source and a       */
/*  FileViewer sink and you see the transform applied live.            */
/*                                                                     */
/*  Nodes are grouped into palette subcategories under "Image            */
/*  Processing" (the category string is a slash-separated path):         */
/*    Color           — Grayscale, Black & White, Sepia, Invert          */
/*    Adjust          — Brightness, Contrast, Saturation, Posterize,     */
/*                      Solarize, Gamma, Levels, Hue Rotate, Vibrance,   */
/*                      Histogram Equalize                               */
/*    Blur            — Gaussian, Box, Motion, Median, Bilateral         */
/*    Sharpen         — Sharpen, Emboss, Edge Enhance, Unsharp Mask      */
/*    Edge Detection  — Sobel, Prewitt, Laplacian, Find Edges            */
/*    Composite       — Blend, Multiply, Screen, Difference, Darken,     */
/*                      Lighten, Add, Overlay, Soft Light, Hard Light,   */
/*                      Color Dodge, Color Burn, Exclusion (two-input)   */
/*    Geometry        — Flip H/V, Rotate 90° CW/CCW/180°, Rotate, Crop,  */
/*                      Resize                                           */
/*    Stylize         — Vignette, Pixelate, Add Noise, Dither,           */
/*                      Threshold, Oil Paint, Crosshatch                 */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

#include "pn-node-factory.h"
#include "pn-plugin.h"

#include "pn-image-grayscale.h"
#include "pn-image-black-white.h"
#include "pn-image-sepia.h"
#include "pn-image-invert.h"
#include "pn-image-brightness.h"
#include "pn-image-contrast.h"
#include "pn-image-saturation.h"
#include "pn-image-posterize.h"
#include "pn-image-solarize.h"
#include "pn-image-gaussian-blur.h"
#include "pn-image-box-blur.h"
#include "pn-image-motion-blur.h"
#include "pn-image-sharpen.h"
#include "pn-image-emboss.h"
#include "pn-image-edge-enhance.h"
#include "pn-image-median.h"
#include "pn-image-sobel.h"
#include "pn-image-prewitt.h"
#include "pn-image-laplacian.h"
#include "pn-image-find-edges.h"
#include "pn-image-blend.h"
#include "pn-image-multiply.h"
#include "pn-image-screen.h"
#include "pn-image-difference.h"
#include "pn-image-darken.h"
#include "pn-image-lighten.h"
#include "pn-image-add.h"
#include "pn-image-overlay.h"
#include "pn-image-soft-light.h"
#include "pn-image-hard-light.h"
#include "pn-image-color-dodge.h"
#include "pn-image-color-burn.h"
#include "pn-image-exclusion.h"
#include "pn-image-gamma.h"
#include "pn-image-levels.h"
#include "pn-image-hue-rotate.h"
#include "pn-image-vibrance.h"
#include "pn-image-hist-equalize.h"
#include "pn-image-unsharp-mask.h"
#include "pn-image-bilateral.h"
#include "pn-image-flip-h.h"
#include "pn-image-flip-v.h"
#include "pn-image-rotate-90.h"
#include "pn-image-rotate-270.h"
#include "pn-image-rotate-180.h"
#include "pn-image-rotate.h"
#include "pn-image-crop.h"
#include "pn-image-resize.h"
#include "pn-image-vignette.h"
#include "pn-image-pixelate.h"
#include "pn-image-noise.h"
#include "pn-image-dither.h"
#include "pn-image-threshold.h"
#include "pn-image-oil-paint.h"
#include "pn-image-crosshatch.h"

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-image",
        .version     = "1.0.0",
        .description = "Image-processing transforms.  Each node takes an "
                       "image message, processes the pixels in C, and emits "
                       "the result, grouped into palette subcategories: Color, "
                       "Adjust (incl. Gamma, Levels, Hue Rotate, Vibrance, "
                       "Histogram Equalize), Blur (incl. Bilateral), Sharpen "
                       "(incl. Unsharp Mask), Edge Detection, two-input "
                       "Composite blend modes (Blend, Multiply, Screen, "
                       "Overlay, Soft/Hard Light, Color Dodge/Burn, …), "
                       "Geometry (flip, rotate, crop, resize) and Stylize "
                       "(Vignette, Pixelate, Noise, Dither, Threshold, Oil "
                       "Paint, Crosshatch).",
    };

    pn_node_factory_register (factory, PN_TYPE_IMAGE_GRAYSCALE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_BLACK_WHITE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_SEPIA);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_INVERT);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_BRIGHTNESS);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_CONTRAST);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_SATURATION);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_POSTERIZE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_SOLARIZE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_GAUSSIAN_BLUR);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_BOX_BLUR);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_MOTION_BLUR);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_SHARPEN);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_EMBOSS);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_EDGE_ENHANCE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_MEDIAN);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_SOBEL);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_PREWITT);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_LAPLACIAN);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_FIND_EDGES);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_BLEND);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_MULTIPLY);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_SCREEN);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_DIFFERENCE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_DARKEN);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_LIGHTEN);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_ADD);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_OVERLAY);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_SOFT_LIGHT);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_HARD_LIGHT);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_COLOR_DODGE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_COLOR_BURN);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_EXCLUSION);

    pn_node_factory_register (factory, PN_TYPE_IMAGE_GAMMA);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_LEVELS);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_HUE_ROTATE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_VIBRANCE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_HIST_EQUALIZE);

    pn_node_factory_register (factory, PN_TYPE_IMAGE_UNSHARP_MASK);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_BILATERAL);

    pn_node_factory_register (factory, PN_TYPE_IMAGE_FLIP_H);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_FLIP_V);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_ROTATE_90);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_ROTATE_270);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_ROTATE_180);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_ROTATE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_CROP);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_RESIZE);

    pn_node_factory_register (factory, PN_TYPE_IMAGE_VIGNETTE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_PIXELATE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_NOISE);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_DITHER);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_THRESHOLD);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_OIL_PAINT);
    pn_node_factory_register (factory, PN_TYPE_IMAGE_CROSSHATCH);

    return &info;
}
