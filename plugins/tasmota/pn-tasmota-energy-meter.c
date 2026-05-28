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

#include "pn-tasmota-energy-meter.h"

#include "pn-message.h"
#include "pn-tasmota-common.h"

#include <string.h>

typedef struct
{
    /* Tasmota device name the node filters on.  Only messages whose
     * topic carries this exact name in the second-to-last segment
     * (Tasmota's <prefix>/<device>/SENSOR topology) are processed;
     * everything else is dropped silently.  Empty / %NULL marks the
     * node as unconfigured and rejects every message -- mirrors the
     * #PnTasmotaRelayStatus convention so a freshly-dropped meter
     * has the needle parked at zero until the user fills the field
     * in.                                                              */
    gchar *switch_name;
} PnTasmotaEnergyMeterPrivate;

G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE (PnTasmotaEnergyMeter,
                                     pn_tasmota_energy_meter,
                                     PN_TYPE_ANALOG_METER)

enum {
    PROP_0,
    PROP_SWITCH_NAME,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Topic filter                                                       */
/* ------------------------------------------------------------------ */

/** Tasmota energy data lands on `tele/<device>/SENSOR`.  Matching the
 *  last topic segment against the literal "SENSOR" covers the only
 *  family that carries the ENERGY sub-object. */
static gboolean
topic_is_tasmota_sensor (const gchar *topic)
{
    const gchar *last_slash;
    const gchar *last_segment;

    if (topic == NULL || *topic == '\0')
        return FALSE;

    last_slash   = strrchr (topic, '/');
    last_segment = (last_slash != NULL) ? last_slash + 1 : topic;

    return g_strcmp0 (last_segment, "SENSOR") == 0;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_energy_meter_receive (PnNode *node, PnMessage *message)
{
    PnTasmotaEnergyMeter        *self  = PN_TASMOTA_ENERGY_METER (node);
    PnTasmotaEnergyMeterPrivate *priv  =
            pn_tasmota_energy_meter_get_instance_private (self);
    const gchar                 *topic;
    gchar                       *device;

    /* Unconfigured nodes drop every message -- there is no sensible
     * default device name we could pick on the user's behalf, and
     * silently driving every meter on the worksheet from the first
     * device that publishes would be a worse failure mode than just
     * leaving the needle parked. */
    if (priv->switch_name == NULL || *priv->switch_name == '\0')
        return;

    topic = pn_message_get_topic (message);
    if (!topic_is_tasmota_sensor (topic))
        return;

    device = pn_tasmota_device_from_topic (topic);
    if (device == NULL || g_strcmp0 (device, priv->switch_name) != 0)
    {
        g_free (device);
        return;
    }
    g_free (device);

    /* Topic / device gate passed -- hand the message off to
     * #PnAnalogMeter::receive, which runs the inherited `key`-based
     * JSON path resolver against the envelope and drives the needle.
     * Each leaf points `key` at the right payload path in its
     * `_init`, so the same chain-up handles voltage / current /
     * power / ... without any per-quantity code here.               */
    PN_NODE_CLASS (pn_tasmota_energy_meter_parent_class)->receive (
            node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tasmota_energy_meter_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTasmotaEnergyMeter        *self = PN_TASMOTA_ENERGY_METER (object);
    PnTasmotaEnergyMeterPrivate *priv =
            pn_tasmota_energy_meter_get_instance_private (self);

    switch (prop_id)
    {
    case PROP_SWITCH_NAME:
        g_value_set_string (value, priv->switch_name);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tasmota_energy_meter_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTasmotaEnergyMeter        *self = PN_TASMOTA_ENERGY_METER (object);
    PnTasmotaEnergyMeterPrivate *priv =
            pn_tasmota_energy_meter_get_instance_private (self);

    switch (prop_id)
    {
    case PROP_SWITCH_NAME:
    {
        const gchar *name        = g_value_get_string (value);
        gchar       *replacement = (name != NULL) ? g_strdup (name) : NULL;

        g_free (priv->switch_name);
        priv->switch_name = replacement;
        g_object_notify_by_pspec (object, props[PROP_SWITCH_NAME]);
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
pn_tasmota_energy_meter_finalize (GObject *object)
{
    PnTasmotaEnergyMeter        *self = PN_TASMOTA_ENERGY_METER (object);
    PnTasmotaEnergyMeterPrivate *priv =
            pn_tasmota_energy_meter_get_instance_private (self);

    g_clear_pointer (&priv->switch_name, g_free);

    G_OBJECT_CLASS (pn_tasmota_energy_meter_parent_class)->finalize (object);
}

static void
pn_tasmota_energy_meter_class_init (PnTasmotaEnergyMeterClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_tasmota_energy_meter_get_property;
    object_class->set_property = pn_tasmota_energy_meter_set_property;
    object_class->finalize     = pn_tasmota_energy_meter_finalize;

    node_class->receive = pn_tasmota_energy_meter_receive;

    /* The C class struct inherits #PnAnalogMeter's build_class_tabs
     * pointer verbatim -- if we leave it intact the dialog walks the
     * type chain and calls the inherited hook once per descendant,
     * stamping a duplicate Data / Scale / Colours triplet for
     * #PnTasmotaEnergyMeter and again for each concrete leaf.  NULL
     * here puts every descendant back on the dialog's default
     * introspection path so the leaf's own properties (switch-name)
     * land in a regular auto-generated tab.                          */
    node_class->build_class_tabs = NULL;

    props[PROP_SWITCH_NAME] = g_param_spec_string (
            "switch-name", "Switch name",
            "Tasmota device name the meter listens for.  Matched "
            "against the second-to-last segment of each incoming "
            "topic (the canonical Tasmota <prefix>/<device>/<command> "
            "topology), so 'sonoff37' accepts publishes whose topic "
            "is 'tele/sonoff37/SENSOR' and drops everything else.  "
            "Empty marks the node as needing configuration "
            "and rejects every message -- the needle stays parked "
            "until the user fills the field in.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tasmota_energy_meter_init (PnTasmotaEnergyMeter *self)
{
    PnTasmotaEnergyMeterPrivate *priv =
            pn_tasmota_energy_meter_get_instance_private (self);

    priv->switch_name = NULL;
}
