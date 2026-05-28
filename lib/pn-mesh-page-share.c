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
/*  Share page — Phase 6.                                              */
/*                                                                     */
/*    Channels (checkboxes; primary pre-checked) -- takes the slack   */
/*    [ o Share channels ]  [   Share full config ]                    */
/*    [          QR CODE              ]                                */
/*    URL  [............................]  [Copy]                     */
/*                                                                     */
/*  Single QR area kept deliberately: putting two QR codes next to    */
/*  each other invites a phone camera to lock on the wrong one.  The  */
/*  radio sits right above the QR so the user reads "I'm scanning the */
/*  channels-only QR" / "I'm scanning the full-config QR" without     */
/*  scrolling.  Same encoder either way; the full-config mode just    */
/*  attaches the LoRa subset pip-mesh's --print-qr-config publishes.  */
/*                                                                     */
/*  URL lives BELOW the QR so phones never have to fight a glowing    */
/*  text field for camera focus, and so the QR is the visual anchor   */
/*  the eye lands on.                                                  */
/*                                                                     */
/*  Read-only with respect to the device: the page never writes back  */
/*  anything.  Every paint goes through pn_mesh_qr_build_share_url    */
/*  which is byte-for-byte equivalent to pip-mesh's bash codec        */
/*  (test-pn-mesh-qr is the regression net).                           */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-share.h"

#include "pn-mesh-qr.h"

#ifdef HAVE_QRENCODE
#include <qrencode.h>
#endif

#define PN_MESH_SHARE_CTX_QDATA "pn-mesh-page-share-ctx"

#define ROLE_DISABLED   0
#define ROLE_PRIMARY    1
#define ROLE_SECONDARY  2

/* Cairo QR drawing area is square; tuned to fit comfortably inside
 * the dialog without forcing it to grow horizontally. */
#define QR_SIDE_PX 240

typedef enum
{
    SHARE_MODE_CHANNELS,    /* additive -- channels only */
    SHARE_MODE_FULL_CONFIG  /* destructive -- channels + LoRa subset */
} ShareMode;

typedef struct
{
    GtkWidget         *page;

    /* Borrowed; the dialog owns it.  NULL until set_state(state). */
    const PnMeshState *state;

    /* Mode toggle radios.  channels_radio is the group "anchor". */
    GtkToggleButton   *channels_radio;
    GtkToggleButton   *full_config_radio;

    /* Channel-selection list, shared across modes.  Wrapped in a
     * scrolled window so the QR area's min size cannot squeeze the
     * checkbox column down to a single row. */
    GtkListBox        *list;

    /* Notice label, visible only when full-config mode is selected
     * but the device has not yet surfaced a LoRa config (we cannot
     * ship region=UNSET -- it would brick the receiver). */
    GtkWidget         *full_config_warning;

    /* URL label + Copy + QR area + cached URL for the draw callback.
     * The URL is a wrapping selectable label rather than a GtkEntry:
     * Meshtastic share URLs run ~50-90 chars and a single-line entry
     * shows only the (identical) "https://meshtastic.org/e/#…" prefix,
     * so the user could not tell when the dynamic tail changed. */
    GtkLabel          *url_label;
    GtkButton         *copy_button;
    GtkWidget         *qr_area;
    gchar             *url;
} ShareCtx;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                                */
/* ------------------------------------------------------------------ */

static void   rebuild_channel_list (ShareCtx *ctx);
static void   refresh_url          (ShareCtx *ctx);
static void   apply_url            (ShareCtx *ctx, const gchar *url);
static ShareMode current_mode      (ShareCtx *ctx);
#ifdef HAVE_QRENCODE
static gboolean on_qr_draw (GtkWidget *area, cairo_t *cr, gpointer ud);
#endif

/* ------------------------------------------------------------------ */
/*  Lifetime                                                            */
/* ------------------------------------------------------------------ */

static void
share_ctx_free (gpointer data)
{
    ShareCtx *ctx = data;
    g_clear_pointer (&ctx->url, g_free);
    g_slice_free (ShareCtx, ctx);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

/* The two radios share a group so picking one un-picks the other.
 * channels_radio is the anchor; full_config_radio joins via the
 * gtk_radio_button_new_with_label_from_widget convenience. */
static GtkWidget *
build_mode_toggle (ShareCtx *ctx)
{
    GtkWidget *row;
    GtkWidget *channels;
    GtkWidget *full;

    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 18);

    channels = gtk_radio_button_new_with_label (NULL,
            "Share channels (additive)");
    full = gtk_radio_button_new_with_label_from_widget (
            GTK_RADIO_BUTTON (channels),
            "Share full config (replaces)");

    gtk_widget_set_tooltip_text (channels,
            "Receiver keeps its existing channels and LoRa settings, "
            "and adds the selected channels.");
    gtk_widget_set_tooltip_text (full,
            "Receiver REPLACES its channels and LoRa settings with "
            "this device's.  Use to clone a setup.");

    g_signal_connect_swapped (channels, "toggled",
            G_CALLBACK (refresh_url), ctx);
    /* full_config_radio's toggle fires together with channels' --
     * radios in a group emit "toggled" on BOTH the newly active and
     * the newly inactive button.  Connecting only to one would
     * already work; connecting to both keeps the intent explicit and
     * keeps the order of signal delivery from mattering. */
    g_signal_connect_swapped (full, "toggled",
            G_CALLBACK (refresh_url), ctx);

    gtk_box_pack_start (GTK_BOX (row), channels, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (row), full,     FALSE, FALSE, 0);

    ctx->channels_radio    = GTK_TOGGLE_BUTTON (channels);
    ctx->full_config_radio = GTK_TOGGLE_BUTTON (full);
    return row;
}

static GtkWidget *
build_channel_list (ShareCtx *ctx)
{
    GtkWidget *scrolled;
    GtkWidget *list;

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    /* Two-layer fix for the "list collapses to one row" problem.
     *
     * (1) set_min_content_height is GTK's hint that the scrolled
     *     window's NATURAL size request should be at least 130 px
     *     -- about four rows worth -- so the box layout reserves
     *     that much even when the page first paints.
     *
     * (2) vexpand=TRUE marks the scrolled window as the slack-
     *     absorbing child of its parent box, so the moment the
     *     dialog has any extra vertical room it goes to the list
     *     rather than to the QR area (whose set_size_request is
     *     its own minimum and naturally grows by being centred,
     *     not by expanding).  Without (2) the QR area's TRUE/TRUE
     *     packing ate every spare pixel and the list got pinned at
     *     its minimum, which on a tight column was a single row.
     *
     * Both layers stay -- (1) gives a sensible floor when the
     * dialog is small, (2) gives the list everything else. */
    gtk_scrolled_window_set_min_content_height (
            GTK_SCROLLED_WINDOW (scrolled), 180);
    gtk_scrolled_window_set_propagate_natural_height (
            GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_scrolled_window_set_shadow_type (
            GTK_SCROLLED_WINDOW (scrolled), GTK_SHADOW_IN);

    list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                     GTK_SELECTION_NONE);
    /* Belt-and-braces: an explicit height request on the listbox
     * itself stops anything in the expander/notebook nesting from
     * negotiating the list down to a single row.  The scrolled
     * window's min_content_height alone has been observed to be
     * ignored when the parent box is internally squeezed. */
    gtk_widget_set_size_request (list, -1, 160);
    gtk_widget_set_vexpand (list, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), list);

    ctx->list = GTK_LIST_BOX (list);
    return scrolled;
}

static GtkWidget *
build_url_row (ShareCtx *ctx)
{
    GtkWidget       *row;
    GtkStyleContext *sc;
    GtkWidget       *label;

    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

    label = gtk_label_new ("Connect a device to generate a share URL.");
    gtk_label_set_xalign     (GTK_LABEL (label), 0.0);
    gtk_label_set_selectable (GTK_LABEL (label), TRUE);
    gtk_label_set_line_wrap         (GTK_LABEL (label), TRUE);
    gtk_label_set_line_wrap_mode    (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars   (GTK_LABEL (label), 72);
    /* Monospace so the suffix that actually changes between modes
     * lines up cleanly when the user compares two URLs. */
    sc = gtk_widget_get_style_context (label);
    gtk_style_context_add_class (sc, GTK_STYLE_CLASS_MONOSPACE);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_box_pack_start (GTK_BOX (row), label, TRUE, TRUE, 0);
    ctx->url_label = GTK_LABEL (label);

    ctx->copy_button = GTK_BUTTON (gtk_button_new_from_icon_name (
            "edit-copy-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text (GTK_WIDGET (ctx->copy_button),
                                 "Copy URL to the clipboard");
    gtk_widget_set_valign (GTK_WIDGET (ctx->copy_button), GTK_ALIGN_START);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->copy_button), FALSE);
    gtk_box_pack_start (GTK_BOX (row), GTK_WIDGET (ctx->copy_button),
                        FALSE, FALSE, 0);

    return row;
}

static void
on_copy_clicked (GtkButton *button, gpointer user_data)
{
    ShareCtx     *ctx = user_data;
    GtkClipboard *clip;

    (void) button;
    if (ctx->url == NULL)
        return;

    clip = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text (clip, ctx->url, -1);
}

/* QR area: a square GtkDrawingArea when libqrencode is available at
 * build time, or a quiet "QR support not built" notice otherwise. */
static GtkWidget *
build_qr_area (ShareCtx *ctx)
{
    GtkWidget *area;

#ifdef HAVE_QRENCODE
    area = gtk_drawing_area_new ();
    gtk_widget_set_size_request (area, QR_SIDE_PX, QR_SIDE_PX);
    gtk_widget_set_halign (area, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (area, GTK_ALIGN_CENTER);
    /* Square: never expand vertically — the channel list above must
     * own the slack.  set_size_request keeps it at a min of
     * QR_SIDE_PX so phone cameras still lock on cleanly. */
    g_signal_connect (area, "draw", G_CALLBACK (on_qr_draw), ctx);
#else
    area = gtk_label_new (
        "Install libqrencode and rebuild for on-screen QR codes.\n"
        "The URL above is the full share payload and is valid on its own.");
    gtk_label_set_xalign    (GTK_LABEL (area), 0.5);
    gtk_label_set_justify   (GTK_LABEL (area), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap (GTK_LABEL (area), TRUE);
    gtk_style_context_add_class (
            gtk_widget_get_style_context (area), "dim-label");
#endif

    ctx->qr_area = area;
    return area;
}

GtkWidget *
pn_mesh_page_share_new (void)
{
    GtkWidget *page;
    ShareCtx  *ctx;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 8);
    gtk_widget_set_margin_end    (page, 8);
    gtk_widget_set_margin_top    (page, 8);
    gtk_widget_set_margin_bottom (page, 8);

    ctx = g_slice_new0 (ShareCtx);
    ctx->page = page;

    /* "Pick the channels…" subtitle sits at the very top -- it
     * explains what the list below is for and changes wording when
     * the user flips the mode radio (refresh_url updates it). */
    {
        GtkWidget *sub = gtk_label_new (
                "Pick the channels to share.");
        gtk_label_set_xalign    (GTK_LABEL (sub), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (sub), TRUE);
        gtk_style_context_add_class (
                gtk_widget_get_style_context (sub), "dim-label");
        g_object_set_data (G_OBJECT (page), "pn-share-subtitle", sub);
        gtk_box_pack_start (GTK_BOX (page), sub, FALSE, FALSE, 0);
    }

    /* Channel list: vexpand=TRUE inside the scrolled window (see
     * build_channel_list); packed TRUE/TRUE here so the box honours
     * that vexpand and the list is the one widget that grows when
     * the dialog is resized. */
    gtk_box_pack_start (GTK_BOX (page), build_channel_list (ctx),
                        TRUE, TRUE, 0);

    /* Mode toggle moved down so it sits RIGHT ABOVE the QR -- "I am
     * scanning the channels-only QR" / "I am scanning the full-config
     * QR" reads without scrolling. */
    gtk_box_pack_start (GTK_BOX (page), build_mode_toggle (ctx),
                        FALSE, FALSE, 0);

    /* Warning shown only when the user picks full-config but the
     * device has not exposed its LoRa config yet.  Hidden by default;
     * refresh_url toggles visibility. */
    {
        GtkWidget *w = gtk_label_new (
                "No LoRa config read from the device yet — full-config "
                "share is unavailable until the handshake completes.");
        gtk_label_set_line_wrap (GTK_LABEL (w), TRUE);
        gtk_label_set_xalign    (GTK_LABEL (w), 0.0);
        gtk_widget_set_no_show_all (w, TRUE);
        gtk_style_context_add_class (
                gtk_widget_get_style_context (w), "dim-label");
        ctx->full_config_warning = w;
        gtk_box_pack_start (GTK_BOX (page), w, FALSE, FALSE, 0);
    }

    gtk_box_pack_start (GTK_BOX (page), build_qr_area (ctx),
                        FALSE, FALSE, 0);

    /* URL row sits BELOW the QR -- keeps the QR the visual anchor
     * (the eye lands on it first), and stops the URL entry's focus
     * highlight from competing with the camera lock-on. */
    gtk_box_pack_start (GTK_BOX (page), build_url_row (ctx),
                        FALSE, FALSE, 0);
    g_signal_connect (ctx->copy_button, "clicked",
                      G_CALLBACK (on_copy_clicked), ctx);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_SHARE_CTX_QDATA,
                            ctx, share_ctx_free);
    return page;
}

/* ------------------------------------------------------------------ */
/*  Mode + selection                                                    */
/* ------------------------------------------------------------------ */

static ShareMode
current_mode (ShareCtx *ctx)
{
    return gtk_toggle_button_get_active (ctx->full_config_radio)
        ? SHARE_MODE_FULL_CONFIG
        : SHARE_MODE_CHANNELS;
}

/* Walk the channel list and build one checkbox row per non-disabled
 * channel.  Tagged with "pn-channel-index" so the toggle handler can
 * collect the selected indices.  Pre-checks every non-disabled slot
 * -- the common case is "share everything" and pre-checking matches
 * what pip-mesh's --print-qr-channels does. */
static void
rebuild_channel_list (ShareCtx *ctx)
{
    GList *children;
    GList *l;
    guint  i;

    children = gtk_container_get_children (GTK_CONTAINER (ctx->list));
    for (l = children; l != NULL; l = l->next)
        gtk_widget_destroy (GTK_WIDGET (l->data));
    g_list_free (children);

    if (ctx->state == NULL || ctx->state->channels == NULL)
        return;

    for (i = 0; i < ctx->state->channels->len; i++)
    {
        const PnMeshChannel *ch = g_ptr_array_index (ctx->state->channels, i);
        GtkWidget           *row;
        GtkWidget           *check;
        gchar               *label;
        const gchar         *role;

        if (ch == NULL || ch->role == ROLE_DISABLED)
            continue;

        role  = (ch->role == ROLE_PRIMARY) ? "PRIMARY" : "SECONDARY";
        label = g_strdup_printf ("Slot %u  —  %s  [%s]",
                                 ch->index,
                                 (ch->name != NULL && ch->name[0] != '\0')
                                     ? ch->name : "(unnamed)",
                                 role);

        check = gtk_check_button_new_with_label (label);
        g_free (label);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), TRUE);
        g_object_set_data (G_OBJECT (check), "pn-channel-index",
                           GUINT_TO_POINTER (ch->index));
        g_signal_connect_swapped (check, "toggled",
                G_CALLBACK (refresh_url), ctx);

        row = gtk_list_box_row_new ();
        gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), FALSE);
        gtk_container_add (GTK_CONTAINER (row), check);
        gtk_container_add (GTK_CONTAINER (ctx->list), row);
    }
    gtk_widget_show_all (GTK_WIDGET (ctx->list));
}

/* Update the subtitle line right under the mode radios to reflect
 * the active mode.  Pulls the label widget back out of qdata stashed
 * during construction. */
static void
update_subtitle (ShareCtx *ctx, ShareMode mode)
{
    GtkLabel *sub = g_object_get_data (G_OBJECT (ctx->page),
                                       "pn-share-subtitle");
    if (sub == NULL)
        return;

    gtk_label_set_text (sub,
        mode == SHARE_MODE_FULL_CONFIG
            ? "Receiver REPLACES its channels and LoRa stack with the "
              "selection below."
            : "Receiver merges the selected channels into its existing "
              "setup.");
}

/* ------------------------------------------------------------------ */
/*  URL + QR refresh                                                    */
/* ------------------------------------------------------------------ */

static void
refresh_url (ShareCtx *ctx)
{
    GArray   *selected;
    GList    *rows;
    GList    *l;
    ShareMode mode;
    gchar    *url   = NULL;
    GError   *error = NULL;

    mode = current_mode (ctx);
    update_subtitle (ctx, mode);

    /* Show the "LoRa not read yet" notice only when full-config is
     * selected but the handshake hasn't surfaced a LoRa config.  Same
     * branch decides whether the URL row can be populated at all. */
    if (ctx->full_config_warning != NULL)
    {
        gboolean show = (mode == SHARE_MODE_FULL_CONFIG
                         && (ctx->state == NULL
                             || !ctx->state->have_lora_config));
        gtk_widget_set_visible (ctx->full_config_warning, show);
    }

    if (ctx->state == NULL || ctx->state->channels == NULL)
    {
        apply_url (ctx, NULL);
        return;
    }

    /* Collect every checked channel index from the list. */
    selected = g_array_new (FALSE, FALSE, sizeof (guint32));
    rows = gtk_container_get_children (GTK_CONTAINER (ctx->list));
    for (l = rows; l != NULL; l = l->next)
    {
        GtkWidget *check = gtk_bin_get_child (GTK_BIN (l->data));
        guint32    idx;

        if (check == NULL || !GTK_IS_TOGGLE_BUTTON (check))
            continue;
        if (!gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (check)))
            continue;
        idx = GPOINTER_TO_UINT (
                g_object_get_data (G_OBJECT (check), "pn-channel-index"));
        g_array_append_val (selected, idx);
    }
    g_list_free (rows);

    if (mode == SHARE_MODE_FULL_CONFIG)
    {
        if (!ctx->state->have_lora_config)
        {
            apply_url (ctx, NULL);
            g_array_free (selected, TRUE);
            return;
        }
        {
            PnMeshLoraConfigWrite lora = {
                .use_preset   = ctx->state->lora_use_preset,
                .modem_preset = ctx->state->lora_modem_preset,
                .region       = ctx->state->lora_region,
                .hop_limit    = ctx->state->lora_hop_limit,
                /* tx_enabled / tx_power / channel_num are not in the
                 * QR subset -- pn_mesh_qr_build_share_url drops
                 * them. */
                .tx_enabled   = FALSE,
                .tx_power     = 0,
                .channel_num  = 0,
            };
            url = pn_mesh_qr_build_share_url (ctx->state->channels,
                                              selected->len > 0
                                                  ? (guint32 *) selected->data
                                                  : NULL,
                                              selected->len,
                                              &lora,
                                              &error);
        }
    }
    else
    {
        url = pn_mesh_qr_build_share_url (ctx->state->channels,
                                          selected->len > 0
                                              ? (guint32 *) selected->data
                                              : NULL,
                                          selected->len,
                                          NULL,
                                          &error);
    }
    g_clear_error (&error);
    g_array_free (selected, TRUE);

    apply_url (ctx, url);
    g_free (url);
}

/* Single point that mutates the URL entry + Copy sensitivity + QR
 * cache + redraw signal.  Pass NULL @url to clear. */
static void
apply_url (ShareCtx *ctx, const gchar *url)
{
    g_free (ctx->url);
    ctx->url = url != NULL ? g_strdup (url) : NULL;

    gtk_label_set_text (ctx->url_label,
                        url != NULL
                            ? url
                            : "Connect a device to generate a share URL.");
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->copy_button), url != NULL);

#ifdef HAVE_QRENCODE
    if (ctx->qr_area != NULL)
        gtk_widget_queue_draw (ctx->qr_area);
#endif
}

/* ------------------------------------------------------------------ */
/*  Cairo QR draw                                                       */
/* ------------------------------------------------------------------ */

#ifdef HAVE_QRENCODE
/* Render a QR code centred in the drawing area.  Background white +
 * 4-module quiet zone per the QR spec; modules drawn solid black.
 * Module size = floor (allocated_side / (matrix_width + 8)) -- the +8
 * budgets four modules of quiet zone per side. */
static gboolean
on_qr_draw (GtkWidget *area, cairo_t *cr, gpointer user_data)
{
    ShareCtx     *ctx = user_data;
    QRcode       *qr;
    GtkAllocation alloc;
    int           quiet = 4;
    int           total;
    double        module;
    double        xoff, yoff;
    int           x, y;

    if (ctx->url == NULL)
        return FALSE;

    qr = QRcode_encodeString (ctx->url, 0,
                              QR_ECLEVEL_M, QR_MODE_8, 1);
    if (qr == NULL)
        return FALSE;

    gtk_widget_get_allocation (area, &alloc);
    total = qr->width + 2 * quiet;
    module = (double) MIN (alloc.width, alloc.height) / (double) total;
    if (module < 1.0)
        module = 1.0;

    {
        double matrix_px = module * total;
        xoff = (alloc.width  - matrix_px) / 2.0;
        yoff = (alloc.height - matrix_px) / 2.0;
    }

    cairo_set_source_rgb (cr, 1.0, 1.0, 1.0);
    cairo_rectangle (cr, xoff, yoff, module * total, module * total);
    cairo_fill (cr);

    cairo_set_source_rgb (cr, 0.0, 0.0, 0.0);
    for (y = 0; y < qr->width; y++)
    {
        for (x = 0; x < qr->width; x++)
        {
            if (qr->data[y * qr->width + x] & 1)
            {
                cairo_rectangle (cr,
                        xoff + (x + quiet) * module,
                        yoff + (y + quiet) * module,
                        module, module);
                cairo_fill (cr);
            }
        }
    }

    QRcode_free (qr);
    return FALSE;
}
#endif  /* HAVE_QRENCODE */

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_share_set_state (GtkWidget *page, const PnMeshState *state)
{
    ShareCtx *ctx;

    g_return_if_fail (page != NULL);

    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_SHARE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->state = state;
    rebuild_channel_list (ctx);
    refresh_url          (ctx);
}
