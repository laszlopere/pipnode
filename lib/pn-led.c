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
/*  PnLed — logic tier (headless core).                                */
/*                                                                     */
/*  This file holds the GTK-free half of the LED node: the GType, its  */
/*  properties, the receive() contract (Flash hold-timer / Steady      */
/*  level latch), and the read seams the GUI tier paints from.  The    */
/*  cairo drawing and the settings-dialog customisation live in the    */
/*  companion gui-tier file pn-led-gui.c, which installs those vfunc    */
/*  slots onto this class at editor startup (see pn_led_gui_install).   */
/*  The headless runtime registers and runs this node without ever     */
/*  pulling GTK.                                                        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-led.h"
#include "pn-message.h"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Standard Node-RED rectangle (40 px header tall).  The right-edge   */
/*  inset where the GUI tier paints the LED disc lives with the        */
/*  painter in pn-led.c's gui companion; here we only need the node's  */
/*  intrinsic size for headless layout (get_size).                     */
/* ------------------------------------------------------------------ */

#define PN_LED_NODE_WIDTH        180.0
#define PN_LED_NODE_HEIGHT        40.0

/* ------------------------------------------------------------------ */
/*  PnLed instance                                                     */
/* ------------------------------------------------------------------ */

struct _PnLed
{
    PnNode parent_instance;

    /* User-configurable.  @color is the colour the LED shows while
     * lit; @hold_ms is the minimum time the LED stays lit after the
     * most-recent message.  Anything below 100 ms is clamped at the
     * property layer so a one-shot flash is still visible on a 60 Hz
     * display. */
    PnColor   color;
    guint     hold_ms;
    PnLedMode mode;

    /* Live state.  @lit is TRUE between the most-recent message and
     * the hold timer firing; @off_timeout_id is the g_timeout source
     * scheduled to turn the LED off when the hold period elapses
     * (0 when the LED is off, or when no message has arrived yet). */
    gboolean lit;
    guint    off_timeout_id;
};

G_DEFINE_TYPE (PnLed, pn_led, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_COLOR,
    PROP_HOLD_MS,
    PROP_MODE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Mode enum GType                                                     */
/* ------------------------------------------------------------------ */

GType
pn_led_mode_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_LED_MODE_FLASH,  "PN_LED_MODE_FLASH",  "Flash"          },
            { PN_LED_MODE_STEADY, "PN_LED_MODE_STEADY", "Steady (level)" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnLedMode", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* The smallest hold period the user can configure.  Anything shorter
 * than this would be invisible on a 60 Hz display (one frame is ~16 ms
 * and a paint round-trip can land on the next frame), so a 100 ms
 * floor guarantees at least a handful of frames of "lit" visibility
 * for a single one-shot message.                                      */
#define PN_LED_MIN_HOLD_MS     100u
#define PN_LED_DEFAULT_HOLD_MS 250u

/* ------------------------------------------------------------------ */
/*  Off timer                                                          */
/* ------------------------------------------------------------------ */

static gboolean
on_off_timeout (gpointer user_data)
{
    PnLed *self = user_data;

    self->off_timeout_id = 0;
    self->lit            = FALSE;
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** Pull a number out of @message under "value".  Returns %TRUE only
 *  when the member exists and holds a numeric JSON value (int or
 *  double); @out is left untouched otherwise.  Matches the reader
 *  PnThreshold / PnComparator use so the boolean-on-value contract is
 *  interpreted identically across the Filters and Sinks nodes. */
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
    return TRUE;
}

static void
pn_led_receive (PnNode *node, PnMessage *message)
{
    PnLed   *self    = PN_LED (node);
    gboolean was_lit = self->lit;

    if (self->mode == PN_LED_MODE_STEADY)
    {
        gdouble value;

        /* State lamp: latch to the boolean on data.value -- on above
         * the 0.5 midpoint, off at or below it.  A message that carries
         * no numeric value leaves the lamp where it is; a state lamp
         * only moves when handed a new state.  No timer is involved, so
         * the lamp holds its level until the next value arrives.        */
        if (!read_value (message, &value))
            return;

        self->lit = (value > 0.5);
        if (self->lit != was_lit)
            pn_node_request_repaint (PN_NODE (self));
        return;
    }

    /* Flash mode: reschedule the off-timer so the LED stays lit for the
     * full hold period from the most-recent message, not from the
     * first.                                                            */
    (void) message;

    if (self->off_timeout_id != 0)
    {
        g_source_remove (self->off_timeout_id);
        self->off_timeout_id = 0;
    }
    self->off_timeout_id =
            g_timeout_add (self->hold_ms, on_off_timeout, self);

    self->lit = TRUE;

    /* No need to thrash the worksheet with a repaint per message when
     * we are already lit -- the visual state has not changed and the
     * off-timer reschedule above already handled the "extended hold"
     * bookkeeping. */
    if (!was_lit)
        pn_node_request_repaint (PN_NODE (self));
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_led_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_LED_NODE_WIDTH;
    if (out_h != NULL) *out_h = PN_LED_NODE_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  GUI read seams                                                     */
/*                                                                     */
/*  The cairo painter lives in the gui tier (pn-led-gui.c) and cannot  */
/*  see this file's private instance struct, so it reads the lit flag  */
/*  and lit colour through these GTK-free accessors.                   */
/* ------------------------------------------------------------------ */

gboolean
pn_led_get_lit (PnLed *self)
{
    g_return_val_if_fail (PN_IS_LED (self), FALSE);
    return self->lit;
}

void
pn_led_peek_color (PnLed *self, PnColor *out)
{
    g_return_if_fail (PN_IS_LED (self));
    g_return_if_fail (out != NULL);
    *out = self->color;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_led_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnLed *self = PN_LED (object);

    switch (prop_id)
    {
    case PROP_COLOR:   g_value_set_boxed (value, &self->color);  break;
    case PROP_HOLD_MS: g_value_set_uint  (value, self->hold_ms); break;
    case PROP_MODE:    g_value_set_enum  (value, self->mode);    break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_led_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnLed *self = PN_LED (object);

    switch (prop_id)
    {
    case PROP_COLOR:
    {
        const PnColor *new_color = g_value_get_boxed (value);
        if (new_color == NULL || pn_color_equal (&self->color, new_color))
            return;
        self->color = *new_color;
        g_object_notify_by_pspec (object, props[PROP_COLOR]);
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_HOLD_MS:
    {
        guint new_hold = g_value_get_uint (value);
        if (new_hold < PN_LED_MIN_HOLD_MS) new_hold = PN_LED_MIN_HOLD_MS;
        if (self->hold_ms == new_hold)
            return;
        self->hold_ms = new_hold;
        g_object_notify_by_pspec (object, props[PROP_HOLD_MS]);
        break;
    }
    case PROP_MODE:
    {
        PnLedMode new_mode = g_value_get_enum (value);
        if (self->mode == new_mode)
            return;
        self->mode = new_mode;

        /* Switching modes starts from a clean, dark lamp: cancel any
         * pending flash off-timer and drop the lit state so the lamp
         * reflects the new mode's next input rather than a stale blink
         * left over from the other mode. */
        if (self->off_timeout_id != 0)
        {
            g_source_remove (self->off_timeout_id);
            self->off_timeout_id = 0;
        }
        self->lit = FALSE;

        g_object_notify_by_pspec (object, props[PROP_MODE]);
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_led_finalize (GObject *object)
{
    PnLed *self = PN_LED (object);

    if (self->off_timeout_id != 0)
    {
        g_source_remove (self->off_timeout_id);
        self->off_timeout_id = 0;
    }

    G_OBJECT_CLASS (pn_led_parent_class)->finalize (object);
}

static void
pn_led_class_init (PnLedClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_led_get_property;
    object_class->set_property = pn_led_set_property;
    object_class->finalize     = pn_led_finalize;

    /* Logic + intrinsic geometry stay in the core class.  The cairo
     * decoration (paint_header_overlay), its reserved label margin
     * (paint_right_decoration_width) and the settings-dialog editor
     * (build_property_editor) are installed by the gui tier — see
     * pn_led_gui_install() in pn-led-gui.c. */
    node_class->receive  = pn_led_receive;
    node_class->get_size = pn_led_get_size;

    node_class->class_name = "LED";
    /* fa-lightbulb-o U+F0EB -- a glyph that is in the bundled
     * FontAwesome subset and reads as a small status indicator. */
    node_class->icon       = "\xef\x83\xab";
    node_class->color      = (PnColor){ 0.40, 0.55, 0.70, 1.0 };
    node_class->category   = "Sinks";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_COLOR] = g_param_spec_boxed (
            "color", "LED colour",
            "Colour the LED face glows in while lit.  The unlit face is "
            "always a neutral grey -- only the lit state uses this colour.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HOLD_MS] = g_param_spec_uint (
            "hold-ms", "Hold (ms)",
            "Minimum time in milliseconds the LED stays lit after the "
            "most-recent message.  The timer resets on every incoming "
            "message so a steady stream keeps the LED lit continuously.  "
            "Floor of 100 ms so a one-shot message is still visible on a "
            "60 Hz display.",
            PN_LED_MIN_HOLD_MS, 60u * 60u * 1000u, PN_LED_DEFAULT_HOLD_MS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MODE] = g_param_spec_enum (
            "mode", "Mode",
            "How the lamp reacts to incoming messages.  Flash: every "
            "message lights the lamp for the Hold period, a momentary "
            "activity blink (the default).  Steady (level): the lamp "
            "latches to the boolean on data.value -- lit while value > "
            "0.5, dark otherwise -- so it reads as a permanent state "
            "lamp.  The Hold (ms) period is ignored in Steady mode.",
            PN_TYPE_LED_MODE, PN_LED_MODE_FLASH,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_led_init (PnLed *self)
{
    PnNode *node = PN_NODE (self);

    /* Default lit colour: a bright green, the canonical "OK / activity"
     * indicator on real-world status panels. */
    self->color   = (PnColor){ 0.20, 0.85, 0.30, 1.0 };
    self->hold_ms = PN_LED_DEFAULT_HOLD_MS;
    self->mode    = PN_LED_MODE_FLASH;
    self->lit            = FALSE;
    self->off_timeout_id = 0;

    pn_node_set_class_name (node, "LED");
    pn_node_set_icon       (node, "\xef\x83\xab");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
    {
        PnColor body = { 0.40, 0.55, 0.70, 1.0 };
        pn_node_set_color (node, &body);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnLed *
pn_led_new (void)
{
    return g_object_new (PN_TYPE_LED, NULL);
}
