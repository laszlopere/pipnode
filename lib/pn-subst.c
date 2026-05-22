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

#include "pn-subst.h"
#include "pn-json-path.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/*  Value renderers                                                    */
/* ------------------------------------------------------------------ */

/** Append @s to @out with JSON string-escape rules applied (control
 *  set, backslash, double-quote).  Multi-byte UTF-8 sequences pass
 *  through verbatim. */
static void
append_json_escaped (GString *out, const gchar *s)
{
    if (s == NULL)
        return;

    for (const gchar *p = s; *p != '\0'; p++)
    {
        guchar c = (guchar) *p;
        switch (c)
        {
        case '"':  g_string_append (out, "\\\""); break;
        case '\\': g_string_append (out, "\\\\"); break;
        case '\b': g_string_append (out, "\\b");  break;
        case '\f': g_string_append (out, "\\f");  break;
        case '\n': g_string_append (out, "\\n");  break;
        case '\r': g_string_append (out, "\\r");  break;
        case '\t': g_string_append (out, "\\t");  break;
        default:
            if (c < 0x20)
                g_string_append_printf (out, "\\u%04x", c);
            else
                g_string_append_c (out, *p);
        }
    }
}

/** Render @node into @out as JSON suitable for inlining into a JSON
 *  document.  Two contexts:
 *
 *    in_string = FALSE — the cursor is in a value slot (after `:` or
 *      `,` / `[`).  Strings emerge quoted; numbers, booleans, null
 *      emerge as their bare JSON tokens; objects / arrays emerge as
 *      compact JSON.
 *
 *    in_string = TRUE  — the cursor is inside a JSON string literal.
 *      Strings emerge as their raw contents with JSON escapes (no
 *      surrounding quotes), so a placeholder like
 *      "rewritten/${topic}" expands inline.  Scalars and structured
 *      nodes serialise to their JSON form and then have their special
 *      characters escaped so the surrounding string stays well-formed.
 *
 *  json-glib's #JsonGenerator only accepts an object or array root,
 *  so the scalar branch hand-formats numbers / booleans / null. */
static void
render_node_json (JsonNode *node, gboolean in_string, GString *out)
{
    if (node == NULL || JSON_NODE_HOLDS_NULL (node))
    {
        g_string_append (out, "null");
        return;
    }

    if (JSON_NODE_HOLDS_VALUE (node))
    {
        GType vtype = json_node_get_value_type (node);

        if (vtype == G_TYPE_STRING)
        {
            const gchar *s = json_node_get_string (node);
            if (in_string)
            {
                append_json_escaped (out, s);
            }
            else
            {
                g_string_append_c (out, '"');
                append_json_escaped (out, s);
                g_string_append_c (out, '"');
            }
            return;
        }
        if (vtype == G_TYPE_INT64)
        {
            g_string_append_printf (out, "%" G_GINT64_FORMAT,
                                    json_node_get_int (node));
            return;
        }
        if (vtype == G_TYPE_DOUBLE)
        {
            gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_dtostr (buf, sizeof buf, json_node_get_double (node));
            g_string_append (out, buf);
            return;
        }
        if (vtype == G_TYPE_BOOLEAN)
        {
            g_string_append (out,
                json_node_get_boolean (node) ? "true" : "false");
            return;
        }

        g_string_append (out, "null");
        return;
    }

    {
        JsonGenerator *gen = json_generator_new ();
        gchar         *json;

        json_generator_set_root (gen, node);
        json = json_generator_to_data (gen, NULL);
        g_object_unref (gen);

        if (json == NULL)
        {
            g_string_append (out, "null");
            return;
        }

        if (in_string)
            append_json_escaped (out, json);
        else
            g_string_append (out, json);

        g_free (json);
    }
}

/** Render @node into @out as plain text suitable for inlining into a
 *  text template.  Scalars use their natural textual form; a JSON null
 *  or a missing node renders as nothing; objects and arrays fall back
 *  to compact JSON so a template author still gets something readable
 *  when they reference a non-leaf path. */
static void
render_node_text (JsonNode *node, GString *out)
{
    if (node == NULL || JSON_NODE_HOLDS_NULL (node))
        return;

    if (JSON_NODE_HOLDS_VALUE (node))
    {
        GType vtype = json_node_get_value_type (node);

        if (vtype == G_TYPE_STRING)
        {
            g_string_append (out, json_node_get_string (node));
            return;
        }
        if (vtype == G_TYPE_INT64)
        {
            g_string_append_printf (out, "%" G_GINT64_FORMAT,
                                    json_node_get_int (node));
            return;
        }
        if (vtype == G_TYPE_DOUBLE)
        {
            g_string_append_printf (out, "%g", json_node_get_double (node));
            return;
        }
        if (vtype == G_TYPE_BOOLEAN)
        {
            g_string_append (out,
                json_node_get_boolean (node) ? "true" : "false");
            return;
        }
    }

    {
        JsonGenerator *gen = json_generator_new ();
        gchar         *s;

        json_generator_set_root (gen, node);
        s = json_generator_to_data (gen, NULL);
        g_object_unref (gen);

        if (s != NULL)
        {
            g_string_append (out, s);
            g_free (s);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Engine                                                             */
/* ------------------------------------------------------------------ */

/** Walk the resolver chain for @key.  On the first non-NULL hit, store
 *  the node and its ownership flag and return %TRUE; otherwise %FALSE. */
static gboolean
resolve_chain (PnSubstResolver **resolvers,
               const gchar      *key,
               JsonNode        **out_node,
               gboolean         *out_owned)
{
    if (resolvers == NULL)
        return FALSE;

    for (guint i = 0; resolvers[i] != NULL; i++)
    {
        PnSubstResolver *r     = resolvers[i];
        gboolean         owned = FALSE;
        JsonNode        *node  = r->resolve (r, key, &owned);

        if (node != NULL)
        {
            *out_node  = node;
            *out_owned = owned;
            return TRUE;
        }
    }

    return FALSE;
}

/** Render the value (or miss) for a single "${key}" into @out. */
static void
render_key (const PnSubstContext *ctx,
            const gchar          *key,
            gboolean              in_string,
            GString              *out)
{
    JsonNode *node  = NULL;
    gboolean  owned = FALSE;

    if (!resolve_chain (ctx->resolvers, key, &node, &owned))
    {
        switch (ctx->miss)
        {
        case PN_SUBST_MISS_EMPTY:
            break;
        case PN_SUBST_MISS_NULL:
            g_string_append (out, "null");
            break;
        case PN_SUBST_MISS_VERBATIM:
            g_string_append (out, "${");
            g_string_append (out, key);
            g_string_append_c (out, '}');
            break;
        }
        return;
    }

    if (ctx->mode == PN_SUBST_JSON)
        render_node_json (node, in_string, out);
    else
        render_node_text (node, out);

    if (owned)
        json_node_unref (node);
}

gchar *
pn_subst_expand (const gchar          *tmpl,
                 const PnSubstContext *ctx)
{
    GString     *out;
    const gchar *p;
    gboolean     json      = (ctx->mode == PN_SUBST_JSON);
    gboolean     in_string = FALSE;
    gboolean     escaped   = FALSE;

    if (tmpl == NULL)
        return g_strdup ("");

    out = g_string_new (NULL);
    p   = tmpl;

    while (*p != '\0')
    {
        /* In JSON mode the per-character string-state update happens
         * after dispatch so the in_string bit reflects the context the
         * placeholder sits inside, not the character we are about to
         * consume; a backslash-escaped "${" is left alone.  In text
         * mode there is no string context, so every "${" dispatches. */
        if ((!json || !escaped) && p[0] == '$' && p[1] == '{')
        {
            const gchar *end = strchr (p + 2, '}');

            if (end != NULL)
            {
                gchar *key = g_strndup (p + 2, end - (p + 2));

                render_key (ctx, key, in_string, out);

                g_free (key);
                p = end + 1;
                continue;
            }
        }

        if (json)
        {
            if (escaped)
            {
                escaped = FALSE;
            }
            else if (in_string)
            {
                if (*p == '\\')
                    escaped = TRUE;
                else if (*p == '"')
                    in_string = FALSE;
            }
            else if (*p == '"')
            {
                in_string = TRUE;
            }
        }

        g_string_append_c (out, *p);
        p++;
    }

    return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Built-in resolvers                                                 */
/* ------------------------------------------------------------------ */

static JsonNode *
json_resolve (PnSubstResolver *self, const gchar *key, gboolean *owned)
{
    JsonObject *root = self->user_data;

    *owned = FALSE;
    if (root == NULL)
        return NULL;

    return pn_json_resolve_path (root, key);
}

void
pn_subst_resolver_json (PnSubstResolver *r, JsonObject *root)
{
    r->resolve   = json_resolve;
    r->user_data = root;
}

static JsonNode *
strv_resolve (PnSubstResolver *self, const gchar *key, gboolean *owned)
{
    const gchar * const *pairs = self->user_data;

    *owned = FALSE;
    if (pairs == NULL)
        return NULL;

    for (guint i = 0; pairs[i] != NULL && pairs[i + 1] != NULL; i += 2)
    {
        if (g_strcmp0 (pairs[i], key) == 0)
        {
            JsonNode *node = json_node_new (JSON_NODE_VALUE);

            json_node_set_string (node, pairs[i + 1]);
            *owned = TRUE;
            return node;
        }
    }

    return NULL;
}

void
pn_subst_resolver_strv (PnSubstResolver *r, const gchar * const *pairs)
{
    r->resolve   = strv_resolve;
    r->user_data = (gpointer) pairs;
}
