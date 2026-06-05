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

#include <string.h>

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

/* Recursive deep-copy of a JSON tree (defined in the cloning section
 * below); used here to fold a parsed data bag off the parser tree. */
static JsonNode *deep_copy_node (JsonNode *node);

/* ------------------------------------------------------------------ */
/*  Serialization (TODO #43.4)                                         */
/*                                                                     */
/*  A naive json_to_string() emits a "$pnvector" marker literally and  */
/*  SILENTLY drops the buffer it stands for, leaving a dangling handle */
/*  on the way back.  We wrap json-glib with a marker-walk so the      */
/*  out-of-band payload travels with the message: every referenced     */
/*  #PnVector is base64-encoded (little-endian f64, for cross-host     */
/*  safety) into a sibling "blobs" section keyed by its handle.        */
/* ------------------------------------------------------------------ */

/* Base64 of @vec's doubles, serialised little-endian so the bytes mean
 * the same thing on a big-endian host.  Returns "" for the empty vector. */
static gchar *
vector_to_base64 (PnVector *vec)
{
    gsize          len  = pn_vector_get_len  (vec);
    const gdouble *data = pn_vector_get_data (vec);
    guchar        *bytes;
    gchar         *b64;
    gsize          i;

    if (len == 0)
        return g_strdup ("");

    bytes = g_malloc (len * sizeof (gdouble));
    for (i = 0; i < len; i++)
    {
        guint64 bits;
        memcpy (&bits, &data[i], sizeof bits);
        bits = GUINT64_TO_LE (bits);
        memcpy (bytes + i * sizeof bits, &bits, sizeof bits);
    }

    b64 = g_base64_encode (bytes, len * sizeof (gdouble));
    g_free (bytes);
    return b64;
}

/* Decode a little-endian f64 base64 blob of @expect_len elements into a
 * fresh #PnVector.  Validates len*8 == nbytes so a truncated/garbled blob
 * fails loudly rather than rehydrating silent garbage. */
static PnVector *
vector_from_base64 (const gchar  *b64,
                    gsize         expect_len,
                    GError      **error)
{
    guchar  *bytes;
    gsize    nbytes = 0;
    gdouble *out    = NULL;
    gsize    i;

    bytes = g_base64_decode (b64 != NULL ? b64 : "", &nbytes);

    if (nbytes != expect_len * sizeof (gdouble))
    {
        g_set_error (error,
                     PN_MESSAGE_ERROR,
                     PN_MESSAGE_ERROR_WRONG_TYPE,
                     "vector blob is %" G_GSIZE_FORMAT " bytes, "
                     "expected %" G_GSIZE_FORMAT " for len=%" G_GSIZE_FORMAT,
                     nbytes, expect_len * sizeof (gdouble), expect_len);
        g_free (bytes);
        return NULL;
    }

    if (expect_len > 0)
    {
        out = g_malloc (nbytes);
        for (i = 0; i < expect_len; i++)
        {
            guint64 bits;
            memcpy (&bits, bytes + i * sizeof bits, sizeof bits);
            bits = GUINT64_FROM_LE (bits);
            memcpy (&out[i], &bits, sizeof bits);
        }
    }

    g_free (bytes);
    return pn_vector_new_take (out, expect_len);   /* adopts @out */
}

/* Deep-walk @node for "$pnvector" markers, externalising each referenced
 * buffer into @blobs (keyed by decimal handle) exactly once.  A marker
 * whose handle does not resolve (a stray descriptor) is left alone. */
static void
externalize_blobs (PnMessage  *self,
                   JsonNode   *node,
                   JsonObject *blobs)
{
    if (node == NULL)
        return;

    switch (json_node_get_node_type (node))
    {
    case JSON_NODE_OBJECT:
    {
        guint64 handle = vector_marker_handle (node);

        if (handle != 0)
        {
            gchar key[32];

            g_snprintf (key, sizeof key, "%" G_GUINT64_FORMAT, handle);
            if (!json_object_has_member (blobs, key))
            {
                PnVector *vec = pn_message_resolve_vector (self, node);
                if (vec != NULL)
                {
                    JsonObject *blob = json_object_new ();
                    gchar      *b64  = vector_to_base64 (vec);

                    json_object_set_string_member (blob, "dtype", "f64");
                    json_object_set_int_member    (blob, "len",
                            (gint64) pn_vector_get_len (vec));
                    json_object_set_string_member (blob, "b64", b64);
                    json_object_set_object_member (blobs, key, blob);
                    g_free (b64);
                }
            }
            /* A marker carries only scalar len/dtype besides the handle —
             * no nested markers to recurse into. */
        }
        else
        {
            JsonObject *obj     = json_node_get_object (node);
            GList      *members = json_object_get_members (obj);
            GList      *l;

            for (l = members; l != NULL; l = l->next)
                externalize_blobs (self,
                                   json_object_get_member (obj, l->data),
                                   blobs);
            g_list_free (members);
        }
        break;
    }

    case JSON_NODE_ARRAY:
    {
        JsonArray *arr = json_node_get_array (node);
        guint      n   = json_array_get_length (arr);
        guint      i;

        for (i = 0; i < n; i++)
            externalize_blobs (self, json_array_get_element (arr, i), blobs);
        break;
    }

    default:
        break;
    }
}

/* Largest handle referenced by any marker in @node (0 if none), so a
 * rehydrated message can resume allocating handles past the ones already
 * baked into its markers — no marker rewrite needed. */
static guint64
max_marker_handle (JsonNode *node)
{
    guint64 max = 0;

    if (node == NULL)
        return 0;

    switch (json_node_get_node_type (node))
    {
    case JSON_NODE_OBJECT:
    {
        guint64 handle = vector_marker_handle (node);

        if (handle != 0)
        {
            max = handle;
        }
        else
        {
            JsonObject *obj     = json_node_get_object (node);
            GList      *members = json_object_get_members (obj);
            GList      *l;

            for (l = members; l != NULL; l = l->next)
            {
                guint64 h = max_marker_handle (
                        json_object_get_member (obj, l->data));
                if (h > max)
                    max = h;
            }
            g_list_free (members);
        }
        break;
    }

    case JSON_NODE_ARRAY:
    {
        JsonArray *arr = json_node_get_array (node);
        guint      n   = json_array_get_length (arr);
        guint      i;

        for (i = 0; i < n; i++)
        {
            guint64 h = max_marker_handle (json_array_get_element (arr, i));
            if (h > max)
                max = h;
        }
        break;
    }

    default:
        break;
    }

    return max;
}

gchar *
pn_message_serialize (
        PnMessage *self,
        gboolean   include_blobs)
{
    PnMessagePrivate *priv;
    PnNode           *source;
    const gchar      *src_name = NULL;
    const gchar      *src_uuid;
    JsonObject       *obj;
    JsonNode         *data_node;
    JsonNode         *root;
    JsonGenerator    *gen;
    gchar            *out;

    g_return_val_if_fail (PN_IS_MESSAGE (self), NULL);

    priv     = pn_message_get_instance_private (self);
    source   = priv->source;
    src_uuid = source != NULL ? pn_node_get_uuid (source) : NULL;

    if (source != NULL)
    {
        src_name = pn_node_get_name (source);
        if (src_name == NULL || *src_name == '\0')
            src_name = pn_node_get_class_name (source);
    }

    obj = json_object_new ();
    json_object_set_string_member (obj, "type", G_OBJECT_TYPE_NAME (self));
    json_object_set_string_member (obj, "from",    src_name     ? src_name     : "");
    json_object_set_string_member (obj, "from_id", src_uuid     ? src_uuid     : "");
    json_object_set_string_member (obj, "topic",   priv->topic  ? priv->topic  : "");
    json_object_set_string_member (obj, "id",      priv->id     ? priv->id     : "");
    json_object_set_string_member (obj, "created", priv->created ? priv->created : "");

    /* Reference (not copy) the live data bag — the generator only reads,
     * and dropping @root below releases just the borrowed ref. */
    data_node = json_node_new (JSON_NODE_OBJECT);
    json_node_set_object   (data_node, priv->data);
    json_object_set_member (obj, "data", data_node);

    if (include_blobs && priv->vectors != NULL)
    {
        JsonObject *blobs = json_object_new ();

        externalize_blobs (self, data_node, blobs);
        if (json_object_get_size (blobs) > 0)
            json_object_set_object_member (obj, "blobs", blobs);
        else
            json_object_unref (blobs);
    }

    root = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (root, obj);
    gen  = json_generator_new ();
    json_generator_set_root (gen, root);
    out  = json_generator_to_data (gen, NULL);

    g_object_unref (gen);
    json_node_free (root);
    return out;
}

/* Pull the elements out of one "blobs" entry and register the decoded
 * #PnVector under @handle in @msg's registry.  Returns %FALSE (with
 * @error set) on a malformed or non-f64 blob. */
static gboolean
ingest_blob (PnMessage    *msg,
             guint64       handle,
             JsonObject   *blob,
             GError      **error)
{
    PnMessagePrivate *priv = pn_message_get_instance_private (msg);
    const gchar      *dtype;
    gint64            len;
    const gchar      *b64;
    PnVector         *vec;
    gint64           *key;

    if (!json_object_has_member (blob, "b64") ||
        !json_object_has_member (blob, "len"))
    {
        g_set_error (error, PN_MESSAGE_ERROR, PN_MESSAGE_ERROR_MISSING_FIELD,
                     "vector blob %" G_GUINT64_FORMAT
                     " is missing its len/b64", handle);
        return FALSE;
    }

    dtype = json_object_has_member (blob, "dtype")
            ? json_object_get_string_member (blob, "dtype") : "f64";
    if (g_strcmp0 (dtype, "f64") != 0)
    {
        g_set_error (error, PN_MESSAGE_ERROR, PN_MESSAGE_ERROR_WRONG_TYPE,
                     "vector blob %" G_GUINT64_FORMAT
                     " has unsupported dtype \"%s\"", handle, dtype);
        return FALSE;
    }

    len = json_object_get_int_member    (blob, "len");
    b64 = json_object_get_string_member (blob, "b64");
    if (len < 0)
    {
        g_set_error (error, PN_MESSAGE_ERROR, PN_MESSAGE_ERROR_WRONG_TYPE,
                     "vector blob %" G_GUINT64_FORMAT
                     " has a negative length", handle);
        return FALSE;
    }

    vec = vector_from_base64 (b64, (gsize) len, error);
    if (vec == NULL)
        return FALSE;

    if (priv->vectors == NULL)
        priv->vectors = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                               g_free, g_object_unref);
    key  = g_new (gint64, 1);
    *key = (gint64) handle;
    g_hash_table_insert (priv->vectors, key, vec);   /* adopts @vec's ref */
    return TRUE;
}

PnMessage *
pn_message_deserialize (
        const gchar  *json,
        GError      **error)
{
    JsonParser *parser = json_parser_new ();
    JsonNode   *root;
    JsonObject *obj;
    JsonObject *data  = NULL;
    JsonObject *blobs = NULL;
    const gchar *topic = NULL;
    const gchar *id    = NULL;
    const gchar *created = NULL;
    PnMessage  *msg;
    GError     *perr  = NULL;
    GList      *members, *l;
    guint64     max_handle;

    if (!json_parser_load_from_data (parser, json != NULL ? json : "",
                                     -1, &perr))
    {
        g_set_error (error, PN_MESSAGE_ERROR, PN_MESSAGE_ERROR_WRONG_TYPE,
                     "cannot parse message JSON: %s",
                     perr != NULL ? perr->message : "(parse error)");
        g_clear_error (&perr);
        g_object_unref (parser);
        return NULL;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_set_error (error, PN_MESSAGE_ERROR, PN_MESSAGE_ERROR_WRONG_TYPE,
                     "message JSON must be a JSON object");
        g_object_unref (parser);
        return NULL;
    }
    obj = json_node_get_object (root);

    /* Full envelope (a "data" object member) vs. a bare data bag. */
    if (json_object_has_member (obj, "data") &&
        JSON_NODE_HOLDS_OBJECT (json_object_get_member (obj, "data")))
    {
        data = json_object_get_object_member (obj, "data");

        if (json_object_has_member (obj, "topic"))
            topic = json_object_get_string_member (obj, "topic");
        if (json_object_has_member (obj, "id"))
            id = json_object_get_string_member (obj, "id");
        if (json_object_has_member (obj, "created"))
            created = json_object_get_string_member (obj, "created");
        if (json_object_has_member (obj, "blobs") &&
            JSON_NODE_HOLDS_OBJECT (json_object_get_member (obj, "blobs")))
            blobs = json_object_get_object_member (obj, "blobs");
    }
    else
    {
        data = obj;     /* the whole object is the data bag */
    }

    msg = pn_message_new (NULL, (topic != NULL && *topic != '\0')
                          ? topic : NULL);
    if (id != NULL && *id != '\0')
        pn_message_set_id (msg, id);
    if (created != NULL && *created != '\0')
        pn_message_set_created (msg, created);

    /* Fold the data bag in, deep-copied off the soon-to-be-freed parser
     * tree so the message owns an independent copy (markers and all). */
    members = json_object_get_members (data);
    for (l = members; l != NULL; l = l->next)
    {
        const gchar *name   = l->data;
        JsonNode    *member = json_object_get_member (data, name);
        pn_message_set_member (msg, name, deep_copy_node (member));
    }
    g_list_free (members);

    /* Rehydrate the out-of-band buffers under their original handles so
     * the in-data markers resolve again. */
    if (blobs != NULL)
    {
        members = json_object_get_members (blobs);
        for (l = members; l != NULL; l = l->next)
        {
            const gchar *key   = l->data;
            JsonNode    *bnode = json_object_get_member (blobs, key);
            guint64      handle;

            handle = (guint64) g_ascii_strtoull (key, NULL, 10);
            if (handle == 0 || !JSON_NODE_HOLDS_OBJECT (bnode))
            {
                g_set_error (error, PN_MESSAGE_ERROR,
                             PN_MESSAGE_ERROR_WRONG_TYPE,
                             "malformed blobs entry \"%s\"", key);
                g_list_free (members);
                g_object_unref (parser);
                g_object_unref (msg);
                return NULL;
            }

            if (!ingest_blob (msg, handle, json_node_get_object (bnode),
                              error))
            {
                g_list_free (members);
                g_object_unref (parser);
                g_object_unref (msg);
                return NULL;
            }
        }
        g_list_free (members);
    }

    /* Resume the handle namespace past every handle already baked into a
     * marker (and thus every rehydrated blob, which shares those handles)
     * so a fresh pn_message_set_vector() on this message cannot collide. */
    {
        PnMessagePrivate *priv = pn_message_get_instance_private (msg);
        JsonObject       *bag  = priv->data;
        GList            *bm   = json_object_get_members (bag);
        GList            *bl;

        max_handle = 0;
        for (bl = bm; bl != NULL; bl = bl->next)
        {
            guint64 h = max_marker_handle (
                    json_object_get_member (bag, bl->data));
            if (h > max_handle)
                max_handle = h;
        }
        g_list_free (bm);

        if (max_handle + 1 > priv->next_vector)
            priv->next_vector = max_handle + 1;
    }

    g_object_unref (parser);
    return msg;
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

    /* Share the out-of-band vector payloads (TODO #43): the deep-copied
     * data bag already carries the "$pnvector" markers verbatim, so we
     * only need to make their handles resolve on the clone.  REF-share
     * the buffers rather than copying megabytes -- every fan-out branch
     * holds the same immutable #PnVector, exactly the trick
     * PnImageMessage uses for its pixbuf.  Handles are copied unchanged
     * so the markers keep resolving; next_vector carries over so a fresh
     * set_vector() on the clone never collides with a shared handle. */
    if (src->vectors != NULL)
    {
        GHashTableIter iter;
        gpointer       key, value;

        dst->vectors = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                               g_free, g_object_unref);
        g_hash_table_iter_init (&iter, src->vectors);
        while (g_hash_table_iter_next (&iter, &key, &value))
        {
            gint64 *k = g_new (gint64, 1);
            *k = *(const gint64 *) key;
            g_hash_table_insert (dst->vectors, k,
                                 g_object_ref (PN_VECTOR (value)));
        }
    }
    dst->next_vector = src->next_vector;

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
