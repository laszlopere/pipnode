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

/* Unit tests for the Meshtastic share-URL builder (pn-mesh-qr.c).  The
 * builder is a hand-port of pip-mesh's build_channel_set_hex /
 * print_qr_url, so these tests pin the wire-format invariants AND the
 * url-safe-base64-no-padding transform — every byte that the
 * Meshtastic Android / iOS apps will parse comes out of this module,
 * so a one-bit drift here silently breaks channel sharing in the
 * field. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "pntest.h"
#include "pn-mesh-qr.h"
#include "pn-mesh-connection.h"

/* ------------------------------------------------------------------ */
/*  Fixtures                                                            */
/* ------------------------------------------------------------------ */

static PnMeshChannel *
make_channel (guint32 index, guint32 role,
              const gchar *name,
              const guint8 *psk, gsize psk_size)
{
    PnMeshChannel *ch = g_new0 (PnMeshChannel, 1);

    ch->index = index;
    ch->role  = role;
    ch->name  = g_strdup (name);
    if (psk_size > 0)
    {
        ch->psk      = g_memdup2 (psk, psk_size);
        ch->psk_size = psk_size;
    }
    return ch;
}

static GPtrArray *
make_channels_array (void)
{
    return g_ptr_array_new_with_free_func (
            (GDestroyNotify) pn_mesh_channel_free);
}

/* ------------------------------------------------------------------ */
/*  Byte-for-byte URL match against pip-mesh                            */
/* ------------------------------------------------------------------ */

/* One channel, name "LongFast", role=PRIMARY, psk=[0x01].  The
 * ChannelSet protobuf is 15 bytes:
 *
 *     0A 0D                              ChannelSet field 1, len 13
 *       12 01 01                         ChannelSettings.psk = {0x01}
 *       1A 08 'L''o''n''g''F''a''s''t'   ChannelSettings.name = "LongFast"
 *
 * Standard base64 of those 15 bytes is "Cg0SAQEaCExvbmdGYXN0".  No
 * '+' or '/' to translate, no padding to strip; url-safe form is
 * identical.  Matches `pip-mesh --print-qr-channels` against a
 * device with just the default channel populated. */
static void
test_url_single_default_channel (void)
{
    GPtrArray    *channels = make_channels_array ();
    const guint8  psk[]    = { 0x01 };
    GError       *error    = NULL;
    gchar        *url;

    g_ptr_array_add (channels,
            make_channel (0, 1 /* PRIMARY */, "LongFast", psk, 1));

    url = pn_mesh_qr_build_share_url (channels, NULL, 0, NULL, &error);
    PN_CHECK (url != NULL);
    PN_CHECK (error == NULL);

    PN_CHECK_CMPSTR (url, ==,
        "https://meshtastic.org/e/#Cg0SAQEaCExvbmdGYXN0");

    g_free (url);
    g_ptr_array_unref (channels);
}

/* Same single channel + a LoRaConfig sub-message
 * (use_preset=1, modem_preset=0=LONG_FAST, region=1=US, hop_limit=3).
 * The 8-byte LoRa payload is embedded as ChannelSet field 2 (10
 * extra wire bytes), bringing the total to 25 bytes.  Base64 of 25
 * bytes is "Cg0SAQEaCExvbmdGYXN0EggIARAAOAFAAw==" (the two trailing
 * '=' get stripped by the url-safe transform). */
static void
test_url_full_config (void)
{
    GPtrArray            *channels = make_channels_array ();
    const guint8          psk[]    = { 0x01 };
    PnMeshLoraConfigWrite lora     = {
        .use_preset   = TRUE,
        .modem_preset = 0,
        .region       = 1,
        .hop_limit    = 3,
        .tx_enabled   = FALSE,  /* ignored: not in QR subset */
        .tx_power     = 0,      /* ignored: not in QR subset */
        .channel_num  = 0,      /* ignored: not in QR subset */
    };
    GError               *error = NULL;
    gchar                *url;

    g_ptr_array_add (channels,
            make_channel (0, 1, "LongFast", psk, 1));

    url = pn_mesh_qr_build_share_url (channels, NULL, 0, &lora, &error);
    PN_CHECK (url != NULL);
    PN_CHECK (error == NULL);

    PN_CHECK_CMPSTR (url, ==,
        "https://meshtastic.org/e/#Cg0SAQEaCExvbmdGYXN0EggIARAAOAFAAw");

    g_free (url);
    g_ptr_array_unref (channels);
}

/* ------------------------------------------------------------------ */
/*  Filtering: disabled slots, selection subset                         */
/* ------------------------------------------------------------------ */

/* A device with one PRIMARY (slot 0) and two DISABLED (slot 1 + 2)
 * must produce exactly the same URL as if the disabled slots were
 * not in the array at all -- pip-mesh's "skip role==0" rule. */
static void
test_skip_disabled (void)
{
    GPtrArray    *channels = make_channels_array ();
    const guint8  psk[]    = { 0x01 };
    GError       *error    = NULL;
    gchar        *url;

    g_ptr_array_add (channels,
            make_channel (0, 1, "LongFast", psk, 1));
    g_ptr_array_add (channels,
            make_channel (1, 0 /* DISABLED */, "ghost", NULL, 0));
    g_ptr_array_add (channels,
            make_channel (2, 0 /* DISABLED */, NULL, NULL, 0));

    url = pn_mesh_qr_build_share_url (channels, NULL, 0, NULL, &error);
    PN_CHECK (url != NULL);
    PN_CHECK (error == NULL);
    PN_CHECK_CMPSTR (url, ==,
        "https://meshtastic.org/e/#Cg0SAQEaCExvbmdGYXN0");

    g_free (url);
    g_ptr_array_unref (channels);
}

/* Multi-channel set with explicit index selection: the page's "Share
 * channels" column lets the user pick a subset.  Selecting only the
 * non-primary by index must drop the primary from the URL. */
static void
test_selected_subset (void)
{
    GPtrArray    *channels = make_channels_array ();
    const guint8  psk_a[]  = { 0x01 };
    const guint8  psk_b[]  = { 0xAA, 0xBB, 0xCC, 0xDD };
    guint32       selected = 1;
    GError       *error    = NULL;
    gchar        *url_a, *url_b;

    g_ptr_array_add (channels,
            make_channel (0, 1 /* PRIMARY */,   "LongFast", psk_a, 1));
    g_ptr_array_add (channels,
            make_channel (1, 2 /* SECONDARY */, "Side",     psk_b, 4));

    /* All channels -> URL must include both names. */
    url_a = pn_mesh_qr_build_share_url (channels, NULL, 0, NULL, &error);
    PN_CHECK (url_a != NULL);
    PN_CHECK (error == NULL);

    /* Only index 1 -> URL must NOT match the "all channels" URL but
     * must still be a valid meshtastic.org/e/# URL. */
    url_b = pn_mesh_qr_build_share_url (channels, &selected, 1, NULL, &error);
    PN_CHECK (url_b != NULL);
    PN_CHECK (error == NULL);

    PN_CHECK (g_str_has_prefix (url_a, "https://meshtastic.org/e/#"));
    PN_CHECK (g_str_has_prefix (url_b, "https://meshtastic.org/e/#"));
    PN_CHECK (g_strcmp0 (url_a, url_b) != 0);
    PN_CHECK (strlen (url_b) < strlen (url_a));

    g_free (url_a);
    g_free (url_b);
    g_ptr_array_unref (channels);
}

/* ------------------------------------------------------------------ */
/*  Errors                                                              */
/* ------------------------------------------------------------------ */

/* Asking for a URL when every channel is disabled (or none are
 * selected) is the only failure path; the caller gets a clear error
 * and NULL so the page can show "No channels to share" rather than
 * an empty QR. */
static void
test_empty_channel_set_errors (void)
{
    GPtrArray    *channels = make_channels_array ();
    GError       *error    = NULL;
    gchar        *url;

    g_ptr_array_add (channels,
            make_channel (0, 0 /* DISABLED */, NULL, NULL, 0));

    url = pn_mesh_qr_build_share_url (channels, NULL, 0, NULL, &error);
    PN_CHECK (url == NULL);
    PN_CHECK (error != NULL);
    g_clear_error (&error);

    g_ptr_array_unref (channels);
}

/* ------------------------------------------------------------------ */
/*  Driver                                                              */
/* ------------------------------------------------------------------ */

int
main (int argc, char *argv[])
{
    pn_test_init (&argc, &argv, "pn-mesh-qr");

    pn_test_add ("url-single-default-channel",  test_url_single_default_channel);
    pn_test_add ("url-full-config",             test_url_full_config);
    pn_test_add ("skip-disabled",               test_skip_disabled);
    pn_test_add ("selected-subset",             test_selected_subset);
    pn_test_add ("empty-channel-set-errors",    test_empty_channel_set_errors);

    return pn_test_run ();
}
