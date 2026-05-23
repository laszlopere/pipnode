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

#include "pn-switch.h"
#include <gtk/gtk.h>
#include "pn-message.h"

#include <json-glib/json-glib.h>

/* fa-toggle-on U+F205 -- pinned (not flipped between on/off variants)
 * because the slider widget in the header already reports the latch
 * position visually; flipping the side glyph too would be redundant
 * and would also pull the palette / inspector / icon panel out of
 * sync with each other depending on which instance happened to be
 * "on" at the moment they were sampled. */
#define PN_SWITCH_ICON  "\xef\x88\x85"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  The switch decoration sits on the right edge of the node header,  */
/*  mirroring PnLed's right-decoration approach so the standard       */
/*  Node-RED header (icon panel + centred label) stays untouched and  */
/*  the input/output ports keep their usual positions on left/right.  */
/* ------------------------------------------------------------------ */

#define PN_SWITCH_NODE_WIDTH      170.0
#define PN_SWITCH_NODE_HEIGHT      40.0

#define PN_SWITCH_RIGHT_PAD        10.0
#define PN_SWITCH_TRACK_WIDTH      38.0
#define PN_SWITCH_TRACK_HEIGHT     20.0

#define PN_SWITCH_RESERVED_RIGHT  (PN_SWITCH_TRACK_WIDTH + PN_SWITCH_RIGHT_PAD + 6.0)

typedef struct
{
    /* Latched switch position.  TRUE = on (emits / passes value=1.0),
     * FALSE = off (value=0.0).  Mirrors the most recently observed
     * "value" -- flipped by a click on the switch, or by an incoming
     * message whose value crosses the midpoint. */
    gboolean on;

    /* Startup announce.  @announce_on_startup gates whether the switch
     * emits its latch state once shortly after construction (default
     * TRUE; subclasses with an outward side effect clear it via
     * pn_switch_set_announce_on_startup()).  @startup_emit_id is the
     * main-loop source id of that one-shot idle, 0 once it has fired or
     * been cancelled, tracked so dispose() can pull it if the node dies
     * before the idle runs. */
    gboolean announce_on_startup;
    guint    startup_emit_id;
} PnSwitchPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PnSwitch, pn_switch, PN_TYPE_NODE)

#define PRIV(self) ((PnSwitchPrivate *) pn_switch_get_instance_private (self))

enum {
    PROP_0,
    PROP_ON,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Default vfunc implementations                                      */
/*                                                                     */
/*  The base behaviour lives in `pn_switch_real_*` so a subclass       */
/*  override that wants to extend (rather than replace) the default    */
/*  can call up to it from the header-exported wrappers.               */
/* ------------------------------------------------------------------ */

void
pn_switch_real_apply_visual_state (PnSwitch *self)
{
    /* The slider widget itself (painted by pn_switch_paint_header_overlay)
     * is the authoritative state indicator -- the side glyph stays
     * pinned regardless of latch position.  All this default needs to
     * do is invalidate so the slider redraws against the new on/off
     * state; subclasses override to also flip the body colour. */
    pn_node_request_repaint (PN_NODE (self));
}

PnMessage *
pn_switch_real_build_outbound_message (PnSwitch *self)
{
    PnNode    *node = PN_NODE (self);
    PnMessage *msg  = pn_message_new (node, NULL);

    pn_message_set_double  (msg, "value",   PRIV (self)->on ? 1.0 : 0.0);
    pn_message_set_boolean (msg, "success", PRIV (self)->on);

    return msg;
}

/* ------------------------------------------------------------------ */
/*  Vfunc dispatchers                                                  */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (PnSwitch *self)
{
    PN_SWITCH_GET_CLASS (self)->apply_visual_state (self);
}

static void
emit_state_message (PnSwitch *self)
{
    PnNode    *node = PN_NODE (self);
    PnMessage *msg  = PN_SWITCH_GET_CLASS (self)->build_outbound_message (self);

    pn_node_emit_message (node, msg);
    g_object_unref (msg);
}

/* ------------------------------------------------------------------ */
/*  Receive -- passthrough with edge-trigger on "value"                */
/* ------------------------------------------------------------------ */

/** Pull a numeric "value" out of @data.  Accepts integers and doubles;
 *  reports FALSE for missing / non-numeric members so the receiver can
 *  decide whether to ignore the message or forward it as-is. */
static gboolean
read_value_member (
        JsonObject *data,
        gdouble    *out_value)
{
    JsonNode *n;
    GType     t;

    if (data == NULL || !json_object_has_member (data, "value"))
        return FALSE;

    n = json_object_get_member (data, "value");
    if (!JSON_NODE_HOLDS_VALUE (n))
        return FALSE;

    t = json_node_get_value_type (n);
    if (t == G_TYPE_DOUBLE)
        *out_value = json_node_get_double (n);
    else if (t == G_TYPE_INT64)
        *out_value = (gdouble) json_node_get_int (n);
    else
        return FALSE;

    return TRUE;
}

static void
pn_switch_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnSwitch        *self = PN_SWITCH (node);
    PnSwitchPrivate *priv = PRIV (self);
    gdouble          value;
    gboolean         has_value;
    gboolean         want_on = FALSE;

    has_value = read_value_member (pn_message_get_data (message), &value);

    /* The whole point of a switch is to be a *latch*: when the inbound
     * "value" matches the position we are already in, the message is
     * dropped on the floor without forwarding.  Otherwise a closed
     * loop -- physical relay -> MQTT Source -> Tasmota Relay Status ->
     * PnSwitch -> Tasmota Relay Command -> MQTT Sink -> physical
     * relay -- where every node faithfully passes the state along
     * would echo the same value around the loop forever (we turn off
     * the light, which tells us the light is off, which we publish to
     * turn the light off, ...).  Dropping the no-op edge breaks that
     * cycle at the one node whose job is to *represent* a single
     * stable state, which is also the natural place to break it
     * because the user can read the current latch off the slider and
     * does not need a stream of confirmations to convince them
     * nothing changed.
     *
     * Messages without a usable "value" cannot drive the cycle (there
     * is nothing for the downstream relay node to interpret) and may
     * carry auxiliary annotations the user expects to flow through,
     * so they pass through unchanged with the latch left alone. */
    if (has_value)
    {
        want_on = (value > 0.5);
        if (want_on == priv->on)
            return;

        priv->on = want_on;
        apply_visual_state (self);
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ON]);
    }

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

/** Trace a horizontal pill (rounded-end rectangle) so the switch
 *  track and thumb share a consistent silhouette. */
static void
pill_path (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    const double r = h * 0.5;

    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r, r, -G_PI_2,  G_PI_2);
    cairo_arc (cr, x + r,     y + r, r,  G_PI_2,  3.0 * G_PI_2);
    cairo_close_path (cr);
}

/** Paint a slide switch -- a rounded "track" with a circular "thumb"
 *  parked at one end.  On state shows the thumb on the right with a
 *  green track; off state parks the thumb on the left with a dark
 *  grey track.  Painted concentric on (@cx, @cy) with the configured
 *  width / height. */
static void
paint_switch (
        cairo_t *cr,
        double   cx,
        double   cy,
        double   w,
        double   h,
        gboolean on)
{
    const double tx = cx - w * 0.5;
    const double ty = cy - h * 0.5;
    const double thumb_r = h * 0.5 - 2.0;
    const double thumb_cx = on ? (tx + w - h * 0.5)
                               : (tx + h * 0.5);

    /* Track fill.  Green when on (canonical "switch engaged"), neutral
     * dark grey when off so the on-state pops against the surrounding
     * teal body colour. */
    pill_path (cr, tx, ty, w, h);
    if (on)
        cairo_set_source_rgb (cr, 0.25, 0.72, 0.35);
    else
        cairo_set_source_rgb (cr, 0.28, 0.30, 0.32);
    cairo_fill_preserve (cr);

    /* Track outline -- a thin dark ring so the pill reads as a real
     * moulded part rather than a flat coloured patch. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.55);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    /* Inset highlight on the upper rim sells the chamfered bevel the
     * surrounding node body uses, so the switch reads as built into
     * the panel rather than glued on top. */
    cairo_save (cr);
    pill_path (cr, tx, ty, w, h);
    cairo_clip (cr);
    pill_path (cr, tx + 1.0, ty + 1.0, w - 2.0, h - 2.0);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* Thumb -- a near-white disc, lit from the upper-left with a
     * subtle radial gradient so it reads as a physical knob.  A thin
     * dark hairline just inside the rim seals it against the track. */
    {
        cairo_pattern_t *grad = cairo_pattern_create_radial (
                thumb_cx - thumb_r * 0.30,
                cy        - thumb_r * 0.30,
                thumb_r * 0.10,
                thumb_cx, cy, thumb_r);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, 1.00, 1.00, 1.00);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.80, 0.80, 0.82);
        cairo_set_source (cr, grad);
        cairo_arc (cr, thumb_cx, cy, thumb_r, 0.0, 2.0 * G_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);

        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
        cairo_set_line_width (cr, 0.8);
        cairo_arc (cr, thumb_cx, cy, thumb_r - 0.4, 0.0, 2.0 * G_PI);
        cairo_stroke (cr);
    }
}

static void
pn_switch_paint_header_overlay (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnSwitch    *self = PN_SWITCH (node);
    const double sw   = PN_SWITCH_TRACK_WIDTH;
    const double sh   = PN_SWITCH_TRACK_HEIGHT;
    const double cx   = x + w - PN_SWITCH_RIGHT_PAD - sw * 0.5;
    const double cy   = y + h * 0.5;

    paint_switch (cr, cx, cy, sw, sh, PRIV (self)->on);
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_switch_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_SWITCH_NODE_WIDTH;
    if (out_h != NULL) *out_h = PN_SWITCH_NODE_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Hit-test against the switch decoration                             */
/*                                                                     */
/*  Exposed via a public helper so the worksheet can route a click on  */
/*  the slider straight into pn_switch_toggle() without having to      */
/*  duplicate the decoration geometry.  Mirrors PnChat's               */
/*  pn_chat_hit_input_in_rect() shape: the worksheet hands the node's  */
/*  full footprint rect, the node reports whether the local (px, py)   */
/*  landed on the slider.                                              */
/* ------------------------------------------------------------------ */

gboolean
pn_switch_hit_slider (
        PnSwitch *self,
        double    px,
        double    py)
{
    PnNode       *node;
    const PnPoint *pos;
    double        w;
    double        h;
    double        cx;
    double        cy;
    double        sw = PN_SWITCH_TRACK_WIDTH;
    double        sh = PN_SWITCH_TRACK_HEIGHT;

    g_return_val_if_fail (PN_IS_SWITCH (self), FALSE);

    node = PN_NODE (self);
    pos  = pn_node_get_position (node);
    pn_node_get_size (node, &w, &h);

    cx = pos->x + w - PN_SWITCH_RIGHT_PAD - sw * 0.5;
    cy = pos->y + pn_node_get_header_height (node) * 0.5;

    return (px >= cx - sw * 0.5 && px <= cx + sw * 0.5 &&
            py >= cy - sh * 0.5 && py <= cy + sh * 0.5);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_switch_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnSwitch *self = PN_SWITCH (object);

    switch (prop_id)
    {
    case PROP_ON:
        g_value_set_boolean (value, PRIV (self)->on);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_switch_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnSwitch        *self = PN_SWITCH (object);
    PnSwitchPrivate *priv = PRIV (self);

    switch (prop_id)
    {
    case PROP_ON:
        {
            gboolean v = g_value_get_boolean (value);
            if (priv->on != v)
            {
                priv->on = v;
                apply_visual_state (self);
                g_object_notify_by_pspec (object, props[PROP_ON]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  Startup announce                                                    */
/*                                                                      */
/*  A switch is a manual source/latch: without a one-shot announce a    */
/*  freshly-loaded (or freshly-dropped) switch would leave every        */
/*  downstream node ignorant of its position until the first click or   */
/*  inbound edge -- the same gap the periodic data sources and PnKnob   */
/*  close by reporting once shortly after the worksheet loads.  The     */
/*  emit goes through the build_outbound_message vfunc, so a subclass'  */
/*  message shape is honoured; subclasses whose outbound has an outward */
/*  side effect (PnTasmotaSwitch builds an MQTT relay *command*) clear  */
/*  announce_on_startup so opening a worksheet never silently actuates  */
/*  hardware.                                                           */
/*                                                                      */
/*  The shot is scheduled unconditionally at construction but consults  */
/*  announce_on_startup only when it *fires*.  The flag is read late on */
/*  purpose: a node's regular properties (e.g. PnTasmotaSwitch's        */
/*  enforce-on-startup) are applied by the loader after constructed()   */
/*  has run, so a construction-time decision would miss a value the     */
/*  file is about to set.  By the time the idle fires the load has      */
/*  returned to the main loop and the flag reflects the loaded state.   */
/* ------------------------------------------------------------------ */

static gboolean
emit_startup_state (gpointer data)
{
    PnSwitch        *self = PN_SWITCH (data);
    PnSwitchPrivate *priv = PRIV (self);

    priv->startup_emit_id = 0;

    if (priv->announce_on_startup)
        emit_state_message (self);

    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_switch_constructed (GObject *object)
{
    PnSwitch        *self = PN_SWITCH (object);
    PnSwitchPrivate *priv = PRIV (self);

    G_OBJECT_CLASS (pn_switch_parent_class)->constructed (object);

    /* Defer the announce to a main-loop idle rather than emitting here:
     * at construction time the load path has built this node but not yet
     * attached its output wires (node_from_json creates every node first,
     * applies their properties, then connects the wires), so an emit now
     * would reach nobody.  By the time the idle runs the load call has
     * returned to the main loop, the graph is fully wired, and priv->on
     * holds the latch position restored from the file.  The idle holds no
     * reference on @self; dispose() pulls it if the node dies first.
     *
     * Scheduled unconditionally; emit_startup_state() reads
     * announce_on_startup when it fires, so a subclass property applied
     * after constructed() (PnTasmotaSwitch's enforce-on-startup) still
     * governs whether the shot actually emits. */
    priv->startup_emit_id = g_idle_add (emit_startup_state, self);
}

static void
pn_switch_dispose (GObject *object)
{
    PnSwitchPrivate *priv = PRIV (PN_SWITCH (object));

    if (priv->startup_emit_id != 0)
    {
        g_source_remove (priv->startup_emit_id);
        priv->startup_emit_id = 0;
    }

    G_OBJECT_CLASS (pn_switch_parent_class)->dispose (object);
}

static void
pn_switch_class_init (PnSwitchClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_switch_get_property;
    object_class->set_property = pn_switch_set_property;
    object_class->constructed  = pn_switch_constructed;
    object_class->dispose      = pn_switch_dispose;

    node_class->receive              = pn_switch_receive;
    node_class->get_size             = pn_switch_get_size;
    node_class->paint_header_overlay = pn_switch_paint_header_overlay;
    node_class->paint_right_decoration_width = PN_SWITCH_RESERVED_RIGHT;

    klass->apply_visual_state     = pn_switch_real_apply_visual_state;
    klass->build_outbound_message = pn_switch_real_build_outbound_message;

    node_class->palette_icon = PN_SWITCH_ICON;
    node_class->class_name   = "Switch";
    node_class->icon         = PN_SWITCH_ICON;
    node_class->color        = (PnColor){ 0.30, 0.66, 0.66, 1.0 };
    node_class->category     = "Sources";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_ON] = g_param_spec_boolean (
            "on", "On",
            "Latched switch position.  TRUE while the switch is in "
            "the \"on\" position (next emitted message carries "
            "data.value = 1.0); FALSE while \"off\" (data.value = "
            "0.0).  Writing the property programmatically flips the "
            "visual state without emitting -- use pn_switch_toggle() "
            "to flip and emit in a single step.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_switch_init (PnSwitch *self)
{
    PnNode          *node = PN_NODE (self);
    PnSwitchPrivate *priv = PRIV (self);

    priv->on                  = FALSE;
    priv->announce_on_startup = TRUE;
    priv->startup_emit_id     = 0;

    pn_node_set_class_name (node, "Switch");
    pn_node_set_icon       (node, PN_SWITCH_ICON);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    {
        PnColor teal = { 0.30, 0.66, 0.66, 1.0 };
        pn_node_set_color (node, &teal);
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnSwitch *
pn_switch_new (void)
{
    return g_object_new (PN_TYPE_SWITCH, NULL);
}

gboolean
pn_switch_get_on (PnSwitch *self)
{
    g_return_val_if_fail (PN_IS_SWITCH (self), FALSE);
    return PRIV (self)->on;
}

gboolean
pn_switch_get_announce_on_startup (PnSwitch *self)
{
    g_return_val_if_fail (PN_IS_SWITCH (self), FALSE);
    return PRIV (self)->announce_on_startup;
}

void
pn_switch_set_announce_on_startup (
        PnSwitch *self,
        gboolean  announce)
{
    g_return_if_fail (PN_IS_SWITCH (self));

    /* Just record the intent.  The one-shot scheduled in constructed()
     * reads this when it fires, so toggling it any time before then --
     * from a subclass init, or from a property setter the loader applies
     * during load -- takes effect without needing to (re)schedule. */
    PRIV (self)->announce_on_startup = announce;
}

void
pn_switch_toggle (PnSwitch *self)
{
    PnSwitchPrivate *priv;

    g_return_if_fail (PN_IS_SWITCH (self));

    priv = PRIV (self);
    priv->on = !priv->on;
    apply_visual_state (self);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ON]);

    emit_state_message (self);
}
