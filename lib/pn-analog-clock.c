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
/*  PnAnalogClock — logic tier (headless core).                        */
/*                                                                     */
/*  The GTK-free half of the Analogue Clock node: the GType, all        */
/*  properties (the configurable text, the timezone combo, the         */
/*  colours), the receive() that reads data.value as a count of         */
/*  seconds, the day-wrapped hours / minutes / seconds breakdown,       */
/*  geometry vfuncs and the read seam the gui tier paints from.  Logic  */
/*  is the #PnDigitalClock node's — same sink, same data feed, same     */
/*  timezone resolver — but the gui tier draws a round analogue dial    */
/*  face with three pivoted hands instead of seven-segment digits.  The */
/*  painter lives in pn-analog-clock-gui.c, which installs its          */
/*  paint_plot slot onto this class at editor startup                    */
/*  (pn_analog_clock_gui_install).  The headless runtime registers and   */
/*  runs this node without pulling GTK.                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include <json-glib/json-glib.h>

#include "pn-analog-clock.h"
#include "pn-message.h"
#include "pn-settings-schema.h"
#include "pn-tz-table.h"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Square footprint matching #PnDial: a 220-px-wide header on top of   */
/*  a 220-px-tall circular face, so the dial reads as a true circle     */
/*  regardless of device pixel density and lines up port-for-port with  */
/*  the other gauge-family nodes on the worksheet.                      */
/* ------------------------------------------------------------------ */

#define PN_ACK_WIDTH          220.0
#define PN_ACK_HEADER_HEIGHT   40.0
#define PN_ACK_GAP              4.0
#define PN_ACK_FACE_SIZE      220.0
#define PN_ACK_TOTAL_HEIGHT  (PN_ACK_HEADER_HEIGHT + PN_ACK_GAP + PN_ACK_FACE_SIZE)

/* Seconds-per-unit constants for the breakdown. */
#define PN_ACK_SECS_PER_DAY    86400
#define PN_ACK_SECS_PER_HOUR    3600
#define PN_ACK_SECS_PER_MIN       60

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnAnalogClock
{
    PnNode    parent_instance;

    /* Configuration. */
    gchar    *text;                   /* below the centre pivot */
    gboolean  show_seconds;
    PnColor   face_color;             /* dial background */
    PnColor   marker_color;           /* hour markers + minute pips */
    PnColor   hour_hand_color;
    PnColor   minute_hand_color;
    PnColor   second_hand_color;
    PnColor   text_color;
    gchar    *timezone;               /* shared tz-table label, or "Not Set" */

    /* Live state.  @value is the most-recent data.value seen, floored to
     * whole seconds; @has_value latches once the first message lands.
     * Negative @value is clamped to zero and the rest wrapped into a
     * 24-hour face so the readout always reads as a clock.  @offset_min
     * is the minutes-east-of-UTC the last message's "timezone" abbreviation
     * resolved to (relevant only when "Not Set" defers to the wire); a
     * configured zone overrides it. */
    gint64    value;
    gboolean  has_value;
    gint      offset_min;
};

G_DEFINE_FINAL_TYPE (PnAnalogClock, pn_analog_clock, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_TEXT,
    PROP_SHOW_SECONDS,
    PROP_TIMEZONE,
    PROP_FACE_COLOR,
    PROP_MARKER_COLOR,
    PROP_HOUR_HAND_COLOR,
    PROP_MINUTE_HAND_COLOR,
    PROP_SECOND_HAND_COLOR,
    PROP_TEXT_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Read the incoming seconds count off data.value.  Accepts a double or an
 * int64 (the Clock emits a double; both decode the same count).  Returns
 * FALSE when there is no numeric value to display. */
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
has_fixed_timezone (PnAnalogClock *self)
{
    return self->timezone != NULL
        && self->timezone[0] != '\0'
        && g_strcmp0 (self->timezone, PN_TZ_NOT_SET) != 0;
}

/* The integer seconds-of-day the face currently shows: the raw value
 * shifted by the active UTC offset, taken modulo a day, and zero until
 * the first message lands.  Negative wall-clock results are wrapped
 * back into [0, 86400) so the readout always reads as a clock face. */
static gint64
display_value (PnAnalogClock *self)
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

    mod = shifted % PN_ACK_SECS_PER_DAY;
    if (mod < 0)
        mod += PN_ACK_SECS_PER_DAY;
    return mod;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_analog_clock_receive (PnNode *node, PnMessage *message)
{
    PnAnalogClock *self = PN_ANALOG_CLOCK (node);
    gdouble        value;
    gint           new_offset = 0;

    if (!read_value (message, &value))
        return;   /* nothing numeric to show — leave the face as-is */

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

    pn_analog_clock_set_value (self, value);
    /* Pure sink: never forwards. */
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_analog_clock_get_paint_state (PnAnalogClock           *self,
                                 PnAnalogClockPaintState *out)
{
    gint64 v;

    g_return_if_fail (PN_IS_ANALOG_CLOCK (self));
    g_return_if_fail (out != NULL);

    v = display_value (self);

    out->hours   = (gint) (v / PN_ACK_SECS_PER_HOUR);
    out->minutes = (gint) ((v % PN_ACK_SECS_PER_HOUR) / PN_ACK_SECS_PER_MIN);
    out->seconds = (gint) (v % PN_ACK_SECS_PER_MIN);

    out->text              = self->text;
    out->show_seconds      = self->show_seconds;
    out->face_color        = self->face_color;
    out->marker_color      = self->marker_color;
    out->hour_hand_color   = self->hour_hand_color;
    out->minute_hand_color = self->minute_hand_color;
    out->second_hand_color = self->second_hand_color;
    out->text_color        = self->text_color;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_analog_clock_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_ACK_WIDTH;
    if (out_h != NULL) *out_h = PN_ACK_TOTAL_HEIGHT;
}

static double
pn_analog_clock_get_header_height (PnNode *node)
{
    (void) node;
    return PN_ACK_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_analog_clock_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    PnAnalogClock *self = PN_ANALOG_CLOCK (object);

    switch (prop_id)
    {
    case PROP_TEXT:
        g_value_set_string (value, self->text);
        break;
    case PROP_SHOW_SECONDS:
        g_value_set_boolean (value, self->show_seconds);
        break;
    case PROP_TIMEZONE:
        g_value_set_string (value, self->timezone);
        break;
    case PROP_FACE_COLOR:
        g_value_set_boxed (value, &self->face_color);
        break;
    case PROP_MARKER_COLOR:
        g_value_set_boxed (value, &self->marker_color);
        break;
    case PROP_HOUR_HAND_COLOR:
        g_value_set_boxed (value, &self->hour_hand_color);
        break;
    case PROP_MINUTE_HAND_COLOR:
        g_value_set_boxed (value, &self->minute_hand_color);
        break;
    case PROP_SECOND_HAND_COLOR:
        g_value_set_boxed (value, &self->second_hand_color);
        break;
    case PROP_TEXT_COLOR:
        g_value_set_boxed (value, &self->text_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_color_prop (PnAnalogClock *self, PnColor *slot,
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
pn_analog_clock_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    PnAnalogClock *self = PN_ANALOG_CLOCK (object);

    switch (prop_id)
    {
    case PROP_TEXT:
    {
        const gchar *v          = g_value_get_string (value);
        const gchar *normalised = v != NULL ? v : "";
        if (g_strcmp0 (normalised, self->text) != 0)
        {
            g_free (self->text);
            self->text = g_strdup (normalised);
            g_object_notify_by_pspec (object, props[PROP_TEXT]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_SHOW_SECONDS:
    {
        gboolean v = g_value_get_boolean (value);
        if (v != self->show_seconds)
        {
            self->show_seconds = v;
            g_object_notify_by_pspec (object, props[PROP_SHOW_SECONDS]);
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
    case PROP_FACE_COLOR:
        set_color_prop (self, &self->face_color, value, PROP_FACE_COLOR);
        break;
    case PROP_MARKER_COLOR:
        set_color_prop (self, &self->marker_color, value, PROP_MARKER_COLOR);
        break;
    case PROP_HOUR_HAND_COLOR:
        set_color_prop (self, &self->hour_hand_color, value,
                        PROP_HOUR_HAND_COLOR);
        break;
    case PROP_MINUTE_HAND_COLOR:
        set_color_prop (self, &self->minute_hand_color, value,
                        PROP_MINUTE_HAND_COLOR);
        break;
    case PROP_SECOND_HAND_COLOR:
        set_color_prop (self, &self->second_hand_color, value,
                        PROP_SECOND_HAND_COLOR);
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
pn_analog_clock_finalize (GObject *object)
{
    PnAnalogClock *self = PN_ANALOG_CLOCK (object);

    g_free (self->text);
    g_free (self->timezone);

    G_OBJECT_CLASS (pn_analog_clock_parent_class)->finalize (object);
}

static void
pn_analog_clock_class_init (PnAnalogClockClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_analog_clock_get_property;
    object_class->set_property = pn_analog_clock_set_property;
    object_class->finalize     = pn_analog_clock_finalize;

    /* Logic + intrinsic geometry stay in the core class; the cairo
     * face/hands drawing (paint_plot + its skip-shadow flag) is
     * installed by the gui tier — see pn_analog_clock_gui_install()
     * in pn-analog-clock-gui.c. */
    node_class->receive           = pn_analog_clock_receive;
    node_class->get_size          = pn_analog_clock_get_size;
    node_class->get_header_height = pn_analog_clock_get_header_height;

    node_class->class_name = "AnalogClock";
    node_class->icon       = "\xef\x80\x97";  /* fa-clock-o U+F017 */
    /* The gauge-sink amber shared with #PnDial, #PnAnalogMeter,
     * #PnCountdown and #PnDigitalClock: single-value readout sinks
     * palette-grouped by colour so the eye spots a worksheet's metering
     * row. */
    node_class->color      = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category   = "GUI/Gauges";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_TEXT] = g_param_spec_string (
            "text", "Text",
            "Short caption drawn below the centre pivot in place of the "
            "manufacturer logo a real wall clock carries.  Empty to "
            "suppress.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SHOW_SECONDS] = g_param_spec_boolean (
            "show-seconds", "Show seconds hand",
            "Draw the thin seconds hand.  Switch off for a quieter "
            "two-hand face that only updates once a minute.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TIMEZONE] = g_param_spec_string (
            "timezone", "Timezone",
            "Timezone the face is shown in.  \"Not Set\" defers to the "
            "abbreviation on the incoming message's \"timezone\" member "
            "(set by the Clock node); an unknown or missing abbreviation "
            "falls back to GMT.",
            PN_TZ_NOT_SET,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FACE_COLOR] = g_param_spec_boxed (
            "face-color", "Face colour",
            "Fill colour of the dial face — the white face of a wall "
            "clock by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MARKER_COLOR] = g_param_spec_boxed (
            "marker-color", "Marker colour",
            "Colour of the twelve hour markers and the sixty minute "
            "pips around the rim — black by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HOUR_HAND_COLOR] = g_param_spec_boxed (
            "hour-hand-color", "Hour hand colour",
            "Fill colour of the short, broad hour hand.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MINUTE_HAND_COLOR] = g_param_spec_boxed (
            "minute-hand-color", "Minute hand colour",
            "Fill colour of the long, narrower minute hand.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SECOND_HAND_COLOR] = g_param_spec_boxed (
            "second-hand-color", "Second hand colour",
            "Stroke colour of the thin seconds hand — the classic "
            "bright red of a wall clock by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the configurable caption below the pivot.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings dialog: two pages.  Display holds the text +
     * seconds toggle + timezone combo, Colours the PnColor rows.  The
     * combo starts with the "Not Set" sentinel and then the shared
     * tz-table labels; no -gui.so companion needed for the dialog itself
     * — only the painter lives in the gui tier. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Display");
        pn_settings_schema_row     (schema, "text",         PN_EDITOR_AUTO);
        pn_settings_schema_row     (schema, "show-seconds", PN_EDITOR_AUTO);
        pn_settings_schema_row     (schema, "timezone",     PN_EDITOR_COMBO);
        pn_settings_schema_choices (schema, "timezone",
                                    pn_tz_table_choices_with_not_set ());

        pn_settings_schema_tab (schema, "Colours");
        pn_settings_schema_row (schema, "face-color",        PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "marker-color",      PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "hour-hand-color",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "minute-hand-color", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "second-hand-color", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "text-color",        PN_EDITOR_AUTO);

        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_analog_clock_init (PnAnalogClock *self)
{
    PnNode *node = PN_NODE (self);

    self->text               = g_strdup ("");
    self->show_seconds       = TRUE;
    /* Defaults chosen for a classic white wall clock with black markers,
     * black hour/minute hands and a bright-red seconds hand. */
    self->face_color         = (PnColor){ 0.97, 0.97, 0.96, 1.0 };
    self->marker_color       = (PnColor){ 0.08, 0.08, 0.09, 1.0 };
    self->hour_hand_color    = (PnColor){ 0.08, 0.08, 0.09, 1.0 };
    self->minute_hand_color  = (PnColor){ 0.08, 0.08, 0.09, 1.0 };
    self->second_hand_color  = (PnColor){ 0.85, 0.12, 0.10, 1.0 };
    self->text_color         = (PnColor){ 0.20, 0.20, 0.22, 1.0 };

    self->timezone   = g_strdup (PN_TZ_NOT_SET);
    self->value      = 0;
    self->has_value  = FALSE;
    self->offset_min = 0;

    {
        PnColor amber = { 0.92, 0.76, 0.27, 1.0 };
        pn_node_set_color (node, &amber);
    }
    pn_node_set_class_name (node, "AnalogClock");
    pn_node_set_icon       (node, "\xef\x80\x97");  /* fa-clock-o */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnAnalogClock *
pn_analog_clock_new (void)
{
    return g_object_new (PN_TYPE_ANALOG_CLOCK, NULL);
}

void
pn_analog_clock_set_value (PnAnalogClock *self, gdouble seconds)
{
    gint64 before, after;

    g_return_if_fail (PN_IS_ANALOG_CLOCK (self));
    if (!isfinite (seconds))
        return;

    /* Snapshot what the face currently shows, apply the new reading,
     * then repaint only when the visible hands would actually change —
     * a once-a-second clock feed must not drive a faster redraw than
     * the seconds it counts. */
    before = display_value (self);

    self->has_value = TRUE;
    self->value     = (gint64) seconds;   /* floor toward zero */

    after = display_value (self);

    if (after != before)
        pn_node_request_repaint (PN_NODE (self));
}
