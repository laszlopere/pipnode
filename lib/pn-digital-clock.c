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
/*  PnDigitalClock — logic tier (headless core).                       */
/*                                                                     */
/*  The GTK-free half of the Digital Clock node: the GType, all          */
/*  properties (the colours and the labels toggle), the receive() that   */
/*  reads data.value as a count of seconds, the day-wrapped hours /      */
/*  minutes / seconds breakdown, geometry vfuncs and the read seam the   */
/*  gui tier paints from.  It is the #PnCountdown node with the days      */
/*  field dropped, so the client area is narrower (HH:MM:SS only) while   */
/*  the look, colours and segment sizes stay identical.  The seven-       */
/*  segment cairo drawing lives in pn-digital-clock-gui.c, which installs */
/*  its paint_plot slot onto this class at editor startup                 */
/*  (pn_digital_clock_gui_install).  The headless runtime registers and   */
/*  runs this node without pulling GTK.                                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <json-glib/json-glib.h>

#include "pn-digital-clock.h"
#include "pn-message.h"
#include "pn-settings-schema.h"
#include "pn-tz-table.h"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  The same panel as #PnCountdown — the standard 40-px header on top,   */
/*  then a black LED display below it — but only wide enough for the      */
/*  HOURS:MINUTES:SECONDS group, no days block.  The width is chosen so   */
/*  the digit cells come out the same size as the Countdown's at its      */
/*  default width, so the two nodes read as the same display family.      */
/* ------------------------------------------------------------------ */

#define PN_DCK_WIDTH          206.0
#define PN_DCK_HEADER_HEIGHT   40.0
#define PN_DCK_GAP              4.0
#define PN_DCK_BODY_HEIGHT    100.0
#define PN_DCK_TOTAL_HEIGHT  (PN_DCK_HEADER_HEIGHT + PN_DCK_GAP + PN_DCK_BODY_HEIGHT)

/* Seconds-per-unit constants for the breakdown. */
#define PN_DCK_SECS_PER_DAY    86400
#define PN_DCK_SECS_PER_HOUR    3600
#define PN_DCK_SECS_PER_MIN       60

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnDigitalClock
{
    PnNode    parent_instance;

    /* Configuration. */
    gboolean  show_labels;       /* draw the HOURS/MINUTES/SECONDS caps */
    PnColor   background_color;
    PnColor   segment_color;          /* lit seven-segment bar */
    PnColor   unlit_segment_color;    /* the off-state ghost bar */
    PnColor   label_color;       /* the caption text colour */
    gchar    *timezone;          /* shared tz-table label, or "Not Set" */

    /* Live state.  @value is the most-recent data.value seen, floored to
     * whole seconds; @has_value latches once the first message lands.  The
     * display clamps a negative @value to zero and wraps the rest into a
     * 24-hour face, so the readout always reads as a clock.  @offset_min
     * is the minutes-east-of-UTC the last message's "timezone" abbreviation
     * resolved to (relevant only when "Not Set" defers to the wire); a
     * configured zone overrides it. */
    gint64    value;
    gboolean  has_value;
    gint      offset_min;
};

G_DEFINE_FINAL_TYPE (PnDigitalClock, pn_digital_clock, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_SHOW_LABELS,
    PROP_TIMEZONE,
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

/* Read the incoming seconds count off data.value.  Accepts a double or an
 * int64 (the Deadline emits a double; both decode the same count).
 * Returns FALSE when there is no numeric value to display. */
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

/* TRUE if the timezone property names a fixed zone — anything but the
 * "Not Set" sentinel (and the empty string, which we treat the same). */
static gboolean
has_fixed_timezone (PnDigitalClock *self)
{
    return self->timezone != NULL
        && self->timezone[0] != '\0'
        && g_strcmp0 (self->timezone, PN_TZ_NOT_SET) != 0;
}

/* The integer seconds-of-day the display currently shows: the raw value
 * shifted by the active UTC offset, taken modulo a day, and zero until
 * the first message lands.  Negative wall-clock results are wrapped back
 * into [0, 86400) so the readout always reads as a clock face. */
static gint64
display_value (PnDigitalClock *self)
{
    gint   off_min;
    gint64 shifted;
    gint64 mod;

    if (!self->has_value || self->value < 0)
        return 0;

    off_min = has_fixed_timezone (self)
                ? pn_tz_table_offset_minutes (self->timezone)
                : self->offset_min;
    shifted = self->value + (gint64) off_min * 60;

    mod = shifted % PN_DCK_SECS_PER_DAY;
    if (mod < 0)
        mod += PN_DCK_SECS_PER_DAY;
    return mod;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_digital_clock_receive (PnNode *node, PnMessage *message)
{
    PnDigitalClock *self = PN_DIGITAL_CLOCK (node);
    gdouble         value;
    gint            new_offset = 0;

    if (!read_value (message, &value))
        return;   /* nothing numeric to show — leave the display as-is */

    /* "Not Set" defers to the message's "timezone" abbreviation (the Clock
     * node emits e.g. "CEST"); an unknown or missing abbreviation falls
     * back to GMT (UTC+0).  A configured zone wins outright and never
     * looks at the wire — but we still snapshot the wire's hint so a
     * later switch back to "Not Set" picks up the same instant. */
    {
        JsonNode *tz_node = pn_message_get_member (message, "timezone");

        if (tz_node != NULL && JSON_NODE_HOLDS_VALUE (tz_node)
            && json_node_get_value_type (tz_node) == G_TYPE_STRING)
        {
            const gchar *abbr  = json_node_get_string (tz_node);
            const gchar *label = pn_tz_table_lookup_by_abbreviation (abbr);

            if (label != NULL)
                new_offset = pn_tz_table_offset_minutes (label);
        }
    }
    self->offset_min = new_offset;

    pn_digital_clock_set_value (self, value);
    /* Pure sink: never forwards. */
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_digital_clock_get_paint_state (PnDigitalClock           *self,
                                  PnDigitalClockPaintState *out)
{
    gint64 v;

    g_return_if_fail (PN_IS_DIGITAL_CLOCK (self));
    g_return_if_fail (out != NULL);

    v = display_value (self);

    out->hours   = (gint) (v / PN_DCK_SECS_PER_HOUR);
    out->minutes = (gint) ((v % PN_DCK_SECS_PER_HOUR) / PN_DCK_SECS_PER_MIN);
    out->seconds = (gint) (v % PN_DCK_SECS_PER_MIN);

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
pn_digital_clock_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_DCK_WIDTH;
    if (out_h != NULL) *out_h = PN_DCK_TOTAL_HEIGHT;
}

static double
pn_digital_clock_get_header_height (PnNode *node)
{
    (void) node;
    return PN_DCK_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_digital_clock_get_property (GObject    *object,
                               guint       prop_id,
                               GValue     *value,
                               GParamSpec *pspec)
{
    PnDigitalClock *self = PN_DIGITAL_CLOCK (object);

    switch (prop_id)
    {
    case PROP_SHOW_LABELS:
        g_value_set_boolean (value, self->show_labels);
        break;
    case PROP_TIMEZONE:
        g_value_set_string (value, self->timezone);
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
set_color_prop (PnDigitalClock *self, PnColor *slot,
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
pn_digital_clock_set_property (GObject      *object,
                               guint         prop_id,
                               const GValue *value,
                               GParamSpec   *pspec)
{
    PnDigitalClock *self = PN_DIGITAL_CLOCK (object);

    switch (prop_id)
    {
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
    case PROP_TIMEZONE:
    {
        const gchar *v = g_value_get_string (value);
        if (g_strcmp0 (v, self->timezone) != 0)
        {
            g_free (self->timezone);
            self->timezone = g_strdup (v != NULL ? v : PN_TZ_NOT_SET);
            g_object_notify_by_pspec (object, props[PROP_TIMEZONE]);
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
pn_digital_clock_finalize (GObject *object)
{
    PnDigitalClock *self = PN_DIGITAL_CLOCK (object);

    g_free (self->timezone);

    G_OBJECT_CLASS (pn_digital_clock_parent_class)->finalize (object);
}

static void
pn_digital_clock_class_init (PnDigitalClockClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_digital_clock_get_property;
    object_class->set_property = pn_digital_clock_set_property;
    object_class->finalize     = pn_digital_clock_finalize;

    /* Logic + intrinsic geometry stay in the core class; the cairo
     * seven-segment drawing (paint_plot + its skip-shadow / skip-zoom
     * flags) is installed by the gui tier — see
     * pn_digital_clock_gui_install() in pn-digital-clock-gui.c. */
    node_class->receive           = pn_digital_clock_receive;
    node_class->get_size          = pn_digital_clock_get_size;
    node_class->get_header_height = pn_digital_clock_get_header_height;

    node_class->class_name = "DigitalClock";
    node_class->icon       = "\xef\x80\x97";  /* fa-clock-o U+F017 */
    /* The gauge-sink amber shared with #PnCountdown, #PnDial and
     * #PnAnalogMeter: another single-value readout sink, palette-grouped
     * with them by colour so the eye spots a worksheet's metering row. */
    node_class->color      = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category   = "GUI/Displays";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_SHOW_LABELS] = g_param_spec_boolean (
            "show-labels", "Show labels",
            "Draw the HOURS / MINUTES / SECONDS captions above their digit "
            "groups.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TIMEZONE] = g_param_spec_string (
            "timezone", "Timezone",
            "Timezone the display is shown in.  \"Not Set\" defers to the "
            "abbreviation on the incoming message's \"timezone\" member "
            "(set by the Clock node); an unknown or missing abbreviation "
            "falls back to GMT.",
            PN_TZ_NOT_SET,
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
            "Colour of the HOURS / MINUTES / SECONDS captions — a soft "
            "off-white by default, like the silk-screened legend above an "
            "LED clock's window.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings dialog: two pages.  Display holds the labels
     * toggle and the timezone combo, Colours the PnColor rows.  The combo
     * starts with the "Not Set" sentinel and then the shared tz-table
     * labels; no -gui.so companion needed for the dialog itself — only the
     * painter lives in the gui tier. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Display");
        pn_settings_schema_row     (schema, "show-labels", PN_EDITOR_AUTO);
        pn_settings_schema_row     (schema, "timezone",    PN_EDITOR_COMBO);
        pn_settings_schema_choices (schema, "timezone",
                                    pn_tz_table_choices_with_not_set ());

        pn_settings_schema_tab (schema, "Colours");
        pn_settings_schema_row (schema, "background-color",     PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "segment-color",        PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "unlit-segment-color",  PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "label-color",          PN_EDITOR_AUTO);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_digital_clock_init (PnDigitalClock *self)
{
    PnNode *node = PN_NODE (self);

    self->show_labels      = TRUE;
    self->background_color     = (PnColor){ 0.04, 0.04, 0.05, 1.0 };
    self->segment_color        = (PnColor){ 0.92, 0.12, 0.08, 1.0 };
    /* Off-state ghost: the lit red dimmed to 16 %, matching the Countdown
     * node's default so the two displays look the same at rest. */
    self->unlit_segment_color  = (PnColor){ 0.1472, 0.0192, 0.0128, 1.0 };
    self->label_color          = (PnColor){ 0.82, 0.84, 0.86, 0.92 };

    self->timezone   = g_strdup (PN_TZ_NOT_SET);
    self->value      = 0;
    self->has_value  = FALSE;
    self->offset_min = 0;

    {
        PnColor amber = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, &amber);
    }
    pn_node_set_class_name (node, "DigitalClock");
    pn_node_set_icon       (node, "\xef\x80\x97");  /* fa-clock-o */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDigitalClock *
pn_digital_clock_new (void)
{
    return g_object_new (PN_TYPE_DIGITAL_CLOCK, NULL);
}

void
pn_digital_clock_set_value (PnDigitalClock *self, gdouble seconds)
{
    gint64 before, after;

    g_return_if_fail (PN_IS_DIGITAL_CLOCK (self));
    if (!isfinite (seconds))
        return;

    /* Snapshot what the display currently shows, apply the new reading,
     * then repaint only when the visible digits would actually change — a
     * once-a-second clock feed must not drive a faster redraw than the
     * seconds it counts. */
    before = display_value (self);

    self->has_value = TRUE;
    self->value     = (gint64) seconds;   /* floor toward zero */

    after = display_value (self);

    if (after != before)
        pn_node_request_repaint (PN_NODE (self));
}
