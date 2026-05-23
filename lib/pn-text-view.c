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

#include "pn-text-view.h"
#include "pn-message.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Match PnTableView / PnTable / PnGraph so a row of mixed sinks      */
/*  aligns visually on the canvas.                                     */
/* ------------------------------------------------------------------ */

#define PN_TEXT_VIEW_WIDTH         280.0
#define PN_TEXT_VIEW_HEADER_HEIGHT  40.0
#define PN_TEXT_VIEW_GAP             4.0
#define PN_TEXT_VIEW_BODY_HEIGHT   173.0
#define PN_TEXT_VIEW_TOTAL_HEIGHT  (PN_TEXT_VIEW_HEADER_HEIGHT + \
                                    PN_TEXT_VIEW_GAP +           \
                                    PN_TEXT_VIEW_BODY_HEIGHT)

/* 10 Hz repaint cap, identical to PnTableView. */
#define PN_TEXT_VIEW_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 100)

#define PN_TEXT_VIEW_INSET           6.0
#define PN_TEXT_VIEW_FONT_PX        12.0
#define PN_TEXT_VIEW_LINE_HEIGHT_PX 14.0

struct _PnTextView
{
    PnNode parent_instance;

    /* Visual properties.  Defaults are a classic green-on-black
     * terminal scheme. */
    PnColor  background_color;
    PnColor  text_color;
    gdouble  font_size;

    /* Latest snapshot of `data.output` split into lines, replaced on
     * every receive.  Owned by the view; freed via g_strfreev. */
    gchar  **lines;
    guint    n_lines;

    /* Scroll state -- index of the topmost visible line. */
    int scroll_offset;

    /* Repaint throttle. */
    gint64 last_repaint_us;
    guint  pending_repaint_id;
};

G_DEFINE_TYPE (PnTextView, pn_text_view, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_BACKGROUND_COLOR,
    PROP_TEXT_COLOR,
    PROP_FONT_SIZE,
    N_PROPS
};

static GParamSpec *props[N_PROPS];

/* Forward decls. */
static void schedule_repaint (PnTextView *self);

/* ------------------------------------------------------------------ */
/*  Lines lifecycle                                                    */
/* ------------------------------------------------------------------ */

static void
clear_lines (PnTextView *self)
{
    g_clear_pointer (&self->lines, g_strfreev);
    self->n_lines = 0;
}

/* Splits @text on '\n' and stores the result.  A trailing empty line
 * (the artefact of a final newline) is dropped so a typical
 * shell-command output doesn't render a phantom blank row at the
 * bottom. */
static void
set_lines_from_text (
        PnTextView  *self,
        const gchar *text)
{
    gchar **split;
    guint   n;

    clear_lines (self);

    if (text == NULL || *text == '\0')
        return;

    split = g_strsplit (text, "\n", -1);
    n     = g_strv_length (split);

    if (n > 0 && split[n - 1] != NULL && *split[n - 1] == '\0')
    {
        g_free (split[n - 1]);
        split[n - 1] = NULL;
        n--;
    }

    self->lines   = split;
    self->n_lines = n;
}

/* ------------------------------------------------------------------ */
/*  Repaint throttle (mirrors PnTableView's, kept independent so the   */
/*  two sinks don't share state)                                       */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnTextView *self = PN_TEXT_VIEW (user_data);

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
schedule_repaint (PnTextView *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_TEXT_VIEW_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_TEXT_VIEW_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/*                                                                     */
/*  Pull `data.output` out of the incoming message, replace the        */
/*  internal snapshot, schedule a repaint, then forward the message    */
/*  verbatim on the output port so a downstream consumer keeps         */
/*  receiving it.  A message without a recognisable string `output`    */
/*  clears the view to its empty state -- the canonical "waiting"      */
/*  render -- but is still forwarded, so the node's passthrough        */
/*  semantics never depend on the payload shape.                       */
/* ------------------------------------------------------------------ */

static void
pn_text_view_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTextView  *self = PN_TEXT_VIEW (node);
    JsonObject  *data;
    const gchar *output = NULL;

    self->scroll_offset = 0;

    data = pn_message_get_data (message);
    if (data != NULL && json_object_has_member (data, "output"))
    {
        JsonNode *out_node = json_object_get_member (data, "output");
        if (out_node != NULL
            && JSON_NODE_HOLDS_VALUE (out_node)
            && json_node_get_value_type (out_node) == G_TYPE_STRING)
        {
            output = json_node_get_string (out_node);
        }
    }

    set_lines_from_text (self, output);
    schedule_repaint (self);

    /* Pass the message through verbatim so the node can sit mid-
     * pipeline as an inline inspector without breaking downstream
     * consumers. */
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The gui-tier cairo painter (pn-text-view-gui.c) reads the scalar    */
/*  drawing config through a snapshot and the line snapshot through a    */
/*  borrowed-pointer accessor.  Scroll clamping crosses back: the       */
/*  painter knows the live extents, so it pins the stored offset.       */
/* ------------------------------------------------------------------ */

void
pn_text_view_get_paint_state (PnTextView *self, PnTextViewPaintState *out)
{
    g_return_if_fail (PN_IS_TEXT_VIEW (self));
    g_return_if_fail (out != NULL);

    out->background_color = self->background_color;
    out->text_color       = self->text_color;
    out->font_size        = self->font_size;
}

const gchar *const *
pn_text_view_peek_lines (PnTextView *self, guint *n_out)
{
    g_return_val_if_fail (PN_IS_TEXT_VIEW (self), NULL);
    if (n_out != NULL)
        *n_out = self->n_lines;
    return (const gchar *const *) self->lines;
}

int
pn_text_view_get_scroll_offset (PnTextView *self)
{
    g_return_val_if_fail (PN_IS_TEXT_VIEW (self), 0);
    return self->scroll_offset;
}

void
pn_text_view_clamp_scroll_offset (PnTextView *self, int max_offset)
{
    g_return_if_fail (PN_IS_TEXT_VIEW (self));
    if (max_offset < 0) max_offset = 0;
    if (self->scroll_offset > max_offset) self->scroll_offset = max_offset;
    if (self->scroll_offset < 0)          self->scroll_offset = 0;
}

/* ------------------------------------------------------------------ */
/*  Scroll                                                             */
/* ------------------------------------------------------------------ */

static void
pn_text_view_scroll (PnNode *node, double dy)
{
    PnTextView *self = PN_TEXT_VIEW (node);
    int         step = (int) lround (dy * 3.0);

    if (step == 0)
        step = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
    if (step == 0)
        return;

    self->scroll_offset += step;
    if (self->scroll_offset < 0)
        self->scroll_offset = 0;

    pn_node_request_repaint (PN_NODE (self));
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_text_view_get_size (
        PnNode *self,
        double *out_width,
        double *out_height)
{
    (void) self;
    if (out_width  != NULL) *out_width  = PN_TEXT_VIEW_WIDTH;
    if (out_height != NULL) *out_height = PN_TEXT_VIEW_TOTAL_HEIGHT;
}

static double
pn_text_view_get_header_height (PnNode *self)
{
    (void) self;
    return PN_TEXT_VIEW_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_text_view_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTextView *self = PN_TEXT_VIEW (object);

    switch (prop_id)
    {
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_TEXT_COLOR:
        g_value_set_boxed (value, &self->text_color);
        break;
    case PROP_FONT_SIZE:
        g_value_set_double (value, self->font_size);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_text_view_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTextView *self = PN_TEXT_VIEW (object);

    switch (prop_id)
    {
    case PROP_BACKGROUND_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_TEXT_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->text_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_FONT_SIZE:
        self->font_size = g_value_get_double (value);
        pn_node_request_repaint (PN_NODE (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_text_view_finalize (GObject *object)
{
    PnTextView *self = PN_TEXT_VIEW (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }

    clear_lines (self);

    G_OBJECT_CLASS (pn_text_view_parent_class)->finalize (object);
}

static void
pn_text_view_class_init (PnTextViewClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_text_view_get_property;
    object_class->set_property = pn_text_view_set_property;
    object_class->finalize     = pn_text_view_finalize;

    node_class->receive           = pn_text_view_receive;
    node_class->get_size          = pn_text_view_get_size;
    node_class->get_header_height = pn_text_view_get_header_height;
    node_class->scroll            = pn_text_view_scroll;
    /* The cairo text painter (paint_plot) is installed onto this class by
     * the gui tier — pn_text_view_gui_install() in pn-text-view-gui.c —
     * so the headless core carries no GTK/cairo.  The scroll vfunc stays
     * here: it only nudges an int. */

    node_class->class_name        = "Text View";
    /* fa-terminal U+F120 -- a glyph in the FA-4 range the bundled
     * icon font supports.  Same FA-4 ceiling (no codepoints past
     * U+F2FF) the shell-plugin icons document applies here. */
    node_class->icon              = "\xef\x84\xa0";
    node_class->color             = (PnColor){ 0.18, 0.78, 0.30, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = TRUE;

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the text rectangle behind the lines",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the rendered output text",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FONT_SIZE] = g_param_spec_double (
            "font-size", "Font size",
            "Monospace font size in pixels",
            6.0, 48.0, PN_TEXT_VIEW_FONT_PX,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_text_view_init (PnTextView *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  green = { 0.18, 0.78, 0.30, 1.0 };

    /* Classic terminal: green-on-black. */
    self->background_color = (PnColor) { 0.0, 0.0, 0.0, 1.0 };
    self->text_color       = (PnColor) { 0.0, 1.0, 0.0, 1.0 };
    self->font_size        = PN_TEXT_VIEW_FONT_PX;

    self->lines              = NULL;
    self->n_lines            = 0;
    self->scroll_offset      = 0;
    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    pn_node_set_class_name (node, "Text View");
    pn_node_set_icon       (node, "\xef\x84\xa0");  /* fa-terminal */
    pn_node_set_color (node, &green);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTextView *
pn_text_view_new (void)
{
    return g_object_new (PN_TYPE_TEXT_VIEW, NULL);
}
