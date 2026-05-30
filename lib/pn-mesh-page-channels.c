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
/*  Channels page — Phase 5.                                           */
/*                                                                     */
/*  A GtkListBox of channel slots in index order on top, and a        */
/*  channel-details editor below it.  The list is single-selection    */
/*  (GTK_SELECTION_BROWSE), so exactly one slot is always selected     */
/*  and the editor below it always has data to show.  Selecting a      */
/*  slot loads it into the editor; the user changes the name / PSK     */
/*  and presses Apply right there.                                     */
/*                                                                     */
/*  An empty (DISABLED) slot is selectable too: the editor then acts   */
/*  as "Add channel" -- it pre-fills a freshly generated PSK, and      */
/*  Apply writes the slot as SECONDARY.  That replaces the old         */
/*  modal "Add channel" dialog and its top-of-page button.            */
/*                                                                     */
/*  A "Delete channel" button in the editor disables the slot; it is   */
/*  sensitive only for SECONDARY channels.  PRIMARY is protected       */
/*  (removing it would orphan the device from every mesh), and an      */
/*  already-empty slot has nothing to delete.                          */
/*                                                                     */
/*  PSK contract: empty = no crypto; a 1-byte (2-hex-char) value is a  */
/*  publicly-known default-key index -- Meshtastic's "open" channel,   */
/*  which we treat as un-encrypted; 16 or 32 bytes (32 / 64 hex) is a  */
/*  real AES-128 / AES-256 key.  The editor shows the slot's current   */
/*  key verbatim, and the list flags each active slot with a closed    */
/*  padlock (real key) or open padlock (open / default channel).      */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-channels.h"

#include <gio/gio.h>

#define PN_MESH_CHANNELS_CTX_QDATA "pn-mesh-page-channels-ctx"

/* Meshtastic Channel.Role enum values. */
#define ROLE_DISABLED   0
#define ROLE_PRIMARY    1
#define ROLE_SECONDARY  2

/* ChannelSettings.ModuleSettings.position_precision: 0 = no position
 * shared, 32 = precise.  When the user shares position but turns
 * "precise" off we write an approximate precision -- a value the
 * Meshtastic apps treat as "imprecise"/reduced location. */
#define POS_PRECISION_PRECISE         32
#define POS_PRECISION_APPROX_DEFAULT  13

typedef struct
{
    GtkWidget        *page;
    GtkListBox       *list;
    GtkSpinner       *spinner;

    /* Details editor below the list. */
    GtkLabel  *detail_title;     /* "Slot #N — ROLE" */
    GtkEntry  *name_entry;
    GtkEntry  *psk_entry;
    GtkButton *generate_button;
    GtkSwitch *uplink_switch;
    GtkSwitch *downlink_switch;
    GtkSwitch *position_switch;
    GtkSwitch *precise_switch;
    GtkButton *apply_button;
    GtkButton *delete_button;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    /* Currently snapshotted channels.  Owned by the connection's
     * state, freed when it is.  We refresh from it on every paint. */
    const PnMeshState *state;

    /* The slot currently loaded into the editor, and its role.  -1 =
     * nothing selected (no connection / empty device list). */
    gint     selected_index;
    guint32  selected_role;

    /* Status string emitted on the next successful write.  Points at a
     * string literal, so it is never freed. */
    const gchar *pending_status;

    gboolean         writing;

    PnMeshChannelsStatusFunc status_cb;
    gpointer                 status_ud;

    PnMeshPageBusyFunc       busy_cb;
    gpointer                 busy_ud;

    PnMeshChannelsChangedFunc changed_cb;
    gpointer                  changed_ud;
} ChannelsCtx;

static void
channels_ctx_free (gpointer data)
{
    g_slice_free (ChannelsCtx, data);
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */

static const char *
role_name (guint32 role)
{
    switch (role)
    {
    case ROLE_DISABLED:  return "DISABLED";
    case ROLE_PRIMARY:   return "PRIMARY";
    case ROLE_SECONDARY: return "SECONDARY";
    default:             return "?";
    }
}

static void
emit_status (ChannelsCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* Forward decls. */
static void rebuild_list (GtkWidget *page);
static void sync_detail_sensitivity (ChannelsCtx *ctx);
static void on_channel_write_done (GObject *source, GAsyncResult *res,
                                   gpointer user_data);

/* ------------------------------------------------------------------ */
/*  Sensitivity + busy                                                  */
/* ------------------------------------------------------------------ */

/* Recompute the editor's sensitivity from the connection / writing /
 * selection state.  Delete is additionally gated to SECONDARY rows:
 * PRIMARY is protected and an empty slot has nothing to delete. */
static void
sync_detail_sensitivity (ChannelsCtx *ctx)
{
    gboolean base = !ctx->writing
                    && ctx->connection != NULL
                    && ctx->selected_index >= 0;

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->name_entry),      base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->psk_entry),       base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->generate_button), base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->uplink_switch),   base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->downlink_switch), base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->position_switch), base);
    /* Precise location only matters when a position is being shared. */
    gtk_widget_set_sensitive (
            GTK_WIDGET (ctx->precise_switch),
            base && gtk_switch_get_active (ctx->position_switch));
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),    base);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->delete_button),
                              base && ctx->selected_role == ROLE_SECONDARY);
}

/* Toggling "Share position" flips the "Precise location" switch between
 * usable and greyed without waiting for Apply. */
static void
on_position_toggled (GObject *obj, GParamSpec *pspec, gpointer user_data)
{
    GtkWidget   *page = user_data;
    ChannelsCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_CHANNELS_CTX_QDATA);
    (void) obj;
    (void) pspec;
    if (ctx != NULL)
        sync_detail_sensitivity (ctx);
}

/* Lock the editor while a write is in flight so a second Apply or
 * Delete cannot interleave on the serial port, and spin the busy
 * indicator next to Apply -- the verify-cycle handshake takes 3-5
 * seconds and the status text alone reads as inert. */
static void
set_writing (ChannelsCtx *ctx, gboolean writing)
{
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;

    if (writing)
    {
        gtk_spinner_start (ctx->spinner);
        gtk_widget_show   (GTK_WIDGET (ctx->spinner));
    }
    else
    {
        gtk_spinner_stop (ctx->spinner);
        gtk_widget_hide  (GTK_WIDGET (ctx->spinner));
    }

    /* The list itself stays browsable during a write so the user can
     * read other slots, but the editor controls go insensitive. */
    sync_detail_sensitivity (ctx);

    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* ------------------------------------------------------------------ */
/*  PSK generation + validation                                         */
/* ------------------------------------------------------------------ */

/* Read @n bytes from /dev/urandom and return them base64-encoded (the
 * same encoding the Meshtastic phone app and channel URLs use).  @n is
 * the AES key length -- 16 for AES-128, 32 for AES-256.  Caller
 * g_free()s.  Returns NULL on a read failure (extremely unlikely on
 * Linux; the page falls back to GLib's PRNG with a warning if it
 * happens). */
static gchar *
generate_psk_b64_len (gsize n)
{
    guint8         buf[32];
    GFile         *urandom;
    GFileInputStream *in;
    gssize         got;
    gsize          i;

    g_return_val_if_fail (n > 0 && n <= sizeof buf, NULL);

    urandom = g_file_new_for_path ("/dev/urandom");
    in = g_file_read (urandom, NULL, NULL);
    g_object_unref (urandom);
    if (in == NULL)
    {
        g_warning ("pn-mesh-page-channels: /dev/urandom unavailable, "
                   "falling back to GLib PRNG (NOT cryptographically "
                   "strong); regenerate the PSK on a real Linux host.");
        for (i = 0; i < n; i++)
            buf[i] = (guint8) g_random_int_range (0, 256);
    }
    else
    {
        got = g_input_stream_read (G_INPUT_STREAM (in),
                                   buf, n, NULL, NULL);
        g_object_unref (in);
        if (got != (gssize) n)
            return NULL;
    }

    return g_base64_encode (buf, n);
}

/* Validate @s and decode into a freshly malloc'd byte buffer.  @s is
 * base64 (matching the phone app): empty -> open channel (*out = NULL);
 * a 1-byte key (e.g. "AQ==") is the publicly-known default; 16 or 32
 * bytes are AES-128 / AES-256.  Returns FALSE with a message in @err on
 * a malformed value. */
static gboolean
decode_psk_b64 (const gchar *s, guint8 **out, gsize *out_size, gchar **err)
{
    guchar *buf;
    gsize   len = 0;

    *out = NULL;
    *out_size = 0;
    *err = NULL;

    if (*s == '\0')
        return TRUE;   /* open channel: no key */

    buf = g_base64_decode (s, &len);
    if (buf == NULL || (len != 1 && len != 16 && len != 32))
    {
        g_free (buf);
        *err = g_strdup (
                "PSK must be a base64 key: empty (open), a 1-byte default "
                "key such as \"AQ==\", or a 16-byte (AES-128) / 32-byte "
                "(AES-256) key.");
        return FALSE;
    }
    *out = buf;
    *out_size = len;
    return TRUE;
}

/* base64 encoding of a PSK (phone-app style), or "" for an absent /
 * empty key.  Caller g_free()s. */
static gchar *
psk_to_b64 (const guint8 *psk, gsize psk_size)
{
    if (psk == NULL || psk_size == 0)
        return g_strdup ("");
    return g_base64_encode (psk, psk_size);
}

/* A channel counts as encrypted only when it carries a real AES key.
 * An empty PSK is no-crypto; a 1-byte PSK is a publicly-known default-
 * key index (the "open" channel Meshtastic users share), which offers
 * no privacy -- so both read as un-encrypted. */
static gboolean
channel_is_encrypted (const guint8 *psk, gsize psk_size)
{
    return psk != NULL && psk_size > 1;
}

/* ------------------------------------------------------------------ */
/*  Role badge                                                          */
/* ------------------------------------------------------------------ */

/* Add a CSS class so we can tint role badges differently in different
 * themes; we keep it minimal and rely on inline markup so a missing
 * stylesheet still reads as a badge. */
static GtkWidget *
make_role_badge (guint32 role)
{
    const char *name = role_name (role);
    const char *colour;
    gchar      *markup;
    GtkWidget  *label;

    switch (role)
    {
    case ROLE_PRIMARY:   colour = "#27ae60"; break;  /* green   */
    case ROLE_SECONDARY: colour = "#2980b9"; break;  /* blue    */
    case ROLE_DISABLED:
    default:             colour = "#7f8c8d"; break;  /* grey    */
    }
    markup = g_markup_printf_escaped (
            "<span foreground='%s' weight='bold' size='small'>"
            "%s</span>", colour, name);
    label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (label), markup);
    g_free (markup);
    return label;
}

/* ------------------------------------------------------------------ */
/*  Editor population                                                   */
/* ------------------------------------------------------------------ */

static const PnMeshChannel *
find_channel_by_index (const PnMeshState *state, guint32 index)
{
    guint i;
    if (state == NULL || state->channels == NULL)
        return NULL;
    for (i = 0; i < state->channels->len; i++)
    {
        const PnMeshChannel *ch = g_ptr_array_index (state->channels, i);
        if (ch->index == index)
            return ch;
    }
    return NULL;
}

/* Load @ch into the editor and remember it as the selection.  An
 * empty (DISABLED) slot switches the editor into "add" mode: blank
 * name and a freshly generated PSK, with Apply relabelled. */
static void
populate_details (ChannelsCtx *ctx, const PnMeshChannel *ch)
{
    gboolean  empty = (ch->role == ROLE_DISABLED);
    gchar    *title;

    ctx->selected_index = (gint) ch->index;
    ctx->selected_role  = ch->role;

    title = g_strdup_printf ("Slot #%u — %s",
                             ch->index, empty ? "EMPTY" : role_name (ch->role));
    gtk_label_set_text (ctx->detail_title, title);
    g_free (title);

    if (empty)
    {
        /* Pre-fill a fresh private key so the common "add a private
         * channel" case is one click; clear it for an open channel.
         * New channels default to no MQTT bridging and no position. */
        gchar *psk = generate_psk_b64_len (32);
        gtk_entry_set_text (ctx->name_entry, "");
        gtk_entry_set_placeholder_text (ctx->name_entry, "e.g. MyMesh");
        gtk_entry_set_text (ctx->psk_entry, psk != NULL ? psk : "");
        gtk_button_set_label (ctx->apply_button, "_Add channel");
        gtk_switch_set_active (ctx->uplink_switch,   FALSE);
        gtk_switch_set_active (ctx->downlink_switch, FALSE);
        gtk_switch_set_active (ctx->position_switch, FALSE);
        gtk_switch_set_active (ctx->precise_switch,  FALSE);
        g_free (psk);
    }
    else
    {
        /* Show the slot's current key verbatim -- the short well-known
         * default key, a real AES key, or empty for an open channel. */
        gchar *psk = psk_to_b64 (ch->psk, ch->psk_size);
        gtk_entry_set_text (ctx->name_entry, ch->name != NULL ? ch->name : "");
        gtk_entry_set_text (ctx->psk_entry, psk);
        gtk_button_set_label (ctx->apply_button, "_Apply changes");
        gtk_switch_set_active (ctx->uplink_switch,   ch->uplink_enabled);
        gtk_switch_set_active (ctx->downlink_switch, ch->downlink_enabled);
        gtk_switch_set_active (ctx->position_switch,
                               ch->position_precision > 0);
        gtk_switch_set_active (ctx->precise_switch,
                               ch->position_precision >= POS_PRECISION_PRECISE);
        g_free (psk);
    }
    gtk_entry_set_placeholder_text (ctx->psk_entry,
            "Empty = open; \"AQ==\" = default; or a base64 AES key");

    sync_detail_sensitivity (ctx);
}

static void
on_row_selected (GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    GtkWidget           *page = user_data;
    ChannelsCtx         *ctx  = g_object_get_data (G_OBJECT (page),
                                                   PN_MESH_CHANNELS_CTX_QDATA);
    const PnMeshChannel *ch;
    gint                 index;

    (void) list;
    /* Selection briefly drops to NULL while rebuild_list clears the
     * rows; ignore that and wait for the real re-selection. */
    if (ctx == NULL || row == NULL)
        return;

    index = (gint) (gintptr) g_object_get_data (G_OBJECT (row), "pn-index");
    ch = find_channel_by_index (ctx->state, (guint32) index);
    if (ch != NULL)
        populate_details (ctx, ch);
}

/* ------------------------------------------------------------------ */
/*  Apply / Delete / Generate                                           */
/* ------------------------------------------------------------------ */

static void
on_channel_write_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget   *page = user_data;
    ChannelsCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_CHANNELS_CTX_QDATA);
    GError      *error = NULL;
    gboolean     ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_channel_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_channel_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not save channel: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    /* Re-read and rebuild; rebuild_list re-selects the same slot,
     * which reloads the editor with the verified device state. */
    ctx->state = pn_mesh_connection_get_state (ctx->connection);
    rebuild_list (page);
    emit_status (ctx, ctx->pending_status != NULL ? ctx->pending_status
                                                  : "Channel saved.");
    set_writing (ctx, FALSE);

    /* The channel set just changed under the dialog's feet; sibling
     * pages that mirror the list (Share) are still showing the old
     * snapshot.  Let the dialog re-push state to them. */
    if (ctx->changed_cb != NULL)
        ctx->changed_cb (ctx->changed_ud);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget   *page = user_data;
    ChannelsCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_CHANNELS_CTX_QDATA);
    const gchar *name;
    const gchar *psk_b64;
    guint8      *psk_bytes = NULL;
    gsize        psk_size  = 0;
    gboolean     own_psk   = FALSE;   /* psk_bytes malloc'd here? */
    gboolean     adding;
    guint32      write_role;
    guint32      precision;
    gchar       *name_owned;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing
        || ctx->selected_index < 0)
        return;

    adding  = (ctx->selected_role == ROLE_DISABLED);
    name    = gtk_entry_get_text (ctx->name_entry);
    psk_b64 = gtk_entry_get_text (ctx->psk_entry);

    /* A new channel needs a name; editing an existing one may blank it
     * (the firmware falls back to its modem-preset default name). */
    if (adding && *name == '\0')
    {
        gtk_widget_error_bell (GTK_WIDGET (ctx->name_entry));
        gtk_widget_grab_focus (GTK_WIDGET (ctx->name_entry));
        return;
    }

    /* The entry holds the channel's key verbatim, so it is the source
     * of truth: empty means open (no crypto), otherwise decode it. */
    {
        gchar *err = NULL;
        if (!decode_psk_b64 (psk_b64, &psk_bytes, &psk_size, &err))
        {
            emit_status (ctx, err);
            g_free (err);
            gtk_widget_error_bell (GTK_WIDGET (ctx->psk_entry));
            gtk_widget_grab_focus (GTK_WIDGET (ctx->psk_entry));
            return;
        }
        own_psk = (psk_bytes != NULL);
    }

    /* Position precision: off (0), precise (32), or an approximate
     * value when position is shared but "precise" is off.  Reuse the
     * channel's existing reduced precision if it had one, so editing an
     * unrelated field does not bump an approximate radius around. */
    if (!gtk_switch_get_active (ctx->position_switch))
        precision = 0;
    else if (gtk_switch_get_active (ctx->precise_switch))
        precision = POS_PRECISION_PRECISE;
    else
    {
        const PnMeshChannel *cur =
                find_channel_by_index (ctx->state,
                                       (guint32) ctx->selected_index);
        precision = (cur != NULL
                     && cur->position_precision > 0
                     && cur->position_precision < POS_PRECISION_PRECISE)
                ? cur->position_precision
                : POS_PRECISION_APPROX_DEFAULT;
    }

    write_role = adding ? ROLE_SECONDARY : ctx->selected_role;

    name_owned = g_strdup (name);
    ctx->pending_status = adding ? "Channel added." : "Channel updated.";
    emit_status (ctx, adding ? "Adding channel…" : "Applying channel…");
    set_writing (ctx, TRUE);
    /* Editing an existing slot needs the begin/commit edit transaction
     * to persist; a fresh add into a free slot does not (and avoids the
     * commit-to-flash). */
    pn_mesh_connection_set_channel_async (
            ctx->connection,
            (guint32) ctx->selected_index, name_owned,
            psk_bytes, psk_size, write_role,
            gtk_switch_get_active (ctx->uplink_switch),
            gtk_switch_get_active (ctx->downlink_switch),
            precision,
            /*transactional=*/!adding,
            NULL, on_channel_write_done, page);
    g_free (name_owned);
    if (own_psk)
        g_free (psk_bytes);
}

static void
on_delete_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget   *page = user_data;
    ChannelsCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_CHANNELS_CTX_QDATA);

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing
        || ctx->selected_index < 0 || ctx->selected_role != ROLE_SECONDARY)
        return;

    ctx->pending_status = "Channel deleted.";
    emit_status (ctx, "Deleting channel…");
    set_writing (ctx, TRUE);
    /* DISABLED write with empty name, no PSK, no MQTT bridge and no
     * position -- pip-mesh's delete_channel does the same. */
    pn_mesh_connection_set_channel_async (
            ctx->connection,
            (guint32) ctx->selected_index, "", NULL, 0, ROLE_DISABLED,
            FALSE, FALSE, 0,
            /*transactional=*/FALSE,
            NULL, on_channel_write_done, page);
}

/* The "key kind" each Generate-menu item carries as qdata.  The AES
 * kinds double as the byte length passed to generate_psk_b64_len (); the
 * two short kinds are handled specially (no randomness involved). */
#define KEY_KIND_EMPTY    0   /* open channel: clear the field          */
#define KEY_KIND_DEFAULT  1   /* the 1-byte well-known default ("AQ==")  */
#define KEY_KIND_AES128  16   /* 16 random bytes                         */
#define KEY_KIND_AES256  32   /* 32 random bytes                         */

#define PN_KEY_KIND_QDATA "pn-key-kind"

/* A Generate-menu item was chosen: set the PSK entry to a key of the
 * item's kind.  EMPTY clears the field; DEFAULT writes the fixed 1-byte
 * default key (0x01 -> "AQ=="); the AES kinds pull fresh random bytes. */
static void
on_generate_key (GtkMenuItem *item, gpointer user_data)
{
    GtkWidget   *page = user_data;
    ChannelsCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_CHANNELS_CTX_QDATA);
    guint  kind;
    gchar *key;

    if (ctx == NULL)
        return;

    kind = GPOINTER_TO_UINT (
            g_object_get_data (G_OBJECT (item), PN_KEY_KIND_QDATA));

    if (kind == KEY_KIND_EMPTY)
    {
        gtk_entry_set_text (ctx->psk_entry, "");
        return;
    }

    if (kind == KEY_KIND_DEFAULT)
    {
        /* The publicly-known default key is the single byte 0x01, which
         * base64-encodes to "AQ==" -- compute it rather than hard-coding
         * the literal so the encoding stays in one place. */
        guint8 b = 0x01;
        key = g_base64_encode (&b, 1);
    }
    else
    {
        key = generate_psk_b64_len ((gsize) kind);
    }

    if (key == NULL)
        return;
    gtk_entry_set_text (ctx->psk_entry, key);
    g_free (key);
}

/* Append one Generate-menu entry that produces a key of @kind. */
static void
add_generate_item (GtkWidget   *menu,
                   const gchar *label,
                   guint        kind,
                   GtkWidget   *page)
{
    GtkWidget *item = gtk_menu_item_new_with_label (label);

    g_object_set_data (G_OBJECT (item), PN_KEY_KIND_QDATA,
                       GUINT_TO_POINTER (kind));
    g_signal_connect (item, "activate", G_CALLBACK (on_generate_key), page);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
}

/* ------------------------------------------------------------------ */
/*  Row construction                                                    */
/* ------------------------------------------------------------------ */

/* Build one list row.  The slot index and role are stashed as qdata
 * ("pn-index" / "pn-role") so the selection handler and rebuild can
 * map a row back to its channel. */
static GtkWidget *
build_channel_row (const PnMeshChannel *ch)
{
    GtkWidget   *row;
    GtkWidget   *grid;
    GtkWidget   *idx_label;
    GtkWidget   *name_label;
    GtkWidget   *badge;
    GtkWidget   *psk_icon;
    gchar       *idx_text;
    const gchar *name_text;

    row = gtk_list_box_row_new ();

    grid = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_start  (grid, 10);
    gtk_widget_set_margin_end    (grid, 10);
    gtk_widget_set_margin_top    (grid, 6);
    gtk_widget_set_margin_bottom (grid, 6);

    /* Column 0: slot index in monospace so the column lines up. */
    idx_text = g_strdup_printf ("<tt>#%u</tt>", ch->index);
    idx_label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (idx_label), idx_text);
    gtk_widget_set_size_request (idx_label, 36, -1);
    gtk_grid_attach (GTK_GRID (grid), idx_label, 0, 0, 1, 1);
    g_free (idx_text);

    /* Column 1: name (or a placeholder reflecting the row's state). */
    name_text = (ch->name != NULL && *ch->name != '\0')
            ? ch->name
            : (ch->role == ROLE_DISABLED ? "(empty slot)"
                                         : "(unnamed)");
    name_label = gtk_label_new (name_text);
    gtk_label_set_xalign  (GTK_LABEL (name_label), 0.0);
    gtk_label_set_ellipsize (GTK_LABEL (name_label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand (name_label, TRUE);
    if (ch->role == ROLE_DISABLED)
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (name_label);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_grid_attach (GTK_GRID (grid), name_label, 1, 0, 1, 1);

    /* Column 2: a padlock on every active slot -- closed for a real
     * AES key, open for an "open"/default channel that offers no
     * privacy.  Disabled slots leave the column blank but reserved so
     * the role badges still line up. */
    if (ch->role != ROLE_DISABLED)
    {
        gboolean enc = channel_is_encrypted (ch->psk, ch->psk_size);
        psk_icon = gtk_image_new_from_icon_name (
                enc ? "changes-prevent-symbolic"   /* closed padlock */
                    : "changes-allow-symbolic",    /* open padlock   */
                GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text (
                psk_icon,
                enc ? (ch->psk_size == 16 ? "Encrypted (AES-128)"
                                          : "Encrypted (AES-256)")
                    : "Open channel — no private key");
    }
    else
    {
        psk_icon = gtk_label_new ("");
    }
    gtk_grid_attach (GTK_GRID (grid), psk_icon, 2, 0, 1, 1);

    /* Column 3: role badge. */
    badge = make_role_badge (ch->role);
    gtk_widget_set_size_request (badge, 90, -1);
    gtk_grid_attach (GTK_GRID (grid), badge, 3, 0, 1, 1);

    gtk_container_add (GTK_CONTAINER (row), grid);

    g_object_set_data (G_OBJECT (row), "pn-index",
                       (gpointer) (gintptr) ch->index);
    g_object_set_data (G_OBJECT (row), "pn-role",
                       (gpointer) (gintptr) ch->role);
    return row;
}

static void
clear_list (GtkListBox *list)
{
    GList *rows = gtk_container_get_children (GTK_CONTAINER (list));
    GList *l;
    for (l = rows; l != NULL; l = l->next)
        gtk_widget_destroy (GTK_WIDGET (l->data));
    g_list_free (rows);
}

/* Select the row for slot @index, falling back to the first row when
 * that slot is gone.  Selecting fires on_row_selected, which loads the
 * editor -- so this is also what keeps the editor populated. */
static void
select_row_by_index (ChannelsCtx *ctx, gint index)
{
    GList         *rows = gtk_container_get_children (GTK_CONTAINER (ctx->list));
    GList         *l;
    GtkListBoxRow *target = NULL;
    GtkListBoxRow *first  = NULL;

    for (l = rows; l != NULL; l = l->next)
    {
        GtkListBoxRow *r  = l->data;
        gint           ri = (gint) (gintptr) g_object_get_data (G_OBJECT (r),
                                                                "pn-index");
        if (first == NULL)
            first = r;
        if (ri == index)
        {
            target = r;
            break;
        }
    }
    g_list_free (rows);

    if (target == NULL)
        target = first;

    if (target != NULL)
    {
        gtk_list_box_select_row (ctx->list, target);
    }
    else
    {
        /* No rows at all (no device / empty list): nothing to edit. */
        ctx->selected_index = -1;
        sync_detail_sensitivity (ctx);
    }
}

static void
rebuild_list (GtkWidget *page)
{
    ChannelsCtx *ctx = g_object_get_data (G_OBJECT (page),
                                          PN_MESH_CHANNELS_CTX_QDATA);
    guint        i;

    g_return_if_fail (ctx != NULL);

    clear_list (ctx->list);

    if (ctx->state == NULL || ctx->state->channels == NULL)
    {
        ctx->selected_index = -1;
        gtk_widget_show_all (GTK_WIDGET (ctx->list));
        sync_detail_sensitivity (ctx);
        return;
    }

    for (i = 0; i < ctx->state->channels->len; i++)
    {
        const PnMeshChannel *ch = g_ptr_array_index (ctx->state->channels, i);
        gtk_container_add (GTK_CONTAINER (ctx->list), build_channel_row (ch));
    }

    gtk_widget_show_all (GTK_WIDGET (ctx->list));

    /* Re-establish the selection (keeps the same slot across a write,
     * defaults to the first row on a fresh load). */
    select_row_by_index (ctx, ctx->selected_index);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget *
make_key_label (const gchar *text)
{
    GtkWidget     *key   = gtk_label_new (text);
    PangoAttrList *attrs = pango_attr_list_new ();
    pango_attr_list_insert (attrs,
                            pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (key), attrs);
    pango_attr_list_unref (attrs);
    gtk_label_set_xalign (GTK_LABEL (key), 0.0);
    return key;
}

/* Bold key label in column 0 and a left-aligned switch in column 1. */
static GtkSwitch *
attach_switch_row (GtkGrid *grid, gint row, const gchar *label,
                   const gchar *tooltip)
{
    GtkWidget *sw = gtk_switch_new ();

    gtk_grid_attach (grid, make_key_label (label), 0, row, 1, 1);
    gtk_widget_set_halign (sw, GTK_ALIGN_START);
    if (tooltip != NULL)
        gtk_widget_set_tooltip_text (sw, tooltip);
    gtk_grid_attach (grid, sw, 1, row, 1, 1);
    return GTK_SWITCH (sw);
}

GtkWidget *
pn_mesh_page_channels_new (void)
{
    ChannelsCtx *ctx;
    GtkWidget   *page;
    GtkWidget   *subtitle;
    GtkWidget   *scrolled;
    GtkWidget   *list;
    GtkWidget   *detail_box;
    GtkWidget   *detail_grid;
    GtkWidget   *title;
    GtkWidget   *psk_row;
    GtkWidget   *button_row;
    GtkWidget   *spacer;
    GtkWidget   *name_entry;
    GtkWidget   *psk_entry;
    GtkWidget   *generate;
    GtkSwitch   *uplink;
    GtkSwitch   *downlink;
    GtkSwitch   *position_sw;
    GtkSwitch   *precise;
    GtkWidget   *apply;
    GtkWidget   *delete_btn;
    GtkWidget   *spinner;

    /* Hosted inside a GtkExpander; small margins, no in-page title
     * (the expander header carries the section name). */
    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "Every channel slot the device exposes.  Select a slot to "
            "edit it below; the primary channel is what nodes talk on "
            "by default, secondary channels are private groups your "
            "peers must also configure.  Select an empty slot to add a "
            "new channel.  Changes affect the device live -- no reboot.");
    gtk_label_set_xalign      (GTK_LABEL (subtitle), 0.0);
    gtk_label_set_line_wrap   (GTK_LABEL (subtitle), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (subtitle), 72);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    /* ---- the list ---- */
    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    /* Floor at ~6 rows and propagate natural height so the page is
     * tall enough to actually display the device's eight slots before
     * scrolling. */
    gtk_scrolled_window_set_min_content_height (
            GTK_SCROLLED_WINDOW (scrolled), 200);
    gtk_scrolled_window_set_propagate_natural_height (
            GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);

    list = gtk_list_box_new ();
    /* BROWSE keeps exactly one row selected at all times, so the
     * editor below always has a slot to show. */
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                     GTK_SELECTION_BROWSE);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (list);
        gtk_style_context_add_class (sc, "frame");
    }
    gtk_widget_set_size_request (list, -1, 180);
    gtk_widget_set_vexpand (list, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), list);
    gtk_box_pack_start (GTK_BOX (page), scrolled, TRUE, TRUE, 0);

    /* ---- the editor ---- */
    detail_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top (detail_box, 6);

    title = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs,
                                pango_attr_weight_new (PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes (GTK_LABEL (title), attrs);
        pango_attr_list_unref (attrs);
    }
    gtk_box_pack_start (GTK_BOX (detail_box), title, FALSE, FALSE, 0);

    detail_grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (detail_grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (detail_grid), 12);

    /* Name */
    gtk_grid_attach (GTK_GRID (detail_grid), make_key_label ("Name"),
                     0, 0, 1, 1);
    name_entry = gtk_entry_new ();
    gtk_widget_set_hexpand (name_entry, TRUE);
    gtk_grid_attach (GTK_GRID (detail_grid), name_entry, 1, 0, 1, 1);

    /* PSK + Generate */
    gtk_grid_attach (GTK_GRID (detail_grid), make_key_label ("PSK (base64)"),
                     0, 1, 1, 1);
    psk_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    psk_entry = gtk_entry_new ();
    /* base64 of a 32-byte key is 44 chars; leave headroom. */
    gtk_entry_set_max_length (GTK_ENTRY (psk_entry), 48);
    gtk_widget_set_hexpand   (psk_entry, TRUE);
    gtk_box_pack_start (GTK_BOX (psk_row), psk_entry, TRUE, TRUE, 0);
    /* Generate is a dropdown: the user picks the key type Meshtastic
     * supports -- a random AES-256 / AES-128 key, the 1-byte default
     * key ("AQ=="), or no key at all (open channel). */
    generate = gtk_menu_button_new ();
    gtk_widget_set_tooltip_text (generate,
            "Set the channel key: a fresh random AES key, the default "
            "\"AQ==\" key, or no key (open channel).");
    {
        GtkWidget *gen_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *gen_lbl = gtk_label_new_with_mnemonic ("_Generate");
        GtkWidget *gen_arrow = gtk_image_new_from_icon_name (
                "pan-down-symbolic", GTK_ICON_SIZE_BUTTON);
        GtkWidget *gen_menu = gtk_menu_new ();

        gtk_label_set_mnemonic_widget (GTK_LABEL (gen_lbl), generate);
        gtk_box_pack_start (GTK_BOX (gen_box), gen_lbl,   FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (gen_box), gen_arrow, FALSE, FALSE, 0);
        gtk_container_add (GTK_CONTAINER (generate), gen_box);

        add_generate_item (gen_menu, "AES-256 key (random)",
                           KEY_KIND_AES256, page);
        add_generate_item (gen_menu, "AES-128 key (random)",
                           KEY_KIND_AES128, page);
        add_generate_item (gen_menu, "Default key (AQ==)",
                           KEY_KIND_DEFAULT, page);
        add_generate_item (gen_menu, "No key (open channel)",
                           KEY_KIND_EMPTY, page);
        gtk_widget_show_all (gen_menu);
        gtk_menu_button_set_popup (GTK_MENU_BUTTON (generate), gen_menu);
    }
    gtk_box_pack_start (GTK_BOX (psk_row), generate, FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (detail_grid), psk_row, 1, 1, 1, 1);

    /* MQTT bridge + position sharing, mirroring the phone app. */
    uplink = attach_switch_row (GTK_GRID (detail_grid), 2, "Uplink enabled",
            "Forward messages from this channel out to the MQTT gateway.");
    downlink = attach_switch_row (GTK_GRID (detail_grid), 3, "Downlink enabled",
            "Inject messages received from the MQTT gateway onto this "
            "channel.");
    position_sw = attach_switch_row (GTK_GRID (detail_grid), 4, "Position",
            "Share this node's location with other nodes on the channel.");
    precise = attach_switch_row (GTK_GRID (detail_grid), 5, "Precise location",
            "Share the exact location.  Off shares an approximate "
            "(reduced-precision) position instead.");

    gtk_box_pack_start (GTK_BOX (detail_box), detail_grid, FALSE, FALSE, 0);

    /* Action row: Delete on the left, Apply on the right, busy spinner
     * tucked just before Apply. */
    button_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top (button_row, 6);

    delete_btn = gtk_button_new_with_mnemonic ("_Delete channel");
    gtk_widget_set_tooltip_text (delete_btn,
            "Disable this slot.  Only secondary channels can be deleted; "
            "the primary channel is protected.");
    gtk_box_pack_start (GTK_BOX (button_row), delete_btn, FALSE, FALSE, 0);

    spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (spacer, TRUE);
    gtk_box_pack_start (GTK_BOX (button_row), spacer, TRUE, TRUE, 0);

    spinner = gtk_spinner_new ();
    gtk_widget_set_no_show_all (spinner, TRUE);
    gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (button_row), spinner, FALSE, FALSE, 0);

    apply = gtk_button_new_with_mnemonic ("_Apply changes");
    gtk_widget_set_tooltip_text (apply,
            "Write the values above to the selected slot.  The device is "
            "read back to confirm the change took.");
    gtk_box_pack_start (GTK_BOX (button_row), apply, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (detail_box), button_row, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (page), detail_box, FALSE, FALSE, 0);

    /* ---- ctx + wiring ---- */
    ctx = g_slice_new0 (ChannelsCtx);
    ctx->page            = page;
    ctx->list            = GTK_LIST_BOX (list);
    ctx->spinner         = GTK_SPINNER (spinner);
    ctx->detail_title    = GTK_LABEL (title);
    ctx->name_entry      = GTK_ENTRY (name_entry);
    ctx->psk_entry       = GTK_ENTRY (psk_entry);
    ctx->generate_button = GTK_BUTTON (generate);
    ctx->uplink_switch   = uplink;
    ctx->downlink_switch = downlink;
    ctx->position_switch = position_sw;
    ctx->precise_switch  = precise;
    ctx->apply_button    = GTK_BUTTON (apply);
    ctx->delete_button   = GTK_BUTTON (delete_btn);
    ctx->selected_index  = -1;

    g_object_set_data_full (G_OBJECT (page), PN_MESH_CHANNELS_CTX_QDATA,
                            ctx, channels_ctx_free);

    g_signal_connect (list, "row-selected",
                      G_CALLBACK (on_row_selected), page);
    /* Generate's behaviour lives on its dropdown menu items
     * (on_generate_key), wired in add_generate_item above. */
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);
    g_signal_connect (delete_btn, "clicked",
                      G_CALLBACK (on_delete_clicked), page);
    g_signal_connect (position_sw, "notify::active",
                      G_CALLBACK (on_position_toggled), page);

    /* Editor starts insensitive until a device + selection arrive. */
    sync_detail_sensitivity (ctx);

    return page;
}

void
pn_mesh_page_channels_set_state (GtkWidget         *page,
                                 const PnMeshState *state,
                                 PnMeshConnection  *connection)
{
    ChannelsCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_CHANNELS_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;
    ctx->state      = state;
    rebuild_list (page);
    set_writing (ctx, FALSE);
}

void
pn_mesh_page_channels_set_status_callback (GtkWidget                *page,
                                           PnMeshChannelsStatusFunc  callback,
                                           gpointer                  user_data)
{
    ChannelsCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_CHANNELS_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_channels_set_busy_callback (GtkWidget          *page,
                                         PnMeshPageBusyFunc  callback,
                                         gpointer            user_data)
{
    ChannelsCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_CHANNELS_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}

void
pn_mesh_page_channels_set_changed_callback (GtkWidget                 *page,
                                            PnMeshChannelsChangedFunc  callback,
                                            gpointer                   user_data)
{
    ChannelsCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_CHANNELS_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->changed_cb = callback;
    ctx->changed_ud = user_data;
}
