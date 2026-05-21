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

#include "pn-led.h"
#include "pn-message.h"

#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Standard Node-RED rectangle (40 px header tall) widened on the     */
/*  right so a small rectangular LED indicator fits inside the body    */
/*  without crowding the label area.  The LED itself is drawn by      */
/*  paint_header_overlay, which clips to the body's rounded rect so   */
/*  the LED respects the corner radius on the right edge.              */
/* ------------------------------------------------------------------ */

#define PN_LED_NODE_WIDTH        180.0
#define PN_LED_NODE_HEIGHT        40.0

/* Inset of the LED disc inside the right portion of the header.  The
 * LED is circular so its diameter is fixed at PN_LED_DIAMETER; the
 * disc is centred vertically in the header and sits PN_LED_RIGHT_PAD
 * pixels clear of the right edge. */
#define PN_LED_RIGHT_PAD          12.0     /* clear of right edge */
#define PN_LED_DIAMETER           24.0     /* outer bracket diameter */

/* The label painter is told to leave the rightmost slice of the     */
/* header alone -- LED diameter + right pad + a small breathing gap  */
/* so the centred label cannot bump into the LED bracket.            */
#define PN_LED_RESERVED_RIGHT    (PN_LED_DIAMETER + PN_LED_RIGHT_PAD + 6.0)

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
    GdkRGBA  color;
    guint    hold_ms;

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
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

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

static void
pn_led_receive (PnNode *node, PnMessage *message)
{
    PnLed   *self    = PN_LED (node);
    gboolean was_lit = self->lit;

    (void) message;

    /* Reschedule the off-timer so the LED stays lit for the full hold
     * period from the most-recent message, not from the first.        */
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
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Paint a circular through-hole LED sitting in a black panel-mount
 *  bracket -- the kind a real 5 mm indicator LED clips into on the
 *  front of a piece of equipment.  The whole assembly is concentric
 *  on (@cx, @cy) with overall radius @outer_r:
 *
 *    - When lit, a soft halo of the LED's colour spills out around
 *      the bracket first so the bracket overpaints (and softly trims)
 *      its outer edge.
 *    - The bracket is a thick black ring with a tiny lighter highlight
 *      on the upper-left lip so it reads as a chunky moulded bezel
 *      rather than a flat ring of paint.
 *    - The dome inside is a coloured circle with a radial gradient
 *      from a bright centre to a darker rim (user colour when lit,
 *      two greys when off).  A thin dark hairline just inside the
 *      bracket sells the "recessed into the bracket" look.
 *    - A small bright catch-light in the upper-left of the dome adds
 *      the obligatory glass highlight every real LED window picks up.
 */
static void
paint_led (
        cairo_t       *cr,
        double         cx,
        double         cy,
        double         outer_r,
        gboolean       lit,
        const GdkRGBA *color)
{
    /* Bracket takes the outer ~22 % of the radius -- thick enough to
     * read as a moulded mounting collar at the small sizes we paint
     * at, thin enough that the coloured dome still dominates the
     * read. */
    const double     bracket_r = outer_r;
    const double     dome_r    = outer_r * 0.78;
    cairo_pattern_t *grad;

    /* Halo: only when lit.  A soft radial gradient centred on the LED
     * with the user's colour at the centre fading to transparent at
     * about 2 r, painted before the bracket so the bracket sits on
     * top of (and softly trims) the halo's outer edge.               */
    if (lit)
    {
        const double halo_r = outer_r * 2.1;

        grad = cairo_pattern_create_radial (cx, cy, outer_r * 0.7,
                                            cx, cy, halo_r);
        cairo_pattern_add_color_stop_rgba (grad, 0.0,
                                           color->red,
                                           color->green,
                                           color->blue,
                                           0.55);
        cairo_pattern_add_color_stop_rgba (grad, 1.0,
                                           color->red,
                                           color->green,
                                           color->blue,
                                           0.0);
        cairo_set_source (cr, grad);
        cairo_rectangle (cr, cx - halo_r, cy - halo_r,
                         halo_r * 2.0, halo_r * 2.0);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);
    }

    /* Bracket body: a black disc filling the full outer radius.  The
     * dome painted on top of this leaves the outer ring of the disc
     * visible as the bracket's mounting collar.                      */
    cairo_set_source_rgb (cr, 0.05, 0.05, 0.06);
    cairo_arc (cr, cx, cy, bracket_r, 0.0, 2.0 * M_PI);
    cairo_fill (cr);

    /* Bracket highlight: a thin lighter arc on the upper-left rim of
     * the bracket so the bezel reads as a chamfered lip catching
     * overhead light.  A linear gradient from a mid-grey at the top
     * to near-black at the bottom, stroked along the outer edge,
     * gives the bracket its sense of thickness without an explicit
     * second concentric disc. */
    grad = cairo_pattern_create_linear (cx, cy - bracket_r,
                                        cx, cy + bracket_r);
    cairo_pattern_add_color_stop_rgb (grad, 0.0, 0.40, 0.40, 0.42);
    cairo_pattern_add_color_stop_rgb (grad, 0.5, 0.12, 0.12, 0.13);
    cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.22, 0.22, 0.24);
    cairo_set_source (cr, grad);
    cairo_set_line_width (cr, 1.2);
    cairo_arc (cr, cx, cy, bracket_r - 0.6, 0.0, 2.0 * M_PI);
    cairo_stroke (cr);
    cairo_pattern_destroy (grad);

    /* Dome: a coloured radial gradient inside the bracket.  Painted
     * as an arc fill so the boundary against the black bracket is a
     * crisp circular edge (a radial-gradient pattern applied to a
     * rectangle would still leave the rectangle outside the dome
     * showing on top of the bracket). */
    {
        double cr_r, cg, cb;
        double or_r, og, ob;

        if (lit)
        {
            /* Lit: bright user colour at the centre, darker ring at
             * the rim -- the standard "shiny LED" read. */
            cr_r = color->red   * 0.40 + 0.60;
            cg   = color->green * 0.40 + 0.60;
            cb   = color->blue  * 0.40 + 0.60;
            or_r = color->red   * 0.85;
            og   = color->green * 0.85;
            ob   = color->blue  * 0.85;
        }
        else
        {
            /* Off: two greys, lighter centre to darker rim, reading
             * as a translucent dome with no light behind it. */
            cr_r = 0.72; cg = 0.72; cb = 0.74;
            or_r = 0.30; og = 0.30; ob = 0.32;
        }

        grad = cairo_pattern_create_radial (cx - dome_r * 0.20,
                                            cy - dome_r * 0.20,
                                            dome_r * 0.05,
                                            cx, cy, dome_r);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, cr_r, cg, cb);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, or_r, og, ob);
        cairo_set_source (cr, grad);
        cairo_arc (cr, cx, cy, dome_r, 0.0, 2.0 * M_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);
    }

    /* Hairline shadow where the dome meets the bracket -- a thin
     * stroke just inside the dome's edge, in translucent black, sells
     * the "dome recessed into the bracket" read by suggesting the
     * tiny dark gap a real moulded part has at that seam. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
    cairo_set_line_width (cr, 0.8);
    cairo_arc (cr, cx, cy, dome_r - 0.4, 0.0, 2.0 * M_PI);
    cairo_stroke (cr);

    /* Small bright catch-light in the upper-left of the dome -- sells
     * the 3D read even when the LED is off, since real LED windows
     * always pick up an ambient highlight on the glass.  Clipped to
     * the dome so it does not bleed onto the bracket. */
    {
        const double hl_r = dome_r * 0.55;

        cairo_save (cr);
        cairo_arc (cr, cx, cy, dome_r, 0.0, 2.0 * M_PI);
        cairo_clip (cr);

        grad = cairo_pattern_create_radial (cx - dome_r * 0.30,
                                            cy - dome_r * 0.30,
                                            0.0,
                                            cx - dome_r * 0.30,
                                            cy - dome_r * 0.30,
                                            hl_r);
        cairo_pattern_add_color_stop_rgba (grad, 0.0, 1.0, 1.0, 1.0,
                                           lit ? 0.65 : 0.45);
        cairo_pattern_add_color_stop_rgba (grad, 1.0, 1.0, 1.0, 1.0, 0.0);
        cairo_set_source (cr, grad);
        cairo_arc (cr, cx, cy, dome_r, 0.0, 2.0 * M_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);

        cairo_restore (cr);
    }
}

static void
pn_led_paint_header_overlay (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnLed       *self = PN_LED (node);
    const double r    = PN_LED_DIAMETER * 0.5;
    const double cx   = x + w - PN_LED_RIGHT_PAD - r;
    const double cy   = y + h * 0.5;

    paint_led (cr, cx, cy, r, self->lit, &self->color);
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
        const GdkRGBA *new_color = g_value_get_boxed (value);
        if (new_color == NULL || gdk_rgba_equal (&self->color, new_color))
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

    node_class->receive              = pn_led_receive;
    node_class->get_size             = pn_led_get_size;
    node_class->paint_header_overlay = pn_led_paint_header_overlay;
    node_class->paint_right_decoration_width = PN_LED_RESERVED_RIGHT;

    node_class->class_name = "LED";
    /* fa-lightbulb-o U+F0EB -- a glyph that is in the bundled
     * FontAwesome subset and reads as a small status indicator. */
    node_class->icon       = "\xef\x83\xab";
    node_class->color      = (GdkRGBA){ 0.40, 0.55, 0.70, 1.0 };
    node_class->category   = "Sinks";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_COLOR] = g_param_spec_boxed (
            "color", "LED colour",
            "Colour the LED face glows in while lit.  The unlit face is "
            "always a neutral grey -- only the lit state uses this colour.",
            GDK_TYPE_RGBA,
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

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_led_init (PnLed *self)
{
    PnNode *node = PN_NODE (self);

    /* Default lit colour: a bright green, the canonical "OK / activity"
     * indicator on real-world status panels. */
    self->color   = (GdkRGBA){ 0.20, 0.85, 0.30, 1.0 };
    self->hold_ms = PN_LED_DEFAULT_HOLD_MS;
    self->lit            = FALSE;
    self->off_timeout_id = 0;

    pn_node_set_class_name (node, "LED");
    pn_node_set_icon       (node, "\xef\x83\xab");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
    {
        GdkRGBA body = { 0.40, 0.55, 0.70, 1.0 };
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
