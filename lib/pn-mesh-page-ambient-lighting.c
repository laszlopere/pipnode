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
/*  Ambient Lighting page — Phase 11 (TODO #48.8).                     */
/*                                                                     */
/*  Standalone form for AmbientLightingConfig.  Hosted inside an       */
/*  expander under the dialog's Notifications top tab.  Apply ships     */
/*  the whole sub-block at once (the device replaces it as a unit, so  */
/*  omitting fields would reset them to proto3 zero), then a verify-   */
/*  cycle handshake refreshes the controls from the device's           */
/*  authoritative reply.                                              */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-ambient-lighting.h"
#include "pn-action-button.h"
#include "pn-mesh-formats.h"

#include "pn-device-spin.h"

#define PN_MESH_AMBIENT_LIGHTING_CTX_QDATA "pn-mesh-page-ambient-lighting-ctx"

typedef struct
{
    /* Widget tree handles. */
    GtkSwitch     *led_state_switch;
    GtkSpinButton *current_spin;
    GtkSpinButton *red_spin;
    GtkSpinButton *green_spin;
    GtkSpinButton *blue_spin;
    GtkButton     *apply_button;

    /* Device-specific "does this board even have the LED" note,
     * derived from the connected hardware model.  Continues the
     * section's help text; hidden until a device connects. */
    GtkLabel      *hw_note_label;

    /* "Not yet streamed" placeholder shown until the device sends an
     * AmbientLightingConfig block.  The form grid hides while the
     * placeholder is visible so the user sees one or the other. */
    GtkLabel      *unavailable_label;
    GtkWidget     *form_grid;
    GtkWidget     *apply_box;

    /* Borrowed; dialog owns it. */
    PnMeshConnection *connection;

    gboolean       writing;
    gboolean       have_data;

    PnMeshAmbientLightingStatusFunc status_cb;
    gpointer                        status_ud;

    PnMeshPageBusyFunc              busy_cb;
    gpointer                        busy_ud;
} AmbientLightingCtx;

static void
ambient_lighting_ctx_free (gpointer data)
{
    g_slice_free (AmbientLightingCtx, data);
}

static void
emit_status (AmbientLightingCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* ------------------------------------------------------------------ */
/*  Sensitivity                                                         */
/* ------------------------------------------------------------------ */

static void
set_writing (AmbientLightingCtx *ctx, gboolean writing)
{
    /* "Form is editable at all": needs a live connection, a streamed
     * config block, and no in-flight write.  The LED on toggle and
     * Apply both follow this gate. */
    gboolean form_enable = !writing && ctx->connection != NULL && ctx->have_data;
    /* "Detail fields are meaningful": the LED also has to be on.  With
     * it off the device ignores the colour / current values, so paint
     * those fields disabled to match. */
    gboolean detail_enable = form_enable
            && gtk_switch_get_active (ctx->led_state_switch);
    gboolean transition = (ctx->writing != writing);

    ctx->writing = writing;

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->led_state_switch), form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),     form_enable);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->current_spin),     detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->red_spin),         detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->green_spin),       detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->blue_spin),        detail_enable);

    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* Re-run the sensitivity calc when the LED on toggle flips, so the
 * detail fields visibly follow.  Pure UI re-evaluation, no I/O. */
static void
on_led_state_switch_toggled (GObject *src, GParamSpec *pspec, gpointer user_data)
{
    AmbientLightingCtx *ctx = user_data;

    (void) src;
    (void) pspec;

    set_writing (ctx, ctx->writing);
}

/* ------------------------------------------------------------------ */
/*  Paint                                                               */
/* ------------------------------------------------------------------ */

/* Set the device-specific LED note from the connected hardware model.
 * Continues the section help text with whether THIS board actually has
 * an RGB LED the module can drive -- best-effort "internet sources"
 * data, openly flagged as possibly wrong.  Hidden when no device is
 * connected (the static section description already covers the
 * general case). */
static void
update_hw_note (AmbientLightingCtx *ctx, const PnMeshState *state)
{
    guint32  hw;
    gchar   *model;
    gchar   *note;

    if (state == NULL)
    {
        gtk_widget_hide (GTK_WIDGET (ctx->hw_note_label));
        return;
    }

    /* Same resolution the Identity page uses: prefer DeviceMetadata's
     * hw_model, fall back to the owner User record's. */
    hw = state->have_metadata && state->hw_model != 0
            ? state->hw_model : state->owner_hw_model;
    if (hw == 0)
    {
        gtk_widget_hide (GTK_WIDGET (ctx->hw_note_label));
        return;
    }

    model = pn_mesh_format_hw_model (hw);

    switch (pn_mesh_hw_ambient_led (hw))
    {
    case PN_MESH_AMBIENT_LED_RGB:
        note = g_strdup_printf (
                "According to internet sources, this device (%s) has %s, "
                "which the Ambient Lighting module can drive — these colour "
                "settings should have a visible effect. Internet sources can "
                "be wrong.",
                model, pn_mesh_hw_led_note (hw));
        break;
    case PN_MESH_AMBIENT_LED_NONE:
        note = g_strdup_printf (
                "According to internet sources, this device (%s) has %s and "
                "no addressable RGB LED for the Ambient Lighting module to "
                "drive, so these settings probably have no visible effect. "
                "Internet sources can be wrong.",
                model, pn_mesh_hw_led_note (hw));
        break;
    case PN_MESH_AMBIENT_LED_UNKNOWN:
    default:
        note = g_strdup_printf (
                "Whether this device (%s) has an addressable RGB LED the "
                "Ambient Lighting module can drive is not known from internet "
                "sources, which can be wrong.",
                model);
        break;
    }

    gtk_label_set_text (ctx->hw_note_label, note);
    gtk_widget_show (GTK_WIDGET (ctx->hw_note_label));

    g_free (note);
    g_free (model);
}

static void
repaint (GtkWidget *page, const PnMeshState *state)
{
    AmbientLightingCtx *ctx;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_AMBIENT_LIGHTING_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    update_hw_note (ctx, state);

    if (state == NULL || !state->have_ambient_lighting)
    {
        ctx->have_data = FALSE;
        gtk_widget_show (GTK_WIDGET (ctx->unavailable_label));
        gtk_widget_hide (ctx->form_grid);
        gtk_widget_hide (ctx->apply_box);
        set_writing (ctx, FALSE);
        return;
    }

    ctx->have_data = TRUE;
    gtk_widget_hide (GTK_WIDGET (ctx->unavailable_label));
    gtk_widget_show (ctx->form_grid);
    gtk_widget_show (ctx->apply_box);

    gtk_switch_set_active     (ctx->led_state_switch, state->al_led_state);
    gtk_spin_button_set_value (ctx->current_spin,     state->al_current);
    gtk_spin_button_set_value (ctx->red_spin,         state->al_red);
    gtk_spin_button_set_value (ctx->green_spin,       state->al_green);
    gtk_spin_button_set_value (ctx->blue_spin,        state->al_blue);

    set_writing (ctx, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_apply_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget          *page = user_data;
    AmbientLightingCtx *ctx;
    GError             *error = NULL;
    gboolean            ok;

    (void) source;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_AMBIENT_LIGHTING_CTX_QDATA);
    if (ctx == NULL)
    {
        pn_mesh_connection_set_ambient_lighting_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_ambient_lighting_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply Ambient Lighting settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    /* Verify-cycle re-handshake refreshed state in place. */
    repaint (page, pn_mesh_connection_get_state (ctx->connection));
    emit_status (ctx, "Ambient Lighting settings applied.");
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                        *page = user_data;
    AmbientLightingCtx               *ctx;
    PnMeshAmbientLightingConfigWrite  cfg;

    (void) button;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_AMBIENT_LIGHTING_CTX_QDATA);
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.led_state = gtk_switch_get_active (ctx->led_state_switch);
    cfg.current   = (guint32) gtk_spin_button_get_value_as_int (ctx->current_spin);
    cfg.red       = (guint32) gtk_spin_button_get_value_as_int (ctx->red_spin);
    cfg.green     = (guint32) gtk_spin_button_get_value_as_int (ctx->green_spin);
    cfg.blue      = (guint32) gtk_spin_button_get_value_as_int (ctx->blue_spin);

    emit_status (ctx, "Applying Ambient Lighting settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_ambient_lighting_async (
            ctx->connection, &cfg, NULL, on_apply_done, page);
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

    /* Spin buttons share a common width via the size group so the
     * value column lines up.  A switch is naturally narrow and already
     * left-aligned (halign START), so leave it out -- otherwise the
     * size group stretches it to the widest control. */
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
add_row (GtkGrid *grid, gint row, const gchar *label_text,
         const gchar *tooltip)
{
    GtkWidget     *key   = gtk_label_new (label_text);
    PangoAttrList *attrs = pango_attr_list_new ();
    GtkWidget     *cell;

    pango_attr_list_insert (attrs,
                            pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (key), attrs);
    pango_attr_list_unref (attrs);
    gtk_label_set_xalign (GTK_LABEL (key), 0.0);
    gtk_widget_set_margin_end (key, 16);
    if (tooltip != NULL)
        gtk_widget_set_tooltip_text (key, tooltip);
    gtk_grid_attach (grid, key, 0, row, 1, 1);

    cell = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (cell, TRUE);
    gtk_grid_attach (grid, cell, 1, row, 1, 1);
    g_signal_connect (cell, "realize",
                      G_CALLBACK (on_cell_realize),
                      get_value_size_group (grid));
    return cell;
}

static GtkWidget *
make_switch (void)
{
    GtkWidget *sw = gtk_switch_new ();
    gtk_widget_set_halign (sw, GTK_ALIGN_START);
    return sw;
}

GtkWidget *
pn_mesh_page_ambient_lighting_new (void)
{
    AmbientLightingCtx *ctx;
    GtkWidget          *page;
    GtkWidget          *hw_note;
    GtkWidget          *unavailable;
    GtkWidget          *grid;
    GtkWidget          *cell;
    GtkWidget          *w;
    GtkWidget          *apply_box;
    GtkWidget          *apply;
    gint                row = 0;

    ctx = g_slice_new0 (AmbientLightingCtx);

    /* Hosted inside a GtkExpander, so the outer box wears small
     * margins -- the expander body already pads.  The dialog's
     * Notifications tab spaces sibling expanders. */
    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    /* Device-specific LED note -- continues the section's help text.
     * Filled in by update_hw_note() once a device connects; starts
     * hidden so it doesn't show a stale line before the handshake. */
    hw_note = gtk_label_new (NULL);
    gtk_label_set_xalign    (GTK_LABEL (hw_note), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (hw_note), TRUE);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (hw_note);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_widget_set_no_show_all (hw_note, TRUE);
    gtk_box_pack_start (GTK_BOX (page), hw_note, FALSE, FALSE, 0);
    ctx->hw_note_label = GTK_LABEL (hw_note);

    unavailable = gtk_label_new (
            "The device has not reported its Ambient Lighting "
            "configuration. Try reconnecting; if the message persists "
            "this firmware does not expose the module.");
    gtk_label_set_xalign    (GTK_LABEL (unavailable), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (unavailable), TRUE);
    gtk_widget_set_margin_top (unavailable, 6);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (unavailable);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), unavailable, FALSE, FALSE, 0);
    ctx->unavailable_label = GTK_LABEL (unavailable);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_widget_set_margin_top   (grid, 6);
    gtk_box_pack_start (GTK_BOX (page), grid, FALSE, FALSE, 0);
    ctx->form_grid = grid;

    cell = add_row (GTK_GRID (grid), row++, "LED on",
            "Master switch for the addressable status LED. Turn off to "
            "leave the LED dark; the colour and current below are then "
            "ignored.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->led_state_switch = GTK_SWITCH (w);
    /* Colour / current fields below stay sensitive only while the LED
     * is on; flipping it greys them out. */
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_led_state_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Current",
            "Drive level for the LED output (NeoPixel current setting). "
            "Higher is brighter; firmware default is 10.");
    w = pn_device_spin_new_with_range (0, 255, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->current_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "Red",
            "Red colour component, 0-255.");
    w = pn_device_spin_new_with_range (0, 255, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->red_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "Green",
            "Green colour component, 0-255.");
    w = pn_device_spin_new_with_range (0, 255, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->green_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "Blue",
            "Blue colour component, 0-255.");
    w = pn_device_spin_new_with_range (0, 255, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->blue_spin = GTK_SPIN_BUTTON (w);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 12);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = pn_action_button_new ("_Apply Ambient Lighting settings",
                                  PN_ACTION_BUTTON_NORMAL);
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);
    ctx->apply_box = apply_box;

    g_object_set_data_full (G_OBJECT (page),
                            PN_MESH_AMBIENT_LIGHTING_CTX_QDATA,
                            ctx, ambient_lighting_ctx_free);

    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_ambient_lighting_set_state (GtkWidget         *page,
                                         const PnMeshState *state,
                                         PnMeshConnection  *connection)
{
    AmbientLightingCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_AMBIENT_LIGHTING_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;
    repaint (page, state);
}

void
pn_mesh_page_ambient_lighting_set_status_callback (
        GtkWidget                       *page,
        PnMeshAmbientLightingStatusFunc  callback,
        gpointer                         user_data)
{
    AmbientLightingCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_AMBIENT_LIGHTING_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_ambient_lighting_set_busy_callback (
        GtkWidget          *page,
        PnMeshPageBusyFunc  callback,
        gpointer            user_data)
{
    AmbientLightingCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_AMBIENT_LIGHTING_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
