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
/*  GtkListBox of channel slots in index order.  Each row shows the   */
/*  slot number, the channel name (or "(unnamed)" / "(disabled)"),    */
/*  a role badge tinted by role, a small "PSK set" lock indicator,    */
/*  and a Delete button on the right.  Delete is sensitive only for   */
/*  SECONDARY rows; PRIMARY is protected (removing it would orphan    */
/*  the device from every mesh) and DISABLED rows have nothing to     */
/*  delete.                                                            */
/*                                                                     */
/*  "Add channel" at the top of the page opens a modal asking for     */
/*  name + PSK with a Generate button that reads 32 random bytes      */
/*  from /dev/urandom and shows them hex-encoded.  pip-mesh's         */
/*  contract: PSK omitted = random AES-256; PSK supplied = 32 or 64   */
/*  hex chars exactly.  The new channel goes into the first DISABLED  */
/*  slot with index > 0 (index 0 is reserved for PRIMARY).            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-channels.h"

#include <gio/gio.h>
#include <string.h>

#define PN_MESH_CHANNELS_CTX_QDATA "pn-mesh-page-channels-ctx"

/* Meshtastic Channel.Role enum values. */
#define ROLE_DISABLED   0
#define ROLE_PRIMARY    1
#define ROLE_SECONDARY  2

typedef struct
{
    GtkWidget        *page;
    GtkWidget        *add_button;
    GtkWidget        *spinner;
    GtkListBox       *list;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    /* Currently snapshotted channels.  Owned by the connection's
     * state, freed when it is.  We refresh from it on every paint. */
    const PnMeshState *state;

    gboolean         writing;

    PnMeshChannelsStatusFunc status_cb;
    gpointer                 status_ud;
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

/* Forward decl: refresh after every write, called from both Add and
 * Delete completion callbacks. */
static void rebuild_list (GtkWidget *page);

/* Disable / enable every actionable control on the page.  Used to
 * lock the UI while a write is in flight so a second Add or Delete
 * cannot interleave on the serial port.  Also spins the busy
 * indicator next to the Add button so the user has a visible
 * "the app is working on it" cue -- the verify-cycle handshake
 * takes 3-5 seconds and the status text alone reads as inert. */
static void
set_writing (ChannelsCtx *ctx, gboolean writing)
{
    gboolean enable = !writing && ctx->connection != NULL;
    ctx->writing = writing;
    gtk_widget_set_sensitive (ctx->add_button, enable);
    if (writing)
    {
        gtk_spinner_start (GTK_SPINNER (ctx->spinner));
        gtk_widget_show   (ctx->spinner);
    }
    else
    {
        gtk_spinner_stop (GTK_SPINNER (ctx->spinner));
        gtk_widget_hide  (ctx->spinner);
    }
    /* Per-row Delete buttons are disabled during a write by
     * rebuild_list -- their sensitivity depends on the row's role
     * as well as the writing flag, so they get re-evaluated on
     * every refresh rather than blindly toggled here. */
    {
        GList *rows = gtk_container_get_children (GTK_CONTAINER (ctx->list));
        GList *l;
        for (l = rows; l != NULL; l = l->next)
        {
            GtkWidget *del = g_object_get_data (G_OBJECT (l->data),
                                                "pn-delete-button");
            guint32 role = (guint32) (gintptr) g_object_get_data (
                    G_OBJECT (l->data), "pn-role");
            gboolean row_ok = enable && role == ROLE_SECONDARY;
            if (del != NULL)
                gtk_widget_set_sensitive (del, row_ok);
        }
        g_list_free (rows);
    }
}

/* ------------------------------------------------------------------ */
/*  PSK generation + validation                                         */
/* ------------------------------------------------------------------ */

/* Read 32 bytes (256 bits) from /dev/urandom and return them as a
 * 64-char lowercase hex string.  Caller g_free()s.  Returns NULL on
 * a read failure (extremely unlikely on Linux; the page falls back
 * to GLib's PRNG with a warning if it happens). */
static gchar *
generate_psk_hex (void)
{
    guint8         buf[32];
    GFile         *urandom;
    GFileInputStream *in;
    gssize         got;
    GString       *hex;
    gsize          i;

    urandom = g_file_new_for_path ("/dev/urandom");
    in = g_file_read (urandom, NULL, NULL);
    g_object_unref (urandom);
    if (in == NULL)
    {
        g_warning ("pn-mesh-page-channels: /dev/urandom unavailable, "
                   "falling back to GLib PRNG (NOT cryptographically "
                   "strong); regenerate the PSK on a real Linux host.");
        for (i = 0; i < sizeof buf; i++)
            buf[i] = (guint8) g_random_int_range (0, 256);
    }
    else
    {
        got = g_input_stream_read (G_INPUT_STREAM (in),
                                   buf, sizeof buf, NULL, NULL);
        g_object_unref (in);
        if (got != (gssize) sizeof buf)
            return NULL;
    }

    hex = g_string_sized_new (64);
    for (i = 0; i < sizeof buf; i++)
        g_string_append_printf (hex, "%02x", buf[i]);
    return g_string_free (hex, FALSE);
}

/* Validate that @s is 32 or 64 hex characters (AES-128 or AES-256
 * PSK), and decode into a freshly malloc'd byte buffer.  Returns
 * FALSE with an explanatory message in @err on mismatch. */
static gboolean
decode_psk_hex (const gchar *s, guint8 **out, gsize *out_size, gchar **err)
{
    gsize len;
    gsize i;
    guint8 *buf;

    *out = NULL;
    *out_size = 0;
    *err = NULL;

    len = strlen (s);
    if (len != 32 && len != 64)
    {
        *err = g_strdup_printf (
                "PSK must be 32 hex characters (AES-128) or 64 hex "
                "characters (AES-256); got %zu.", len);
        return FALSE;
    }
    for (i = 0; i < len; i++)
        if (!g_ascii_isxdigit (s[i]))
        {
            *err = g_strdup_printf (
                    "PSK contains a non-hex character at position %zu.",
                    i + 1);
            return FALSE;
        }

    buf = g_malloc (len / 2);
    for (i = 0; i < len / 2; i++)
    {
        gchar pair[3] = { s[i * 2], s[i * 2 + 1], '\0' };
        buf[i] = (guint8) g_ascii_strtoull (pair, NULL, 16);
    }
    *out = buf;
    *out_size = len / 2;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Add-channel dialog                                                  */
/* ------------------------------------------------------------------ */

/* Find the first DISABLED slot with index > 0.  Returns -1 if every
 * slot is in use.  pip-mesh's create_channel does the same scan. */
static gint
find_free_slot (const PnMeshState *state)
{
    guint i;
    if (state == NULL || state->channels == NULL)
        return -1;
    for (i = 0; i < state->channels->len; i++)
    {
        const PnMeshChannel *ch = g_ptr_array_index (state->channels, i);
        if (ch->index > 0 && ch->role == ROLE_DISABLED)
            return (gint) ch->index;
    }
    return -1;
}

typedef struct
{
    GtkWidget *dialog;
    GtkEntry  *name_entry;
    GtkEntry  *psk_entry;
} AddDialogCtx;

static void
on_generate_psk_clicked (GtkButton *button, gpointer user_data)
{
    AddDialogCtx *adlg = user_data;
    gchar *hex;

    (void) button;
    hex = generate_psk_hex ();
    if (hex == NULL)
        return;
    gtk_entry_set_text (adlg->psk_entry, hex);
    g_free (hex);
}

/* The set_channel reply for the Add. */
static void
on_add_channel_done (GObject *source, GAsyncResult *res, gpointer user_data)
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
                "Could not add channel: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    ctx->state = pn_mesh_connection_get_state (ctx->connection);
    rebuild_list (page);
    emit_status (ctx, "Channel added.");
    set_writing (ctx, FALSE);
}

static void
on_add_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget    *page = user_data;
    ChannelsCtx  *ctx  = g_object_get_data (G_OBJECT (page),
                                            PN_MESH_CHANNELS_CTX_QDATA);
    GtkWidget    *dlg;
    GtkWidget    *content;
    GtkWidget    *grid;
    GtkWidget    *name_entry;
    GtkWidget    *psk_entry;
    GtkWidget    *psk_box;
    GtkWidget    *generate;
    AddDialogCtx  adlg;
    gint          slot;
    gint          response;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    slot = find_free_slot (ctx->state);
    if (slot < 0)
    {
        emit_status (ctx,
                     "No free channel slots on the device "
                     "(delete a secondary channel first).");
        return;
    }

    dlg = gtk_dialog_new_with_buttons (
            "Add Channel",
            GTK_WINDOW (gtk_widget_get_toplevel (page)),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Add",    GTK_RESPONSE_ACCEPT,
            NULL);
    gtk_window_set_default_size (GTK_WINDOW (dlg), 480, -1);

    content = gtk_dialog_get_content_area (GTK_DIALOG (dlg));
    gtk_container_set_border_width (GTK_CONTAINER (content), 12);

    {
        gchar *blurb_text = g_strdup_printf (
                "New channel will be written to slot %d as SECONDARY.",
                slot);
        GtkWidget *blurb = gtk_label_new (blurb_text);
        GtkStyleContext *sc = gtk_widget_get_style_context (blurb);
        gtk_label_set_xalign (GTK_LABEL (blurb), 0.0);
        gtk_style_context_add_class (sc, "dim-label");
        gtk_widget_set_margin_bottom (blurb, 6);
        gtk_box_pack_start (GTK_BOX (content), blurb, FALSE, FALSE, 0);
        g_free (blurb_text);
    }

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_box_pack_start (GTK_BOX (content), grid, TRUE, TRUE, 0);

    /* Name */
    {
        GtkWidget *key = gtk_label_new ("Name");
        gtk_label_set_xalign (GTK_LABEL (key), 0.0);
        gtk_grid_attach (GTK_GRID (grid), key, 0, 0, 1, 1);
    }
    name_entry = gtk_entry_new ();
    /* No client-side length cap: the device truncates if too long
     * and the verify-cycle reflects what it kept. */
    gtk_entry_set_placeholder_text (GTK_ENTRY (name_entry),
                                    "e.g. MyMesh");
    gtk_widget_set_hexpand (name_entry, TRUE);
    gtk_grid_attach (GTK_GRID (grid), name_entry, 1, 0, 2, 1);

    /* PSK + Generate */
    {
        GtkWidget *key = gtk_label_new ("PSK (hex)");
        gtk_label_set_xalign (GTK_LABEL (key), 0.0);
        gtk_grid_attach (GTK_GRID (grid), key, 0, 1, 1, 1);
    }
    psk_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    psk_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (psk_entry),
                                    "32 or 64 hex chars");
    gtk_entry_set_max_length (GTK_ENTRY (psk_entry), 64);
    gtk_widget_set_hexpand   (psk_entry, TRUE);
    /* Pre-fill with a fresh random key so the common case (accept
     * defaults, type a name, click Add) is one click and no thinking. */
    {
        gchar *initial = generate_psk_hex ();
        if (initial != NULL)
        {
            gtk_entry_set_text (GTK_ENTRY (psk_entry), initial);
            g_free (initial);
        }
    }
    gtk_box_pack_start (GTK_BOX (psk_box), psk_entry, TRUE, TRUE, 0);

    generate = gtk_button_new_with_mnemonic ("_Generate");
    gtk_widget_set_tooltip_text (generate,
            "Read 32 bytes from /dev/urandom for a fresh AES-256 key.");
    gtk_box_pack_start (GTK_BOX (psk_box), generate, FALSE, FALSE, 0);
    gtk_grid_attach (GTK_GRID (grid), psk_box, 1, 1, 2, 1);

    adlg.dialog     = dlg;
    adlg.name_entry = GTK_ENTRY (name_entry);
    adlg.psk_entry  = GTK_ENTRY (psk_entry);
    g_signal_connect (generate, "clicked",
                      G_CALLBACK (on_generate_psk_clicked), &adlg);

    gtk_widget_show_all (dlg);

    for (;;)
    {
        const gchar *name;
        const gchar *psk_hex;
        guint8      *psk_bytes = NULL;
        gsize        psk_size  = 0;
        gchar       *err       = NULL;

        response = gtk_dialog_run (GTK_DIALOG (dlg));
        if (response != GTK_RESPONSE_ACCEPT)
            break;

        name    = gtk_entry_get_text (GTK_ENTRY (name_entry));
        psk_hex = gtk_entry_get_text (GTK_ENTRY (psk_entry));

        if (*name == '\0')
        {
            /* No status-bar message here -- the user is staring at
             * the dialog, so an in-dialog beep is enough.  We could
             * surface an inline label later if it becomes confusing. */
            gtk_widget_error_bell (name_entry);
            gtk_widget_grab_focus (name_entry);
            continue;
        }

        if (!decode_psk_hex (psk_hex, &psk_bytes, &psk_size, &err))
        {
            emit_status (ctx, err);
            g_free (err);
            gtk_widget_error_bell (psk_entry);
            gtk_widget_grab_focus (psk_entry);
            continue;
        }

        /* Copy @name BEFORE destroying the dialog: gtk_entry_get_text
         * returned a borrowed pointer into the entry's buffer, and
         * the entry dies with the dialog.  set_channel_async would
         * g_strdup() it internally, but that runs AFTER the destroy
         * here -- by then the source bytes are freed and the strdup
         * picks up whatever the allocator left behind (looks like
         * garbage when displayed as the channel name on the next
         * read-back). */
        {
            gchar *name_owned = g_strdup (name);
            gtk_widget_destroy (dlg);

            emit_status (ctx, "Adding channel…");
            set_writing (ctx, TRUE);
            pn_mesh_connection_set_channel_async (
                    ctx->connection,
                    (guint32) slot, name_owned,
                    psk_bytes, psk_size,
                    ROLE_SECONDARY,
                    NULL, on_add_channel_done, page);
            g_free (name_owned);
            g_free (psk_bytes);
        }
        return;
    }

    gtk_widget_destroy (dlg);
}

/* ------------------------------------------------------------------ */
/*  Delete                                                              */
/* ------------------------------------------------------------------ */

static void
on_delete_channel_done (GObject *source, GAsyncResult *res, gpointer user_data)
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
                "Could not delete channel: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    ctx->state = pn_mesh_connection_get_state (ctx->connection);
    rebuild_list (page);
    emit_status (ctx, "Channel deleted.");
    set_writing (ctx, FALSE);
}

/* Per-row Delete handler.  The slot index is stashed on the button
 * as qdata; the row's role is known to be SECONDARY because Delete
 * is sensitive only for those rows (gated in rebuild_list). */
static void
on_delete_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget   *page  = user_data;
    ChannelsCtx *ctx   = g_object_get_data (G_OBJECT (page),
                                            PN_MESH_CHANNELS_CTX_QDATA);
    guint32      index;

    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;
    index = (guint32) (gintptr) g_object_get_data (G_OBJECT (button),
                                                   "pn-index");

    emit_status (ctx, "Deleting channel…");
    set_writing (ctx, TRUE);
    /* DISABLED write with empty name + no PSK -- pip-mesh's
     * delete_channel does the same. */
    pn_mesh_connection_set_channel_async (
            ctx->connection,
            index, "", NULL, 0, ROLE_DISABLED,
            NULL, on_delete_channel_done, page);
}

/* ------------------------------------------------------------------ */
/*  Row construction                                                    */
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

/* Build one row.  Returns the GtkListBoxRow; the Delete button is
 * stashed as qdata "pn-delete-button" and the row's role as
 * "pn-role" so set_writing can re-evaluate sensitivity from outside. */
static GtkWidget *
build_channel_row (GtkWidget           *page,
                   const PnMeshChannel *ch)
{
    GtkWidget *row;
    GtkWidget *grid;
    GtkWidget *idx_label;
    GtkWidget *name_label;
    GtkWidget *badge;
    GtkWidget *psk_icon;
    GtkWidget *del;
    gchar     *idx_text;
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

    /* Column 2: PSK indicator -- only on non-disabled rows.  The
     * icon reads as "encrypted with a key", not as a button. */
    if (ch->role != ROLE_DISABLED && ch->psk != NULL && ch->psk_size > 0)
    {
        psk_icon = gtk_image_new_from_icon_name ("dialog-password",
                                                 GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text (
                psk_icon,
                ch->psk_size == 16 ? "PSK set (AES-128)"
                                   : "PSK set (AES-256)");
    }
    else
    {
        /* Reserve the column so role badges line up across rows. */
        psk_icon = gtk_label_new ("");
    }
    gtk_grid_attach (GTK_GRID (grid), psk_icon, 2, 0, 1, 1);

    /* Column 3: role badge. */
    badge = make_role_badge (ch->role);
    gtk_widget_set_size_request (badge, 90, -1);
    gtk_grid_attach (GTK_GRID (grid), badge, 3, 0, 1, 1);

    /* Column 4: Delete button.  Sensitive only for SECONDARY rows.
     * Trash-can glyph reads as "discard" more clearly than the
     * generic edit-delete eraser does. */
    del = gtk_button_new_from_icon_name ("user-trash-symbolic",
                                         GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (
            del,
            ch->role == ROLE_PRIMARY
                ? "The primary channel cannot be deleted."
                : (ch->role == ROLE_DISABLED
                    ? "Slot is already empty."
                    : "Disable this channel slot."));
    gtk_widget_set_sensitive (del, ch->role == ROLE_SECONDARY);
    g_object_set_data (G_OBJECT (del), "pn-index",
                       (gpointer) (gintptr) ch->index);
    g_signal_connect (del, "clicked",
                      G_CALLBACK (on_delete_clicked), page);
    gtk_grid_attach (GTK_GRID (grid), del, 4, 0, 1, 1);

    gtk_container_add (GTK_CONTAINER (row), grid);

    g_object_set_data (G_OBJECT (row), "pn-delete-button", del);
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

static void
rebuild_list (GtkWidget *page)
{
    ChannelsCtx *ctx = g_object_get_data (G_OBJECT (page),
                                          PN_MESH_CHANNELS_CTX_QDATA);
    guint        i;
    guint        free_slots;

    g_return_if_fail (ctx != NULL);

    clear_list (ctx->list);

    if (ctx->state == NULL || ctx->state->channels == NULL)
    {
        gtk_widget_set_sensitive (ctx->add_button, FALSE);
        gtk_widget_show_all (GTK_WIDGET (ctx->list));
        return;
    }

    free_slots = 0;
    for (i = 0; i < ctx->state->channels->len; i++)
    {
        const PnMeshChannel *ch = g_ptr_array_index (ctx->state->channels, i);
        gtk_container_add (GTK_CONTAINER (ctx->list),
                           build_channel_row (page, ch));
        if (ch->index > 0 && ch->role == ROLE_DISABLED)
            free_slots++;
    }

    /* Add stays enabled until every secondary slot is in use; the
     * "no free slots" path emits a status message rather than
     * leaving the user staring at a greyed button with no
     * explanation. */
    gtk_widget_set_sensitive (
            ctx->add_button,
            ctx->connection != NULL && !ctx->writing && free_slots > 0);

    gtk_widget_show_all (GTK_WIDGET (ctx->list));
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_mesh_page_channels_new (void)
{
    ChannelsCtx *ctx;
    GtkWidget   *page;
    GtkWidget   *subtitle;
    GtkWidget   *top_box;
    GtkWidget   *add;
    GtkWidget   *scrolled;
    GtkWidget   *list;

    /* Hosted inside a GtkExpander; small margins, no in-page title
     * (the expander header carries the section name). */
    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "Every channel slot the device exposes.  The primary "
            "channel is what nodes talk on by default; secondary "
            "channels are private groups your peers also have to "
            "configure.  Add or delete affects the device live -- "
            "no reboot.");
    gtk_label_set_xalign      (GTK_LABEL (subtitle), 0.0);
    gtk_label_set_line_wrap   (GTK_LABEL (subtitle), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (subtitle), 72);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    top_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top (top_box, 6);
    add = gtk_button_new_from_icon_name ("list-add", GTK_ICON_SIZE_BUTTON);
    gtk_button_set_label (GTK_BUTTON (add), "Add channel");
    gtk_button_set_always_show_image (GTK_BUTTON (add), TRUE);
    gtk_widget_set_tooltip_text (add,
            "Create a new SECONDARY channel in the first free slot.");
    gtk_widget_set_sensitive (add, FALSE);
    gtk_box_pack_start (GTK_BOX (top_box), add, FALSE, FALSE, 0);

    /* Busy spinner sits to the right of the Add button.  Hidden by
     * default; set_writing() spins+shows it while a write is in
     * flight and stops+hides it on completion.  Without it the 3-5s
     * verify-cycle pause reads as the app having frozen.  Stored in
     * the ctx alongside the Add button so set_writing reaches it. */
    {
        GtkWidget *spin = gtk_spinner_new ();
        gtk_widget_set_no_show_all (spin, TRUE);
        gtk_box_pack_start (GTK_BOX (top_box), spin, FALSE, FALSE, 0);
        /* Defer setting ctx->spinner until ctx is allocated below. */
        g_object_set_data (G_OBJECT (top_box), "pn-spinner", spin);
    }

    gtk_box_pack_start (GTK_BOX (page),    top_box, FALSE, FALSE, 0);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    /* Floor at ~6 rows and propagate natural height so the page is
     * tall enough to actually display the device's eight slots before
     * scrolling.  Without the min the scrolled window's natural is 0
     * and the FALSE-FALSE-packed Channels expander gets allocated
     * only its non-list children's height — pinning the list at a
     * single row. */
    gtk_scrolled_window_set_min_content_height (
            GTK_SCROLLED_WINDOW (scrolled), 220);
    gtk_scrolled_window_set_propagate_natural_height (
            GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);

    list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                     GTK_SELECTION_NONE);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (list);
        gtk_style_context_add_class (sc, "frame");
    }
    /* Belt-and-braces: an explicit min size on the listbox itself in
     * case any parent in the expander/notebook nesting negotiates the
     * scrolled window's hints down. */
    gtk_widget_set_size_request (list, -1, 200);
    gtk_widget_set_vexpand (list, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), list);
    gtk_box_pack_start (GTK_BOX (page), scrolled, TRUE, TRUE, 0);

    ctx = g_slice_new0 (ChannelsCtx);
    ctx->page       = page;
    ctx->add_button = add;
    ctx->spinner    = g_object_get_data (G_OBJECT (top_box), "pn-spinner");
    ctx->list       = GTK_LIST_BOX (list);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_CHANNELS_CTX_QDATA,
                            ctx, channels_ctx_free);
    g_signal_connect (add, "clicked",
                      G_CALLBACK (on_add_clicked), page);

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
