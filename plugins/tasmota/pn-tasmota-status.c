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

#include "pn-tasmota-status.h"

#include "pn-message.h"

#include <string.h>

/* fa-info-circle U+F05A — the conventional "status" / "info" glyph. */
#define PN_TASMOTA_STATUS_ICON "\xef\x81\x9a"

struct _PnTasmotaStatus
{
    PnNode parent_instance;
};

G_DEFINE_TYPE (PnTasmotaStatus, pn_tasmota_status, PN_TYPE_NODE)

/** Tasmota answers a `cmnd/<device>/Status[N]` request on
 *  `stat/<device>/STATUS[N]` — the response code's last segment is
 *  always either the bare word "STATUS" (the omnibus dump) or
 *  "STATUS" followed by a digit (STATUS5 = network info, STATUS11 =
 *  state, etc.).  Matching "starts with STATUS" on the last topic
 *  segment covers every variant without listing the dozen STATUS<N>
 *  codes individually. */
static gboolean
topic_is_tasmota_status (const gchar *topic)
{
    const gchar *last_slash;
    const gchar *last_segment;

    if (topic == NULL || *topic == '\0')
        return FALSE;

    last_slash   = strrchr (topic, '/');
    last_segment = (last_slash != NULL) ? last_slash + 1 : topic;

    return g_str_has_prefix (last_segment, "STATUS");
}

static void
pn_tasmota_status_receive (
        PnNode    *node,
        PnMessage *message)
{
    if (!topic_is_tasmota_status (pn_message_get_topic (message)))
        return;

    pn_node_emit_message (node, message);
}

static void
pn_tasmota_status_class_init (PnTasmotaStatusClass *klass)
{
    PnNodeClass *node_class = PN_NODE_CLASS (klass);

    node_class->receive       = pn_tasmota_status_receive;

    node_class->palette_icon  = PN_TASMOTA_STATUS_ICON;
    node_class->class_name    = "Tasmota Status";
    node_class->icon          = PN_TASMOTA_STATUS_ICON;
    node_class->color         = (GdkRGBA){ 0.42, 0.55, 0.72, 1.0 };
    node_class->category      = "Tasmota";
    node_class->has_input     = TRUE;
    node_class->has_output    = TRUE;
}

static void
pn_tasmota_status_init (PnTasmotaStatus *self)
{
    PnNode  *node = PN_NODE (self);
    GdkRGBA  steel = { 0.42, 0.55, 0.72, 1.0 };

    pn_node_set_class_name (node, "Tasmota Status");
    pn_node_set_icon       (node, PN_TASMOTA_STATUS_ICON);
    pn_node_set_color      (node, &steel);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

PnTasmotaStatus *
pn_tasmota_status_new (void)
{
    return g_object_new (PN_TYPE_TASMOTA_STATUS, NULL);
}
