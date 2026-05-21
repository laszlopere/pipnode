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

#include "pn-json-path.h"

JsonNode *
pn_json_resolve_path (
        JsonObject  *root,
        const gchar *path)
{
    JsonNode  *cur = NULL;
    gchar    **parts;
    guint      i;

    if (root == NULL || path == NULL || *path == '\0')
        return NULL;

    parts = g_strsplit (path, "/", -1);

    for (i = 0; parts[i] != NULL; i++)
    {
        /* Tolerate leading/trailing/double slashes — splits come back
         * with empty segments that should be skipped, not cause the
         * whole resolution to fail. */
        if (*parts[i] == '\0')
            continue;

        if (cur == NULL)
        {
            if (!json_object_has_member (root, parts[i]))
            {
                cur = NULL;
                break;
            }
            cur = json_object_get_member (root, parts[i]);
        }
        else
        {
            JsonObject *obj;

            if (!JSON_NODE_HOLDS_OBJECT (cur))
            {
                cur = NULL;
                break;
            }
            obj = json_node_get_object (cur);
            if (!json_object_has_member (obj, parts[i]))
            {
                cur = NULL;
                break;
            }
            cur = json_object_get_member (obj, parts[i]);
        }
    }

    g_strfreev (parts);
    return cur;
}

JsonObject *
pn_json_lookup_root_for_message (PnMessage *message)
{
    JsonObject  *root    = json_object_new ();
    JsonObject  *data    = pn_message_get_data    (message);
    const gchar *topic   = pn_message_get_topic   (message);
    const gchar *id      = pn_message_get_id      (message);
    const gchar *created = pn_message_get_created (message);
    JsonNode    *data_node;

    json_object_set_string_member (root, "topic",   topic   ? topic   : "");
    json_object_set_string_member (root, "id",      id      ? id      : "");
    json_object_set_string_member (root, "created", created ? created : "");

    /* The data node references — does not copy — the live data bag.
     * That keeps placeholder expansion cheap and means callers see
     * the same object the rest of the pipeline is mutating. */
    data_node = json_node_new (JSON_NODE_OBJECT);
    json_node_set_object   (data_node, data);
    json_object_set_member (root, "data", data_node);

    return root;
}
