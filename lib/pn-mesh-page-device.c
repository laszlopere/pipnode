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
/*  Device page — Phase 14.                                            */
/*                                                                     */
/*  Combos for device role + rebroadcast mode, spinners for button /   */
/*  buzzer GPIO and the NodeInfo broadcast interval.  Fields the UI    */
/*  does not surface (serial_enabled, double_tap_as_button_press,      */
/*  is_managed (deprecated), tzdef, led_heartbeat_disabled,            */
/*  buzzer_mode) are mirrored at set_state-time and shipped back       */
/*  verbatim on Apply.                                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-device.h"

#define PN_MESH_DEVICE_CTX_QDATA "pn-mesh-page-device-ctx"

typedef struct
{
    GtkComboBoxText *role_combo;
    GtkComboBoxText *rebroadcast_combo;
    GtkSpinButton   *button_gpio_spin;
    GtkSpinButton   *buzzer_gpio_spin;
    GtkSpinButton   *node_info_secs_spin;
    GtkButton       *apply_button;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    /* Mirrored at set_state-time so Apply ships them back unchanged. */
    gboolean         last_serial_enabled;
    gboolean         last_double_tap_as_button_press;
    gboolean         last_is_managed;            /* deprecated */
    gboolean         last_disable_triple_click;
    gchar           *last_tzdef;
    gboolean         last_led_heartbeat_disabled;
    guint32          last_buzzer_mode;

    gboolean         writing;

    PnMeshDeviceStatusFunc status_cb;
    gpointer               status_ud;

    PnMeshPageBusyFunc     busy_cb;
    gpointer               busy_ud;
} DeviceCtx;

static void
device_ctx_free (gpointer data)
{
    DeviceCtx *ctx = data;
    g_free (ctx->last_tzdef);
    g_slice_free (DeviceCtx, ctx);
}

/* ------------------------------------------------------------------ */
/*  Enum tables                                                         */
/* ------------------------------------------------------------------ */

typedef struct { guint32 id; const char *name; } EnumEntry;

/* Meshtastic DeviceRole.  Matches the Role enum in Config.proto. */
static const EnumEntry DEVICE_ROLES[] = {
    {  0, "CLIENT"           },
    {  1, "CLIENT_MUTE"      },
    {  2, "ROUTER"           },
    {  3, "ROUTER_CLIENT"    },
    {  4, "REPEATER"         },
    {  5, "TRACKER"          },
    {  6, "SENSOR"           },
    {  7, "TAK"              },
    {  8, "CLIENT_HIDDEN"    },
    {  9, "LOST_AND_FOUND"   },
    { 10, "TAK_TRACKER"      },
    { 11, "ROUTER_LATE"      },
    { 12, "CLIENT_BASE"      },
};

/* Meshtastic RebroadcastMode.  Affects which packets the device
 * forwards on the mesh -- mistakes here can stop a node from acting
 * as a useful relay, but they cannot brick it. */
static const EnumEntry REBROADCAST_MODES[] = {
    { 0, "ALL"                 },
    { 1, "ALL_SKIP_DECODING"   },
    { 2, "LOCAL_ONLY"          },
    { 3, "KNOWN_ONLY"          },
    { 4, "NONE"                },
    { 5, "CORE_PORTNUMS_ONLY"  },
};

static void
fill_combo (GtkComboBoxText *combo,
            const EnumEntry *table, gsize n)
{
    gsize i;
    for (i = 0; i < n; i++)
    {
        gchar id[12];
        g_snprintf (id, sizeof id, "%u", table[i].id);
        gtk_combo_box_text_append (combo, id, table[i].name);
    }
}

static void
select_combo_by_id (GtkComboBoxText *combo,
                    const EnumEntry *table, gsize n,
                    guint32 value)
{
    gchar id[12];
    gsize i;

    for (i = 0; i < n; i++)
        if (table[i].id == value)
        {
            g_snprintf (id, sizeof id, "%u", value);
            gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), id);
            return;
        }

    g_snprintf (id, sizeof id, "%u", value);
    {
        gchar *label = g_strdup_printf ("(unknown #%u)", value);
        gtk_combo_box_text_append (combo, id, label);
        g_free (label);
    }
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), id);
}

static guint32
get_combo_id (GtkComboBoxText *combo, guint32 fallback)
{
    const gchar *id = gtk_combo_box_get_active_id (GTK_COMBO_BOX (combo));
    if (id == NULL)
        return fallback;
    return (guint32) g_ascii_strtoull (id, NULL, 10);
}

/* ------------------------------------------------------------------ */
/*  Status + writing flag                                               */
/* ------------------------------------------------------------------ */

static void
emit_status (DeviceCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

static void
set_writing (DeviceCtx *ctx, gboolean writing)
{
    gboolean enable     = !writing && ctx->connection != NULL;
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->role_combo),          enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->rebroadcast_combo),   enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->button_gpio_spin),    enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->buzzer_gpio_spin),    enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->node_info_secs_spin), enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),        enable);
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_set_device_config_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget *page = user_data;
    DeviceCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                         PN_MESH_DEVICE_CTX_QDATA);
    GError    *error = NULL;
    gboolean   ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_device_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_device_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply device settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    pn_mesh_page_device_set_state (
            page,
            pn_mesh_connection_get_state (ctx->connection),
            ctx->connection);
    emit_status (ctx, "Device settings applied.");
    set_writing (ctx, FALSE);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                *page = user_data;
    DeviceCtx                *ctx  = g_object_get_data (G_OBJECT (page),
                                                       PN_MESH_DEVICE_CTX_QDATA);
    PnMeshDeviceConfigWrite   cfg;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.role             = get_combo_id (ctx->role_combo,        0);
    cfg.rebroadcast_mode = get_combo_id (ctx->rebroadcast_combo, 0);
    cfg.button_gpio      =
            (guint32) gtk_spin_button_get_value_as_int (ctx->button_gpio_spin);
    cfg.buzzer_gpio      =
            (guint32) gtk_spin_button_get_value_as_int (ctx->buzzer_gpio_spin);
    cfg.node_info_broadcast_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->node_info_secs_spin);

    cfg.serial_enabled             = ctx->last_serial_enabled;
    cfg.double_tap_as_button_press = ctx->last_double_tap_as_button_press;
    cfg.is_managed                 = ctx->last_is_managed;
    cfg.disable_triple_click       = ctx->last_disable_triple_click;
    cfg.tzdef                      = ctx->last_tzdef;
    cfg.led_heartbeat_disabled     = ctx->last_led_heartbeat_disabled;
    cfg.buzzer_mode                = ctx->last_buzzer_mode;

    emit_status (ctx, "Applying device settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_device_config_async (
            ctx->connection, &cfg, NULL,
            on_set_device_config_done, page);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

#define VALUE_MIN_WIDTH 260

/* Once the cell is realized the caller has packed its children; size the
 * first one (the value widget) to a uniform minimum and add it to a
 * per-grid horizontal SizeGroup so every row lines up to the widest. */
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

static void
attach_unit (GtkWidget *cell, const gchar *unit)
{
    GtkWidget       *suffix = gtk_label_new (unit);
    GtkStyleContext *sc     = gtk_widget_get_style_context (suffix);
    gtk_style_context_add_class (sc, "dim-label");
    gtk_box_pack_start (GTK_BOX (cell), suffix, FALSE, FALSE, 0);
}

GtkWidget *
pn_mesh_page_device_new (void)
{
    GtkWidget *page;
    GtkWidget *subtitle;
    GtkWidget *grid;
    GtkWidget *cell;
    GtkWidget *role;
    GtkWidget *rebroadcast;
    GtkWidget *button_gpio;
    GtkWidget *buzzer_gpio;
    GtkWidget *node_info_secs;
    GtkWidget *apply_box;
    GtkWidget *apply;
    DeviceCtx *ctx;
    gint       row = 0;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "Device role determines how this node behaves on the mesh "
            "(client, router, repeater, …).  Rebroadcast mode controls "
            "which packets are forwarded.  Apply writes the whole "
            "DeviceConfig block; tzdef, buzzer mode and the other "
            "advanced flags are kept verbatim from the device.");
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

    ctx = g_slice_new0 (DeviceCtx);

    cell = add_row (GTK_GRID (grid), row++, "Role");
    role = gtk_combo_box_text_new ();
    gtk_widget_set_tooltip_text (role,
            "CLIENT is the normal handheld role; ROUTER nodes prioritise "
            "forwarding; REPEATER nodes forward without consuming "
            "channel slots; TRACKER and SENSOR optimise for low power.");
    fill_combo (GTK_COMBO_BOX_TEXT (role),
                DEVICE_ROLES, G_N_ELEMENTS (DEVICE_ROLES));
    gtk_box_pack_start (GTK_BOX (cell), role, FALSE, FALSE, 0);
    ctx->role_combo = GTK_COMBO_BOX_TEXT (role);

    cell = add_row (GTK_GRID (grid), row++, "Rebroadcast mode");
    rebroadcast = gtk_combo_box_text_new ();
    gtk_widget_set_tooltip_text (rebroadcast,
            "Which packets the node forwards on the mesh.  ALL is the "
            "default; LOCAL_ONLY / KNOWN_ONLY / NONE progressively "
            "restrict forwarding.");
    fill_combo (GTK_COMBO_BOX_TEXT (rebroadcast),
                REBROADCAST_MODES, G_N_ELEMENTS (REBROADCAST_MODES));
    gtk_box_pack_start (GTK_BOX (cell), rebroadcast, FALSE, FALSE, 0);
    ctx->rebroadcast_combo = GTK_COMBO_BOX_TEXT (rebroadcast);

    cell = add_row (GTK_GRID (grid), row++, "NodeInfo broadcast interval");
    /* 0 = firmware default (~3 hours).  Cap at 86400 (one day). */
    node_info_secs = gtk_spin_button_new_with_range (0, 86400, 60);
    gtk_widget_set_tooltip_text (node_info_secs,
            "Seconds between NodeInfo broadcasts (the heartbeat that "
            "tells other nodes this one is alive).  0 lets the firmware "
            "pick its default.");
    gtk_box_pack_start (GTK_BOX (cell), node_info_secs, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->node_info_secs_spin = GTK_SPIN_BUTTON (node_info_secs);

    cell = add_row (GTK_GRID (grid), row++, "Button GPIO");
    /* GPIO 0 means "use the board's default" upstream; cap at 48 (well
     * past the highest pin on any current Meshtastic board). */
    button_gpio = gtk_spin_button_new_with_range (0, 48, 1);
    gtk_widget_set_tooltip_text (button_gpio,
            "GPIO pin for the user button.  0 lets the firmware use the "
            "board's default; non-zero overrides it.  Board-specific "
            "values rarely need to change here.");
    gtk_box_pack_start (GTK_BOX (cell), button_gpio, FALSE, FALSE, 0);
    ctx->button_gpio_spin = GTK_SPIN_BUTTON (button_gpio);

    cell = add_row (GTK_GRID (grid), row++, "Buzzer GPIO");
    buzzer_gpio = gtk_spin_button_new_with_range (0, 48, 1);
    gtk_widget_set_tooltip_text (buzzer_gpio,
            "GPIO pin for the optional buzzer.  0 lets the firmware use "
            "the board's default; non-zero overrides it.");
    gtk_box_pack_start (GTK_BOX (cell), buzzer_gpio, FALSE, FALSE, 0);
    ctx->buzzer_gpio_spin = GTK_SPIN_BUTTON (buzzer_gpio);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 18);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = gtk_button_new_with_mnemonic ("_Apply device settings");
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_DEVICE_CTX_QDATA,
                            ctx, device_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_device_set_state (GtkWidget         *page,
                               const PnMeshState *state,
                               PnMeshConnection  *connection)
{
    DeviceCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_DEVICE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    if (state == NULL || !state->have_device)
    {
        ctx->last_serial_enabled             = FALSE;
        ctx->last_double_tap_as_button_press = FALSE;
        ctx->last_is_managed                 = FALSE;
        ctx->last_disable_triple_click       = FALSE;
        g_clear_pointer (&ctx->last_tzdef, g_free);
        ctx->last_led_heartbeat_disabled     = FALSE;
        ctx->last_buzzer_mode                = 0;
        set_writing (ctx, FALSE);
        return;
    }

    ctx->last_serial_enabled             = state->dev_serial_enabled;
    ctx->last_double_tap_as_button_press = state->dev_double_tap_as_button_press;
    ctx->last_is_managed                 = state->dev_is_managed;
    ctx->last_disable_triple_click       = state->dev_disable_triple_click;
    g_free (ctx->last_tzdef);
    ctx->last_tzdef = g_strdup (state->dev_tzdef);
    ctx->last_led_heartbeat_disabled     = state->dev_led_heartbeat_disabled;
    ctx->last_buzzer_mode                = state->dev_buzzer_mode;

    select_combo_by_id (ctx->role_combo,        DEVICE_ROLES,
                        G_N_ELEMENTS (DEVICE_ROLES), state->dev_role);
    select_combo_by_id (ctx->rebroadcast_combo, REBROADCAST_MODES,
                        G_N_ELEMENTS (REBROADCAST_MODES),
                        state->dev_rebroadcast_mode);
    gtk_spin_button_set_value (ctx->button_gpio_spin,
                               (gdouble) state->dev_button_gpio);
    gtk_spin_button_set_value (ctx->buzzer_gpio_spin,
                               (gdouble) state->dev_buzzer_gpio);
    gtk_spin_button_set_value (ctx->node_info_secs_spin,
                               (gdouble) state->dev_node_info_broadcast_secs);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_device_set_status_callback (GtkWidget              *page,
                                         PnMeshDeviceStatusFunc  callback,
                                         gpointer                user_data)
{
    DeviceCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_DEVICE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_device_set_busy_callback (GtkWidget          *page,
                                       PnMeshPageBusyFunc  callback,
                                       gpointer            user_data)
{
    DeviceCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_DEVICE_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
