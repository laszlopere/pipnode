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
/*  PnMatrix57 — logic tier (headless core).                           */
/*                                                                     */
/*  The GTK-free half of the 5x7 character-LCD readout: the GType, the */
/*  properties (cells, colours), receive() that reads data.output as a */
/*  string, and the read seam the gui tier paints from.  The cairo     */
/*  drawing and the ASCII glyph table live in pn-matrix57-gui.c.  The  */
/*  headless runtime registers and runs the node without GTK.          */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <json-glib/json-glib.h>

#include "pn-matrix57.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Standard 40-px header on top, then the LCD screen below it sized   */
/*  to hold the default sixteen 5x7 cells — same panel proportions as  */
/*  #PnSegment16, just a touch wider since 16 cells is the canonical   */
/*  character-LCD line.                                                */
/* ------------------------------------------------------------------ */

#define PN_M57_WIDTH          380.0
#define PN_M57_HEADER_HEIGHT   40.0
#define PN_M57_GAP              4.0
#define PN_M57_BODY_HEIGHT     90.0
#define PN_M57_TOTAL_HEIGHT  (PN_M57_HEADER_HEIGHT + PN_M57_GAP + PN_M57_BODY_HEIGHT)

#define PN_M57_DEFAULT_CELLS   16

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnMatrix57
{
    PnNode    parent_instance;

    /* Configuration. */
    guint     cells;                  /* visible cell count (1..40)         */
    PnColor   background_color;       /* LCD face                           */
    PnColor   pixel_color;            /* lit (dark) dot                     */
    PnColor   unlit_pixel_color;      /* off-state ghost                    */

    /* Live state.  See pn-segment16.c for the NULL/"" repaint trick. */
    gchar    *text;
};

G_DEFINE_FINAL_TYPE (PnMatrix57, pn_matrix57, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_CELLS,
    PROP_BACKGROUND_COLOR,
    PROP_PIXEL_COLOR,
    PROP_UNLIT_PIXEL_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_matrix57_receive (PnNode *node, PnMessage *message)
{
    PnMatrix57  *self = PN_MATRIX57 (node);
    JsonNode    *member;
    const gchar *text = NULL;

    /* Headline string is data.output — same convention as #PnLabel and
     * #PnSegment16.  A non-string or missing member blanks the readout. */
    member = pn_message_get_member (message, "output");
    if (member != NULL && JSON_NODE_HOLDS_VALUE (member)
        && json_node_get_value_type (member) == G_TYPE_STRING)
        text = json_node_get_string (member);

    pn_matrix57_set_text (self, text);
    /* Pure sink: never forwards. */
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_matrix57_get_paint_state (PnMatrix57           *self,
                             PnMatrix57PaintState *out)
{
    g_return_if_fail (PN_IS_MATRIX57 (self));
    g_return_if_fail (out != NULL);

    out->text              = self->text != NULL ? self->text : "";
    out->cells             = self->cells;
    out->background_color  = self->background_color;
    out->pixel_color       = self->pixel_color;
    out->unlit_pixel_color = self->unlit_pixel_color;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_matrix57_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_M57_WIDTH;
    if (out_h != NULL) *out_h = PN_M57_TOTAL_HEIGHT;
}

static double
pn_matrix57_get_header_height (PnNode *node)
{
    (void) node;
    return PN_M57_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_matrix57_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    PnMatrix57 *self = PN_MATRIX57 (object);

    switch (prop_id)
    {
    case PROP_CELLS:
        g_value_set_uint (value, self->cells);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_PIXEL_COLOR:
        g_value_set_boxed (value, &self->pixel_color);
        break;
    case PROP_UNLIT_PIXEL_COLOR:
        g_value_set_boxed (value, &self->unlit_pixel_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_color_prop (PnMatrix57 *self, PnColor *slot,
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
pn_matrix57_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    PnMatrix57 *self = PN_MATRIX57 (object);

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
    case PROP_PIXEL_COLOR:
        set_color_prop (self, &self->pixel_color, value, PROP_PIXEL_COLOR);
        break;
    case PROP_UNLIT_PIXEL_COLOR:
        set_color_prop (self, &self->unlit_pixel_color, value,
                        PROP_UNLIT_PIXEL_COLOR);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_matrix57_finalize (GObject *object)
{
    PnMatrix57 *self = PN_MATRIX57 (object);

    g_free (self->text);

    G_OBJECT_CLASS (pn_matrix57_parent_class)->finalize (object);
}

static void
pn_matrix57_class_init (PnMatrix57Class *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_matrix57_get_property;
    object_class->set_property = pn_matrix57_set_property;
    object_class->finalize     = pn_matrix57_finalize;

    /* Logic + intrinsic geometry stay in the core class; the cairo dot-
     * matrix drawing is installed by the gui tier — see
     * pn_matrix57_gui_install(). */
    node_class->receive           = pn_matrix57_receive;
    node_class->get_size          = pn_matrix57_get_size;
    node_class->get_header_height = pn_matrix57_get_header_height;

    node_class->class_name = "Matrix57";
    /* fa-th U+F00A: a 3x3 grid, an unambiguous "dot matrix display"
     * indicator on the node header. */
    node_class->icon       = "\xef\x80\x8a";
    /* Match the gauge-sink amber of #PnCountdown, #PnDial and
     * #PnSegment16 — colour-grouping the readout sinks helps the eye
     * spot the metering row on a worksheet, even though the LCD itself
     * is greenish-yellow. */
    node_class->color      = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category   = "Sinks";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_CELLS] = g_param_spec_uint (
            "cells", "Cells",
            "Number of 5x7 character cells in the row.  Sixteen (the "
            "default) is the canonical HD44780 line length; raise it for "
            "longer readouts.",
            1, 40, PN_M57_DEFAULT_CELLS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the LCD face behind the dots — the classic "
            "greenish-yellow of a character LCD by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_PIXEL_COLOR] = g_param_spec_boxed (
            "pixel-color", "Pixel colour",
            "Colour of a lit dot — near-black by default, the way an LCD "
            "pixel reads.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_UNLIT_PIXEL_COLOR] = g_param_spec_boxed (
            "unlit-pixel-color", "Unlit pixel colour",
            "Colour of an unlit dot — the very faint off-state ghost over "
            "the LCD face, almost invisible by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings dialog: Display for the cell count, Colours
     * for the three PnColor rows — same shape as #PnSegment16 so the two
     * character-readout sinks read the same in the dialog. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Display");
        pn_settings_schema_row (schema, "cells", PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Colours");
        pn_settings_schema_row (schema, "background-color",  PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "pixel-color",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "unlit-pixel-color", PN_EDITOR_AUTO);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_matrix57_init (PnMatrix57 *self)
{
    PnNode *node = PN_NODE (self);

    self->cells             = PN_M57_DEFAULT_CELLS;
    /* Classic greenish-yellow LCD face (the colour you see on an HD44780
     * module sitting in the sun). */
    self->background_color  = (PnColor){ 0.72, 0.80, 0.28, 1.0 };
    /* Lit pixel: near-black with a hint of green so it doesn't read pure
     * monochrome over the LCD face. */
    self->pixel_color       = (PnColor){ 0.06, 0.08, 0.04, 1.0 };
    /* Off-state ghost: a barely-perceptible darkening of the face — real
     * LCD dots are almost invisible when off, but a touch of ghosting
     * helps the editor preview read as a panel rather than a flat fill. */
    self->unlit_pixel_color = (PnColor){ 0.66, 0.74, 0.26, 1.0 };

    self->text = NULL;

    {
        PnColor amber = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, &amber);
    }
    pn_node_set_class_name (node, "Matrix57");
    pn_node_set_icon       (node, "\xef\x80\x8a");  /* fa-th */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMatrix57 *
pn_matrix57_new (void)
{
    return g_object_new (PN_TYPE_MATRIX57, NULL);
}

void
pn_matrix57_set_text (PnMatrix57 *self, const gchar *text)
{
    g_return_if_fail (PN_IS_MATRIX57 (self));

    /* Normalise NULL and "" both to the blank state but only repaint
     * when the visible string actually changes — see pn-segment16.c. */
    if (text != NULL && text[0] == '\0')
        text = NULL;

    if (g_strcmp0 (self->text, text) == 0)
        return;

    g_free (self->text);
    self->text = g_strdup (text);
    pn_node_request_repaint (PN_NODE (self));
}
