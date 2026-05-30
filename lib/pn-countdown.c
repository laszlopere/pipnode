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
/*  PnCountdown — logic tier (headless core).                          */
/*                                                                     */
/*  The GTK-free half of the Countdown node: the GType, all properties */
/*  (the two colours, the day-digit count and the labels toggle), the   */
/*  receive() that reads data.value as "seconds remaining", the         */
/*  days/hours/minutes/seconds breakdown, geometry vfuncs and the read  */
/*  seam the gui tier paints from.  The seven-segment cairo drawing      */
/*  lives in pn-countdown-gui.c, which installs its paint_plot slot onto */
/*  this class at editor startup (pn_countdown_gui_install).  The         */
/*  headless runtime registers and runs this node without pulling GTK.   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <json-glib/json-glib.h>

#include "pn-countdown.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  A wide panel: the standard 40-px header on top, then a black LED    */
/*  display below it sized to fit "DAYS HOURS:MINUTES:SECONDS" with the */
/*  default three day digits.                                          */
/* ------------------------------------------------------------------ */

#define PN_CD_WIDTH          300.0
#define PN_CD_HEADER_HEIGHT   40.0
#define PN_CD_GAP              4.0
#define PN_CD_BODY_HEIGHT    100.0
#define PN_CD_TOTAL_HEIGHT  (PN_CD_HEADER_HEIGHT + PN_CD_GAP + PN_CD_BODY_HEIGHT)

/* Seconds-per-unit constants for the breakdown. */
#define PN_CD_SECS_PER_DAY    86400
#define PN_CD_SECS_PER_HOUR    3600
#define PN_CD_SECS_PER_MIN       60

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnCountdown
{
    PnNode    parent_instance;

    /* Configuration. */
    guint     day_digits;        /* digit cells reserved for the days field */
    gboolean  show_labels;       /* draw the DAYS/HOURS/MINUTES/SECONDS caps */
    PnColor   background_color;
    PnColor   segment_color;          /* lit seven-segment bar */
    PnColor   unlit_segment_color;    /* the off-state ghost bar */
    PnColor   label_color;       /* the caption text colour */

    /* Live state.  @remaining is the most-recent data.value seen, floored
     * to whole seconds; @has_value latches once the first message lands.
     * The display clamps a negative @remaining to zero, so a passed
     * deadline reads as all-zeroes rather than as a negative clock. */
    gint64    remaining;
    gboolean  has_value;
};

G_DEFINE_FINAL_TYPE (PnCountdown, pn_countdown, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_DAY_DIGITS,
    PROP_SHOW_LABELS,
    PROP_BACKGROUND_COLOR,
    PROP_SEGMENT_COLOR,
    PROP_UNLIT_SEGMENT_COLOR,
    PROP_LABEL_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Read the incoming "seconds remaining" off data.value.  Accepts a
 * double or an int64 (the Deadline emits a double; both decode the same
 * count).  Returns FALSE when there is no numeric value to display. */
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

/* The integer "seconds remaining" the display currently shows: the raw
 * value clamped at zero, and zero again until the first message lands. */
static gint64
display_remaining (PnCountdown *self)
{
    if (!self->has_value || self->remaining < 0)
        return 0;
    return self->remaining;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_countdown_receive (PnNode *node, PnMessage *message)
{
    PnCountdown *self = PN_COUNTDOWN (node);
    gdouble      value;

    if (!read_value (message, &value))
        return;   /* nothing numeric to show — leave the display as-is */

    pn_countdown_set_value (self, value);
    /* Pure sink: never forwards. */
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_countdown_get_paint_state (PnCountdown           *self,
                              PnCountdownPaintState *out)
{
    gint64 rem;

    g_return_if_fail (PN_IS_COUNTDOWN (self));
    g_return_if_fail (out != NULL);

    rem = display_remaining (self);

    out->days    = rem / PN_CD_SECS_PER_DAY;
    out->hours   = (gint) ((rem % PN_CD_SECS_PER_DAY) / PN_CD_SECS_PER_HOUR);
    out->minutes = (gint) ((rem % PN_CD_SECS_PER_HOUR) / PN_CD_SECS_PER_MIN);
    out->seconds = (gint) (rem % PN_CD_SECS_PER_MIN);

    out->day_digits       = self->day_digits;
    out->show_labels      = self->show_labels;
    out->background_color     = self->background_color;
    out->segment_color        = self->segment_color;
    out->unlit_segment_color  = self->unlit_segment_color;
    out->label_color          = self->label_color;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_countdown_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_CD_WIDTH;
    if (out_h != NULL) *out_h = PN_CD_TOTAL_HEIGHT;
}

static double
pn_countdown_get_header_height (PnNode *node)
{
    (void) node;
    return PN_CD_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_countdown_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
    PnCountdown *self = PN_COUNTDOWN (object);

    switch (prop_id)
    {
    case PROP_DAY_DIGITS:
        g_value_set_uint (value, self->day_digits);
        break;
    case PROP_SHOW_LABELS:
        g_value_set_boolean (value, self->show_labels);
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
    case PROP_LABEL_COLOR:
        g_value_set_boxed (value, &self->label_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_color_prop (PnCountdown *self, PnColor *slot,
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
pn_countdown_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
    PnCountdown *self = PN_COUNTDOWN (object);

    switch (prop_id)
    {
    case PROP_DAY_DIGITS:
    {
        guint v = g_value_get_uint (value);
        if (v != self->day_digits)
        {
            self->day_digits = v;
            g_object_notify_by_pspec (object, props[PROP_DAY_DIGITS]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_SHOW_LABELS:
    {
        gboolean v = g_value_get_boolean (value);
        if (v != self->show_labels)
        {
            self->show_labels = v;
            g_object_notify_by_pspec (object, props[PROP_SHOW_LABELS]);
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
    case PROP_LABEL_COLOR:
        set_color_prop (self, &self->label_color, value,
                        PROP_LABEL_COLOR);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_countdown_class_init (PnCountdownClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_countdown_get_property;
    object_class->set_property = pn_countdown_set_property;

    /* Logic + intrinsic geometry stay in the core class; the cairo
     * seven-segment drawing (paint_plot + its skip-shadow / skip-zoom
     * flags) is installed by the gui tier — see pn_countdown_gui_install()
     * in pn-countdown-gui.c. */
    node_class->receive           = pn_countdown_receive;
    node_class->get_size          = pn_countdown_get_size;
    node_class->get_header_height = pn_countdown_get_header_height;

    node_class->class_name = "Countdown";
    node_class->icon       = "\xef\x80\x97";  /* fa-clock-o U+F017 */
    /* The gauge-sink amber shared with #PnDial and #PnAnalogMeter: this
     * is another single-value readout sink, so palette-grouping it with
     * them by colour helps the eye spot a worksheet's metering row.  (Red
     * is deliberately avoided — it reads as an error state on a node.)  */
    node_class->color      = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category   = "GUI/Displays";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_DAY_DIGITS] = g_param_spec_uint (
            "day-digits", "Day digits",
            "Number of seven-segment cells reserved for the days field. "
            "Three (the default) counts up to 999 days; raise it for "
            "longer horizons.",
            1, 6, 3,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SHOW_LABELS] = g_param_spec_boolean (
            "show-labels", "Show labels",
            "Draw the DAYS / HOURS / MINUTES / SECONDS captions above "
            "their digit groups.",
            TRUE,
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

    props[PROP_LABEL_COLOR] = g_param_spec_boxed (
            "label-color", "Label colour",
            "Colour of the DAYS / HOURS / MINUTES / SECONDS captions — a "
            "soft off-white by default, like the silk-screened legend "
            "above an LED clock's window.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings dialog: two pages.  Display groups the layout
     * knobs, Colours the two PnColor rows.  No -gui.so companion needed
     * for the dialog itself; only the painter lives in the gui tier. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Display");
        pn_settings_schema_row (schema, "day-digits",  PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "show-labels", PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Colours");
        pn_settings_schema_row (schema, "background-color",     PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "segment-color",        PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "unlit-segment-color",  PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "label-color",          PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_countdown_init (PnCountdown *self)
{
    PnNode *node = PN_NODE (self);

    self->day_digits       = 3;
    self->show_labels      = TRUE;
    self->background_color     = (PnColor){ 0.04, 0.04, 0.05, 1.0 };
    self->segment_color        = (PnColor){ 0.92, 0.12, 0.08, 1.0 };
    /* Off-state ghost: the lit red dimmed to 16 %, matching the dim bar
     * the painter used to derive from the lit colour before this became a
     * configurable property. */
    self->unlit_segment_color  = (PnColor){ 0.1472, 0.0192, 0.0128, 1.0 };
    self->label_color          = (PnColor){ 0.82, 0.84, 0.86, 0.92 };

    self->remaining = 0;
    self->has_value = FALSE;

    {
        PnColor amber = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, &amber);
    }
    pn_node_set_class_name (node, "Countdown");
    pn_node_set_icon       (node, "\xef\x80\x97");  /* fa-clock-o */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnCountdown *
pn_countdown_new (void)
{
    return g_object_new (PN_TYPE_COUNTDOWN, NULL);
}

void
pn_countdown_set_value (PnCountdown *self, gdouble seconds)
{
    gint64 before, after;

    g_return_if_fail (PN_IS_COUNTDOWN (self));
    if (!isfinite (seconds))
        return;

    /* Snapshot what the display currently shows, apply the new reading,
     * then repaint only when the visible digits would actually change —
     * a once-a-second clock feed must not drive a faster redraw than the
     * seconds it counts. */
    before = display_remaining (self);

    self->has_value = TRUE;
    self->remaining = (gint64) seconds;   /* floor toward zero */

    after = display_remaining (self);

    if (after != before)
        pn_node_request_repaint (PN_NODE (self));
}
