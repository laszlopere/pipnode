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
/*  Firmware page.                                                      */
/*                                                                     */
/*  Hand-off to the official web flasher rather than flashing in-app:  */
/*  the device's hardware model + firmware version as guidance, plus a */
/*  clickable flasher URL.  Activating the link opens                   */
/*  flasher.meshtastic.org in the user's browser and then closes the    */
/*  Meshtastic Devices dialog, whose teardown frees the serial port so  */
/*  the browser's WebSerial can claim it.                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-firmware.h"

#include "pn-device-form.h"
#include "pn-mesh-formats.h"
#include "pn-value-label.h"

#define PN_MESH_FIRMWARE_CTX_QDATA "pn-mesh-page-firmware-ctx"

/* The official Meshtastic web flasher.  WebSerial-based; works in
 * Chromium browsers (Chrome / Edge / Brave). */
#define PN_MESH_FLASHER_URL "https://flasher.meshtastic.org/"

typedef struct
{
    PnValueLabel *hw_value;
    PnValueLabel *fw_value;

    /* TRUE while a live connection is set on the page -- i.e. pipnode is
     * holding the serial port and must release it before the browser can
     * flash.  Mirrors set_state's state != NULL. */
    gboolean      connected;

    PnMeshFirmwareCloseFunc close_cb;
    gpointer                close_ud;
} FirmwareCtx;

static void
firmware_ctx_free (gpointer data)
{
    g_slice_free (FirmwareCtx, data);
}

/* ------------------------------------------------------------------ */
/*  Flasher hand-off                                                    */
/* ------------------------------------------------------------------ */

static gboolean
on_flasher_link (GtkLabel *link, const gchar *uri, gpointer user_data)
{
    GtkWidget   *page = user_data;
    FirmwareCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_FIRMWARE_CTX_QDATA);
    GtkWidget   *top    = gtk_widget_get_toplevel (page);
    GtkWindow   *parent = GTK_IS_WINDOW (top) ? GTK_WINDOW (top) : NULL;
    GError      *error  = NULL;
    GtkWidget   *dlg;
    GtkWidget   *ok;
    gint         resp;
    PnMeshFirmwareCloseFunc close_cb;
    gpointer                close_ud;

    (void) link;
    (void) uri;
    if (ctx == NULL)
        return FALSE;   /* let GTK open it the default way */

    /* The browser's WebSerial needs exclusive access to the port, which
     * pipnode holds for the whole session -- so opening the flasher closes
     * this dialog (its teardown frees the fd).  Confirm first, since that
     * ends the configuring session.  Parented to the dialog window so the
     * nested modal stacks correctly over it. */
    dlg = gtk_message_dialog_new (
            parent,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "Open the web flasher and close this dialog?");
    gtk_message_dialog_format_secondary_text (
            GTK_MESSAGE_DIALOG (dlg),
            ctx->connected
            ? "Flashing runs in your browser, which needs exclusive access "
              "to the serial port.  The Meshtastic Devices dialog will "
              "close and pipnode will disconnect from this device so the "
              "browser can use the port.  The web flasher works in Chrome "
              "or Edge.\n\n"
              "When flashing is done, reopen Devices \342\206\222 Meshtastic "
              "and Scan to reconnect."
            : "Flashing runs in your browser, which needs exclusive access "
              "to the serial port.  The Meshtastic Devices dialog will "
              "close so the browser can use the port.  The web flasher "
              "works in Chrome or Edge.\n\n"
              "When flashing is done, reopen Devices \342\206\222 Meshtastic "
              "and Scan to reconnect.");
    gtk_dialog_add_button (GTK_DIALOG (dlg), "_Cancel", GTK_RESPONSE_CANCEL);
    ok = gtk_dialog_add_button (GTK_DIALOG (dlg), "_Open Flasher",
                                GTK_RESPONSE_ACCEPT);
    gtk_style_context_add_class (gtk_widget_get_style_context (ok),
                                 "suggested-action");
    gtk_dialog_set_default_response (GTK_DIALOG (dlg), GTK_RESPONSE_ACCEPT);
    resp = gtk_dialog_run (GTK_DIALOG (dlg));
    gtk_widget_destroy (dlg);

    if (resp != GTK_RESPONSE_ACCEPT)
        return TRUE;   /* handled: do not navigate */

    /* Open the flasher while the dialog window is still alive --
     * gtk_show_uri_on_window needs a valid parent for its timestamp.  If
     * the browser is not Chromium-based the flasher page itself reports
     * that WebSerial is unsupported, clearer than anything we could
     * pre-empt here. */
    if (!gtk_show_uri_on_window (parent, PN_MESH_FLASHER_URL,
                                 GDK_CURRENT_TIME, &error))
    {
        g_warning ("Meshtastic: could not open flasher URL '%s': %s",
                   PN_MESH_FLASHER_URL,
                   error != NULL ? error->message : "(unknown error)");
        g_clear_error (&error);
    }

    /* Now close the dialog (which releases the serial port).  Grab the
     * callback first: the call destroys this page and frees ctx, so we
     * must not touch ctx -- or page or parent -- afterwards. */
    close_cb = ctx->close_cb;
    close_ud = ctx->close_ud;
    if (close_cb != NULL)
        close_cb (close_ud);

    return TRUE;   /* we handled the activation */
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_mesh_page_firmware_new (void)
{
    GtkWidget    *page;
    GtkWidget    *hw;
    GtkWidget    *fw;
    GtkSizeGroup *keys;
    GtkWidget    *link;
    GtkWidget    *link_align;
    FirmwareCtx  *ctx;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    ctx = g_slice_new0 (FirmwareCtx);

    /* Hardware / firmware read-out, keys sized into one column. */
    keys = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);

    hw = pn_value_label_new ("Hardware");
    pn_value_label_set_key_size_group (PN_VALUE_LABEL (hw), keys);
    gtk_widget_set_margin_top (hw, 6);
    gtk_box_pack_start (GTK_BOX (page), hw, FALSE, FALSE, 0);
    ctx->hw_value = PN_VALUE_LABEL (hw);

    fw = pn_value_label_new ("Firmware");
    pn_value_label_set_key_size_group (PN_VALUE_LABEL (fw), keys);
    gtk_box_pack_start (GTK_BOX (page), fw, FALSE, FALSE, 0);
    ctx->fw_value = PN_VALUE_LABEL (fw);

    /* The clickable flasher URL, shown as a "Web Flasher: <link>" row whose
     * bold key lines up with the Hardware / Firmware read-outs above it.
     * A plain GtkLabel with link markup -- not a GtkLinkButton -- so the
     * URL has no button padding and its text starts flush with the value
     * labels above it. */
    {
        GtkWidget *flash_key = pn_device_form_key_label ("Web Flasher");

        gtk_size_group_add_widget (keys, flash_key);

        link = gtk_label_new (NULL);
        gtk_label_set_markup (GTK_LABEL (link),
                "<a href=\"" PN_MESH_FLASHER_URL "\">"
                PN_MESH_FLASHER_URL "</a>");
        gtk_label_set_xalign (GTK_LABEL (link), 0.0);
        gtk_widget_set_tooltip_text (link,
                "Open the Meshtastic web flasher.  The Devices dialog "
                "closes so your browser can use the serial port.");

        link_align = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_box_pack_start (GTK_BOX (link_align), flash_key, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (link_align), link, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (page), link_align, FALSE, FALSE, 0);
    }

    g_object_unref (keys);   /* the value labels hold their own refs */

    g_object_set_data_full (G_OBJECT (page), PN_MESH_FIRMWARE_CTX_QDATA,
                            ctx, firmware_ctx_free);
    g_signal_connect (link, "activate-link",
                      G_CALLBACK (on_flasher_link), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_firmware_set_state (GtkWidget         *page,
                                 const PnMeshState *state)
{
    FirmwareCtx *ctx;
    guint32      hw_model;
    gchar       *hw_name;
    const gchar *fw;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_FIRMWARE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    if (state == NULL)
    {
        ctx->connected = FALSE;
        pn_value_label_set_value (ctx->hw_value, NULL);   /* → "—" */
        pn_value_label_set_value (ctx->fw_value, NULL);
        return;
    }

    ctx->connected = TRUE;

    /* Prefer the metadata hw_model; fall back to the User.hw_model the
     * handshake always carries when the get_device_metadata round-trip
     * did not land (same fallback the Identity page uses). */
    hw_model = state->have_metadata ? state->hw_model : state->owner_hw_model;
    hw_name  = pn_mesh_format_hw_model (hw_model);
    pn_value_label_set_value (ctx->hw_value, hw_name);
    g_free (hw_name);

    fw = (state->have_metadata && state->firmware_version != NULL
          && *state->firmware_version != '\0')
         ? state->firmware_version : NULL;
    pn_value_label_set_value (ctx->fw_value, fw);   /* NULL → "—" */
}

void
pn_mesh_page_firmware_set_close_callback (
        GtkWidget                  *page,
        PnMeshFirmwareCloseFunc     callback,
        gpointer                    user_data)
{
    FirmwareCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_FIRMWARE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->close_cb = callback;
    ctx->close_ud = user_data;
}
