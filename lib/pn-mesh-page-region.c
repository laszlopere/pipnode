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
/*  Region / LoRa page — Phase 4.                                      */
/*                                                                     */
/*  Combos for region + modem preset, spinners for hop limit + TX     */
/*  power, switch for TX enabled, single Apply button at the bottom.  */
/*  channel_num is read from the device and round-tripped verbatim    */
/*  -- we deliberately don't expose it as a control in this phase     */
/*  (it's a fine-tuning offset, almost always 0).                     */
/*                                                                     */
/*  Both combos are built from a (id, name) table so the underlying   */
/*  enum value the firmware expects is what we ship, regardless of    */
/*  display order or any future entries.                              */
/*                                                                     */
/*  Region or modem-preset changes commonly trigger a device reboot   */
/*  (the firmware re-initialises the radio), which is why the         */
/*  connection's set_lora_config_sync settles 500 ms then runs a      */
/*  fresh handshake; the 3 s handshake budget normally covers the     */
/*  reboot window.                                                     */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-region.h"

#include "pn-device-combo.h"
#include "pn-device-spin.h"

#define PN_MESH_REGION_CTX_QDATA "pn-mesh-page-region-ctx"

typedef struct
{
    GtkComboBoxText *region_combo;
    GtkComboBoxText *preset_combo;
    GtkSpinButton   *hop_spin;
    GtkSpinButton   *tx_power_spin;
    GtkSwitch       *tx_enabled_switch;
    GtkButton       *apply_button;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    /* Mirrored at set_state-time so Apply can round-trip channel_num
     * unchanged, and so use_preset is preserved (always TRUE in this
     * phase but the device value is what we ship). */
    guint32          last_channel_num;
    gboolean         last_use_preset;

    gboolean         writing;

    PnMeshRegionStatusFunc status_cb;
    gpointer               status_ud;

    PnMeshPageBusyFunc     busy_cb;
    gpointer               busy_ud;
} RegionCtx;

static void
region_ctx_free (gpointer data)
{
    g_slice_free (RegionCtx, data);
}

/* ------------------------------------------------------------------ */
/*  Enum tables                                                         */
/* ------------------------------------------------------------------ */

/* Region (Meshtastic Config.LoRaConfig.RegionCode), ported from
 * /usr/bin/pip-mesh's format_region.  The first row (UNSET) is the
 * device's "unconfigured" state -- shown so a Heltec straight out
 * of the box reads as "(unset — pick one)" rather than blank. */
typedef struct { guint32 id; const char *name; } EnumEntry;

static const EnumEntry REGIONS[] = {
    {  0, "UNSET" },
    {  1, "US" },
    {  2, "EU_433" },
    {  3, "EU_868" },
    {  4, "CN" },
    {  5, "JP" },
    {  6, "ANZ" },
    {  7, "KR" },
    {  8, "TW" },
    {  9, "RU" },
    { 10, "IN" },
    { 11, "NZ_865" },
    { 12, "TH" },
    { 13, "LORA_24" },
    { 14, "UA_433" },
    { 15, "UA_868" },
    { 16, "MY_433" },
    { 17, "MY_919" },
    { 18, "SG_923" },
};

/* Modem preset (Meshtastic Config.LoRaConfig.ModemPreset).  Note the
 * sparse numbering: upstream skipped 1 and 2 when reorganising the
 * presets, so the row at index 1 of this array carries id=3 etc. */
static const EnumEntry MODEM_PRESETS[] = {
    { 0, "LONG_FAST" },
    { 3, "MEDIUM_SLOW" },
    { 4, "MEDIUM_FAST" },
    { 5, "SHORT_SLOW" },
    { 6, "SHORT_FAST" },
    { 7, "LONG_MODERATE" },
    { 8, "SHORT_TURBO" },
    { 9, "LONG_TURBO" },
};

/* Populate a GtkComboBoxText from an enum table; the row's `id` is
 * the stringified enum integer so we can fetch it back with
 * gtk_combo_box_get_active_id and parse without an index lookup. */
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

/* Select the row whose enum id matches @value; if it isn't in the
 * table, append a synthetic "<value>" entry so the user can see what
 * the device has even when we don't know its name. */
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

    /* Unknown id from a newer firmware: surface it rather than silently
     * snapping to a different value the user didn't choose. */
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
emit_status (RegionCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

static void
set_writing (RegionCtx *ctx, gboolean writing)
{
    gboolean enable     = !writing && ctx->connection != NULL;
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->region_combo),      enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->preset_combo),      enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->hop_spin),          enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->tx_power_spin),     enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->tx_enabled_switch), enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),      enable);
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_set_lora_config_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget *page = user_data;
    RegionCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                         PN_MESH_REGION_CTX_QDATA);
    GError    *error = NULL;
    gboolean   ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_lora_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_lora_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply LoRa settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    /* Re-read what the device now reports.  A region change in
     * particular can be subtly different from what we sent (the
     * firmware may snap to a supported subset). */
    pn_mesh_page_region_set_state (
            page,
            pn_mesh_connection_get_state (ctx->connection),
            ctx->connection);
    emit_status (ctx, "LoRa settings applied.");
    set_writing (ctx, FALSE);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget             *page = user_data;
    RegionCtx             *ctx  = g_object_get_data (G_OBJECT (page),
                                                     PN_MESH_REGION_CTX_QDATA);
    PnMeshLoraConfigWrite  cfg;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.use_preset  = ctx->last_use_preset;
    cfg.modem_preset = get_combo_id (ctx->preset_combo, 0);
    cfg.region       = get_combo_id (ctx->region_combo, 0);
    cfg.hop_limit    = (guint32) gtk_spin_button_get_value_as_int (ctx->hop_spin);
    cfg.tx_enabled   = gtk_switch_get_active (ctx->tx_enabled_switch);
    cfg.tx_power     = (guint32) gtk_spin_button_get_value_as_int (ctx->tx_power_spin);
    cfg.channel_num  = ctx->last_channel_num;

    emit_status (ctx,
                 "Applying LoRa settings… (the device may briefly "
                 "reboot if you changed region or modem preset).");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_lora_config_async (
            ctx->connection, &cfg, NULL,
            on_set_lora_config_done, page);
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

    /* Combos, spin buttons and entries share a common width via the
     * size group so the value column lines up.  A switch is naturally
     * narrow and already left-aligned (halign START), so leave it out
     * -- otherwise the size group stretches it to the widest control. */
    if (children != NULL && !GTK_IS_SWITCH (children->data)) {
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

/* Build the standard "bold key | control" grid row.  Returns the
 * holder cell so the caller can attach whichever control widget. */
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

    /* Holder ensures the control sits flush-left rather than
     * stretching to fill an over-wide column. */
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

GtkWidget *
pn_mesh_page_region_new (void)
{
    GtkWidget *page;
    GtkWidget *subtitle;
    GtkWidget *grid;
    GtkWidget *cell;
    GtkWidget *region;
    GtkWidget *preset;
    GtkWidget *hop;
    GtkWidget *tx_power;
    GtkWidget *tx_enabled;
    GtkWidget *apply_box;
    GtkWidget *apply;
    RegionCtx *ctx;
    gint       row = 0;

    /* Hosted inside a GtkExpander; small margins, no in-page title
     * (the expander header carries the section name). */
    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "Regulatory region, radio preset, hop limit and "
            "transmit power.  Apply writes the whole LoRa block at "
            "once; changes to the region or modem preset commonly "
            "reboot the device for the radio to re-initialise.");
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

    ctx = g_slice_new0 (RegionCtx);
    ctx->last_use_preset = TRUE;

    cell = add_row (GTK_GRID (grid), row++, "Region");
    region = pn_device_combo_new ();
    gtk_widget_set_tooltip_text (region,
            "Regulatory domain.  Sets the allowed LoRa frequency "
            "band; pick the one for your country.");
    fill_combo (GTK_COMBO_BOX_TEXT (region), REGIONS, G_N_ELEMENTS (REGIONS));
    gtk_box_pack_start (GTK_BOX (cell), region, FALSE, FALSE, 0);
    ctx->region_combo = GTK_COMBO_BOX_TEXT (region);

    cell = add_row (GTK_GRID (grid), row++, "Modem preset");
    preset = pn_device_combo_new ();
    gtk_widget_set_tooltip_text (preset,
            "Bandwidth / spreading-factor preset.  LONG_FAST is "
            "the firmware default; SHORT_* presets are faster but "
            "have less range.");
    fill_combo (GTK_COMBO_BOX_TEXT (preset),
                MODEM_PRESETS, G_N_ELEMENTS (MODEM_PRESETS));
    gtk_box_pack_start (GTK_BOX (cell), preset, FALSE, FALSE, 0);
    ctx->preset_combo = GTK_COMBO_BOX_TEXT (preset);

    cell = add_row (GTK_GRID (grid), row++, "Hop limit");
    /* Meshtastic firmware caps hop_limit at 7 (the protocol's hard
     * limit); 3 is the firmware default for a fresh device. */
    hop = pn_device_spin_new_with_range (0, 7, 1);
    gtk_widget_set_tooltip_text (hop,
            "Maximum number of mesh hops a packet may take.  3 is "
            "a good default; higher values flood the mesh more.");
    gtk_box_pack_start (GTK_BOX (cell), hop, FALSE, FALSE, 0);
    ctx->hop_spin = GTK_SPIN_BUTTON (hop);

    cell = add_row (GTK_GRID (grid), row++, "TX power");
    /* dBm.  Meshtastic clamps the effective value to whatever the
     * region+hardware allows, but 0..30 covers every legitimate
     * setting and 0 means "let the firmware pick a safe max". */
    tx_power = pn_device_spin_new_with_range (0, 30, 1);
    gtk_widget_set_tooltip_text (tx_power,
            "Transmit power in dBm.  0 lets the firmware pick the "
            "regional default; the firmware caps the effective "
            "value to whatever the region and hardware allow.");
    {
        GtkWidget *dbm = gtk_label_new (" dBm");
        GtkStyleContext *sc = gtk_widget_get_style_context (dbm);
        gtk_style_context_add_class (sc, "dim-label");
        gtk_box_pack_start (GTK_BOX (cell), tx_power, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (cell), dbm,      FALSE, FALSE, 0);
    }
    ctx->tx_power_spin = GTK_SPIN_BUTTON (tx_power);

    cell = add_row (GTK_GRID (grid), row++, "TX enabled");
    tx_enabled = gtk_switch_new ();
    gtk_widget_set_halign (tx_enabled, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (tx_enabled,
            "Whether the radio is allowed to transmit at all.  "
            "Disable to put the device in RX-only / listen mode.");
    gtk_box_pack_start (GTK_BOX (cell), tx_enabled, FALSE, FALSE, 0);
    ctx->tx_enabled_switch = GTK_SWITCH (tx_enabled);

    /* Apply button on its own row at the bottom of the page, right-
     * aligned -- the only mutation on the page, so it deserves
     * visual weight rather than sitting at the end of a control row. */
    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 18);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = gtk_button_new_with_mnemonic ("_Apply LoRa settings");
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took -- a "
            "region or modem-preset change may briefly reboot the "
            "device.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_REGION_CTX_QDATA,
                            ctx, region_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_region_set_state (GtkWidget         *page,
                               const PnMeshState *state,
                               PnMeshConnection  *connection)
{
    RegionCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_REGION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    if (state == NULL || !state->have_lora_config)
    {
        /* No usable data yet (no device, or device hasn't streamed
         * its LoRaConfig sub-block).  Leave the controls at their
         * defaults but disabled. */
        ctx->last_channel_num = 0;
        ctx->last_use_preset  = TRUE;
        set_writing (ctx, FALSE);
        return;
    }

    ctx->last_channel_num = state->lora_channel_num;
    ctx->last_use_preset  = state->lora_use_preset;

    select_combo_by_id (ctx->region_combo, REGIONS,
                        G_N_ELEMENTS (REGIONS), state->lora_region);
    select_combo_by_id (ctx->preset_combo, MODEM_PRESETS,
                        G_N_ELEMENTS (MODEM_PRESETS),
                        state->lora_modem_preset);
    gtk_spin_button_set_value (ctx->hop_spin,
                               (gdouble) state->lora_hop_limit);
    gtk_spin_button_set_value (ctx->tx_power_spin,
                               (gdouble) state->lora_tx_power);
    gtk_switch_set_active (ctx->tx_enabled_switch, state->lora_tx_enabled);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_region_set_status_callback (GtkWidget              *page,
                                         PnMeshRegionStatusFunc  callback,
                                         gpointer                user_data)
{
    RegionCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_REGION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_region_set_busy_callback (GtkWidget          *page,
                                       PnMeshPageBusyFunc  callback,
                                       gpointer            user_data)
{
    RegionCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_REGION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
