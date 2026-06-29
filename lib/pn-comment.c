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
/*  PnComment — free-text annotation box.                              */
/*                                                                     */
/*  A port-less #PnNode that carries a few paragraphs of free text plus */
/*  a resizable footprint and three colours.  It has no message         */
/*  behaviour at all (no receive, no forward); it exists only to        */
/*  annotate the worksheet.  This whole node is GTK-free: the cairo/    */
/*  Pango drawing and the resize interaction live in the worksheet      */
/*  (pn-worksheet.c), which reads the GTK-free snapshot returned by      */
/*  pn_comment_get_paint_state().  As a regular node it serialises       */
/*  through node_to_json / node_from_json and so round-trips through     */
/*  save/load and undo/redo with no extra code.                         */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-comment.h"
#include "pn-settings-schema.h"

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnComment
{
    PnNode    parent_instance;

    gchar    *text;
    double    width;
    double    height;
    double    text_size;

    PnColor   background_color;
    PnColor   frame_color;
    PnColor   text_color;
};

G_DEFINE_FINAL_TYPE (PnComment, pn_comment, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_TEXT,
    PROP_WIDTH,
    PROP_HEIGHT,
    PROP_TEXT_SIZE,
    PROP_BACKGROUND_COLOR,
    PROP_FRAME_COLOR,
    PROP_TEXT_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_comment_get_paint_state (PnComment *self, PnCommentPaintState *out)
{
    g_return_if_fail (PN_IS_COMMENT (self));
    g_return_if_fail (out != NULL);

    out->text             = self->text != NULL ? self->text : "";
    out->width            = self->width;
    out->height           = self->height;
    out->text_size        = self->text_size;
    out->background_color = self->background_color;
    out->frame_color      = self->frame_color;
    out->text_color       = self->text_color;
}

void
pn_comment_set_size (PnComment *self, double width, double height)
{
    g_return_if_fail (PN_IS_COMMENT (self));

    if (width  < PN_COMMENT_MIN_WIDTH)  width  = PN_COMMENT_MIN_WIDTH;
    if (height < PN_COMMENT_MIN_HEIGHT) height = PN_COMMENT_MIN_HEIGHT;

    if (self->width != width)
    {
        self->width = width;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_WIDTH]);
    }
    if (self->height != height)
    {
        self->height = height;
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HEIGHT]);
    }
    pn_node_request_repaint (PN_NODE (self));
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/*                                                                     */
/*  The footprint is the stored width/height.  The whole box counts as  */
/*  the "header" so it is hit-testable and selectable end to end (a     */
/*  comment has no passive plot extension below a header).              */
/* ------------------------------------------------------------------ */

static void
pn_comment_get_size (PnNode *node, double *out_w, double *out_h)
{
    PnComment *self = PN_COMMENT (node);

    if (out_w != NULL) *out_w = self->width;
    if (out_h != NULL) *out_h = self->height;
}

static double
pn_comment_get_header_height (PnNode *node)
{
    return PN_COMMENT (node)->height;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
set_color_prop (PnComment *self, PnColor *slot,
                const GValue *value, guint prop_id)
{
    const PnColor *new_value = g_value_get_boxed (value);

    if (new_value == NULL || pn_color_equal (slot, new_value))
        return;
    *slot = *new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
pn_comment_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
    PnComment *self = PN_COMMENT (object);

    switch (prop_id)
    {
    case PROP_TEXT:
        g_value_set_string (value, self->text);
        break;
    case PROP_WIDTH:
        g_value_set_double (value, self->width);
        break;
    case PROP_HEIGHT:
        g_value_set_double (value, self->height);
        break;
    case PROP_TEXT_SIZE:
        g_value_set_double (value, self->text_size);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_FRAME_COLOR:
        g_value_set_boxed (value, &self->frame_color);
        break;
    case PROP_TEXT_COLOR:
        g_value_set_boxed (value, &self->text_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_comment_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
    PnComment *self = PN_COMMENT (object);

    switch (prop_id)
    {
    case PROP_TEXT:
    {
        const gchar *new_value = g_value_get_string (value);
        if (g_strcmp0 (self->text, new_value) != 0)
        {
            g_free (self->text);
            self->text = g_strdup (new_value != NULL ? new_value : "");
            g_object_notify_by_pspec (object, props[PROP_TEXT]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_WIDTH:
    {
        double v = g_value_get_double (value);
        if (v < PN_COMMENT_MIN_WIDTH) v = PN_COMMENT_MIN_WIDTH;
        if (self->width != v)
        {
            self->width = v;
            g_object_notify_by_pspec (object, props[PROP_WIDTH]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_HEIGHT:
    {
        double v = g_value_get_double (value);
        if (v < PN_COMMENT_MIN_HEIGHT) v = PN_COMMENT_MIN_HEIGHT;
        if (self->height != v)
        {
            self->height = v;
            g_object_notify_by_pspec (object, props[PROP_HEIGHT]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_TEXT_SIZE:
    {
        double v = g_value_get_double (value);
        if (self->text_size != v)
        {
            self->text_size = v;
            g_object_notify_by_pspec (object, props[PROP_TEXT_SIZE]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_BACKGROUND_COLOR:
        set_color_prop (self, &self->background_color, value,
                        PROP_BACKGROUND_COLOR);
        break;
    case PROP_FRAME_COLOR:
        set_color_prop (self, &self->frame_color, value, PROP_FRAME_COLOR);
        break;
    case PROP_TEXT_COLOR:
        set_color_prop (self, &self->text_color, value, PROP_TEXT_COLOR);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_comment_finalize (GObject *object)
{
    PnComment *self = PN_COMMENT (object);

    g_free (self->text);

    G_OBJECT_CLASS (pn_comment_parent_class)->finalize (object);
}

static void
pn_comment_class_init (PnCommentClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_comment_get_property;
    object_class->set_property = pn_comment_set_property;
    object_class->finalize     = pn_comment_finalize;

    node_class->get_size          = pn_comment_get_size;
    node_class->get_header_height = pn_comment_get_header_height;

    node_class->class_name = "Comment";
    /* fa-sticky-note-o (U+F24A) — the annotation glyph. */
    node_class->icon       = "\xef\x89\x8a";
    node_class->color      = (PnColor){ 0.62, 0.62, 0.62, 1.0 };
    node_class->category   = "Annotation";
    node_class->has_input  = FALSE;
    node_class->has_output = FALSE;

    props[PROP_TEXT] = g_param_spec_string (
            "text", "Text",
            "The comment text — a few paragraphs of free text that wrap "
            "inside the box.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_WIDTH] = g_param_spec_double (
            "width", "Width",
            "Width of the comment box in pixels.",
            PN_COMMENT_MIN_WIDTH, 100000.0, PN_COMMENT_DEFAULT_WIDTH,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEIGHT] = g_param_spec_double (
            "height", "Height",
            "Height of the comment box in pixels.",
            PN_COMMENT_MIN_HEIGHT, 100000.0, PN_COMMENT_DEFAULT_HEIGHT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_SIZE] = g_param_spec_double (
            "text-size", "Text size",
            "Font size of the comment text in pixels.",
            6.0, 96.0, PN_COMMENT_DEFAULT_TEXT_SIZE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the box — white by default; set its alpha to "
            "zero for a transparent note.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FRAME_COLOR] = g_param_spec_boxed (
            "frame-color", "Frame colour",
            "Colour of the box outline — grey by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the text — a dark slate by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings dialog: a Text page (the wrapped editor + the
     * font size) and a Colours page for the three PnColor rows.  width and
     * height are edited on the canvas via resize handles, so they are not
     * offered as rows. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Text");
        pn_settings_schema_row (schema, "text",      PN_EDITOR_MULTILINE);
        pn_settings_schema_row (schema, "text-size", PN_EDITOR_SPIN);

        pn_settings_schema_tab (schema, "Colours");
        pn_settings_schema_row (schema, "background-color", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "frame-color",      PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "text-color",       PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_comment_init (PnComment *self)
{
    PnNode *node = PN_NODE (self);

    self->text             = g_strdup ("");
    self->width            = PN_COMMENT_DEFAULT_WIDTH;
    self->height           = PN_COMMENT_DEFAULT_HEIGHT;
    self->text_size        = PN_COMMENT_DEFAULT_TEXT_SIZE;
    self->background_color  = (PnColor){ 1.0,  1.0,  1.0,  1.0 };
    self->frame_color       = (PnColor){ 0.62, 0.62, 0.62, 1.0 };
    self->text_color        = (PnColor){ 0.18, 0.20, 0.23, 1.0 };

    {
        PnColor grey = { 0.62, 0.62, 0.62, 1.0 };
        pn_node_set_color (node, &grey);
    }
    pn_node_set_class_name (node, "Comment");
    pn_node_set_icon       (node, "\xef\x89\x8a");  /* fa-sticky-note-o */
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnComment *
pn_comment_new (void)
{
    return g_object_new (PN_TYPE_COMMENT, NULL);
}
