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
/*  Display page — Phase 13.                                           */
/*                                                                     */
/*  Screen-timeout and carousel spinners, units / coordinate-format /  */
/*  display-mode / OLED combos and the orientation + wake toggles.     */
/*  compass_orientation is board/mount specific and not surfaced; it   */
/*  is mirrored at set_state-time and shipped back verbatim on Apply,   */
/*  the same round-trip pattern the Device and Power pages use.        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-display.h"
#include "pn-action-button.h"

#include "pn-device-combo.h"
#include "pn-device-spin.h"

#define PN_MESH_DISPLAY_CTX_QDATA "pn-mesh-page-display-ctx"

typedef struct
{
    GtkSpinButton   *screen_on_spin;
    GtkSpinButton   *carousel_spin;
    GtkComboBoxText *units_combo;
    GtkComboBoxText *gps_format_combo;
    GtkComboBoxText *displaymode_combo;
    GtkComboBoxText *oled_combo;
    GtkSwitch       *use_12h_switch;
    GtkSwitch       *flip_screen_switch;
    GtkSwitch       *compass_north_switch;
    GtkSwitch       *heading_bold_switch;
    GtkSwitch       *wake_on_tap_switch;
    GtkButton       *apply_button;

    /* Borrowed; the dialog owns it. */
    PnMeshConnection *connection;

    /* Mirrored at set_state-time so Apply ships it back unchanged. */
    guint32          last_compass_orientation;

    gboolean         writing;

    PnMeshDisplayStatusFunc status_cb;
    gpointer                status_ud;

    PnMeshPageBusyFunc      busy_cb;
    gpointer                busy_ud;
} DisplayCtx;

static void
display_ctx_free (gpointer data)
{
    g_slice_free (DisplayCtx, data);
}

/* ------------------------------------------------------------------ */
/*  Enum tables                                                         */
/* ------------------------------------------------------------------ */

typedef struct { guint32 id; const char *name; } EnumEntry;

/* Meshtastic DisplayUnits. */
static const EnumEntry DISPLAY_UNITS[] = {
    { 0, "Metric"   },
    { 1, "Imperial" },
};

/* Meshtastic GpsCoordinateFormat — how a location is rendered on screen. */
static const EnumEntry GPS_FORMATS[] = {
    { 0, "Decimal degrees (DEC)"          },
    { 1, "Degrees/minutes/seconds (DMS)"  },
    { 2, "UTM"                            },
    { 3, "MGRS"                           },
    { 4, "Open Location Code (OLC)"       },
    { 5, "Ordnance Survey (OSGR)"         },
};

/* Meshtastic DisplayMode — screen layout / colour treatment. */
static const EnumEntry DISPLAY_MODES[] = {
    { 0, "Default (128x64)" },
    { 1, "Two-colour"       },
    { 2, "Inverted"         },
    { 3, "Colour"           },
};

/* Meshtastic OledType — the OLED controller chip.  AUTO lets the
 * firmware probe; the explicit values force a driver for boards where
 * detection misfires. */
static const EnumEntry OLED_TYPES[] = {
    { 0, "Auto-detect" },
    { 1, "SSD1306"     },
    { 2, "SH1106"      },
    { 3, "SH1107"      },
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
emit_status (DisplayCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

static void
set_writing (DisplayCtx *ctx, gboolean writing)
{
    gboolean enable     = !writing && ctx->connection != NULL;
    gboolean transition = (ctx->writing != writing);
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->screen_on_spin),       enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->carousel_spin),        enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->units_combo),          enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->gps_format_combo),     enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->displaymode_combo),    enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->oled_combo),           enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->use_12h_switch),       enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->flip_screen_switch),   enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->compass_north_switch), enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->heading_bold_switch),  enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->wake_on_tap_switch),   enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),         enable);
    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_set_display_config_done (GObject *source, GAsyncResult *res,
                            gpointer user_data)
{
    GtkWidget  *page = user_data;
    DisplayCtx *ctx  = g_object_get_data (G_OBJECT (page),
                                          PN_MESH_DISPLAY_CTX_QDATA);
    GError     *error = NULL;
    gboolean    ok;

    (void) source;

    if (ctx == NULL)
    {
        pn_mesh_connection_set_display_config_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_display_config_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply display settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    pn_mesh_page_display_set_state (
            page,
            pn_mesh_connection_get_state (ctx->connection),
            ctx->connection);
    emit_status (ctx, "Display settings applied.");
    set_writing (ctx, FALSE);
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                 *page = user_data;
    DisplayCtx                *ctx  = g_object_get_data (G_OBJECT (page),
                                              PN_MESH_DISPLAY_CTX_QDATA);
    PnMeshDisplayConfigWrite   cfg;

    (void) button;
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.screen_on_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->screen_on_spin);
    cfg.auto_screen_carousel_secs =
            (guint32) gtk_spin_button_get_value_as_int (ctx->carousel_spin);
    cfg.units       = get_combo_id (ctx->units_combo,       0);
    cfg.gps_format  = get_combo_id (ctx->gps_format_combo,  0);
    cfg.displaymode = get_combo_id (ctx->displaymode_combo, 0);
    cfg.oled        = get_combo_id (ctx->oled_combo,        0);
    cfg.use_12h_clock =
            gtk_switch_get_active (ctx->use_12h_switch);
    cfg.flip_screen =
            gtk_switch_get_active (ctx->flip_screen_switch);
    cfg.compass_north_top =
            gtk_switch_get_active (ctx->compass_north_switch);
    cfg.heading_bold =
            gtk_switch_get_active (ctx->heading_bold_switch);
    cfg.wake_on_tap_or_motion =
            gtk_switch_get_active (ctx->wake_on_tap_switch);

    cfg.compass_orientation = ctx->last_compass_orientation;

    emit_status (ctx, "Applying display settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_display_config_async (
            ctx->connection, &cfg, NULL,
            on_set_display_config_done, page);
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

static GtkWidget *
add_switch (GtkGrid *grid, gint row, const gchar *label,
            const gchar *tooltip)
{
    GtkWidget *cell = add_row (grid, row, label);
    GtkWidget *sw   = gtk_switch_new ();
    gtk_widget_set_halign (sw, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text (sw, tooltip);
    gtk_box_pack_start (GTK_BOX (cell), sw, FALSE, FALSE, 0);
    return sw;
}

GtkWidget *
pn_mesh_page_display_new (void)
{
    GtkWidget  *page;
    GtkWidget  *grid;
    GtkWidget  *cell;
    GtkWidget  *screen_on;
    GtkWidget  *carousel;
    GtkWidget  *units;
    GtkWidget  *gps_format;
    GtkWidget  *displaymode;
    GtkWidget  *oled;
    GtkWidget  *apply_box;
    GtkWidget  *apply;
    DisplayCtx *ctx;
    gint        row = 0;

    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 12);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);

    ctx = g_slice_new0 (DisplayCtx);

    cell = add_row (GTK_GRID (grid), row++, "Screen timeout");
    /* 0 = always on; cap at one hour. */
    screen_on = pn_device_spin_new_with_range (0, 3600, 5);
    gtk_widget_set_tooltip_text (screen_on,
            "Seconds the screen stays lit after the last interaction "
            "before it turns off to save power.  0 keeps it always on.");
    gtk_box_pack_start (GTK_BOX (cell), screen_on, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->screen_on_spin = GTK_SPIN_BUTTON (screen_on);

    cell = add_row (GTK_GRID (grid), row++, "Auto carousel");
    carousel = pn_device_spin_new_with_range (0, 3600, 5);
    gtk_widget_set_tooltip_text (carousel,
            "Seconds between automatic cycling through the screens "
            "(node list, map, etc.).  0 disables the carousel.");
    gtk_box_pack_start (GTK_BOX (cell), carousel, FALSE, FALSE, 0);
    attach_unit (cell, " s");
    ctx->carousel_spin = GTK_SPIN_BUTTON (carousel);

    cell = add_row (GTK_GRID (grid), row++, "Units");
    units = pn_device_combo_new ();
    gtk_widget_set_tooltip_text (units,
            "Whether distances and altitudes are shown in metric "
            "(metres / km) or imperial (feet / miles).");
    fill_combo (GTK_COMBO_BOX_TEXT (units),
                DISPLAY_UNITS, G_N_ELEMENTS (DISPLAY_UNITS));
    gtk_box_pack_start (GTK_BOX (cell), units, FALSE, FALSE, 0);
    ctx->units_combo = GTK_COMBO_BOX_TEXT (units);

    cell = add_row (GTK_GRID (grid), row++, "Coordinate format");
    gps_format = pn_device_combo_new ();
    gtk_widget_set_tooltip_text (gps_format,
            "How positions are rendered on screen: decimal degrees, "
            "degrees/minutes/seconds, or one of the grid systems "
            "(UTM / MGRS / OLC / OSGR).");
    fill_combo (GTK_COMBO_BOX_TEXT (gps_format),
                GPS_FORMATS, G_N_ELEMENTS (GPS_FORMATS));
    gtk_box_pack_start (GTK_BOX (cell), gps_format, FALSE, FALSE, 0);
    ctx->gps_format_combo = GTK_COMBO_BOX_TEXT (gps_format);

    cell = add_row (GTK_GRID (grid), row++, "Display mode");
    displaymode = pn_device_combo_new ();
    gtk_widget_set_tooltip_text (displaymode,
            "Screen layout and colour treatment.  Default suits the "
            "common 128x64 OLED; the others adapt to two-colour, "
            "inverted or full-colour panels.");
    fill_combo (GTK_COMBO_BOX_TEXT (displaymode),
                DISPLAY_MODES, G_N_ELEMENTS (DISPLAY_MODES));
    gtk_box_pack_start (GTK_BOX (cell), displaymode, FALSE, FALSE, 0);
    ctx->displaymode_combo = GTK_COMBO_BOX_TEXT (displaymode);

    cell = add_row (GTK_GRID (grid), row++, "OLED driver");
    oled = pn_device_combo_new ();
    gtk_widget_set_tooltip_text (oled,
            "The OLED controller chip.  Auto-detect works on most "
            "boards; force a specific driver only if the screen is "
            "blank or garbled with Auto-detect.");
    fill_combo (GTK_COMBO_BOX_TEXT (oled),
                OLED_TYPES, G_N_ELEMENTS (OLED_TYPES));
    gtk_box_pack_start (GTK_BOX (cell), oled, FALSE, FALSE, 0);
    ctx->oled_combo = GTK_COMBO_BOX_TEXT (oled);

    ctx->use_12h_switch = GTK_SWITCH (add_switch (GTK_GRID (grid), row++,
            "12-hour clock",
            "Show times in 12-hour (AM/PM) format instead of 24-hour."));
    ctx->flip_screen_switch = GTK_SWITCH (add_switch (GTK_GRID (grid), row++,
            "Flip screen",
            "Rotate the screen 180° for boards mounted upside-down."));
    ctx->compass_north_switch = GTK_SWITCH (add_switch (GTK_GRID (grid), row++,
            "Compass north up",
            "Keep north fixed at the top of the compass instead of "
            "rotating the compass to the current heading."));
    ctx->heading_bold_switch = GTK_SWITCH (add_switch (GTK_GRID (grid), row++,
            "Bold heading",
            "Draw the heading / title text in bold for readability."));
    ctx->wake_on_tap_switch = GTK_SWITCH (add_switch (GTK_GRID (grid), row++,
            "Wake on tap or motion",
            "Wake the screen when the device is tapped or moved "
            "(needs an accelerometer)."));

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 18);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = pn_action_button_new ("_Apply display settings",
                                  PN_ACTION_BUTTON_NORMAL);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (page), PN_MESH_DISPLAY_CTX_QDATA,
                            ctx, display_ctx_free);
    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  set_state                                                           */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_display_set_state (GtkWidget         *page,
                                const PnMeshState *state,
                                PnMeshConnection  *connection)
{
    DisplayCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_DISPLAY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;

    if (state == NULL || !state->have_display)
    {
        ctx->last_compass_orientation = 0;
        set_writing (ctx, FALSE);
        return;
    }

    ctx->last_compass_orientation = state->disp_compass_orientation;

    gtk_spin_button_set_value (ctx->screen_on_spin,
                               (gdouble) state->disp_screen_on_secs);
    gtk_spin_button_set_value (ctx->carousel_spin,
                               (gdouble) state->disp_auto_screen_carousel_secs);
    select_combo_by_id (ctx->units_combo, DISPLAY_UNITS,
                        G_N_ELEMENTS (DISPLAY_UNITS), state->disp_units);
    select_combo_by_id (ctx->gps_format_combo, GPS_FORMATS,
                        G_N_ELEMENTS (GPS_FORMATS), state->disp_gps_format);
    select_combo_by_id (ctx->displaymode_combo, DISPLAY_MODES,
                        G_N_ELEMENTS (DISPLAY_MODES), state->disp_displaymode);
    select_combo_by_id (ctx->oled_combo, OLED_TYPES,
                        G_N_ELEMENTS (OLED_TYPES), state->disp_oled);
    gtk_switch_set_active (ctx->use_12h_switch,       state->disp_use_12h_clock);
    gtk_switch_set_active (ctx->flip_screen_switch,   state->disp_flip_screen);
    gtk_switch_set_active (ctx->compass_north_switch, state->disp_compass_north_top);
    gtk_switch_set_active (ctx->heading_bold_switch,  state->disp_heading_bold);
    gtk_switch_set_active (ctx->wake_on_tap_switch,
                           state->disp_wake_on_tap_or_motion);

    set_writing (ctx, FALSE);
}

void
pn_mesh_page_display_set_status_callback (GtkWidget               *page,
                                          PnMeshDisplayStatusFunc  callback,
                                          gpointer                 user_data)
{
    DisplayCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_DISPLAY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_display_set_busy_callback (GtkWidget          *page,
                                        PnMeshPageBusyFunc  callback,
                                        gpointer            user_data)
{
    DisplayCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page), PN_MESH_DISPLAY_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
