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
/*  PnSegment16 — logic tier (headless core).                          */
/*                                                                     */
/*  The GTK-free half of the 16-segment LED readout: the GType, the    */
/*  properties (cells, colours), receive() that reads data.output as a */
/*  string, and the read seam the gui tier paints from.  The cairo     */
/*  drawing and the ASCII glyph table live in pn-segment16-gui.c.      */
/*  The headless runtime registers and runs the node without GTK.     */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <json-glib/json-glib.h>

#include "pn-segment16.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  A wide panel like #PnCountdown's: standard 40-px header on top,    */
/*  then a black LED screen below it sized to hold the default eight   */
/*  16-segment cells.                                                  */
/* ------------------------------------------------------------------ */

#define PN_S16_WIDTH          340.0
#define PN_S16_HEADER_HEIGHT   40.0
#define PN_S16_GAP              4.0
#define PN_S16_BODY_HEIGHT    100.0
#define PN_S16_TOTAL_HEIGHT  (PN_S16_HEADER_HEIGHT + PN_S16_GAP + PN_S16_BODY_HEIGHT)

#define PN_S16_DEFAULT_CELLS    8

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnSegment16
{
    PnNode    parent_instance;

    /* Configuration. */
    guint     cells;                  /* visible cell count (1..32)         */
    PnColor   background_color;
    PnColor   segment_color;          /* lit bar                            */
    PnColor   unlit_segment_color;    /* off-state ghost                    */

    /* Live state.  @text is the most-recent data.output seen, or NULL when
     * nothing has landed yet (the row reads blank either way; we keep it
     * NULL vs. "" so we can tell the two apart for repaint suppression). */
    gchar    *text;
};

G_DEFINE_FINAL_TYPE (PnSegment16, pn_segment16, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_CELLS,
    PROP_BACKGROUND_COLOR,
    PROP_SEGMENT_COLOR,
    PROP_UNLIT_SEGMENT_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_segment16_receive (PnNode *node, PnMessage *message)
{
    PnSegment16 *self = PN_SEGMENT16 (node);
    JsonNode    *member;
    const gchar *text = NULL;

    /* Headline string is data.output — same convention as #PnLabel.  A
     * non-string or missing member blanks the readout. */
    member = pn_message_get_member (message, "output");
    if (member != NULL && JSON_NODE_HOLDS_VALUE (member)
        && json_node_get_value_type (member) == G_TYPE_STRING)
        text = json_node_get_string (member);

    pn_segment16_set_text (self, text);
    /* Pure sink: never forwards. */
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_segment16_get_paint_state (PnSegment16           *self,
                              PnSegment16PaintState *out)
{
    g_return_if_fail (PN_IS_SEGMENT16 (self));
    g_return_if_fail (out != NULL);

    out->text                = self->text != NULL ? self->text : "";
    out->cells               = self->cells;
    out->background_color    = self->background_color;
    out->segment_color       = self->segment_color;
    out->unlit_segment_color = self->unlit_segment_color;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_segment16_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_S16_WIDTH;
    if (out_h != NULL) *out_h = PN_S16_TOTAL_HEIGHT;
}

static double
pn_segment16_get_header_height (PnNode *node)
{
    (void) node;
    return PN_S16_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_segment16_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
    PnSegment16 *self = PN_SEGMENT16 (object);

    switch (prop_id)
    {
    case PROP_CELLS:
        g_value_set_uint (value, self->cells);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_SEGMENT_COLOR:
        g_value_set_boxed (value, &self->segment_color);
        break;
    case PROP_UNLIT_SEGMENT_COLOR:
        g_value_set_boxed (value, &self->unlit_segment_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_color_prop (PnSegment16 *self, PnColor *slot,
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
pn_segment16_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
    PnSegment16 *self = PN_SEGMENT16 (object);

    switch (prop_id)
    {
    case PROP_CELLS:
    {
        guint v = g_value_get_uint (value);
        if (v != self->cells)
        {
            self->cells = v;
            g_object_notify_by_pspec (object, props[PROP_CELLS]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_BACKGROUND_COLOR:
        set_color_prop (self, &self->background_color, value,
                        PROP_BACKGROUND_COLOR);
        break;
    case PROP_SEGMENT_COLOR:
        set_color_prop (self, &self->segment_color, value,
                        PROP_SEGMENT_COLOR);
        break;
    case PROP_UNLIT_SEGMENT_COLOR:
        set_color_prop (self, &self->unlit_segment_color, value,
                        PROP_UNLIT_SEGMENT_COLOR);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_segment16_finalize (GObject *object)
{
    PnSegment16 *self = PN_SEGMENT16 (object);

    g_free (self->text);

    G_OBJECT_CLASS (pn_segment16_parent_class)->finalize (object);
}

static void
pn_segment16_class_init (PnSegment16Class *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_segment16_get_property;
    object_class->set_property = pn_segment16_set_property;
    object_class->finalize     = pn_segment16_finalize;

    /* Logic + intrinsic geometry stay in the core class; the cairo 16-
     * segment drawing (paint_plot + skip-shadow / skip-zoom flags) is
     * installed by the gui tier — see pn_segment16_gui_install(). */
    node_class->receive           = pn_segment16_receive;
    node_class->get_size          = pn_segment16_get_size;
    node_class->get_header_height = pn_segment16_get_header_height;

    node_class->class_name = "Segment16";
    /* fa-text-width U+F035: a stylised text indicator matching the
     * alphanumeric character of the readout. */
    node_class->icon       = "\xef\x80\xb5";
    /* The same gauge-sink amber as #PnCountdown and #PnDial — another
     * LED-panel readout sink, so palette-grouping by colour helps the
     * eye spot a worksheet's metering row. */
    node_class->color      = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category   = "Sinks";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_CELLS] = g_param_spec_uint (
            "cells", "Cells",
            "Number of 16-segment character cells in the row.  Eight (the "
            "default) is about as wide as a panel clock; raise it for "
            "longer readouts.",
            1, 32, PN_S16_DEFAULT_CELLS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the display panel behind the digits — the "
            "near-black face of an LED clock by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SEGMENT_COLOR] = g_param_spec_boxed (
            "segment-color", "Segment colour",
            "Colour of a lit 16-segment bar — the classic LED red by "
            "default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_UNLIT_SEGMENT_COLOR] = g_param_spec_boxed (
            "unlit-segment-color", "Unlit segment colour",
            "Colour of an unlit 16-segment bar — the dark off-state ghost "
            "of a cell, a dim red over the black face by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings dialog: Display for the cell count, Colours
     * for the three PnColor rows.  Mirrors the #PnCountdown schema so the
     * two LED sinks read the same in the dialog. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Display");
        pn_settings_schema_row (schema, "cells", PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Colours");
        pn_settings_schema_row (schema, "background-color",    PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "segment-color",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "unlit-segment-color", PN_EDITOR_AUTO);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_segment16_init (PnSegment16 *self)
{
    PnNode *node = PN_NODE (self);

    self->cells               = PN_S16_DEFAULT_CELLS;
    self->background_color    = (PnColor){ 0.04, 0.04, 0.05, 1.0 };
    self->segment_color       = (PnColor){ 0.92, 0.12, 0.08, 1.0 };
    /* Off-state ghost: the lit red dimmed to 16 %, matching #PnCountdown. */
    self->unlit_segment_color = (PnColor){ 0.1472, 0.0192, 0.0128, 1.0 };

    self->text = NULL;

    {
        PnColor amber = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, &amber);
    }
    pn_node_set_class_name (node, "Segment16");
    pn_node_set_icon       (node, "\xef\x80\xb5");  /* fa-text-width */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnSegment16 *
pn_segment16_new (void)
{
    return g_object_new (PN_TYPE_SEGMENT16, NULL);
}

void
pn_segment16_set_text (PnSegment16 *self, const gchar *text)
{
    g_return_if_fail (PN_IS_SEGMENT16 (self));

    /* Normalise NULL and "" both to the blank state but only repaint when
     * the visible string actually changes — receive() runs once per
     * incoming message and a hot source must not drive a redraw faster
     * than the display content moves. */
    if (text != NULL && text[0] == '\0')
        text = NULL;

    if (g_strcmp0 (self->text, text) == 0)
        return;

    g_free (self->text);
    self->text = g_strdup (text);
    pn_node_request_repaint (PN_NODE (self));
}
