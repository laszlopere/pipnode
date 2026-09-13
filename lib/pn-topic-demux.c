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

#include "pn-topic-demux.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* Longest output label painted beside a port, in characters.  A longer
 * pattern shows its tail, which is where MQTT topics differ
 * ("…/SENSOR" vs "…/STATE"). */
#define PN_TOPIC_DEMUX_LABEL_CHARS 16

struct _PnTopicDemux
{
    PnNode parent_instance;

    gint   n_outputs;   /* configurable output count, 2..16            */

    /* Always PN_TOPIC_DEMUX_MAX_OUTPUTS slots, so the patterns of outputs
     * removed by lowering the count survive until it is raised again.
     * topics[i] is never NULL ("" = no pattern); globs[i] is the compiled
     * glob when topics[i] contains '*' or '?', NULL otherwise. */
    gchar        *topics[PN_TOPIC_DEMUX_MAX_OUTPUTS];
    GPatternSpec *globs[PN_TOPIC_DEMUX_MAX_OUTPUTS];
};

G_DEFINE_TYPE (PnTopicDemux, pn_topic_demux, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_OUTPUTS,
    PROP_TOPICS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Matching                                                           */
/* ------------------------------------------------------------------ */

static gboolean
is_glob (const gchar *pattern)
{
    return strchr (pattern, '*') != NULL || strchr (pattern, '?') != NULL;
}

/* MQTT topic-filter match, level by level, without allocating. */
static gboolean
mqtt_match (const gchar *p, const gchar *t)
{
    for (;;)
    {
        /* A trailing '#' swallows the rest of the topic. */
        if (p[0] == '#' && p[1] == '\0')
            return TRUE;

        if (p[0] == '+' && (p[1] == '/' || p[1] == '\0'))
        {
            const gchar *slash = strchr (t, '/');

            t = (slash != NULL) ? slash : t + strlen (t);
            p++;
        }
        else
        {
            while (*p != '\0' && *p != '/')
            {
                if (*p != *t)
                    return FALSE;
                p++;
                t++;
            }
            if (*t != '\0' && *t != '/')
                return FALSE;
        }

        /* Both sides now sit on a level boundary. */
        if (*p == '\0')
            return *t == '\0';
        if (*t == '\0')
            return strcmp (p, "/#") == 0;   /* "a/#" also matches "a" */

        p++;
        t++;
    }
}

gboolean
pn_topic_demux_topic_matches (
        const gchar *pattern,
        const gchar *topic)
{
    if (pattern == NULL || *pattern == '\0')
        return FALSE;
    if (topic == NULL)
        topic = "";

    if (is_glob (pattern))
        return g_pattern_match_simple (pattern, topic);

    return mqtt_match (pattern, topic);
}

gint
pn_topic_demux_route (PnTopicDemux *self, const gchar *topic)
{
    gint i;

    g_return_val_if_fail (PN_IS_TOPIC_DEMUX (self), -1);

    if (topic == NULL)
        topic = "";

    for (i = 0; i < self->n_outputs; i++)
    {
        const gchar *pattern = self->topics[i];

        if (*pattern == '\0')
            continue;

        if (self->globs[i] != NULL
                ? g_pattern_spec_match_string (self->globs[i], topic)
                : mqtt_match (pattern, topic))
            return i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_topic_demux_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTopicDemux *self = PN_TOPIC_DEMUX (node);
    gint          out  = pn_topic_demux_route (self,
                                               pn_message_get_topic (message));

    /* Forwarded untouched, like the Filter node; unmatched is dropped. */
    if (out >= 0)
        pn_node_emit_message_on_output (node, message, out);
}

/* ------------------------------------------------------------------ */
/*  Topic slots                                                        */
/* ------------------------------------------------------------------ */

/* The worksheet label of output @index: its pattern, or its tail when
 * long, or the "outN" default when it has none. */
static void
update_output_name (PnTopicDemux *self, gint index)
{
    const gchar *pattern = self->topics[index];
    glong        len     = g_utf8_strlen (pattern, -1);

    if (len <= PN_TOPIC_DEMUX_LABEL_CHARS)
    {
        pn_node_set_output_name (PN_NODE (self), index, pattern);
    }
    else
    {
        const gchar *tail  = g_utf8_offset_to_pointer (
                pattern, len - (PN_TOPIC_DEMUX_LABEL_CHARS - 1));
        gchar       *label = g_strconcat ("\xe2\x80\xa6", tail, NULL);  /* … */

        pn_node_set_output_name (PN_NODE (self), index, label);
        g_free (label);
    }
}

/* Store one slot; returns TRUE when it changed.  Does not notify. */
static gboolean
store_topic (PnTopicDemux *self, gint index, const gchar *pattern)
{
    gchar *stripped;

    stripped = g_strstrip (g_strdup (pattern != NULL ? pattern : ""));
    if (strcmp (stripped, self->topics[index]) == 0)
    {
        g_free (stripped);
        return FALSE;
    }

    g_free (self->topics[index]);
    self->topics[index] = stripped;

    g_clear_pointer (&self->globs[index], g_pattern_spec_free);
    if (is_glob (stripped))
        self->globs[index] = g_pattern_spec_new (stripped);

    update_output_name (self, index);
    return TRUE;
}

/* JSON array of every slot, trailing empty slots trimmed. */
static gchar *
topics_to_json (PnTopicDemux *self)
{
    JsonArray *arr  = json_array_new ();
    JsonNode  *root = json_node_new (JSON_NODE_ARRAY);
    gchar     *json;
    gint       last = PN_TOPIC_DEMUX_MAX_OUTPUTS - 1;
    gint       i;

    while (last >= 0 && *self->topics[last] == '\0')
        last--;

    for (i = 0; i <= last; i++)
        json_array_add_string_element (arr, self->topics[i]);

    json_node_take_array (root, arr);
    json = json_to_string (root, FALSE);
    json_node_unref (root);
    return json;
}

/* Replace every slot from a JSON array of strings.  Anything that is not
 * such an array clears them all; a non-string element clears its slot. */
static void
topics_from_json (PnTopicDemux *self, const gchar *json)
{
    JsonParser *parser = json_parser_new ();
    JsonArray  *arr    = NULL;
    guint       n      = 0;
    gint        i;

    if (json != NULL && *json != '\0' &&
        json_parser_load_from_data (parser, json, -1, NULL))
    {
        JsonNode *root = json_parser_get_root (parser);

        if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
        {
            arr = json_node_get_array (root);
            n   = json_array_get_length (arr);
        }
    }

    for (i = 0; i < PN_TOPIC_DEMUX_MAX_OUTPUTS; i++)
    {
        const gchar *pattern = NULL;

        if ((guint) i < n)
        {
            JsonNode *el = json_array_get_element (arr, i);

            if (JSON_NODE_HOLDS_VALUE (el) &&
                json_node_get_value_type (el) == G_TYPE_STRING)
                pattern = json_node_get_string (el);
        }
        store_topic (self, i, pattern);
    }

    g_object_unref (parser);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
topic_demux_set_outputs (PnTopicDemux *self, gint n)
{
    PnNode *node = PN_NODE (self);

    if (n == self->n_outputs)
        return;

    self->n_outputs = n;
    pn_node_set_n_outputs (node, n);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_OUTPUTS]);
    pn_node_request_repaint (node);
}

static void
pn_topic_demux_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTopicDemux *self = PN_TOPIC_DEMUX (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        g_value_set_int (value, self->n_outputs);
        break;
    case PROP_TOPICS:
        g_value_take_string (value, topics_to_json (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_topic_demux_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTopicDemux *self = PN_TOPIC_DEMUX (object);

    switch (prop_id)
    {
    case PROP_OUTPUTS:
        topic_demux_set_outputs (self, g_value_get_int (value));
        break;
    case PROP_TOPICS:
        topics_from_json (self, g_value_get_string (value));
        pn_node_request_repaint (PN_NODE (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_topic_demux_finalize (GObject *object)
{
    PnTopicDemux *self = PN_TOPIC_DEMUX (object);
    gint          i;

    for (i = 0; i < PN_TOPIC_DEMUX_MAX_OUTPUTS; i++)
    {
        g_free (self->topics[i]);
        g_clear_pointer (&self->globs[i], g_pattern_spec_free);
    }

    G_OBJECT_CLASS (pn_topic_demux_parent_class)->finalize (object);
}

static void
pn_topic_demux_class_init (PnTopicDemuxClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_topic_demux_get_property;
    object_class->set_property = pn_topic_demux_set_property;
    object_class->finalize     = pn_topic_demux_finalize;
    node_class->receive        = pn_topic_demux_receive;
    /* build_class_tab installed by the gui tier (pn_topic_demux_gui_install). */

    node_class->class_name     = "Topic Demux";
    node_class->icon           = "\xef\x83\xa8";  /* fa-sitemap U+F0E8 */
    node_class->color          = (PnColor){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters/Gate";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_OUTPUTS] = g_param_spec_int (
            "outputs", "Outputs",
            "Number of outputs, each with its own topic pattern.",
            PN_TOPIC_DEMUX_MIN_OUTPUTS, PN_TOPIC_DEMUX_MAX_OUTPUTS,
            PN_TOPIC_DEMUX_DEF_OUTPUTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
            G_PARAM_EXPLICIT_NOTIFY);

    props[PROP_TOPICS] = g_param_spec_string (
            "topics", "Topics",
            "JSON array of topic patterns, one per output. A message "
            "leaves by the first output whose pattern matches its topic; "
            "unmatched messages are dropped. MQTT wildcards (+ one level, "
            "trailing # the rest) or globs (* and ?) are accepted.",
            "[]",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_topic_demux_init (PnTopicDemux *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  yellow = { 0.92, 0.76, 0.27, 1.0 };
    gint     i;

    self->n_outputs = PN_TOPIC_DEMUX_DEF_OUTPUTS;
    for (i = 0; i < PN_TOPIC_DEMUX_MAX_OUTPUTS; i++)
        self->topics[i] = g_strdup ("");

    pn_node_set_class_name (node, "Topic Demux");
    pn_node_set_icon       (node, "\xef\x83\xa8");  /* fa-sitemap U+F0E8 */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_n_outputs  (node, self->n_outputs);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTopicDemux *
pn_topic_demux_new (void)
{
    return g_object_new (PN_TYPE_TOPIC_DEMUX, NULL);
}

const gchar *
pn_topic_demux_get_topic (PnTopicDemux *self, gint index)
{
    g_return_val_if_fail (PN_IS_TOPIC_DEMUX (self), "");
    g_return_val_if_fail (index >= 0 && index < PN_TOPIC_DEMUX_MAX_OUTPUTS, "");

    return self->topics[index];
}

void
pn_topic_demux_set_topic (
        PnTopicDemux *self,
        gint          index,
        const gchar  *pattern)
{
    g_return_if_fail (PN_IS_TOPIC_DEMUX (self));
    g_return_if_fail (index >= 0 && index < PN_TOPIC_DEMUX_MAX_OUTPUTS);

    if (store_topic (self, index, pattern))
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TOPICS]);
}
