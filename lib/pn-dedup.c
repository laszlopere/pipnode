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

#include "pn-dedup.h"
#include "pn-json-path.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>

#define PN_DEDUP_TIMEOUT_MIN  1u
#define PN_DEDUP_TIMEOUT_MAX  1440u   /* 24 hours */
#define PN_DEDUP_TIMEOUT_DEF  15u

struct _PnDedup
{
    PnNode parent_instance;

    /* "/"-separated JSON path into the message root, e.g.
     * "data/blockhash".  An empty key disables the node — every
     * message is dropped, since no value can be extracted. */
    gchar *key;

    /* Retention window in minutes.  Entries whose insertion time is
     * older than this on a fresh receive are pruned and the value is
     * considered fresh again. */
    guint  timeout;

    /* Maps the stringified value seen at @key to a gint64 monotonic
     * timestamp (microseconds) of when it was inserted.  Both keys
     * and values are owned by the table. */
    GHashTable *seen;
};

G_DEFINE_TYPE (PnDedup, pn_dedup, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_KEY,
    PROP_TIMEOUT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Stringify a #JsonNode for use as a hash key.  Scalars use their
 *  natural textual form; objects and arrays serialise to compact
 *  JSON so distinct structures still collide only when they are
 *  byte-for-byte identical.  Returns %NULL when the node is %NULL or
 *  JSON null — a missing value cannot be deduped against. */
static gchar *
node_to_key (JsonNode *node)
{
    if (node == NULL || JSON_NODE_HOLDS_NULL (node))
        return NULL;

    if (JSON_NODE_HOLDS_VALUE (node))
    {
        GType vtype = json_node_get_value_type (node);

        if (vtype == G_TYPE_STRING)
            return g_strdup (json_node_get_string (node));
        if (vtype == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT,
                                    json_node_get_int (node));
        if (vtype == G_TYPE_DOUBLE)
            return g_strdup_printf ("%.17g", json_node_get_double (node));
        if (vtype == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (node) ? "true" : "false");
    }

    {
        JsonGenerator *gen = json_generator_new ();
        gchar         *out;

        json_generator_set_root (gen, node);
        out = json_generator_to_data (gen, NULL);
        g_object_unref (gen);
        return out;
    }
}

/** Drop entries from @seen whose insertion timestamp is older than
 *  @timeout minutes ago.  Called lazily on every receive so an idle
 *  flow eventually drains without a background timer. */
static void
prune_expired (
        GHashTable *seen,
        guint       timeout_minutes,
        gint64      now_us)
{
    GHashTableIter iter;
    gpointer       k, v;
    gint64         max_age_us = (gint64) timeout_minutes * 60 * G_TIME_SPAN_SECOND;

    g_hash_table_iter_init (&iter, seen);
    while (g_hash_table_iter_next (&iter, &k, &v))
    {
        gint64 stamp = *(gint64 *) v;
        if (now_us - stamp >= max_age_us)
            g_hash_table_iter_remove (&iter);
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_dedup_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnDedup    *self = PN_DEDUP (node);
    JsonObject *root;
    JsonNode   *value_node;
    gchar      *key;
    gint64      now_us;

    if (self->key == NULL || *self->key == '\0')
        return;

    root       = pn_json_lookup_root_for_message (message);
    value_node = pn_json_resolve_path (root, self->key);
    key        = node_to_key (value_node);
    json_object_unref (root);

    if (key == NULL)
        return;

    now_us = g_get_monotonic_time ();
    prune_expired (self->seen, self->timeout, now_us);

    if (g_hash_table_contains (self->seen, key))
    {
        g_free (key);
        return;
    }

    {
        gint64 *stamp = g_new (gint64, 1);
        *stamp = now_us;
        g_hash_table_insert (self->seen, key, stamp);  /* takes key */
    }

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_dedup_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnDedup *self = PN_DEDUP (object);

    switch (prop_id)
    {
    case PROP_KEY:
        g_value_set_string (value, self->key);
        break;
    case PROP_TIMEOUT:
        g_value_set_uint (value, self->timeout);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_dedup_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnDedup *self = PN_DEDUP (object);

    switch (prop_id)
    {
    case PROP_KEY:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->key, s) != 0)
            {
                g_free (self->key);
                self->key = g_strdup (s ? s : "");
                /* Switching the watched key invalidates everything we
                 * have remembered so far — the old strings live in a
                 * different namespace. */
                g_hash_table_remove_all (self->seen);
                g_object_notify_by_pspec (object, props[PROP_KEY]);
            }
        }
        break;
    case PROP_TIMEOUT:
        {
            guint v = g_value_get_uint (value);
            if (self->timeout != v)
            {
                self->timeout = v;
                g_object_notify_by_pspec (object, props[PROP_TIMEOUT]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_dedup_finalize (GObject *object)
{
    PnDedup *self = PN_DEDUP (object);

    g_clear_pointer (&self->key,  g_free);
    g_clear_pointer (&self->seen, g_hash_table_unref);

    G_OBJECT_CLASS (pn_dedup_parent_class)->finalize (object);
}

static void
pn_dedup_class_init (PnDedupClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_dedup_get_property;
    object_class->set_property = pn_dedup_set_property;
    object_class->finalize     = pn_dedup_finalize;
    node_class->receive        = pn_dedup_receive;

    node_class->class_name     = "Dedup";
    node_class->icon           = "\xef\x89\x8d";  /* fa-clone U+F24D */
    node_class->color          = (GdkRGBA){ 0.92, 0.76, 0.27, 1.0 };
    node_class->category       = "Filters";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_KEY] = g_param_spec_string (
            "key", "Key",
            "JSON path (\"/\"-separated, e.g. data/value) to the "
            "value used as the dedup identity",
            "data/value",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TIMEOUT] = g_param_spec_uint (
            "timeout", "Timeout",
            "Minutes to remember a value before it is treated as fresh again",
            PN_DEDUP_TIMEOUT_MIN,
            PN_DEDUP_TIMEOUT_MAX,
            PN_DEDUP_TIMEOUT_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_dedup_init (PnDedup *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  yellow = { 0.92, 0.76, 0.27, 1.0 };

    self->key     = g_strdup ("data/value");
    self->timeout = PN_DEDUP_TIMEOUT_DEF;
    self->seen    = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, g_free);

    pn_node_set_class_name (node, "Dedup");
    pn_node_set_icon       (node, "\xef\x89\x8d");  /* fa-clone U+F24D */
    pn_node_set_color      (node, &yellow);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDedup *
pn_dedup_new (void)
{
    return g_object_new (PN_TYPE_DEDUP, NULL);
}
