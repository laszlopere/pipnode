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

#include "pn-jump.h"
#include "pn-jump-in.h"
#include "pn-jump-out.h"
#include "pn-flow.h"
#include "pn-node-store.h"

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/* ------------------------------------------------------------------ */

void
pn_jump_measure (
        const gchar *tag,
        double      *out_width,
        double      *out_height)
{
    /* Measure in characters, not bytes: a UTF-8 tag must not inflate the
     * pennant by its encoding length. */
    const glong chars = (tag != NULL && *tag != '\0')
                            ? g_utf8_strlen (tag, -1)
                            : 0;
    const double text = (double) chars * PN_JUMP_CHAR_ADVANCE;
    const double want = PN_JUMP_POINT + 2.0 * PN_JUMP_PADDING + text;

    if (out_width != NULL)
        *out_width = MAX (PN_JUMP_MIN_WIDTH, want);
    if (out_height != NULL)
        *out_height = PN_JUMP_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Tag matching                                                       */
/* ------------------------------------------------------------------ */

/** An empty tag names nothing and must never match another empty tag —
 *  otherwise every freshly-dropped flag would silently join one bus. */
static inline gboolean
tag_matches (const gchar *a, const gchar *b)
{
    return a != NULL && *a != '\0' && g_strcmp0 (a, b) == 0;
}

/** The tag of @node when it is a flag of either direction, else %NULL. */
static const gchar *
node_tag (PnNode *node)
{
    if (PN_IS_JUMP_IN (node))
        return pn_jump_in_get_tag (PN_JUMP_IN (node));
    if (PN_IS_JUMP_OUT (node))
        return pn_jump_out_get_tag (PN_JUMP_OUT (node));
    return NULL;
}

GList *
pn_jump_collect_outputs (
        PnFlow      *flow,
        const gchar *tag)
{
    PnNodeStore *store;
    GList       *out = NULL;
    guint        i, n;

    if (flow == NULL || tag == NULL || *tag == '\0')
        return NULL;

    store = pn_flow_get_nodes (flow);
    if (store == NULL)
        return NULL;

    n = pn_node_store_get_length (store);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (store, i);

        if (!PN_IS_JUMP_OUT (node) || pn_node_get_disabled (node))
            continue;

        if (tag_matches (tag, pn_jump_out_get_tag (PN_JUMP_OUT (node))))
            out = g_list_prepend (out, node);
    }

    /* Restore document order: g_list_prepend built the list backwards,
     * and delivery order should be stable across runs so a flow that
     * depends (however unwisely) on fan-out order behaves the same on
     * every load. */
    return g_list_reverse (out);
}

/* ------------------------------------------------------------------ */
/*  Error state                                                        */
/* ------------------------------------------------------------------ */

void
pn_jump_refresh_errors (PnFlow *flow)
{
    PnNodeStore *store;
    GPtrArray   *flags;
    guint        i, j, n;

    if (flow == NULL)
        return;

    store = pn_flow_get_nodes (flow);
    if (store == NULL)
        return;

    /* One pass to gather the flags, then an O(f²) cross-check among them
     * alone.  Flags are a handful per document, so an index would cost
     * more to keep correct than the scan saves. */
    flags = g_ptr_array_new ();

    n = pn_node_store_get_length (store);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (store, i);

        if (PN_IS_JUMP_IN (node) || PN_IS_JUMP_OUT (node))
            g_ptr_array_add (flags, node);
    }

    for (i = 0; i < flags->len; i++)
    {
        PnNode        *node    = g_ptr_array_index (flags, i);
        const gchar   *tag     = node_tag (node);
        const gboolean is_in   = PN_IS_JUMP_IN (node);
        gboolean       matched = FALSE;

        /* An untagged flag is always an error: it can never be half of a
         * connection, so there is nothing to look for. */
        if (tag == NULL || *tag == '\0')
        {
            pn_node_set_has_error (node, TRUE);
            continue;
        }

        /* A flag needs at least one partner of the OPPOSITE direction.
         * Two jump-ins sharing a tag is legal (they merge onto one bus),
         * as is two jump-outs (the bus fans out) — but an end with no
         * counterpart swallows or starves messages in silence. */
        for (j = 0; j < flags->len && !matched; j++)
        {
            PnNode *other = g_ptr_array_index (flags, j);

            if (is_in ? !PN_IS_JUMP_OUT (other) : !PN_IS_JUMP_IN (other))
                continue;

            matched = tag_matches (tag, node_tag (other));
        }

        pn_node_set_has_error (node, !matched);
    }

    g_ptr_array_free (flags, TRUE);
}
