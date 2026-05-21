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

#include "pn-tasmota-common.h"

gchar *
pn_tasmota_device_from_topic (const gchar *topic)
{
    gchar **parts;
    gint    n;
    gchar  *out = NULL;

    if (topic == NULL || *topic == '\0')
        return NULL;

    parts = g_strsplit (topic, "/", -1);
    n     = (gint) g_strv_length (parts);

    /* Tasmota's <prefix>/<device>/<command> topology means the device
     * name lives at the second-to-last position regardless of what
     * the family prefix is.  A two-segment topic like "tele/tasmota13"
     * still yields "tele" via this rule, which is wrong; require at
     * least three segments before trusting the second-to-last. */
    if (n >= 3)
    {
        const gchar *seg = parts[n - 2];
        if (seg != NULL && *seg != '\0')
            out = g_strdup (seg);
    }

    g_strfreev (parts);
    return out;
}

/* Read a JsonNode's value as a double when it holds a JSON number. */
static gboolean
node_as_double (JsonNode *node, gdouble *out)
{
    GType t;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    t = json_node_get_value_type (node);
    if (t != G_TYPE_DOUBLE && t != G_TYPE_INT64)
        return FALSE;

    *out = json_node_get_double (node);
    return TRUE;
}

gboolean
pn_tasmota_find_number (
        JsonObject  *payload,
        const gchar *key,
        gdouble     *out_value)
{
    GList *members;
    GList *iter;

    if (payload == NULL || key == NULL || out_value == NULL)
        return FALSE;

    /* Top-level hit first (some Tasmota templates promote the reading
     * up out of the per-sensor wrapper). */
    if (json_object_has_member (payload, key) &&
        node_as_double (json_object_get_member (payload, key), out_value))
        return TRUE;

    /* Otherwise probe each direct-child object.  json_object_get_members
     * returns a borrowed list of the payload's own member names; we
     * own the list itself and free it when done. */
    members = json_object_get_members (payload);
    for (iter = members; iter != NULL; iter = iter->next)
    {
        const gchar *name  = iter->data;
        JsonNode    *child = json_object_get_member (payload, name);
        JsonObject  *obj;

        if (child == NULL || !JSON_NODE_HOLDS_OBJECT (child))
            continue;

        obj = json_node_get_object (child);
        if (obj == NULL || !json_object_has_member (obj, key))
            continue;

        if (node_as_double (json_object_get_member (obj, key), out_value))
        {
            g_list_free (members);
            return TRUE;
        }
    }
    g_list_free (members);

    return FALSE;
}

const gchar *
pn_tasmota_find_top_string (
        JsonObject  *payload,
        const gchar *key)
{
    JsonNode *node;

    if (payload == NULL || key == NULL)
        return NULL;
    if (!json_object_has_member (payload, key))
        return NULL;

    node = json_object_get_member (payload, key);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return NULL;
    if (json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string (node);
}
