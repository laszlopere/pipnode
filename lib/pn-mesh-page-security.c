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
/*  Security page — Phase 14.                                          */
/*                                                                     */
/*  Switches for is_managed, serial-API, debug-log-API and             */
/*  admin-channel-enabled; read-only base64 view of the X25519         */
/*  public/private/admin keys.  An "I understand the risks" checkbox  */
/*  gates the Apply button -- a careless write can lock the user out  */
/*  of admin (is_managed=TRUE without an admin_key, etc.).             */
/*                                                                     */
/*  Key bytes are round-tripped verbatim.  No UI for editing them in   */
/*  this phase; typing 32 random bytes by hand is not useful.          */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-security.h"

#define PN_MESH_SECURITY_CTX_QDATA "pn-mesh-page-security-ctx"

typedef struct
{
    GtkLabel  *public_key_label;
    GtkLabel  *private_key_label;
    GtkLabel  *admin_keys_label;
    GtkSwitch *is_managed_switch;
    GtkSwitch *serial_enabled_switch;
    GtkSwitch *debug_log_api_switch;
    GtkSwitch *admin_channel_switch;
    GtkCheckButton *understand_check;
    GtkButton *apply_button;

    PnMeshConnection *connection;

    /* Round-trip slots for the key bytes. */
    GBytes    *last_public_key;
    GBytes    *last_private_key;
    GPtrArray *last_admin_keys;       /* of GBytes*; never NULL */

    gboolean   writing;

    PnMeshSecurityStatusFunc status_cb;
    gpointer                 status_ud;

    PnMeshPageBusyFunc       busy_cb;
    gpointer                 busy_ud;
} SecurityCtx;

static void
security_ctx_free (gpointer data)
{
    SecurityCtx *ctx = data;
    if (ctx->last_public_key  != NULL) g_bytes_unref (ctx->last_public_key);
    if (ctx->last_private_key != NULL) g_bytes_unref (ctx->last_private_key);
    if (ctx->last_admin_keys  != NULL) g_ptr_array_unref (ctx->last_admin_keys);
    g_slice_free (SecurityCtx, ctx);
}

/* ------------------------------------------------------------------ */
/*  Status + writing                                                    */
/* ------------------------------------------------------------------ */

static void
emit_status (SecurityCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

static gboolean
apply_should_enable (SecurityCtx *ctx)
{
    return !ctx->writing
        && ctx->connection != NULL
        && gtk_toggle_button_get_active (
                GTK_TOGGLE_BUTTON (ctx->understand_check));
}

static void
set_writing (SecurityCtx *ctx, gboolean writing)
{
    gboolean enable     = !writing && ctx->connection != NULL;
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->is_managed_switch),     enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->serial_enabled_switch), enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->debug_log_api_switch),  enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->admin_channel_switch),  enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->understand_check),      enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),
                              apply_should_enable (ctx));
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

static void
on_understand_toggled (GtkToggleButton *btn, gpointer user_data)
{
    GtkWidget   *page = user_data;
    SecurityCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_SECURITY_CTX_QDATA);
    (void) btn;
    if (ctx == NULL)
        return;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),
                              apply_should_enable (ctx));
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_set_security_config_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget   *page = user_data;
    SecurityCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                           PN_MESH_SECURITY_CTX_QDATA);
    GError      *error = NULL;
    gboolean     ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_security_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_security_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply security settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    /* On success, drop the "I understand" check so the next write
     * requires an explicit re-confirmation. */
    gtk_toggle_button_set_active (
            GTK_TOGGLE_BUTTON (ctx->understand_check), FALSE);
    pn_mesh_page_security_set_state (
            page,
            pn_mesh_connection_get_state (ctx->connection),
            ctx->connection);
    emit_status (ctx, "Security settings applied.");
    set_writing (ctx, FALSE);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                  *page = user_data;
    SecurityCtx                *ctx  = g_object_get_data (G_OBJECT (page),
                                                         PN_MESH_SECURITY_CTX_QDATA);
    PnMeshSecurityConfigWrite   cfg;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;
    if (!gtk_toggle_button_get_active (
                GTK_TOGGLE_BUTTON (ctx->understand_check)))
        return;

    cfg.public_key             = ctx->last_public_key;
    cfg.private_key            = ctx->last_private_key;
    cfg.admin_keys             = ctx->last_admin_keys;
    cfg.is_managed             = gtk_switch_get_active (ctx->is_managed_switch);
    cfg.serial_enabled         = gtk_switch_get_active (ctx->serial_enabled_switch);
    cfg.debug_log_api_enabled  = gtk_switch_get_active (ctx->debug_log_api_switch);
    cfg.admin_channel_enabled  = gtk_switch_get_active (ctx->admin_channel_switch);

    emit_status (ctx, "Applying security settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_security_config_async (
            ctx->connection, &cfg, NULL,
            on_set_security_config_done, page);
}

/* ------------------------------------------------------------------ */
/*  Base64 helpers                                                      */
/* ------------------------------------------------------------------ */

static gchar *
gbytes_to_base64_or_dash (GBytes *bytes)
{
    gsize         s;
    const guint8 *p;

    if (bytes == NULL)
        return g_strdup ("—");
    p = g_bytes_get_data (bytes, &s);
    if (s == 0)
        return g_strdup ("—");
    return g_base64_encode (p, s);
}

static gchar *
admin_keys_to_text (GPtrArray *admin_keys)
{
    GString *out;
    guint    i;

    if (admin_keys == NULL || admin_keys->len == 0)
        return g_strdup ("(none)");

    out = g_string_new (NULL);
    for (i = 0; i < admin_keys->len; i++)
    {
        GBytes *b   = g_ptr_array_index (admin_keys, i);
        gchar  *b64 = gbytes_to_base64_or_dash (b);
        if (i > 0)
            g_string_append_c (out, '\n');
        g_string_append (out, b64);
        g_free (b64);
    }
    return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

#define VALUE_MIN_WIDTH 260

static void
on_cell_realize (GtkWidget *cell, gpointer user_data)
{
    GtkSizeGroup *sg = user_data;
    GList        *children = gtk_container_get_children (GTK_CONTAINER (cell));

    if (children != NULL) {
        GtkWidget *first = children->data;
        gtk_widget_set_size_request (first, VALUE_MIN_WIDTH, -1);
        gtk_size_group_add_widget (sg, first);
    }
    g_list_free (children);
    g_signal_handlers_disconnect_by_func (cell, on_cell_realize, sg);
}

static GtkSizeGroup *
get_value_size_group (GtkGrid *grid)
{
    GtkSizeGroup *sg = g_object_get_data (G_OBJECT (grid), "value-sg");
    if (sg == NULL) {
        sg = gtk_size_group_new (GTK_SIZE_GROUP_HORIZONTAL);
        g_object_set_data_full (G_OBJECT (grid), "value-sg",
                                sg, g_object_unref);
    }
    return sg;
}

static GtkWidget *
add_row (GtkGrid *grid, gint row, const gchar *label_text)
{
    GtkWidget *key = gtk_label_new (label_text);
    PangoAttrList *attrs = pango_attr_list_new ();

    pango_attr_list_insert (attrs,
                            pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (key), attrs);
    pango_attr_list_unref (attrs);
    gtk_label_set_xalign (GTK_LABEL (key), 0.0);
    gtk_label_set_yalign (GTK_LABEL (key), 0.0);
    gtk_widget_set_margin_end (key, 16);
    gtk_grid_attach (grid, key, 0, row, 1, 1);

    {
        GtkWidget *cell = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand (cell, TRUE);
        gtk_grid_attach (grid, cell, 1, row, 1, 1);
        g_signal_connect (cell, "realize",
                          G_CALLBACK (on_cell_realize),
                          get_value_size_group (grid));
        return cell;
    }
}

static GtkWidget *
build_key_label (void)
{
    GtkWidget       *label = gtk_label_new ("—");
    GtkStyleContext *sc    = gtk_widget_get_style_context (label);
    PangoAttrList   *attrs = pango_attr_list_new ();

    /* Monospace for base64 readability. */
    pango_attr_list_insert (attrs,
                            pango_attr_family_new ("monospace"));
    gtk_label_set_attributes (GTK_LABEL (label), attrs);
    pango_attr_list_unref (attrs);

    gtk_label_set_selectable (GTK_LABEL (label), TRUE);
    gtk_label_set_xalign     (GTK_LABEL (label), 0.0);
    gtk_label_set_line_wrap  (GTK_LABEL (label), TRUE);
    gtk_label_set_line_wrap_mode (GTK_LABEL (label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 48);
    gtk_style_context_add_class (sc, "dim-label");
    return label;
}

GtkWidget *
pn_mesh_page_security_new (void)
{
    GtkWidget   *page;
    GtkWidget   *subtitle;
    GtkWidget   *grid;
    GtkWidget   *cell;
    GtkWidget   *is_managed;
    GtkWidget   *serial_enabled;
    GtkWidget   *debug_log_api;
    GtkWidget   *admin_channel;
    GtkWidget   *public_key_label;
    GtkWidget   *private_key_label;
    GtkWidget   *admin_keys_label;
    GtkWidget   *understand;
    GtkWidget   *apply_box;
    GtkWidget   *apply;
    SecurityCtx *ctx;
    gint         row = 0;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "Admin and serial-API permissions, plus a read-only view of "
            "the X25519 keypair the device uses for admin authentication. "
            " A mistake here can lock you out: enabling \"Managed\" "
            "without an admin key in the list below disables local "
            "admin until the device is re-flashed.  Apply is disabled "
            "until you tick \"I understand the risks\".");
    gtk_label_set_xalign      (GTK_LABEL (subtitle), 0.0);
    gtk_label_set_line_wrap   (GTK_LABEL (subtitle), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (subtitle), 72);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 12);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);

    ctx = g_slice_new0 (SecurityCtx);
    ctx->last_admin_keys = g_ptr_array_new_with_free_func (
            (GDestroyNotify) g_bytes_unref);

    cell = add_row (GTK_GRID (grid), row++, "Managed");
    is_managed = gtk_switch_new ();
    gtk_widget_set_halign (is_managed, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (is_managed,
            "When on, only signed admin messages from holders of an "
            "admin key can change settings.  Disables the local serial "
            "admin path entirely -- DO NOT enable without an admin key "
            "configured.");
    gtk_box_pack_start (GTK_BOX (cell), is_managed, FALSE, FALSE, 0);
    ctx->is_managed_switch = GTK_SWITCH (is_managed);

    cell = add_row (GTK_GRID (grid), row++, "Serial-API admin");
    serial_enabled = gtk_switch_new ();
    gtk_widget_set_halign (serial_enabled, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (serial_enabled,
            "Allow admin messages over the USB serial connection.  "
            "Turning this off blocks pipnode (and pip-mesh / the CLI) "
            "from making any further changes over the wire.");
    gtk_box_pack_start (GTK_BOX (cell), serial_enabled, FALSE, FALSE, 0);
    ctx->serial_enabled_switch = GTK_SWITCH (serial_enabled);

    cell = add_row (GTK_GRID (grid), row++, "Debug-log API");
    debug_log_api = gtk_switch_new ();
    gtk_widget_set_halign (debug_log_api, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (debug_log_api,
            "Allow the device to stream its serial debug log to "
            "phone-app / pipnode clients (verbose; off by default).");
    gtk_box_pack_start (GTK_BOX (cell), debug_log_api, FALSE, FALSE, 0);
    ctx->debug_log_api_switch = GTK_SWITCH (debug_log_api);

    cell = add_row (GTK_GRID (grid), row++, "Admin channel");
    admin_channel = gtk_switch_new ();
    gtk_widget_set_halign (admin_channel, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (admin_channel,
            "Enable the well-known \"admin\" channel for remote admin "
            "messages over the mesh.  Requires an admin key.");
    gtk_box_pack_start (GTK_BOX (cell), admin_channel, FALSE, FALSE, 0);
    ctx->admin_channel_switch = GTK_SWITCH (admin_channel);

    cell = add_row (GTK_GRID (grid), row++, "Public key");
    public_key_label = build_key_label ();
    gtk_box_pack_start (GTK_BOX (cell), public_key_label, TRUE, TRUE, 0);
    ctx->public_key_label = GTK_LABEL (public_key_label);

    cell = add_row (GTK_GRID (grid), row++, "Private key");
    private_key_label = build_key_label ();
    gtk_widget_set_tooltip_text (private_key_label,
            "Sensitive — copy with care.  Shown for backup / recovery; "
            "do not paste into untrusted places.");
    gtk_box_pack_start (GTK_BOX (cell), private_key_label, TRUE, TRUE, 0);
    ctx->private_key_label = GTK_LABEL (private_key_label);

    cell = add_row (GTK_GRID (grid), row++, "Admin keys");
    admin_keys_label = build_key_label ();
    gtk_box_pack_start (GTK_BOX (cell), admin_keys_label, TRUE, TRUE, 0);
    ctx->admin_keys_label = GTK_LABEL (admin_keys_label);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top (apply_box, 18);

    understand = gtk_check_button_new_with_mnemonic (
            "I _understand the risks");
    gtk_widget_set_tooltip_text (understand,
            "Required before Apply is enabled.  A mistake on this page "
            "can lock you out of admin until the device is re-flashed.");
    gtk_box_pack_start (GTK_BOX (apply_box), understand, FALSE, FALSE, 0);
    ctx->understand_check = GTK_CHECK_BUTTON (understand);

    {
        GtkWidget *spacer = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_hexpand (spacer, TRUE);
        gtk_box_pack_start (GTK_BOX (apply_box), spacer, TRUE, TRUE, 0);
    }

    apply = gtk_button_new_with_mnemonic ("_Apply security settings");
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  Key bytes are "
            "shipped back verbatim — only the switches change.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_end (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_SECURITY_CTX_QDATA,
                            ctx, security_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);
    g_signal_connect (understand, "toggled",
                      G_CALLBACK (on_understand_toggled), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_security_set_state (GtkWidget         *page,
                                 const PnMeshState *state,
                                 PnMeshConnection  *connection)
{
    SecurityCtx *ctx;
    gchar       *pub_b64;
    gchar       *priv_b64;
    gchar       *admin_text;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_SECURITY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    /* Always reset the "I understand" check on any state refresh so
     * post-handshake Apply needs a fresh re-confirmation. */
    gtk_toggle_button_set_active (
            GTK_TOGGLE_BUTTON (ctx->understand_check), FALSE);

    if (state == NULL || !state->have_security)
    {
        gtk_switch_set_active (ctx->is_managed_switch,     FALSE);
        gtk_switch_set_active (ctx->serial_enabled_switch, FALSE);
        gtk_switch_set_active (ctx->debug_log_api_switch,  FALSE);
        gtk_switch_set_active (ctx->admin_channel_switch,  FALSE);
        gtk_label_set_text (ctx->public_key_label,  "—");
        gtk_label_set_text (ctx->private_key_label, "—");
        gtk_label_set_text (ctx->admin_keys_label,  "(none)");
        g_clear_pointer (&ctx->last_public_key,  g_bytes_unref);
        g_clear_pointer (&ctx->last_private_key, g_bytes_unref);
        g_ptr_array_set_size (ctx->last_admin_keys, 0);
        set_writing (ctx, FALSE);
        return;
    }

    g_clear_pointer (&ctx->last_public_key,  g_bytes_unref);
    g_clear_pointer (&ctx->last_private_key, g_bytes_unref);
    g_ptr_array_set_size (ctx->last_admin_keys, 0);
    if (state->sec_public_key != NULL)
        ctx->last_public_key = g_bytes_ref (state->sec_public_key);
    if (state->sec_private_key != NULL)
        ctx->last_private_key = g_bytes_ref (state->sec_private_key);
    if (state->sec_admin_keys != NULL)
    {
        guint i;
        for (i = 0; i < state->sec_admin_keys->len; i++)
        {
            GBytes *b = g_ptr_array_index (state->sec_admin_keys, i);
            g_ptr_array_add (ctx->last_admin_keys, g_bytes_ref (b));
        }
    }

    gtk_switch_set_active (ctx->is_managed_switch,     state->sec_is_managed);
    gtk_switch_set_active (ctx->serial_enabled_switch, state->sec_serial_enabled);
    gtk_switch_set_active (ctx->debug_log_api_switch,
                           state->sec_debug_log_api_enabled);
    gtk_switch_set_active (ctx->admin_channel_switch,
                           state->sec_admin_channel_enabled);

    pub_b64    = gbytes_to_base64_or_dash (ctx->last_public_key);
    priv_b64   = gbytes_to_base64_or_dash (ctx->last_private_key);
    admin_text = admin_keys_to_text       (ctx->last_admin_keys);
    gtk_label_set_text (ctx->public_key_label,  pub_b64);
    gtk_label_set_text (ctx->private_key_label, priv_b64);
    gtk_label_set_text (ctx->admin_keys_label,  admin_text);
    g_free (pub_b64);
    g_free (priv_b64);
    g_free (admin_text);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_security_set_status_callback (GtkWidget                *page,
                                           PnMeshSecurityStatusFunc  callback,
                                           gpointer                  user_data)
{
    SecurityCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_SECURITY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_security_set_busy_callback (GtkWidget          *page,
                                         PnMeshPageBusyFunc  callback,
                                         gpointer            user_data)
{
    SecurityCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_SECURITY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
