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

#ifndef PN_JSON_PATH_H
#define PN_JSON_PATH_H

#include <glib.h>
#include <json-glib/json-glib.h>

#include "pn-message.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  JSON path helpers                                                  */
/*                                                                     */
/*  Tiny utility pair shared by every node that exposes a "path into   */
/*  the message" property — Dedup, Format, Graph, ….  The path syntax  */
/*  is "/"-separated segment names: empty / leading / trailing slashes */
/*  are tolerated.  The lookup root carries the message's topic, id,   */
/*  created timestamp, and a *reference* to the live data object, so   */
/*  expressions like "data/value" navigate straight into the message   */
/*  payload without copying.                                            */
/* ------------------------------------------------------------------ */

/**
 * pn_json_resolve_path:
 * @root: lookup root, typically built by pn_json_lookup_root_for_message()
 * @path: "/"-separated path, e.g. "data/per_minute"
 *
 * Walks @path through @root segment-by-segment and returns the
 * addressed #JsonNode, or %NULL if any segment is missing or hits a
 * non-object.  The returned node is borrowed from @root.
 */
JsonNode  *pn_json_resolve_path           (JsonObject  *root,
                                           const gchar *path);

/**
 * pn_json_lookup_root_for_message:
 * @message: the message whose payload should be addressable
 *
 * Builds a fresh #JsonObject usable as a lookup root for
 * pn_json_resolve_path().  The root carries `topic`, `id`, `created`
 * as string members and `data` as a node referencing the message's
 * live data bag (no copy).  The caller owns the returned root and
 * must json_object_unref() it; the embedded data bag stays owned by
 * the message.
 */
JsonObject *pn_json_lookup_root_for_message (PnMessage *message);

G_END_DECLS

#endif /* PN_JSON_PATH_H */
