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

#include "pn-expr-bind.h"

#include "pn-var-store.h"

#include <json-glib/json-glib.h>

/* ------------------------------------------------------------------ */
/*  One member                                                         */
/* ------------------------------------------------------------------ */

/** Bind @node (the JSON one) under @name, if it is something the
 *  expression language can hold: a number, or a `$pnvector` marker.
 *  Anything else — a string, a nested object, a null — is skipped in
 *  silence, which is what keeps a message's text out of the variables. */
static void
bind_member (PnMessage      *message,
             const gchar    *name,
             JsonNode       *value,
             PnExprBindFunc  bind,
             gpointer        user_data)
{
    if (value == NULL)
        return;

    if (JSON_NODE_HOLDS_VALUE (value))
    {
        GType vt = json_node_get_value_type (value);

        if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
            bind (name, json_node_get_double (value), NULL, user_data);
    }
    else
    {
        PnVector *vec = pn_message_resolve_vector (message, value);

        if (vec != NULL)
            bind (name, 0.0, vec, user_data);
    }
}

/* ------------------------------------------------------------------ */
/*  Flat                                                               */
/* ------------------------------------------------------------------ */

void
pn_expr_bind_flat (
        PnMessage      *message,
        PnExprBindFunc  bind,
        gpointer        user_data)
{
    JsonObject     *data;
    JsonObjectIter  iter;
    const gchar    *name;
    JsonNode       *value;

    g_return_if_fail (message != NULL);
    g_return_if_fail (bind != NULL);

    data = pn_message_get_data (message);
    if (data == NULL)
        return;

    json_object_iter_init (&iter, data);
    while (json_object_iter_next (&iter, &name, &value))
        bind_member (message, name, value, bind, user_data);
}

/* ------------------------------------------------------------------ */
/*  Collated                                                           */
/* ------------------------------------------------------------------ */

/** TRUE if @name is the display name of one of @node's inputs — i.e. a
 *  member the core's input-value collation injected into the data bag.
 *  Used to tell core-injected per-input headline values (bound by their
 *  own name) apart from this message's own sibling fields (suffixed). */
static gboolean
is_input_name (PnNode *node, gint n, const gchar *name)
{
    gint i;

    for (i = 0; i < n; i++)
        if (g_strcmp0 (pn_node_get_input_name (node, i), name) == 0)
            return TRUE;
    return FALSE;
}

void
pn_expr_bind_collated (
        PnNode         *node,
        PnMessage      *message,
        PnExprBindFunc  bind,
        gpointer        user_data)
{
    gint            n;
    gint            idx;
    JsonObject     *data;
    JsonObjectIter  iter;
    const gchar    *name;
    JsonNode       *value;
    gint            i;

    g_return_if_fail (PN_IS_NODE (node));
    g_return_if_fail (message != NULL);
    g_return_if_fail (bind != NULL);

    n   = pn_node_get_n_inputs (node);
    idx = pn_node_current_input ();
    if (idx < 0)
        idx = 0;
    else if (idx >= n)
        idx = n - 1;

    data = pn_message_get_data (message);

    /* (1) The latched per-input headline values, bound under their input
     *     names exactly as the core injected them. */
    for (i = 0; i < n; i++)
    {
        const gchar *nm = pn_node_get_input_name (node, i);
        JsonNode    *vn = (data != NULL) ? json_object_get_member (data, nm)
                                         : NULL;

        /* The core only collates for a node with two or more inputs, so
         * a SINGLE-input node sees a plain message with nothing injected
         * under its input's name.  Bind that message's own `value` under
         * the name anyway, so the rule reads the same at one input as at
         * eight — which is what lets a one-input figure name `value1`
         * (80.8d).  For two inputs and up this never fires: the core has
         * just latched the arriving input, so the member is there. */
        if (vn == NULL && i == idx && data != NULL)
            vn = json_object_get_member (data, "value");

        bind_member (message, nm, vn, bind, user_data);
    }

    /* (2) Sibling numeric fields from *this* message only, suffixed with
     *     the 1-based arriving input number (data.temp -> temp1/temp2,
     *     ...).  Unlike the headline values these are not latched across
     *     inputs — only the message actually being processed contributes
     *     them.  Skip the input-name members (the latched values bound
     *     above) and the bare reserved "value" (already surfaced under
     *     its input name). */
    if (data == NULL)
        return;

    json_object_iter_init (&iter, data);
    while (json_object_iter_next (&iter, &name, &value))
    {
        gchar *vname;

        if (value == NULL)
            continue;
        if (g_strcmp0 (name, "value") == 0)
            continue;
        if (is_input_name (node, n, name))
            continue;

        vname = g_strdup_printf ("%s%d", name, idx + 1);
        bind_member (message, vname, value, bind, user_data);
        g_free (vname);
    }
}

/* ------------------------------------------------------------------ */
/*  Into a variable store                                              */
/* ------------------------------------------------------------------ */

void
pn_expr_bind_to_store (
        const gchar *name,
        gdouble      scalar,
        PnVector    *vec,
        gpointer     user_data)
{
    PnVarStore *store = user_data;

    if (vec != NULL)
        pn_var_store_set_vector (store, name, vec);
    else
        pn_var_store_set (store, name, scalar);
}
