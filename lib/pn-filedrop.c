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
/*  PnFileDrop — logic tier (headless core).                           */
/*                                                                     */
/*  This file holds the GTK-free half of the FileDrop source node: the */
/*  GType, the two appearance colour properties, the intrinsic size    */
/*  (get_size / get_header_height, driven by the dropped image's       */
/*  aspect ratio) and the drop-emit logic — pn_filedrop_drop_file()    */
/*  loads the dropped path into a #GdkPixbuf (gdk-pixbuf is an allowed  */
/*  core image-data dep), keeps it for the preview, and emits the      */
/*  corresponding #PnImageMessage / #PnMessage.  The GTK drag-and-drop */
/*  that routes a desktop drop to this node lives in the gui-tier      */
/*  worksheet (pn-worksheet.c), which calls the public                 */
/*  pn_filedrop_drop_file().  The cairo drop-area painter lives in the */
/*  companion gui-tier file pn-filedrop-gui.c, which installs that      */
/*  paint vfunc slot onto this class at editor startup (see            */
/*  pn_filedrop_gui_install) and reads the preview state through the    */
/*  GTK-free pn_filedrop_get_paint_state() seam.  The headless runtime */
/*  registers and runs this node without ever pulling GTK.             */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-filedrop.h"
#include "pn-image-message.h"
#include "pn-message.h"

#include <gio/gio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Standard-style header on top, a square-ish drop area below it,     */
/*  separated by the same small gap the Graph node leaves between its  */
/*  body and its plot rectangle so the two read as distinct elements.  */
/* ------------------------------------------------------------------ */

#define PN_FILEDROP_WIDTH         200.0
#define PN_FILEDROP_HEADER_HEIGHT  40.0
#define PN_FILEDROP_GAP             4.0

/* Drop-area height shown before any image has been dropped (and after a
 * non-image drop): a square-ish placeholder for the "Drop a file here"
 * hint. */
#define PN_FILEDROP_EMPTY_AREA_HEIGHT 150.0

/* Bounds on the image-driven drop-area height.  Once an image is
 * dropped the area takes the image's aspect ratio at the fixed
 * PN_FILEDROP_WIDTH, so the picture fills it edge-to-edge with no
 * letterbox; these clamps stop a panorama (very short) or a tall
 * portrait (very long) from producing an absurdly-shaped node. */
#define PN_FILEDROP_AREA_MIN_HEIGHT    80.0
#define PN_FILEDROP_AREA_MAX_HEIGHT   360.0

/* Inset used only by the empty-state hint (dashed box + label) so the
 * placeholder does not butt right up against the frame.  A previewed
 * image deliberately uses no inset — it runs to the frame. */
#define PN_FILEDROP_PADDING            6.0

struct _PnFileDrop
{
    PnNode parent_instance;

    /* Last dropped image, kept at full resolution and scaled at paint
     * time so it stays crisp when the node is zoomed.  %NULL whenever
     * the most recent drop was not an image (or nothing was dropped
     * yet). */
    GdkPixbuf *pixbuf;

    /* Basename of the most recent drop, shown as the hint text when no
     * image preview is up.  %NULL before the first drop. */
    gchar     *last_filename;

    /* Appearance — drop-area fill and frame.  Both serialise like any
     * other writable property and only affect painting. */
    PnColor    area_color;
    PnColor    border_color;
};

G_DEFINE_TYPE (PnFileDrop, pn_filedrop, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_AREA_COLOR,
    PROP_BORDER_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

/** Height of the drop-area rectangle for the current state.  With an
 *  image loaded the area takes the image's aspect ratio at the fixed
 *  area width so the picture fills it with no white margin (clamped to
 *  sane bounds); with no image it falls back to the placeholder
 *  height. */
static double
filedrop_area_height (PnFileDrop *self)
{
    double iw, ih, h;

    if (self->pixbuf == NULL)
        return PN_FILEDROP_EMPTY_AREA_HEIGHT;

    iw = (double) gdk_pixbuf_get_width  (self->pixbuf);
    ih = (double) gdk_pixbuf_get_height (self->pixbuf);
    if (iw <= 0.0 || ih <= 0.0)
        return PN_FILEDROP_EMPTY_AREA_HEIGHT;

    h = PN_FILEDROP_WIDTH * ih / iw;
    return CLAMP (h, PN_FILEDROP_AREA_MIN_HEIGHT, PN_FILEDROP_AREA_MAX_HEIGHT);
}

static void
pn_filedrop_get_size (
        PnNode *node,
        double *out_width,
        double *out_height)
{
    if (out_width  != NULL) *out_width  = PN_FILEDROP_WIDTH;
    if (out_height != NULL)
        *out_height = PN_FILEDROP_HEADER_HEIGHT
                    + PN_FILEDROP_GAP
                    + filedrop_area_height (PN_FILEDROP (node));
}

static double
pn_filedrop_get_header_height (PnNode *node)
{
    (void) node;
    return PN_FILEDROP_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  GUI paint-state seam (GTK-free)                                    */
/*                                                                     */
/*  The cairo drop-area painter lives in the gui tier (pn-filedrop-    */
/*  gui.c) and cannot see this file's private instance struct.  It     */
/*  reads everything it needs through this one snapshot accessor: the  */
/*  borrowed preview pixbuf (or %NULL), the borrowed last-drop         */
/*  filename hint (or %NULL), and the two appearance colours by value. */
/*  gdk-pixbuf is an allowed core dep, so the pixbuf can ride the      */
/*  snapshot as a plain (borrowed) pointer with no GTK involvement.    */
/* ------------------------------------------------------------------ */

void
pn_filedrop_get_paint_state (
        PnFileDrop            *self,
        PnFileDropPaintState  *out)
{
    g_return_if_fail (PN_IS_FILEDROP (self));
    g_return_if_fail (out != NULL);

    out->pixbuf        = self->pixbuf;        /* borrowed */
    out->last_filename = self->last_filename; /* borrowed */
    out->area_color    = self->area_color;
    out->border_color  = self->border_color;
}

/* ------------------------------------------------------------------ */
/*  Drop handling                                                      */
/* ------------------------------------------------------------------ */

/** Query @path's byte size, or -1 if it cannot be determined. */
static gint64
query_file_size (const gchar *path)
{
    GFile     *file = g_file_new_for_path (path);
    GFileInfo *info;
    gint64     size = -1;

    info = g_file_query_info (file,
                              G_FILE_ATTRIBUTE_STANDARD_SIZE,
                              G_FILE_QUERY_INFO_NONE,
                              NULL, NULL);
    if (info != NULL)
    {
        size = (gint64) g_file_info_get_size (info);
        g_object_unref (info);
    }

    g_object_unref (file);
    return size;
}

void
pn_filedrop_drop_file (
        PnFileDrop  *self,
        const gchar *path)
{
    PnNode    *node = PN_NODE (self);
    GdkPixbuf *pixbuf;
    gchar     *basename;
    gchar     *content_type;
    gint64     size;

    g_return_if_fail (PN_IS_FILEDROP (self));
    g_return_if_fail (path != NULL);

    basename     = g_path_get_basename (path);
    content_type = g_content_type_guess (path, NULL, 0, NULL);
    size         = query_file_size (path);

    /* The cheapest reliable "is this an image?" test is to try to
     * decode it: gdk-pixbuf understands every format we could preview
     * anyway, and a successful load hands us exactly the pixbuf we want
     * to both show and ship.  A NULL return (unsupported / corrupt /
     * not-an-image) falls through to the plain-file path. */
    pixbuf = gdk_pixbuf_new_from_file (path, NULL);

    /* Remember the basename for the empty-area hint regardless of type. */
    g_free (self->last_filename);
    self->last_filename = g_strdup (basename);

    if (pixbuf != NULL)
    {
        PnImageMessage *msg;
        gchar          *summary;
        const gint      iw = gdk_pixbuf_get_width  (pixbuf);
        const gint      ih = gdk_pixbuf_get_height (pixbuf);

        /* Show it: take over the load reference for the on-canvas
         * preview; the message takes its own ref below. */
        g_clear_object (&self->pixbuf);
        self->pixbuf = pixbuf;   /* transfer ownership of the load ref */

        msg = pn_image_message_new (node, NULL, pixbuf);

        /* Metadata only — the pixels ride the pixbuf pointer, never the
         * data bag, so they never reach a JSON serialisation. */
        pn_message_set_string  (PN_MESSAGE (msg), "filename", basename);
        pn_message_set_string  (PN_MESSAGE (msg), "path",     path);
        pn_message_set_string  (PN_MESSAGE (msg), "mimetype",
                                content_type ? content_type : "image");
        pn_message_set_boolean (PN_MESSAGE (msg), "image",    TRUE);
        pn_message_set_int     (PN_MESSAGE (msg), "width",    iw);
        pn_message_set_int     (PN_MESSAGE (msg), "height",   ih);
        if (size >= 0)
            pn_message_set_int64 (PN_MESSAGE (msg), "size", size);
        pn_message_set_boolean (PN_MESSAGE (msg), "success", TRUE);

        summary = g_strdup_printf ("%s (%d×%d)", basename, iw, ih);
        pn_message_set_string  (PN_MESSAGE (msg), "output", summary);
        g_free (summary);

        pn_node_emit_message (node, PN_MESSAGE (msg));
        g_object_unref (msg);
    }
    else
    {
        PnMessage *msg = pn_message_new (node, NULL);

        /* No preview for a non-image: clear any image left from a
         * previous drop so the area falls back to the filename hint. */
        g_clear_object (&self->pixbuf);

        pn_message_set_string  (msg, "filename", basename);
        pn_message_set_string  (msg, "path",     path);
        pn_message_set_string  (msg, "mimetype",
                                content_type ? content_type
                                             : "application/octet-stream");
        pn_message_set_boolean (msg, "image", FALSE);
        if (size >= 0)
            pn_message_set_int64 (msg, "size", size);
        pn_message_set_boolean (msg, "success", TRUE);
        pn_message_set_string  (msg, "output", basename);

        pn_node_emit_message (node, msg);
        g_object_unref (msg);
    }

    g_free (basename);
    g_free (content_type);

    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_filedrop_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnFileDrop *self = PN_FILEDROP (object);

    switch (prop_id)
    {
    case PROP_AREA_COLOR:
        g_value_set_boxed (value, &self->area_color);
        break;
    case PROP_BORDER_COLOR:
        g_value_set_boxed (value, &self->border_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_filedrop_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnFileDrop *self = PN_FILEDROP (object);

    switch (prop_id)
    {
    case PROP_AREA_COLOR:
        {
            const PnColor *c = g_value_get_boxed (value);
            if (c != NULL && !pn_color_equal (c, &self->area_color))
            {
                self->area_color = *c;
                g_object_notify_by_pspec (object, props[PROP_AREA_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    case PROP_BORDER_COLOR:
        {
            const PnColor *c = g_value_get_boxed (value);
            if (c != NULL && !pn_color_equal (c, &self->border_color))
            {
                self->border_color = *c;
                g_object_notify_by_pspec (object, props[PROP_BORDER_COLOR]);
                pn_node_request_repaint (PN_NODE (self));
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_filedrop_finalize (GObject *object)
{
    PnFileDrop *self = PN_FILEDROP (object);

    g_clear_object  (&self->pixbuf);
    g_clear_pointer (&self->last_filename, g_free);

    G_OBJECT_CLASS (pn_filedrop_parent_class)->finalize (object);
}

static void
pn_filedrop_class_init (PnFileDropClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_filedrop_get_property;
    object_class->set_property = pn_filedrop_set_property;
    object_class->finalize     = pn_filedrop_finalize;

    node_class->get_size          = pn_filedrop_get_size;
    node_class->get_header_height = pn_filedrop_get_header_height;
    /* The cairo paint_plot painter and its companion
     * paint_plot_zoom_keep_aspect flag are installed by the gui tier
     * (pn_filedrop_gui_install); the headless core leaves them NULL. */

    node_class->class_name = "FileDrop";
    node_class->icon       = "\xef\x80\x9c";  /* fa-inbox U+F01C */
    node_class->color      = (PnColor){ 0.36, 0.60, 0.74, 1.0 };
    node_class->category   = "Sources";
    node_class->has_input  = FALSE;
    node_class->has_output = TRUE;

    props[PROP_AREA_COLOR] = g_param_spec_boxed (
            "area-color", "Area colour",
            "Fill colour of the drop-area rectangle drawn below the header",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BORDER_COLOR] = g_param_spec_boxed (
            "border-color", "Border colour",
            "Colour of the 1 px frame around the drop area",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_filedrop_init (PnFileDrop *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  color = { 0.36, 0.60, 0.74, 1.0 };

    self->pixbuf        = NULL;
    self->last_filename = NULL;
    self->area_color    = (PnColor){ 1.0, 1.0, 1.0, 1.0 };   /* white */
    self->border_color  = (PnColor){ 70.0/255.0, 70.0/255.0, 70.0/255.0, 1.0 };

    pn_node_set_class_name (node, "FileDrop");
    pn_node_set_icon       (node, "\xef\x80\x9c");  /* fa-inbox U+F01C */
    pn_node_set_color (node, &color);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnFileDrop *
pn_filedrop_new (void)
{
    return g_object_new (PN_TYPE_FILEDROP, NULL);
}
