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

/* ------------------------------------------------------------------ */
/*  PnMeshQr — builds the ChannelSet protobuf and a Meshtastic share   */
/*  URL out of a borrowed channel list.  Direct C port of pip-mesh's   */
/*  build_channel_set_hex + print_qr_url; matches their byte layout    */
/*  so URLs imported by the Meshtastic Android / iOS apps deposit the  */
/*  same channels as pip-mesh would have written.                      */
/* ------------------------------------------------------------------ */

#include "pn-mesh-qr.h"

#include <string.h>

#include "pn-mesh-pb.h"

/* ChannelSettings { bytes psk = 2; string name = 3; } */
#define CS_PSK    2
#define CS_NAME   3

/* ChannelSet { repeated ChannelSettings settings = 1; LoRaConfig lora_config = 2; } */
#define CSET_SETTINGS 1
#define CSET_LORA     2

/* LoRaConfig (subset pip-mesh's --print-qr-config publishes). */
#define LC_USE_PRESET   1
#define LC_MODEM_PRESET 2
#define LC_REGION       7
#define LC_HOP_LIMIT    8

static GQuark
pn_mesh_qr_error_quark (void)
{
    return g_quark_from_static_string ("pn-mesh-qr-error");
}

#define PN_MESH_QR_ERROR (pn_mesh_qr_error_quark ())

static gboolean
index_selected (guint32        idx,
                const guint32 *selected,
                gsize          n_selected)
{
    if (selected == NULL)
        return TRUE;

    for (gsize i = 0; i < n_selected; i++)
        if (selected[i] == idx)
            return TRUE;

    return FALSE;
}

/* Build the ChannelSettings sub-message bytes for one channel. */
static GBytes *
encode_channel_settings (const PnMeshChannel *ch)
{
    PnMeshPbWriter w;

    pn_mesh_pb_writer_init (&w);

    /* pip-mesh order: psk first (field 2), name second (field 3).
     * Field order on the wire is not protobuf-significant but matching
     * pip-mesh byte-for-byte makes tests trivial to compare. */
    if (ch->psk != NULL && ch->psk_size > 0)
        pn_mesh_pb_write_bytes_field (&w, CS_PSK, ch->psk, ch->psk_size);

    if (ch->name != NULL && ch->name[0] != '\0')
        pn_mesh_pb_write_string_field (&w, CS_NAME, ch->name);

    return pn_mesh_pb_writer_take_bytes (&w);
}

/* Build the LoRaConfig sub-message bytes (pip-mesh's QR subset). */
static GBytes *
encode_lora_subset (const PnMeshLoraConfigWrite *lora)
{
    PnMeshPbWriter w;

    pn_mesh_pb_writer_init (&w);

    pn_mesh_pb_write_varint_field (&w, LC_USE_PRESET,   lora->use_preset ? 1 : 0);
    pn_mesh_pb_write_varint_field (&w, LC_MODEM_PRESET, lora->modem_preset);
    pn_mesh_pb_write_varint_field (&w, LC_REGION,       lora->region);
    pn_mesh_pb_write_varint_field (&w, LC_HOP_LIMIT,    lora->hop_limit);

    return pn_mesh_pb_writer_take_bytes (&w);
}

/* Convert standard base64 (RFC 4648) to URL-safe base64 with no
 * padding: '+' -> '-', '/' -> '_', strip trailing '='.  In place. */
static void
to_base64url (gchar *s)
{
    gsize len, i;

    if (s == NULL)
        return;

    len = strlen (s);
    while (len > 0 && s[len - 1] == '=')
        s[--len] = '\0';

    for (i = 0; i < len; i++)
    {
        if (s[i] == '+') s[i] = '-';
        else if (s[i] == '/') s[i] = '_';
    }
}

gchar *
pn_mesh_qr_build_share_url (const GPtrArray              *channels,
                            const guint32                *selected_indices,
                            gsize                         n_selected_indices,
                            const PnMeshLoraConfigWrite  *lora_or_null,
                            GError                      **error)
{
    PnMeshPbWriter set_w;
    gboolean       any = FALSE;
    GBytes        *bytes;
    gconstpointer  raw;
    gsize          raw_size;
    gchar         *b64;
    gchar         *url;

    g_return_val_if_fail (channels != NULL, NULL);

    pn_mesh_pb_writer_init (&set_w);

    /* Emit one ChannelSettings sub-message per surviving channel, in
     * device-index order (channels are already index-ordered in
     * PnMeshState; trust the order rather than re-sorting). */
    for (guint i = 0; i < channels->len; i++)
    {
        const PnMeshChannel *ch = g_ptr_array_index ((GPtrArray *) channels, i);
        GBytes              *settings;
        gconstpointer        sraw;
        gsize                ssize;

        if (ch == NULL || ch->role == 0)
            continue;
        if (!index_selected (ch->index, selected_indices, n_selected_indices))
            continue;

        settings = encode_channel_settings (ch);
        sraw = g_bytes_get_data (settings, &ssize);
        pn_mesh_pb_write_embedded_field (&set_w, CSET_SETTINGS, sraw, ssize);
        g_bytes_unref (settings);
        any = TRUE;
    }

    if (!any)
    {
        pn_mesh_pb_writer_clear (&set_w);
        g_set_error_literal (error, PN_MESH_QR_ERROR, 0,
                             "No channels selected to share.");
        return NULL;
    }

    if (lora_or_null != NULL)
    {
        GBytes        *lora = encode_lora_subset (lora_or_null);
        gconstpointer  lraw;
        gsize          lsize;

        lraw = g_bytes_get_data (lora, &lsize);
        pn_mesh_pb_write_embedded_field (&set_w, CSET_LORA, lraw, lsize);
        g_bytes_unref (lora);
    }

    bytes = pn_mesh_pb_writer_take_bytes (&set_w);
    raw   = g_bytes_get_data (bytes, &raw_size);

    b64 = g_base64_encode (raw, raw_size);
    to_base64url (b64);

    url = g_strconcat ("https://meshtastic.org/e/#", b64, NULL);

    g_free (b64);
    g_bytes_unref (bytes);
    return url;
}
