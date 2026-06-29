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

#include "pn-shell-output.h"

#include <json-glib/json-glib.h>

GType
pn_shell_output_format_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_SHELL_OUTPUT_TEXT, "PN_SHELL_OUTPUT_TEXT", "text" },
            { PN_SHELL_OUTPUT_JSON, "PN_SHELL_OUTPUT_JSON", "JSON" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnShellOutputFormat", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Set @msg's data.value from a non-object JSON node (scalar/array/null).
 * A boolean is coerced to 1.0/0.0 so it honours the bag's boolean
 * convention; a number stays a number; everything else is stored
 * verbatim under `value` (mirroring how the JMESPath node wraps a
 * non-object result). */
static void
apply_scalar_value (PnMessage *msg,
                    JsonNode  *root)
{
    if (JSON_NODE_HOLDS_VALUE (root))
    {
        GType vt = json_node_get_value_type (root);

        if (vt == G_TYPE_BOOLEAN)
            pn_message_set_double (msg, "value",
                                   json_node_get_boolean (root) ? 1.0 : 0.0);
        else if (vt == G_TYPE_INT64)
            pn_message_set_int64 (msg, "value", json_node_get_int (root));
        else if (vt == G_TYPE_DOUBLE)
            pn_message_set_double (msg, "value", json_node_get_double (root));
        else if (vt == G_TYPE_STRING)
            pn_message_set_string (msg, "value", json_node_get_string (root));
        else
            pn_message_set_member (msg, "value", json_node_copy (root));
    }
    else
    {
        /* Array or null: keep the structure verbatim under `value`. */
        pn_message_set_member (msg, "value", json_node_copy (root));
    }
}

/* Deep-merge every member of @obj onto @msg's data bag.  The script owns
 * the payload here, so its own `success` / `output` members (if present)
 * win over the baseline set just before this call. */
static void
merge_object (PnMessage  *msg,
              JsonObject *obj)
{
    GList *members = json_object_get_members (obj);
    GList *l;

    for (l = members; l != NULL; l = l->next)
    {
        const gchar *name   = l->data;
        JsonNode    *member = json_object_get_member (obj, name);

        pn_message_set_member (msg, name, json_node_copy (member));
    }

    g_list_free (members);
}

/* The human-readable baseline for `data.output` in JSON mode: the raw
 * stdout with edge whitespace stripped (so the captured trailing newline
 * does not ride along). */
static void
set_output_baseline (PnMessage   *msg,
                     const gchar *body)
{
    gchar *trimmed = g_strdup (body != NULL ? body : "");

    g_strstrip (trimmed);
    pn_message_set_string (msg, "output", trimmed);
    g_free (trimmed);
}

/* ------------------------------------------------------------------ */
/*  Public                                                             */
/* ------------------------------------------------------------------ */

void
pn_shell_output_apply (PnMessage           *msg,
                       PnShellOutputFormat  format,
                       gboolean             success,
                       const gchar         *stdout_text,
                       const gchar         *stderr_text)
{
    gchar *combined;

    g_return_if_fail (PN_IS_MESSAGE (msg));

    combined = g_strconcat (stdout_text != NULL ? stdout_text : "",
                            stderr_text != NULL ? stderr_text : "",
                            NULL);

    if (format == PN_SHELL_OUTPUT_JSON)
    {
        JsonParser  *parser = json_parser_new ();
        GError      *perr   = NULL;
        const gchar *body   = (stdout_text != NULL) ? stdout_text : "";
        JsonNode    *root   = NULL;

        if (json_parser_load_from_data (parser, body, -1, &perr))
            root = json_parser_get_root (parser);

        if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
        {
            /* Baseline first, then let the object's members override. */
            pn_message_set_boolean (msg, "success", success);
            set_output_baseline (msg, body);
            merge_object (msg, json_node_get_object (root));
        }
        else if (root != NULL)
        {
            pn_message_set_boolean (msg, "success", success);
            set_output_baseline (msg, body);
            apply_scalar_value (msg, root);
        }
        else
        {
            /* Not valid JSON (or empty stdout): report it as a failure
             * with the raw text, so the user can see what the script
             * actually printed.  Errors surface via the message, never
             * stderr (invisible from a desktop launch). */
            pn_message_set_boolean (msg, "success", FALSE);

            if (*combined != '\0')
                pn_message_set_string (msg, "output", combined);
            else
                pn_message_set_string (
                        msg, "output",
                        (perr != NULL) ? perr->message
                                       : "shell output is not valid JSON");
        }

        g_clear_error (&perr);
        g_object_unref (parser);
    }
    else
    {
        /* Text mode — the historical contract, verbatim. */
        pn_message_set_boolean (msg, "success", success);
        pn_message_set_string  (msg, "output",  combined);
    }

    g_free (combined);
}
