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
/*  PnNumeric — logic tier (headless core).                            */
/*                                                                     */
/*  The GTK-free half of the numeric LED readout: the GType, the       */
/*  properties (digit count, decimal places, the three colours), the   */
/*  receive() that reads data.value as a number, and the read seam the */
/*  gui tier paints from.  The seven-segment cairo drawing lives in    */
/*  pn-numeric-gui.c.                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <json-glib/json-glib.h>

#include "pn-numeric.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  A wide panel matching #PnCountdown: standard 40-px header on top,  */
/*  then a black LED screen below it sized to fit the default 6-digit  */
/*  integer + 2-place fractional layout with the sign cell.            */
/* ------------------------------------------------------------------ */

#define PN_NM_WIDTH          300.0
#define PN_NM_HEADER_HEIGHT   40.0
#define PN_NM_GAP              4.0
#define PN_NM_BODY_HEIGHT    100.0
#define PN_NM_TOTAL_HEIGHT  (PN_NM_HEADER_HEIGHT + PN_NM_GAP + PN_NM_BODY_HEIGHT)

#define PN_NM_DEFAULT_DIGITS          6
#define PN_NM_DEFAULT_DECIMAL_PLACES  2

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnNumeric
{
    PnNode    parent_instance;

    /* Configuration. */
    guint     digits;             /* integer digit cells (1..12)         */
    guint     decimal_places;     /* fractional digit cells (0..6)       */
    PnColor   background_color;
    PnColor   segment_color;          /* lit seven-segment bar */
    PnColor   unlit_segment_color;    /* off-state ghost bar    */

    /* Live state.  @value is the most-recent data.value seen;
     * @has_value latches once the first numeric message lands so a node
     * with no input yet reads as a blank screen rather than as 0.00. */
    gdouble   value;
    gboolean  has_value;
};

G_DEFINE_FINAL_TYPE (PnNumeric, pn_numeric, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_DIGITS,
    PROP_DECIMAL_PLACES,
    PROP_BACKGROUND_COLOR,
    PROP_SEGMENT_COLOR,
    PROP_UNLIT_SEGMENT_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Read the incoming number off data.value.  Accepts a double or an
 * int64.  Returns FALSE when there is no finite numeric value. */
static gboolean
read_value (PnMessage *message, gdouble *out)
{
    JsonNode *node = pn_message_get_member (message, "value");
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt != G_TYPE_DOUBLE && vt != G_TYPE_INT64)
        return FALSE;

    *out = json_node_get_double (node);
    if (!isfinite (*out))
        return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_numeric_receive (PnNode *node, PnMessage *message)
{
    PnNumeric *self = PN_NUMERIC (node);
    gdouble    value;

    if (!read_value (message, &value))
        return;   /* nothing numeric to show — leave the display as-is */

    pn_numeric_set_value (self, value);
    /* Pure sink: never forwards. */
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_numeric_get_paint_state (PnNumeric           *self,
                            PnNumericPaintState *out)
{
    g_return_if_fail (PN_IS_NUMERIC (self));
    g_return_if_fail (out != NULL);

    out->value                = self->value;
    out->has_value            = self->has_value;
    out->digits               = self->digits;
    out->decimal_places       = self->decimal_places;
    out->background_color     = self->background_color;
    out->segment_color        = self->segment_color;
    out->unlit_segment_color  = self->unlit_segment_color;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_numeric_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_NM_WIDTH;
    if (out_h != NULL) *out_h = PN_NM_TOTAL_HEIGHT;
}

static double
pn_numeric_get_header_height (PnNode *node)
{
    (void) node;
    return PN_NM_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_numeric_get_property (GObject    *object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
    PnNumeric *self = PN_NUMERIC (object);

    switch (prop_id)
    {
    case PROP_DIGITS:
        g_value_set_uint (value, self->digits);
        break;
    case PROP_DECIMAL_PLACES:
        g_value_set_uint (value, self->decimal_places);
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
set_color_prop (PnNumeric *self, PnColor *slot,
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
pn_numeric_set_property (GObject      *object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
    PnNumeric *self = PN_NUMERIC (object);

    switch (prop_id)
    {
    case PROP_DIGITS:
    {
        guint v = g_value_get_uint (value);
        if (v != self->digits)
        {
            self->digits = v;
            g_object_notify_by_pspec (object, props[PROP_DIGITS]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_DECIMAL_PLACES:
    {
        guint v = g_value_get_uint (value);
        if (v != self->decimal_places)
        {
            self->decimal_places = v;
            g_object_notify_by_pspec (object, props[PROP_DECIMAL_PLACES]);
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
pn_numeric_class_init (PnNumericClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_numeric_get_property;
    object_class->set_property = pn_numeric_set_property;

    /* Logic + intrinsic geometry stay in the core class; the cairo
     * seven-segment drawing is installed by the gui tier — see
     * pn_numeric_gui_install() in pn-numeric-gui.c. */
    node_class->receive           = pn_numeric_receive;
    node_class->get_size          = pn_numeric_get_size;
    node_class->get_header_height = pn_numeric_get_header_height;

    node_class->class_name = "Numeric";
    node_class->icon       = "\xef\x8a\x92";  /* fa-hashtag U+F292 */
    /* The gauge-sink amber shared with #PnDial, #PnAnalogMeter and the
     * other LED readouts — palette-grouping single-value sinks by colour
     * helps the eye spot a worksheet's metering row. */
    node_class->color      = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category   = "Sinks";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_DIGITS] = g_param_spec_uint (
            "digits", "Digits",
            "Number of seven-segment cells reserved for the integer part "
            "of the value (1–12).  Six (the default) shows up to "
            "999999; a value whose integer part does not fit reads as "
            "all dashes — the LED-meter overload indicator.",
            1, 12, PN_NM_DEFAULT_DIGITS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DECIMAL_PLACES] = g_param_spec_uint (
            "decimal-places", "Decimal places",
            "Number of seven-segment cells reserved for the fractional "
            "part of the value (0–6).  Zero drops the decimal point and "
            "the fractional block entirely; two (the default) is the "
            "common two-decimal voltmeter look.",
            0, 6, PN_NM_DEFAULT_DECIMAL_PLACES,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the display panel behind the digits — the "
            "near-black face of an LED clock by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SEGMENT_COLOR] = g_param_spec_boxed (
            "segment-color", "Segment colour",
            "Colour of a lit seven-segment bar — the classic LED red by "
            "default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_UNLIT_SEGMENT_COLOR] = g_param_spec_boxed (
            "unlit-segment-color", "Unlit segment colour",
            "Colour of an unlit seven-segment bar — the dark off-state "
            "ghost of a digit, a dim red over the black face by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings dialog: two pages mirroring #PnCountdown. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Display");
        pn_settings_schema_row (schema, "digits",          PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "decimal-places",  PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Colours");
        pn_settings_schema_row (schema, "background-color",     PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "segment-color",        PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "unlit-segment-color",  PN_EDITOR_AUTO);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_numeric_init (PnNumeric *self)
{
    PnNode *node = PN_NODE (self);

    self->digits               = PN_NM_DEFAULT_DIGITS;
    self->decimal_places       = PN_NM_DEFAULT_DECIMAL_PLACES;
    self->background_color     = (PnColor){ 0.04, 0.04, 0.05, 1.0 };
    self->segment_color        = (PnColor){ 0.92, 0.12, 0.08, 1.0 };
    /* Off-state ghost: the lit red dimmed to 16 %, matching #PnCountdown. */
    self->unlit_segment_color  = (PnColor){ 0.1472, 0.0192, 0.0128, 1.0 };

    self->value     = 0.0;
    self->has_value = FALSE;

    {
        PnColor amber = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, &amber);
    }
    pn_node_set_class_name (node, "Numeric");
    pn_node_set_icon       (node, "\xef\x8a\x92");  /* fa-hashtag */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnNumeric *
pn_numeric_new (void)
{
    return g_object_new (PN_TYPE_NUMERIC, NULL);
}

void
pn_numeric_set_value (PnNumeric *self, gdouble value)
{
    g_return_if_fail (PN_IS_NUMERIC (self));
    if (!isfinite (value))
        return;

    /* Repaint only when the visible reading would change.  The painter
     * rounds to the configured decimal places, so two values that differ
     * below that resolution must not drive a redraw. */
    if (self->has_value && self->value == value)
        return;

    self->has_value = TRUE;
    self->value     = value;
    pn_node_request_repaint (PN_NODE (self));
}
