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

#include "pn-message.h"

typedef struct
{
    gchar      *id;
    gchar      *topic;
    /* ISO-8601 with timezone offset, e.g. "2026-05-07T14:32:11+02:00".
     * Stamped at construction so downstream nodes can tell the wall-
     * clock time the message was emitted. */
    gchar      *created;
    JsonObject *data;       /* owned, never NULL */
    PnNode     *source;     /* strong ref, may be NULL */

    /* Out-of-band large-vector payloads (TODO #43).  Lazily allocated --
     * NULL until the first pn_message_set_vector() -- so the common
     * vector-free message pays nothing.  Maps gint64 handle -> PnVector*
     * (owns a ref).  Handles are referenced from the data bag by
     * "$pnvector" markers. */
    GHashTable *vectors;
    guint64     next_vector;
} PnMessagePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PnMessage, pn_message, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_ID,
    PROP_TOPIC,
    PROP_CREATED,
    PROP_SOURCE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_message_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnMessage        *self = PN_MESSAGE (object);
    PnMessagePrivate *priv = pn_message_get_instance_private (self);

    switch (prop_id)
    {
    case PROP_ID:
        g_value_set_string (value, priv->id);
        break;
    case PROP_TOPIC:
        g_value_set_string (value, priv->topic);
        break;
    case PROP_CREATED:
        g_value_set_string (value, priv->created);
        break;
    case PROP_SOURCE:
        g_value_set_object (value, priv->source);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_message_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnMessage *self = PN_MESSAGE (object);

    switch (prop_id)
    {
    case PROP_ID:
        pn_message_set_id (self, g_value_get_string (value));
        break;
    case PROP_TOPIC:
        pn_message_set_topic (self, g_value_get_string (value));
        break;
    case PROP_CREATED:
        pn_message_set_created (self, g_value_get_string (value));
        break;
    case PROP_SOURCE:
        pn_message_set_source (self, g_value_get_object (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_message_dispose (GObject *object)
{
    PnMessage        *self = PN_MESSAGE (object);
    PnMessagePrivate *priv = pn_message_get_instance_private (self);

    g_clear_object (&priv->source);

    G_OBJECT_CLASS (pn_message_parent_class)->dispose (object);
}

static void
pn_message_finalize (GObject *object)
{
    PnMessage        *self = PN_MESSAGE (object);
    PnMessagePrivate *priv = pn_message_get_instance_private (self);

    g_clear_pointer (&priv->id,      g_free);
    g_clear_pointer (&priv->topic,   g_free);
    g_clear_pointer (&priv->created, g_free);
    g_clear_pointer (&priv->data,    json_object_unref);
    g_clear_pointer (&priv->vectors, g_hash_table_unref);

    G_OBJECT_CLASS (pn_message_parent_class)->finalize (object);
}

G_DEFINE_QUARK (pn-message-error-quark, pn_message_error)

/* ------------------------------------------------------------------ */
/*  Validation                                                         */
/*                                                                     */
/*  data.value / data.output / data.success are the universal three-   */
/*  field contract every wire-bound message must carry; the base       */
/*  implementation of the ::validate vfunc enforces it.                */
/* ------------------------------------------------------------------ */

static gboolean
mandatory_member (
        JsonObject   *data,
        const gchar  *name,
        GError      **error)
{
    if (!json_object_has_member (data, name))
    {
        g_set_error (error,
                     PN_MESSAGE_ERROR,
                     PN_MESSAGE_ERROR_MISSING_FIELD,
                     "mandatory data.%s is missing",
                     name);
        return FALSE;
    }
    return TRUE;
}

static gboolean
member_is_number_like (JsonNode *node)
{
    if (json_node_get_node_type (node) == JSON_NODE_VALUE)
    {
        GType vt = json_node_get_value_type (node);

        if (vt == G_TYPE_INT64  ||
            vt == G_TYPE_DOUBLE)
            return TRUE;

        /* A numeric string is acceptable too — producers that need
         * more range than a double (hex-encoded JSON-RPC quantities,
         * arbitrary-precision integers) put the magnitude in a
         * string and downstream nodes parse it on demand. */
        if (vt == G_TYPE_STRING)
            return TRUE;
    }
    return FALSE;
}

static gboolean
pn_message_real_validate (
        PnMessage  *self,
        GError    **error)
{
    PnMessagePrivate *priv = pn_message_get_instance_private (self);
    JsonObject       *data = priv->data;
    JsonNode         *node;

    if (!mandatory_member (data, "value",   error)) return FALSE;
    if (!mandatory_member (data, "output",  error)) return FALSE;
    if (!mandatory_member (data, "success", error)) return FALSE;

    node = json_object_get_member (data, "value");
    if (!member_is_number_like (node))
    {
        g_set_error (error,
                     PN_MESSAGE_ERROR,
                     PN_MESSAGE_ERROR_WRONG_TYPE,
                     "data.value must be a number or numeric string");
        return FALSE;
    }

    node = json_object_get_member (data, "output");
    if (json_node_get_node_type (node) != JSON_NODE_VALUE ||
        json_node_get_value_type (node) != G_TYPE_STRING)
    {
        g_set_error (error,
                     PN_MESSAGE_ERROR,
                     PN_MESSAGE_ERROR_WRONG_TYPE,
                     "data.output must be a string");
        return FALSE;
    }

    node = json_object_get_member (data, "success");
    if (json_node_get_node_type (node) != JSON_NODE_VALUE ||
        json_node_get_value_type (node) != G_TYPE_BOOLEAN)
    {
        g_set_error (error,
                     PN_MESSAGE_ERROR,
                     PN_MESSAGE_ERROR_WRONG_TYPE,
                     "data.success must be a boolean");
        return FALSE;
    }

    return TRUE;
}

static void
pn_message_class_init (PnMessageClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = pn_message_get_property;
    object_class->set_property = pn_message_set_property;
    object_class->dispose      = pn_message_dispose;
    object_class->finalize     = pn_message_finalize;

    klass->validate = pn_message_real_validate;

    props[PROP_ID] = g_param_spec_string (
            "id", "Id",
            "Unique message identifier; auto-generated when not set",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TOPIC] = g_param_spec_string (
            "topic", "Topic",
            "Optional short string tag for routing or filtering",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_CREATED] = g_param_spec_string (
            "created", "Created",
            "ISO-8601 timestamp with timezone offset captured when "
            "the message was constructed",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SOURCE] = g_param_spec_object (
            "source", "Source",
            "Node that emitted the message",
            PN_TYPE_NODE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_message_init (PnMessage *self)
{
    PnMessagePrivate *priv = pn_message_get_instance_private (self);
    GDateTime        *now  = g_date_time_new_now_local ();

    /* Auto-generate an id so freshly constructed messages are
     * already addressable.  The caller can still override it via the
     * "id" property. */
    priv->id      = g_uuid_string_random ();
    priv->topic   = NULL;
    /* ISO-8601 with timezone offset (e.g. "+02:00") so the recorded
     * instant is unambiguous across machines and DST shifts. */
    priv->created = g_date_time_format_iso8601 (now);
    priv->source  = NULL;
    priv->data    = json_object_new ();

    /* The vector registry is allocated on demand (see set_vector). */
    priv->vectors     = NULL;
    priv->next_vector = 1;   /* 0 is reserved as the "no handle" sentinel */

    g_date_time_unref (now);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnMessage *
pn_message_new (
        PnNode      *source,
        const gchar *topic)
{
    PnMessage *msg;
    gchar     *resolved = NULL;

    /* When the caller passes %NULL (or an empty string) for @topic and
     * a real source node is available, stamp the envelope with the
     * source node's resolved topic — the user-configured template with
     * ${nodeclass} / ${nodename} / ${hostname} substituted.  This is
     * the path every in-tree emitter takes; callers with a topic that
     * does not belong to the node (the MQTT subscriber forwarding the
     * broker-supplied topic, the HTTPS tunnel receiver replaying the
     * sender's envelope, PnChat forwarding the room topic the user
     * configured on the chat node itself) keep passing an explicit
     * @topic and bypass this defaulting. */
    if ((topic == NULL || *topic == '\0') && source != NULL)
    {
        resolved = pn_node_resolve_topic (source);
        topic    = resolved;
    }

    msg = g_object_new (PN_TYPE_MESSAGE,
                        "source", source,
                        "topic",  topic,
                        NULL);

    g_free (resolved);
    return msg;
}

const gchar *
pn_message_get_id (PnMessage *self)
{
    PnMessagePrivate *priv;
    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);
    priv = pn_message_get_instance_private (self);
    return priv->id;
}

void
pn_message_set_id (
        PnMessage   *self,
        const gchar *id)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));

    priv = pn_message_get_instance_private (self);
    if (g_strcmp0 (priv->id, id) == 0)
        return;

    g_free (priv->id);
    priv->id = g_strdup (id);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ID]);
}

const gchar *
pn_message_get_topic (PnMessage *self)
{
    PnMessagePrivate *priv;
    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);
    priv = pn_message_get_instance_private (self);
    return priv->topic;
}

void
pn_message_set_topic (
        PnMessage   *self,
        const gchar *topic)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));

    priv = pn_message_get_instance_private (self);
    if (g_strcmp0 (priv->topic, topic) == 0)
        return;

    g_free (priv->topic);
    priv->topic = g_strdup (topic);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TOPIC]);
}

const gchar *
pn_message_get_created (PnMessage *self)
{
    PnMessagePrivate *priv;
    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);
    priv = pn_message_get_instance_private (self);
    return priv->created;
}

void
pn_message_set_created (
        PnMessage   *self,
        const gchar *created)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));

    priv = pn_message_get_instance_private (self);
    if (g_strcmp0 (priv->created, created) == 0)
        return;

    g_free (priv->created);
    priv->created = g_strdup (created);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CREATED]);
}

PnNode *
pn_message_get_source (PnMessage *self)
{
    PnMessagePrivate *priv;
    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);
    priv = pn_message_get_instance_private (self);
    return priv->source;
}

void
pn_message_set_source (
        PnMessage *self,
        PnNode    *source)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));
    g_return_if_fail (source == NULL || PN_IS_NODE (source));

    priv = pn_message_get_instance_private (self);
    if (priv->source == source)
        return;

    g_set_object (&priv->source, source);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SOURCE]);
}

/* ------------------------------------------------------------------ */
/*  Data accessors                                                     */
/* ------------------------------------------------------------------ */

JsonObject *
pn_message_get_data (PnMessage *self)
{
    PnMessagePrivate *priv;
    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);
    priv = pn_message_get_instance_private (self);
    return priv->data;
}

void
pn_message_set_string (
        PnMessage   *self,
        const gchar *name,
        const gchar *value)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));
    g_return_if_fail (name != NULL);

    priv = pn_message_get_instance_private (self);
    json_object_set_string_member (priv->data, name, value ? value : "");
}

void
pn_message_set_int (
        PnMessage   *self,
        const gchar *name,
        gint         value)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));
    g_return_if_fail (name != NULL);

    priv = pn_message_get_instance_private (self);
    json_object_set_int_member (priv->data, name, (gint64) value);
}

void
pn_message_set_int64 (
        PnMessage   *self,
        const gchar *name,
        gint64       value)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));
    g_return_if_fail (name != NULL);

    priv = pn_message_get_instance_private (self);
    json_object_set_int_member (priv->data, name, value);
}

void
pn_message_set_double (
        PnMessage   *self,
        const gchar *name,
        gdouble      value)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));
    g_return_if_fail (name != NULL);

    priv = pn_message_get_instance_private (self);
    json_object_set_double_member (priv->data, name, value);
}

void
pn_message_set_boolean (
        PnMessage   *self,
        const gchar *name,
        gboolean     value)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));
    g_return_if_fail (name != NULL);

    priv = pn_message_get_instance_private (self);
    json_object_set_boolean_member (priv->data, name, value);
}

void
pn_message_set_member (
        PnMessage   *self,
        const gchar *name,
        JsonNode    *node)
{
    PnMessagePrivate *priv;

    g_return_if_fail (PN_IS_MESSAGE (self));
    g_return_if_fail (name != NULL);
    g_return_if_fail (node != NULL);

    priv = pn_message_get_instance_private (self);
    /* json_object_set_member takes ownership of @node. */
    json_object_set_member (priv->data, name, node);
}

JsonNode *
pn_message_get_member (
        PnMessage   *self,
        const gchar *name)
{
    PnMessagePrivate *priv;

    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);
    g_return_val_if_fail (name != NULL, NULL);

    priv = pn_message_get_instance_private (self);
    if (!json_object_has_member (priv->data, name))
        return NULL;

    return json_object_get_member (priv->data, name);
}

/* ------------------------------------------------------------------ */
/*  Large numeric vectors (TODO #43)                                   */
/* ------------------------------------------------------------------ */

/* Pull the handle out of a "$pnvector" marker node.  Returns 0 (the
 * no-handle sentinel) for anything that is not a well-formed marker. */
static guint64
vector_marker_handle (JsonNode *marker)
{
    JsonObject *obj;
    JsonNode   *h;

    if (marker == NULL || !JSON_NODE_HOLDS_OBJECT (marker))
        return 0;

    obj = json_node_get_object (marker);
    if (!json_object_has_member (obj, PN_MESSAGE_VECTOR_MARKER))
        return 0;

    h = json_object_get_member (obj, PN_MESSAGE_VECTOR_MARKER);
    if (h == NULL || !JSON_NODE_HOLDS_VALUE (h) ||
        json_node_get_value_type (h) != G_TYPE_INT64)
        return 0;

    return (guint64) json_node_get_int (h);
}

guint64
pn_message_set_vector (
        PnMessage   *self,
        const gchar *name,
        PnVector    *vec)
{
    PnMessagePrivate *priv;
    guint64           handle;
    JsonObject       *marker;

    g_return_val_if_fail (PN_IS_MESSAGE (self), 0);
    g_return_val_if_fail (name != NULL, 0);
    g_return_val_if_fail (PN_IS_VECTOR (vec), 0);

    priv = pn_message_get_instance_private (self);

    if (priv->vectors == NULL)
        priv->vectors = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                               g_free, g_object_unref);

    handle = priv->next_vector++;

    {
        gint64 *key = g_new (gint64, 1);
        *key = (gint64) handle;
        g_hash_table_insert (priv->vectors, key, g_object_ref (vec));
    }

    /* The reference in the data bag is a self-describing marker object so
     * it serialises as ordinary JSON and JSON-only consumers see a sane
     * {len,dtype} descriptor. */
    marker = json_object_new ();
    json_object_set_int_member    (marker, PN_MESSAGE_VECTOR_MARKER,
                                   (gint64) handle);
    json_object_set_int_member    (marker, "len",
                                   (gint64) pn_vector_get_len (vec));
    json_object_set_string_member (marker, "dtype", "f64");

    {
        JsonNode *node = json_node_new (JSON_NODE_OBJECT);
        json_node_take_object (node, marker);
        json_object_set_member (priv->data, name, node);   /* takes @node */
    }

    return handle;
}

PnVector *
pn_message_resolve_vector (
        PnMessage *self,
        JsonNode  *marker)
{
    PnMessagePrivate *priv;
    guint64           handle;
    gint64            key;

    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);

    handle = vector_marker_handle (marker);
    if (handle == 0)
        return NULL;

    priv = pn_message_get_instance_private (self);
    if (priv->vectors == NULL)
        return NULL;

    key = (gint64) handle;
    return g_hash_table_lookup (priv->vectors, &key);
}

gboolean
pn_message_has_member (
        PnMessage   *self,
        const gchar *name)
{
    PnMessagePrivate *priv;

    g_return_val_if_fail (PN_IS_MESSAGE (self), FALSE);
    g_return_val_if_fail (name != NULL, FALSE);

    priv = pn_message_get_instance_private (self);
    return json_object_has_member (priv->data, name);
}

gboolean
pn_message_validate (
        PnMessage  *self,
        GError    **error)
{
    PnMessageClass *klass;

    g_return_val_if_fail (PN_IS_MESSAGE (self), FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    klass = PN_MESSAGE_GET_CLASS (self);
    g_return_val_if_fail (klass->validate != NULL, FALSE);

    return klass->validate (self, error);
}

gboolean
pn_message_is_valid (PnMessage *self)
{
    return pn_message_validate (self, NULL);
}

/* ------------------------------------------------------------------ */
/*  Cloning                                                            */
/*                                                                     */
/*  The wire layer hands every fan-out branch its own copy so that a   */
/*  filter mutating its received message in place (Table Model, Set,   */
/*  Format, …) cannot contaminate a sibling branch wired to the same   */
/*  output.  The id is preserved across the fork so a Debug Print on   */
/*  one branch and a Debug Print on another can still be correlated    */
/*  back to the original emission by their shared id.                  */
/*                                                                     */
/*  json-glib's json_node_copy() is a deep copy only for scalar value  */
/*  nodes -- for OBJECT and ARRAY nodes it merely ref-counts the       */
/*  underlying container, so a naive clone built on top of it would    */
/*  still share the data bag with the original.  The helpers below     */
/*  walk every member recursively to produce a genuinely independent   */
/*  tree.                                                              */
/* ------------------------------------------------------------------ */

static JsonNode   *deep_copy_node   (JsonNode   *node);
static JsonObject *deep_copy_object (JsonObject *obj);
static JsonArray  *deep_copy_array  (JsonArray  *arr);

static JsonObject *
deep_copy_object (JsonObject *obj)
{
    JsonObject *copy = json_object_new ();
    GList      *members;
    GList      *l;

    members = json_object_get_members (obj);
    for (l = members; l != NULL; l = l->next)
    {
        const gchar *name  = l->data;
        JsonNode    *value = json_object_get_member (obj, name);
        json_object_set_member (copy, name, deep_copy_node (value));
    }
    g_list_free (members);

    return copy;
}

static JsonArray *
deep_copy_array (JsonArray *arr)
{
    JsonArray *copy = json_array_new ();
    guint      n    = json_array_get_length (arr);
    guint      i;

    for (i = 0; i < n; i++)
    {
        JsonNode *element = json_array_get_element (arr, i);
        json_array_add_element (copy, deep_copy_node (element));
    }

    return copy;
}

static JsonNode *
deep_copy_node (JsonNode *node)
{
    JsonNode *out;

    if (node == NULL)
        return NULL;

    switch (json_node_get_node_type (node))
    {
    case JSON_NODE_OBJECT:
        out = json_node_new (JSON_NODE_OBJECT);
        json_node_take_object (out,
                               deep_copy_object (json_node_get_object (node)));
        return out;

    case JSON_NODE_ARRAY:
        out = json_node_new (JSON_NODE_ARRAY);
        json_node_take_array (out,
                              deep_copy_array (json_node_get_array (node)));
        return out;

    case JSON_NODE_VALUE:
    case JSON_NODE_NULL:
    default:
        /* json_node_copy() is a true copy for value / null nodes
         * because there is no container to share. */
        return json_node_copy (node);
    }
}

PnMessage *
pn_message_clone (PnMessage *self)
{
    PnMessagePrivate *src;
    PnMessage        *copy;
    PnMessagePrivate *dst;

    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);

    src  = pn_message_get_instance_private (self);
    /* Construct an instance of the *concrete* type, not the base, so a
     * subclass (e.g. PnImageMessage) survives the fan-out clone the
     * wire layer performs.  The private struct lives on PN_TYPE_MESSAGE
     * so pn_message_get_instance_private() resolves correctly on the
     * derived instance too. */
    copy = g_object_new (G_OBJECT_TYPE (self), NULL);
    dst  = pn_message_get_instance_private (copy);

    /* Replace the auto-generated id/created of the freshly-constructed
     * clone with the originals so traceability across fan-out branches
     * is preserved. */
    g_free (dst->id);
    g_free (dst->created);
    dst->id      = g_strdup (src->id);
    dst->created = g_strdup (src->created);
    dst->topic   = g_strdup (src->topic);

    if (src->source != NULL)
        g_set_object (&dst->source, src->source);

    /* Replace the freshly-constructed clone's empty data bag with a
     * recursive deep-copy of the source's bag -- the two trees share
     * no JsonObject / JsonArray container, so mutations on either
     * side leave the other untouched. */
    json_object_unref (dst->data);
    dst->data = deep_copy_object (src->data);

    /* Give a subclass the chance to carry its out-of-band payload (the
     * image pointer in PnImageMessage, …) across the clone.  The base
     * class leaves the hook %NULL. */
    {
        PnMessageClass *klass = PN_MESSAGE_GET_CLASS (self);
        if (klass->copy_private != NULL)
            klass->copy_private (copy, self);
    }

    return copy;
}
