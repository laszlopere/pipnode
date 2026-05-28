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

#include "pn-tasmota-switch.h"

#include "pn-message.h"
#include "pn-tasmota-common.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* fa-toggle-on U+F205 -- pinned (not flipped between on/off variants
 * the way the bare PnSwitch base does) because the slider widget in
 * the header already reports the latch position visually; flipping
 * the side glyph too would be redundant.  Swapped to the warning
 * ❗ glyph (U+2757) while the switch-name field is empty, matching
 * the convention #PnTasmotaRelayStatus / #PnTasmotaRelayCommand /
 * #PnInject use to flag a node that needs configuration before it
 * can do anything useful. */
#define PN_TASMOTA_SWITCH_ICON         "\xef\x88\x85"
#define PN_TASMOTA_SWITCH_WARNING_ICON "\xe2\x9d\x97"

struct _PnTasmotaSwitch
{
    PnSwitch parent_instance;

    /* Tasmota device name the node both filters inbound on (only
     * messages whose topic carries this name in the second-to-last
     * segment update the latch) and stamps into outbound command
     * envelopes (`cmnd/<switch-name>/POWER`).  Mandatory: an empty
     * value rejects every inbound message and refuses to emit on
     * user-click, since silently defaulting would risk reading or
     * flipping the wrong physical relay. */
    gchar *switch_name;
};

G_DEFINE_TYPE (PnTasmotaSwitch, pn_tasmota_switch, PN_TYPE_SWITCH)

enum {
    PROP_0,
    PROP_SWITCH_NAME,
    PROP_ENFORCE_ON_STARTUP,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

/** Override of the base #PnSwitch vfunc.  Steel-blue + toggle glyph
 *  when the switch-name field is set (matches the rest of the Tasmota
 *  palette), red + ❗ glyph when not.  Repaint is still needed for
 *  the slider widget to redraw against the new colour, so we keep the
 *  base contract (icon + repaint). */
static void
pn_tasmota_switch_apply_visual_state (PnSwitch *base)
{
    PnTasmotaSwitch *self = PN_TASMOTA_SWITCH (base);
    PnNode          *node = PN_NODE (self);
    gboolean         configured;

    configured = (self->switch_name != NULL && *self->switch_name != '\0');

    if (configured)
    {
        PnColor steel = { 0.42, 0.55, 0.72, 1.0 };
        pn_node_set_color (node, &steel);
        pn_node_set_icon  (node, PN_TASMOTA_SWITCH_ICON);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_TASMOTA_SWITCH_WARNING_ICON);
    }

    pn_node_request_repaint (node);
}

/* ------------------------------------------------------------------ */
/*  Outbound -- Tasmota command shape                                  */
/* ------------------------------------------------------------------ */

/** Override of the base #PnSwitch vfunc.  Build a message in the
 *  exact shape #PnTasmotaRelayCommand would have produced if the
 *  factored chain were still wired: envelope topic set to
 *  `cmnd/<switch-name>/POWER`, `data.payload = "ON"/"OFF"` so a
 *  downstream #PnMqttSink publishes the literal Tasmota expects, and
 *  the standard `data.value` / `data.success` / `data.device` /
 *  `data.output` mirror so a downstream LED / Debug / Graph reads the
 *  same shape regardless of which encoding path drove the click. */
static PnMessage *
pn_tasmota_switch_build_outbound_message (PnSwitch *base)
{
    PnTasmotaSwitch *self = PN_TASMOTA_SWITCH (base);
    PnNode          *node = PN_NODE (self);
    PnMessage       *msg;
    gboolean         on;
    gchar           *topic;
    gchar           *output;

    on = pn_switch_get_on (base);

    /* Unconfigured nodes still build a message (the dispatcher will
     * emit it) but without a switch_name there is nothing useful to
     * stamp -- fall back to the base shape so a misconfigured node
     * does not silently publish to an empty `cmnd//POWER` topic.  The
     * red ❗ visual already nags the user to configure the field. */
    if (self->switch_name == NULL || *self->switch_name == '\0')
        return pn_switch_real_build_outbound_message (base);

    msg   = pn_message_new (node, NULL);
    topic = g_strdup_printf ("cmnd/%s/POWER", self->switch_name);
    pn_message_set_topic (msg, topic);
    g_free (topic);

    pn_message_set_string  (msg, "payload", on ? "ON" : "OFF");
    pn_message_set_double  (msg, "value",   on ? 1.0 : 0.0);
    pn_message_set_boolean (msg, "success", TRUE);
    pn_message_set_string  (msg, "device",  self->switch_name);

    output = g_strdup_printf ("%s: command power %s",
                              self->switch_name, on ? "on" : "off");
    pn_message_set_string (msg, "output", output);
    g_free (output);

    return msg;
}

/* ------------------------------------------------------------------ */
/*  Inbound -- Tasmota status decode, latch sync silently              */
/* ------------------------------------------------------------------ */

/** Tasmota relay state lands on `stat/<device>/POWER` for the single-
 *  relay default plus `POWER1`..`POWER8` for multi-relay devices.
 *  Same last-segment check #PnTasmotaRelayStatus uses. */
static gboolean
topic_is_tasmota_power (const gchar *topic)
{
    const gchar *last_slash;
    const gchar *last_segment;

    if (topic == NULL || *topic == '\0')
        return FALSE;

    last_slash   = strrchr (topic, '/');
    last_segment = (last_slash != NULL) ? last_slash + 1 : topic;

    return g_str_has_prefix (last_segment, "POWER");
}

/** Pull a string `data.payload` member off @message.  Tasmota POWER
 *  publishes carry the bare strings "ON" / "OFF" rather than the
 *  JSON object shape SENSOR / STATE messages use, so the payload
 *  arrives as a string member rather than a structured sub-object. */
static const gchar *
get_payload_string (PnMessage *message)
{
    JsonNode *n;

    n = pn_message_get_member (message, "payload");
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n))
        return NULL;
    if (json_node_get_value_type (n) != G_TYPE_STRING)
        return NULL;
    return json_node_get_string (n);
}

/** Complete override of the base receive vfunc (does *not* chain to
 *  parent).  An inbound Tasmota POWER message authoritatively
 *  describes the physical state of the relay; we mirror it on the
 *  latch by writing the "on" property (which dispatches the
 *  apply_visual_state vfunc and notifies, the same path a programmatic
 *  set goes through) and stop.  Nothing is emitted downstream because:
 *
 *    * The inbound is a raw MQTT envelope -- forwarding it would
 *      leak broker bytes into nodes (LEDs, graphs) that expect the
 *      canonical data.value shape.
 *
 *    * Even after decoding, re-emitting would close a loop straight
 *      back through PnMqttSink to the broker, commanding the relay
 *      to do what it already does.  The base PnSwitch's latch-
 *      equality storm-breaker is implicit here: if the inbound state
 *      matches the current latch, nothing happens; if it differs, we
 *      only update internal state, never emit.  Either way the cycle
 *      cannot form. */
static void
pn_tasmota_switch_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTasmotaSwitch *self = PN_TASMOTA_SWITCH (node);
    PnSwitch        *base = PN_SWITCH (node);
    const gchar     *payload;
    const gchar     *topic;
    gchar           *device;
    gboolean         want_on;

    if (self->switch_name == NULL || *self->switch_name == '\0')
        return;

    topic = pn_message_get_topic (message);
    if (!topic_is_tasmota_power (topic))
        return;

    /* Compare extracted device names (rather than substring-matching
     * the raw topic) so "sonoff1" does not accidentally accept
     * publishes from "sonoff19". */
    device = pn_tasmota_device_from_topic (topic);
    if (device == NULL || g_strcmp0 (device, self->switch_name) != 0)
    {
        g_free (device);
        return;
    }
    g_free (device);

    payload = get_payload_string (message);
    if (payload == NULL)
        return;

    /* Only the literal "ON" / "OFF" strings move the latch; the other
     * responses Tasmota emits on the same POWER topic family
     * ("TOGGLE", "Blink", "PulseTime") are dropped so a noise burst
     * cannot synthesise a false transition. */
    if (g_ascii_strcasecmp (payload, "ON") == 0)
        want_on = TRUE;
    else if (g_ascii_strcasecmp (payload, "OFF") == 0)
        want_on = FALSE;
    else
        return;

    if (want_on != pn_switch_get_on (base))
    {
        /* g_object_set on the "on" property goes through the base
         * class's set_property, which updates internal state, calls
         * the apply_visual_state vfunc (dispatching to our override)
         * and notifies listeners -- without emitting.  Exactly the
         * silent-latch-sync semantics this branch needs. */
        g_object_set (base, "on", want_on, NULL);
    }
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
tasmota_switch_set_name (
        PnTasmotaSwitch *self,
        const gchar     *name)
{
    /* Normalise NULL / "" so both forms produce the same unconfigured
     * state, but round-trip whatever the caller passed so the dialog
     * sees a stable value on notify::switch-name. */
    gchar *replacement = (name != NULL) ? g_strdup (name) : NULL;

    g_free (self->switch_name);
    self->switch_name = replacement;

    /* The visual cue (steel-blue vs red ❗) depends on switch_name,
     * not on the latch position, so a name change has to re-run the
     * vfunc.  Going through the base class's apply_visual_state hook
     * keeps the dispatch consistent with everything else. */
    PN_SWITCH_GET_CLASS (self)->apply_visual_state (PN_SWITCH (self));

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SWITCH_NAME]);
}

static void
pn_tasmota_switch_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTasmotaSwitch *self = PN_TASMOTA_SWITCH (object);

    switch (prop_id)
    {
    case PROP_SWITCH_NAME:
        g_value_set_string (value, self->switch_name);
        break;
    case PROP_ENFORCE_ON_STARTUP:
        /* Backed directly by the base #PnSwitch announce flag -- the
         * property *is* the switch's startup-announce control, just
         * surfaced with a device-flavoured name. */
        g_value_set_boolean (value,
                pn_switch_get_announce_on_startup (PN_SWITCH (object)));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tasmota_switch_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTasmotaSwitch *self = PN_TASMOTA_SWITCH (object);

    switch (prop_id)
    {
    case PROP_SWITCH_NAME:
        tasmota_switch_set_name (self, g_value_get_string (value));
        break;
    case PROP_ENFORCE_ON_STARTUP:
    {
        gboolean enforce = g_value_get_boolean (value);
        if (pn_switch_get_announce_on_startup (PN_SWITCH (self)) != enforce)
        {
            pn_switch_set_announce_on_startup (PN_SWITCH (self), enforce);
            g_object_notify_by_pspec (object, props[PROP_ENFORCE_ON_STARTUP]);
        }
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
pn_tasmota_switch_finalize (GObject *object)
{
    PnTasmotaSwitch *self = PN_TASMOTA_SWITCH (object);

    g_clear_pointer (&self->switch_name, g_free);

    G_OBJECT_CLASS (pn_tasmota_switch_parent_class)->finalize (object);
}

static void
pn_tasmota_switch_class_init (PnTasmotaSwitchClass *klass)
{
    GObjectClass  *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass   *node_class   = PN_NODE_CLASS (klass);
    PnSwitchClass *switch_class = PN_SWITCH_CLASS (klass);

    object_class->get_property = pn_tasmota_switch_get_property;
    object_class->set_property = pn_tasmota_switch_set_property;
    object_class->finalize     = pn_tasmota_switch_finalize;

    /* Complete override of the base receive vfunc (no chaining). */
    node_class->receive = pn_tasmota_switch_receive;

    /* Hook into the two #PnSwitch extension points so the slider
     * widget, hit-testing and the worksheet's click routing keep
     * working unmodified while the visual scheme and the outbound
     * message shape become Tasmota-flavoured. */
    switch_class->apply_visual_state     = pn_tasmota_switch_apply_visual_state;
    switch_class->build_outbound_message = pn_tasmota_switch_build_outbound_message;

    node_class->palette_icon = PN_TASMOTA_SWITCH_ICON;
    node_class->class_name   = "Tasmota Switch";
    node_class->icon         = PN_TASMOTA_SWITCH_ICON;
    node_class->color        = (PnColor){ 0.42, 0.55, 0.72, 1.0 };
    node_class->category     = "Tasmota";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_SWITCH_NAME] = g_param_spec_string (
            "switch-name", "Switch name",
            "Tasmota device name the node binds to.  Inbound: only "
            "messages whose topic carries this exact name in the "
            "second-to-last segment (the canonical Tasmota "
            "<prefix>/<device>/<command> topology) sync the latch.  "
            "Outbound: stamped into the command envelope as "
            "cmnd/<switch-name>/POWER so a downstream MQTT Sink "
            "publishes straight to the right Tasmota relay.  Empty "
            "marks the node as needing configuration and rejects "
            "every inbound message -- mandatory by design, since "
            "silently defaulting would risk reading or flipping the "
            "wrong physical relay.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ENFORCE_ON_STARTUP] = g_param_spec_boolean (
            "enforce-on-startup", "Enforce on startup",
            "When TRUE, the node commands the relay to its saved latch "
            "position once shortly after the worksheet loads, emitting a "
            "cmnd/<switch-name>/POWER message (ON/OFF) so a downstream "
            "MQTT Sink forces the physical relay to match the worksheet "
            "on startup.  Default FALSE: opening a worksheet never "
            "actuates the relay -- the latch instead tracks the relay's "
            "reported state via inbound stat/POWER messages.  Enabling "
            "this is the right choice when the worksheet, not the device, "
            "is the source of truth for the desired relay state.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tasmota_switch_init (PnTasmotaSwitch *self)
{
    PnNode *node = PN_NODE (self);

    self->switch_name = NULL;

    pn_node_set_class_name (node, "Tasmota Switch");

    /* Default the base #PnSwitch startup announce OFF: our
     * build_outbound_message() emits an MQTT relay *command*
     * (cmnd/<device>/POWER = ON/OFF), so announcing on load would
     * actuate the physical relay every time a worksheet opens.  The
     * safe default is to leave the relay alone and let its true state
     * arrive via inbound stat/POWER messages (see
     * pn_tasmota_switch_receive).  Users who do want the worksheet to
     * force the relay to its saved state on load opt back in through the
     * "enforce-on-startup" property, which drives this same flag. */
    pn_switch_set_announce_on_startup (PN_SWITCH (self), FALSE);

    /* Start in the unconfigured (red ❗) state so a freshly-dropped
     * node visibly refuses to operate until the user fills in the
     * switch-name field.  apply_visual_state inspects switch_name
     * directly, so this picks up the correct red glyph. */
    PN_SWITCH_GET_CLASS (self)->apply_visual_state (PN_SWITCH (self));
}

PnTasmotaSwitch *
pn_tasmota_switch_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_SWITCH, NULL);
}
