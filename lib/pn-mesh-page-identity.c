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
/*  Two columns: a bold key label on the left, a read-only value      */
/*  label (or editable entry + Apply button row) on the right.  The   */
/*  two writable rows -- long_name / short_name -- live in their own  */
/*  little subgrid each so the Apply button sits flush at the row's   */
/*  right edge.                                                       */
/*                                                                     */
/*  Per-page state struct holds widget pointers, the active           */
/*  PnMeshConnection (borrowed) and a write-in-flight flag so the     */
/*  two Apply buttons can disable themselves while either write is    */
/*  running and re-enable on completion.                              */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-identity.h"

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
    GtkEntry *long_name_entry;
    GtkButton *long_name_apply;
    GtkEntry *short_name_entry;
    GtkButton *short_name_apply;
    GtkLabel *hw_model_label;
    GtkLabel *role_label;
    GtkLabel *channels_label;
    GtkLabel *caps_label;

    /* Borrowed; lifetime is the dialog's, not the page's. */
    PnMeshConnection *connection;

    /* While a write is in flight, both Apply buttons stay disabled
     * even though only one of them was clicked -- two simultaneous
     * writes on the same serial port would interleave frames.  The
     * flag is reset in the finish callback. */
    gboolean          writing;

    /* Which field the in-flight Apply is mutating.  Determines which
     * entry on_set_owner_done is allowed to repaint -- repainting
     * the *other* entry from the device state would clobber a
     * mid-edit value the user has typed but not yet applied. */
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
} IdentityCtx;

static void
identity_ctx_free (gpointer data)
{
    g_slice_free (IdentityCtx, data);
}

/* ------------------------------------------------------------------ */
/*  HW model name table                                                 */
/* ------------------------------------------------------------------ */

/* Port of /usr/bin/pip-mesh's format_hw_model.  The Meshtastic
 * HardwareModel enum is sparse, so we use a small lookup table
 * rather than an array indexed by value.  Unknown ids fall through
 * to a "model #N" string so the user is not confronted with a bare
 * "—" when their device is newer than this table. */
typedef struct { guint32 id; const char *name; } HwEntry;

static const HwEntry HW_MODELS[] = {
    {  1, "TLORA_V2" },
    {  2, "TLORA_V1" },
    {  3, "TLORA_V2_1_1P6" },
    {  4, "TBEAM" },
    {  5, "HELTEC_V2_0" },
    {  6, "TBEAM_V0P7" },
    {  7, "T_ECHO" },
    {  8, "TLORA_V1_1P3" },
    {  9, "RAK4631" },
    { 10, "HELTEC_V2_1" },
    { 11, "HELTEC_V1" },
    { 12, "TBEAM_S3_CORE" },
    { 13, "RAK11200" },
    { 14, "NANO_G1" },
    { 15, "TLORA_V2_1_1P8" },
    { 16, "TLORA_T3_S3" },
    { 17, "NANO_G1_EXPLORER" },
    { 18, "NANO_G2_ULTRA" },
    { 25, "STATION_G1" },
    { 26, "RAK11310" },
    { 33, "T_ECHO_PLUS" },
    { 37, "PORTDUINO" },
    { 43, "HELTEC_V3" },
    { 44, "HELTEC_WSL_V3" },
    { 47, "RPI_PICO" },
    { 48, "HELTEC_WIRELESS_TRACKER" },
    { 49, "HELTEC_WIRELESS_PAPER" },
    { 50, "T_DECK" },
    { 51, "T_WATCH_S3" },
    { 53, "HELTEC_HT62" },
    { 65, "HELTEC_CAPSULE_SENSOR_V3" },
    { 69, "HELTEC_MESH_NODE_T114" },
    { 70, "SENSECAP_INDICATOR" },
    { 71, "TRACKER_T1000_E" },
    { 79, "RPI_PICO2" },
    { 84, "WISMESH_TAP" },
    { 89, "THINKNODE_M1" },
    { 91, "T_ETH_ELITE" },
    { 94, "HELTEC_MESH_POCKET" },
    { 95, "SEEED_SOLAR_NODE" },
    {102, "T_DECK_PRO" },
    {103, "T_LORA_PAGER" },
    {108, "HELTEC_MESH_SOLAR" },
    {109, "T_ECHO_LITE" },
    {110, "HELTEC_V4" },
    {113, "HELTEC_WIRELESS_TRACKER_V2" },
    {114, "T_WATCH_ULTRA" },
    {255, "PRIVATE_HW" },
};

static gchar *
format_hw_model (guint32 id)
{
    gsize i;
    for (i = 0; i < G_N_ELEMENTS (HW_MODELS); i++)
        if (HW_MODELS[i].id == id)
            return g_strdup_printf ("%s (#%u)", HW_MODELS[i].name, id);
    return g_strdup_printf ("model #%u", id);
}

/* Meshtastic DeviceRole enum -- the values pip-mesh prints with
 * format_device_role.  Small enough to inline. */
static const char *
format_role (guint32 r)
{
    switch (r)
    {
    case 0:  return "CLIENT";
    case 1:  return "CLIENT_MUTE";
    case 2:  return "ROUTER";
    case 3:  return "ROUTER_CLIENT";   /* deprecated upstream */
    case 4:  return "REPEATER";
    case 5:  return "TRACKER";
    case 6:  return "SENSOR";
    case 7:  return "TAK";
    case 8:  return "CLIENT_HIDDEN";
    case 9:  return "LOST_AND_FOUND";
    case 10: return "TAK_TRACKER";
    case 11: return "ROUTER_LATE";
    default: return NULL;
    }
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

/* Row with an entry on the left and an Apply button on the right.
 * Stores the entry into @out_entry and the button into @out_button. */
static void
attach_entry_row (GtkGrid *grid, gint row, const gchar *key_text,
                  gint max_length,
                  GtkEntry **out_entry, GtkButton **out_button)
{
    GtkWidget *holder;
    GtkWidget *entry;
    GtkWidget *apply;

    gtk_grid_attach (grid, make_key (key_text), 0, row, 1, 1);

    holder = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_hexpand (holder, TRUE);

    entry = gtk_entry_new ();
    gtk_entry_set_max_length (GTK_ENTRY (entry), max_length);
    gtk_widget_set_hexpand   (entry, TRUE);
    /* Per-character counter on the right helps the user feel the
     * 4-character short-name limit before Apply rejects it. */
    gtk_entry_set_input_purpose (GTK_ENTRY (entry), GTK_INPUT_PURPOSE_FREE_FORM);
    gtk_box_pack_start (GTK_BOX (holder), entry, TRUE, TRUE, 0);

    apply = gtk_button_new_with_mnemonic ("_Apply");
    gtk_widget_set_tooltip_text (
            apply,
            "Write the value to the device and verify it took effect.");
    gtk_widget_set_sensitive (apply, FALSE);   /* disabled until connected */
    gtk_box_pack_start (GTK_BOX (holder), apply, FALSE, FALSE, 0);

    gtk_grid_attach (grid, holder, 1, row, 1, 1);

    *out_entry  = GTK_ENTRY (entry);
    *out_button = GTK_BUTTON (apply);
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
    gboolean enable = !writing && ctx->connection != NULL;
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->long_name_apply),  enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->short_name_apply), enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->long_name_entry),  enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->short_name_entry), enable);
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

static void
apply_long_name (GtkButton *button, gpointer user_data)
{
    GtkWidget   *page = user_data;
    IdentityCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_PAGE_CTX_QDATA);
    const gchar *text;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    text = gtk_entry_get_text (ctx->long_name_entry);

    emit_status (ctx, "Applying long name…");
    ctx->applying = APPLY_LONG;
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_owner_async (
            ctx->connection, text, NULL, NULL,
            on_set_owner_done, page);
}

static void
apply_short_name (GtkButton *button, gpointer user_data)
{
    GtkWidget   *page = user_data;
    IdentityCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_PAGE_CTX_QDATA);
    const gchar *text;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    text = gtk_entry_get_text (ctx->short_name_entry);

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
    GtkWidget   *title;
    GtkWidget   *subtitle;
    GtkWidget   *grid;
    IdentityCtx *ctx;
    gint         row = 0;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start  (page, 24);
    gtk_widget_set_margin_end    (page, 24);
    gtk_widget_set_margin_top    (page, 18);
    gtk_widget_set_margin_bottom (page, 18);

    title = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (title),
            "<span size='large' weight='bold'>Identity</span>");
    gtk_label_set_xalign (GTK_LABEL (title), 0.0);
    gtk_box_pack_start (GTK_BOX (page), title, FALSE, FALSE, 0);

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

    attach_entry_row (GTK_GRID (grid), row++, "Long name",
                      PN_MESH_LONG_NAME_MAX,
                      &ctx->long_name_entry, &ctx->long_name_apply);
    attach_entry_row (GTK_GRID (grid), row++, "Short name",
                      PN_MESH_SHORT_NAME_MAX,
                      &ctx->short_name_entry, &ctx->short_name_apply);

    ctx->hw_model_label = attach_label_row (GTK_GRID (grid), row++, "Hardware model");
    ctx->role_label     = attach_label_row (GTK_GRID (grid), row++, "Role");
    ctx->caps_label     = attach_label_row (GTK_GRID (grid), row++, "Capabilities");
    ctx->channels_label = attach_label_row (GTK_GRID (grid), row++, "Channels");

    g_object_set_data_full (G_OBJECT (page), PN_MESH_PAGE_CTX_QDATA,
                            ctx, identity_ctx_free);

    g_signal_connect (ctx->long_name_apply,  "clicked",
                      G_CALLBACK (apply_long_name),  page);
    g_signal_connect (ctx->short_name_apply, "clicked",
                      G_CALLBACK (apply_short_name), page);
    /* Activate = Enter in the entry => same as clicking Apply. */
    g_signal_connect_swapped (ctx->long_name_entry,  "activate",
                              G_CALLBACK (apply_long_name),  page);
    g_signal_connect_swapped (ctx->short_name_entry, "activate",
                              G_CALLBACK (apply_short_name), page);

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

    /* Replace the entry contents without firing the activate
     * handler.  GtkEntry's set_text already suppresses ::activate;
     * just go ahead.  But only repaint the entry whose write we
     * are currently finishing -- on a fresh device pick (applying
     * == APPLY_NONE) we paint both; after an Apply we paint only
     * that one, so the other entry's in-progress edit survives. */
    if (ctx->applying == APPLY_NONE || ctx->applying == APPLY_LONG)
        gtk_entry_set_text (ctx->long_name_entry,
                            state->owner_long_name != NULL
                                ? state->owner_long_name : "");
    if (ctx->applying == APPLY_NONE || ctx->applying == APPLY_SHORT)
        gtk_entry_set_text (ctx->short_name_entry,
                            state->owner_short_name != NULL
                                ? state->owner_short_name : "");

    {
        /* Prefer the DeviceMetadata.hw_model when present; fall back
         * to the User.hw_model from the handshake.  They are usually
         * the same enum value. */
        guint32 hw = state->have_metadata && state->hw_model != 0
                ? state->hw_model : state->owner_hw_model;
        gchar *txt = hw != 0 ? format_hw_model (hw) : g_strdup ("—");
        set_label (ctx->hw_model_label, txt);
        g_free (txt);
    }

    {
        const char *role = state->have_metadata ? format_role (state->role) : NULL;
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
        gtk_entry_set_text (ctx->long_name_entry, "");
        gtk_entry_set_text (ctx->short_name_entry, "");
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
