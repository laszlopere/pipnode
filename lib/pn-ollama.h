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

#ifndef PN_OLLAMA_H
#define PN_OLLAMA_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnOllama                                                           */
/*                                                                     */
/*  Filter node that submits the incoming message's `data.output`      */
/*  string to a local or remote Ollama server's `/api/generate`        */
/*  endpoint and emits a new message whose `data.output` carries the   */
/*  model's reply.  Hostname, port and model are configurable, and a   */
/*  prefix / suffix pair is concatenated around the input so a single  */
/*  node can carry the system framing the model expects.  The          */
/*  `keep-alive` property is forwarded to Ollama verbatim so the user  */
/*  can choose between "warm" (model stays loaded for repeat calls)    */
/*  and "cold" (model unloaded between calls) operation.               */
/* ------------------------------------------------------------------ */

#define PN_TYPE_OLLAMA (pn_ollama_get_type ())

G_DECLARE_FINAL_TYPE (PnOllama, pn_ollama, PN, OLLAMA, PnNode)

PnOllama *pn_ollama_new (void);

/* pn_ollama_list_models:
 * @hostname: Ollama server host (empty/NULL -> local fallback)
 * @port:     Ollama server port
 *
 * Synchronously enumerate the model names installed on @hostname:@port
 * via GET /api/tags.  Returns a NULL-terminated string array the caller
 * owns (free with g_strfreev), or an empty array when the host is
 * unreachable / not running Ollama.  GTK-free (libsoup + json-glib), so
 * the core keeps this network helper; the gui settings dialog calls it
 * to populate the model combo.  */
gchar **pn_ollama_list_models (const gchar *hostname, gint port);

G_END_DECLS

#endif /* PN_OLLAMA_H */
