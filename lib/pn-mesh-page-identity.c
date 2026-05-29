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
/*  Identity page — Phase 3.                                           */
/*                                                                     */
/*  Two columns: a bold key label on the left, a value on the right.  */
/*  The two writable rows -- long_name / short_name -- use a          */
/*  PnInlineEditLabel: it looks like a plain value with a small        */
/*  pencil button, and clicking the pencil swaps in an entry that      */
/*  commits on Enter (or focus-out) and cancels on Escape.  No Apply   */
/*  button -- the commit itself triggers the device write.            */
/*                                                                     */
/*  Per-page state struct holds widget pointers, the active           */
/*  PnMeshConnection (borrowed) and a write-in-flight flag so both     */
/*  editable rows disable themselves while either write is running    */
/*  and re-enable on completion.                                      */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-identity.h"
#include "pn-mesh-formats.h"
#include "pn-inline-edit-label.h"

#define PN_MESH_PAGE_CTX_QDATA "pn-mesh-page-identity-ctx"

/* Meshtastic owner-name length limits (device-enforced; we mirror
 * them client-side so the Apply button doesn't ship a doomed write). */
#define PN_MESH_LONG_NAME_MAX  39
#define PN_MESH_SHORT_NAME_MAX  4

typedef struct
{
    /* Widget tree handles. */
    GtkLabel *kind_label;
    GtkLabel *tty_label;
    GtkLabel *firmware_label;
    GtkLabel *node_num_label;
    PnInlineEditLabel *long_name_edit;
    PnInlineEditLabel *short_name_edit;
    GtkLabel *hw_model_label;
    GtkLabel *role_label;
    GtkLabel *channels_label;
    GtkLabel *caps_label;

    /* Borrowed; lifetime is the dialog's, not the page's. */
    PnMeshConnection *connection;

    /* While a write is in flight, both editable rows stay disabled
     * even though only one of them committed -- two simultaneous
     * writes on the same serial port would interleave frames.  The
     * flag is reset in the finish callback. */
    gboolean          writing;

    /* Which field the in-flight write is mutating.  Determines which
     * row on_set_owner_done is allowed to repaint -- repainting the
     * *other* row from the device state would clobber a value the
     * user committed but whose write has not finished. */
    enum
    {
        APPLY_NONE = 0,
        APPLY_LONG,
        APPLY_SHORT,
    } applying;

    /* Dialog-side status sink; the page calls this with one-line
     * progress strings ("Applying owner long name…", "Long name
     * applied.") so the dialog's status bar reflects what the page
     * is doing. */
    PnMeshIdentityStatusFunc status_cb;
    gpointer                 status_ud;

    /* Dialog-side busy sink; fired on every TRUE/FALSE *transition* of
     * the per-Apply writing flag so the dialog can show its big
     * full-pane spinner for the duration of any device round-trip. */
    PnMeshPageBusyFunc       busy_cb;
    gpointer                 busy_ud;
} IdentityCtx;

static void
identity_ctx_free (gpointer data)
{
    g_slice_free (IdentityCtx, data);
}

/* ------------------------------------------------------------------ */
/*  Field row helpers                                                   */
/* ------------------------------------------------------------------ */

static GtkWidget *
make_key (const gchar *text)
{
    GtkWidget *key = gtk_label_new (text);
    PangoAttrList *attrs = pango_attr_list_new ();
    gtk_label_set_xalign (GTK_LABEL (key), 0.0);
    pango_attr_list_insert (attrs,
                            pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (key), attrs);
    pango_attr_list_unref (attrs);
    gtk_widget_set_margin_end (key, 16);
    return key;
}

static GtkLabel *
attach_label_row (GtkGrid *grid, gint row, const gchar *key_text)
{
    GtkWidget *val = gtk_label_new ("—");

    gtk_grid_attach (grid, make_key (key_text), 0, row, 1, 1);

    gtk_label_set_xalign     (GTK_LABEL (val), 0.0);
    gtk_label_set_selectable (GTK_LABEL (val), TRUE);
    gtk_label_set_ellipsize  (GTK_LABEL (val), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand   (val, TRUE);
    gtk_grid_attach (grid, val, 1, row, 1, 1);
    return GTK_LABEL (val);
}

/* Row with a key label and an inline click-to-edit value on the right.
 * The value commits on Enter (or focus-out) and cancels on Escape via
 * the PnInlineEditLabel widget, so there is no separate Apply button --
 * confirming the edit is what triggers the device write. */
static PnInlineEditLabel *
attach_inline_edit_row (GtkGrid *grid, gint row, const gchar *key_text,
                        gint max_length)
{
    GtkWidget *edit = pn_inline_edit_label_new ();

    gtk_grid_attach (grid, make_key (key_text), 0, row, 1, 1);

    /* Mirror the device-enforced name limit client-side -- mostly so
     * the 4-character short name cannot grow past what the firmware
     * will keep. */
    pn_inline_edit_label_set_max_length (PN_INLINE_EDIT_LABEL (edit),
                                         max_length);
    gtk_widget_set_hexpand (edit, TRUE);
    gtk_widget_set_sensitive (edit, FALSE);   /* disabled until connected */
    gtk_grid_attach (grid, edit, 1, row, 1, 1);

    return PN_INLINE_EDIT_LABEL (edit);
}

/* ------------------------------------------------------------------ */
/*  set_label helpers                                                   */
/* ------------------------------------------------------------------ */

static void
set_label (GtkLabel *label, const gchar *text)
{
    gtk_label_set_text (label,
                        (text != NULL && *text != '\0') ? text : "—");
}

/* Build a "wifi · bluetooth · ethernet" line from the boolean flags,
 * or "—" if none.  Reads nicer than three separate rows. */
static gchar *
format_caps (const PnMeshState *state)
{
    GString *out = g_string_new (NULL);
    const struct { gboolean on; const char *tag; } caps[] = {
        { state->has_wifi,      "Wi-Fi" },
        { state->has_bluetooth, "Bluetooth" },
        { state->has_ethernet,  "Ethernet" },
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS (caps); i++)
    {
        if (!caps[i].on)
            continue;
        if (out->len > 0)
            g_string_append (out, " · ");
        g_string_append (out, caps[i].tag);
    }
    if (out->len == 0)
        g_string_append (out, "—");
    return g_string_free (out, FALSE);
}

static guint
count_active_channels (const PnMeshState *state)
{
    guint i, n = 0;
    if (state == NULL || state->channels == NULL)
        return 0;
    for (i = 0; i < state->channels->len; i++)
    {
        const PnMeshChannel *ch = g_ptr_array_index (state->channels, i);
        if (ch->role != 0) n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/*  Apply handlers                                                      */
/* ------------------------------------------------------------------ */

/* Forward decl: needed by both Apply paths. */
static void refresh_from_state (GtkWidget *page);

static void
emit_status (IdentityCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

static void
set_writing (IdentityCtx *ctx, gboolean writing)
{
    gboolean enable     = !writing && ctx->connection != NULL;
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->long_name_edit),  enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->short_name_edit), enable);
    /* Notify the dialog only on the TRUE↔FALSE edge so the dialog's
     * busy counter stays balanced even when a no-op set_writing call
     * sneaks through. */
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

static void
on_set_owner_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget   *page  = user_data;
    IdentityCtx *ctx   = g_object_get_data (G_OBJECT (page),
                                            PN_MESH_PAGE_CTX_QDATA);
    GError      *error = NULL;
    gboolean     ok;

    (void) source;

    /* The page might have been destroyed under us if the user closed
     * the dialog while the write was in flight.  ctx is NULL in that
     * case -- bail. */
    if (ctx == NULL)
    {
        pn_mesh_connection_set_owner_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_owner_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply owner name: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        ctx->applying = APPLY_NONE;
        set_writing (ctx, FALSE);
        return;
    }

    /* Re-paint from the verified state, which now contains whatever
     * the device replied with -- so even a name truncated by the
     * device shows up here, not what the user typed.  Only the
     * entry we just wrote is refreshed; the other entry may hold a
     * mid-edit value the user has not applied yet, and overwriting
     * that would silently throw their typing away. */
    refresh_from_state (page);
    emit_status (ctx, "Owner name applied.");
    ctx->applying = APPLY_NONE;
    set_writing (ctx, FALSE);
}

/* The current device-side owner name for the field being committed;
 * NULL-safe.  Used to skip a doomed/no-op write when the committed
 * value matches what the device already holds -- ::committed fires on
 * every Enter / focus-out, even when the user changed nothing. */
static gboolean
owner_name_unchanged (IdentityCtx *ctx, gboolean is_long, const gchar *text)
{
    const PnMeshState *state   = pn_mesh_connection_get_state (ctx->connection);
    const gchar       *current = NULL;

    if (state != NULL)
        current = is_long ? state->owner_long_name : state->owner_short_name;

    return g_strcmp0 (text, current != NULL ? current : "") == 0;
}

static void
on_long_name_committed (PnInlineEditLabel *edit, const gchar *text,
                        gpointer user_data)
{
    GtkWidget   *page = user_data;
    IdentityCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_PAGE_CTX_QDATA);

    (void) edit;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;
    if (owner_name_unchanged (ctx, TRUE, text))
        return;

    emit_status (ctx, "Applying long name…");
    ctx->applying = APPLY_LONG;
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_owner_async (
            ctx->connection, text, NULL, NULL,
            on_set_owner_done, page);
}

static void
on_short_name_committed (PnInlineEditLabel *edit, const gchar *text,
                         gpointer user_data)
{
    GtkWidget   *page = user_data;
    IdentityCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_PAGE_CTX_QDATA);

    (void) edit;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;
    if (owner_name_unchanged (ctx, FALSE, text))
        return;

    emit_status (ctx, "Applying short name…");
    ctx->applying = APPLY_SHORT;
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_owner_async (
            ctx->connection, NULL, text, NULL,
            on_set_owner_done, page);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_mesh_page_identity_new (void)
{
    GtkWidget   *page;
    GtkWidget   *subtitle;
    GtkWidget   *grid;
    IdentityCtx *ctx;
    gint         row = 0;

    /* Hosted inside a GtkExpander, so the outer box wears small
     * margins and skips a redundant title row (the expander header
     * already labels the section). */
    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "What the device reported during the configuration "
            "handshake.  Long / short name changes are written "
            "live and verified by a round-trip read.");
    gtk_label_set_xalign (GTK_LABEL (subtitle), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (subtitle), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (subtitle), 72);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 12);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);

    ctx = g_slice_new0 (IdentityCtx);

    ctx->kind_label     = attach_label_row (GTK_GRID (grid), row++, "Device");
    ctx->tty_label      = attach_label_row (GTK_GRID (grid), row++, "Serial port");
    ctx->firmware_label = attach_label_row (GTK_GRID (grid), row++, "Firmware");
    ctx->node_num_label = attach_label_row (GTK_GRID (grid), row++, "Mesh node #");

    ctx->long_name_edit = attach_inline_edit_row (
            GTK_GRID (grid), row++, "Long name", PN_MESH_LONG_NAME_MAX);
    ctx->short_name_edit = attach_inline_edit_row (
            GTK_GRID (grid), row++, "Short name", PN_MESH_SHORT_NAME_MAX);

    ctx->hw_model_label = attach_label_row (GTK_GRID (grid), row++, "Hardware model");
    ctx->role_label     = attach_label_row (GTK_GRID (grid), row++, "Role");
    ctx->caps_label     = attach_label_row (GTK_GRID (grid), row++, "Capabilities");
    ctx->channels_label = attach_label_row (GTK_GRID (grid), row++, "Channels");

    g_object_set_data_full (G_OBJECT (page), PN_MESH_PAGE_CTX_QDATA,
                            ctx, identity_ctx_free);

    /* Confirming the inline edit (Enter or focus-out) commits the
     * value to the device; Escape cancels and emits nothing. */
    g_signal_connect (ctx->long_name_edit,  "committed",
                      G_CALLBACK (on_long_name_committed),  page);
    g_signal_connect (ctx->short_name_edit, "committed",
                      G_CALLBACK (on_short_name_committed), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

/* Repaint the page from the connection's current state.  Used both
 * by set_state() (initial paint) and by on_set_owner_done() (after a
 * successful write). */
static void
refresh_from_state (GtkWidget *page)
{
    IdentityCtx       *ctx;
    const PnMeshState *state;

    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_PAGE_CTX_QDATA);
    if (ctx == NULL || ctx->connection == NULL)
        return;

    state = pn_mesh_connection_get_state (ctx->connection);
    if (state == NULL)
        return;

    set_label (ctx->firmware_label,
               state->have_metadata ? state->firmware_version : NULL);

    {
        gchar *node_text = state->owner_id != NULL && *state->owner_id != '\0'
                ? g_strdup_printf ("%u (%s)",
                                   state->my_node_num, state->owner_id)
                : g_strdup_printf ("%u", state->my_node_num);
        set_label (ctx->node_num_label, node_text);
        g_free (node_text);
    }

    /* Repaint the value labels.  Setting the text only touches the
     * display label, not an in-progress edit, so it is safe even
     * mid-edit.  Only repaint the row whose write we are currently
     * finishing -- on a fresh device pick (applying == APPLY_NONE)
     * we paint both; after a write we paint only that one, so the
     * other row's just-committed value is not clobbered. */
    if (ctx->applying == APPLY_NONE || ctx->applying == APPLY_LONG)
        pn_inline_edit_label_set_text (ctx->long_name_edit,
                            state->owner_long_name != NULL
                                ? state->owner_long_name : "");
    if (ctx->applying == APPLY_NONE || ctx->applying == APPLY_SHORT)
        pn_inline_edit_label_set_text (ctx->short_name_edit,
                            state->owner_short_name != NULL
                                ? state->owner_short_name : "");

    {
        /* Prefer the DeviceMetadata.hw_model when present; fall back
         * to the User.hw_model from the handshake.  They are usually
         * the same enum value. */
        guint32 hw = state->have_metadata && state->hw_model != 0
                ? state->hw_model : state->owner_hw_model;
        gchar *txt = hw != 0 ? pn_mesh_format_hw_model (hw) : g_strdup ("—");
        set_label (ctx->hw_model_label, txt);
        g_free (txt);
    }

    {
        const char *role = state->have_metadata ? pn_mesh_format_role (state->role) : NULL;
        if (role != NULL)
        {
            gchar *txt = g_strdup_printf ("%s (#%u)", role, state->role);
            set_label (ctx->role_label, txt);
            g_free (txt);
        }
        else
        {
            set_label (ctx->role_label, NULL);
        }
    }

    {
        gchar *caps = state->have_metadata ? format_caps (state) : NULL;
        set_label (ctx->caps_label, caps);
        g_free (caps);
    }

    {
        guint active = count_active_channels (state);
        gchar *txt;
        if (state->channels != NULL && state->channels->len > active)
            txt = g_strdup_printf ("%u active (%u total)",
                                   active, state->channels->len);
        else
            txt = g_strdup_printf ("%u", active);
        set_label (ctx->channels_label, txt);
        g_free (txt);
    }
}

void
pn_mesh_page_identity_set_state (GtkWidget         *page,
                                 const gchar       *device_kind,
                                 const gchar       *tty_path,
                                 const PnMeshState *state,
                                 PnMeshConnection  *connection)
{
    IdentityCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_PAGE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    set_label (ctx->kind_label, device_kind);
    set_label (ctx->tty_label,  tty_path);

    if (state == NULL)
    {
        set_label (ctx->firmware_label, NULL);
        set_label (ctx->node_num_label, NULL);
        pn_inline_edit_label_set_text (ctx->long_name_edit, "");
        pn_inline_edit_label_set_text (ctx->short_name_edit, "");
        set_label (ctx->hw_model_label, NULL);
        set_label (ctx->role_label,     NULL);
        set_label (ctx->caps_label,     NULL);
        set_label (ctx->channels_label, NULL);
        set_writing (ctx, FALSE);   /* re-evaluate Apply enable */
        return;
    }

    refresh_from_state (page);
    set_writing (ctx, FALSE);
}

void
pn_mesh_page_identity_set_status_callback (GtkWidget                *page,
                                           PnMeshIdentityStatusFunc  callback,
                                           gpointer                  user_data)
{
    IdentityCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_PAGE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_identity_set_busy_callback (GtkWidget          *page,
                                         PnMeshPageBusyFunc  callback,
                                         gpointer            user_data)
{
    IdentityCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_PAGE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
