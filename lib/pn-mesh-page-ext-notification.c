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
/*  External Notification page — Phase 9.                              */
/*                                                                     */
/*  Standalone form for ExternalNotificationConfig.  Hosted inside an  */
/*  expander under the dialog's Notifications top tab.  Apply ships    */
/*  the whole sub-block at once (the device replaces it as a unit,    */
/*  so omitting fields would reset them to proto3 zero), then a       */
/*  verify-cycle handshake refreshes the controls from the device's   */
/*  authoritative reply.                                              */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-ext-notification.h"

#define PN_MESH_EXT_NOTIFICATION_CTX_QDATA "pn-mesh-page-ext-notification-ctx"

typedef struct
{
    /* Widget tree handles. */
    GtkSwitch     *enabled_switch;
    GtkSpinButton *nag_timeout_spin;
    GtkSpinButton *output_ms_spin;
    GtkSpinButton *output_spin;
    GtkSpinButton *output_vibra_spin;
    GtkSpinButton *output_buzzer_spin;
    GtkSwitch     *active_switch;
    GtkSwitch     *alert_message_switch;
    GtkSwitch     *alert_message_vibra_switch;
    GtkSwitch     *alert_message_buzzer_switch;
    GtkSwitch     *alert_bell_switch;
    GtkSwitch     *alert_bell_vibra_switch;
    GtkSwitch     *alert_bell_buzzer_switch;
    GtkSwitch     *use_pwm_switch;
    GtkSwitch     *use_i2s_switch;
    GtkButton     *apply_button;

    /* "Not yet streamed" placeholder shown until the device sends an
     * ExternalNotificationConfig block.  The form grid hides while
     * the placeholder is visible so the user sees one or the other. */
    GtkLabel      *unavailable_label;
    GtkWidget     *form_grid;
    GtkWidget     *apply_box;

    /* Borrowed; dialog owns it. */
    PnMeshConnection *connection;

    gboolean       writing;
    gboolean       have_data;

    PnMeshExtNotificationStatusFunc status_cb;
    gpointer                        status_ud;

    PnMeshPageBusyFunc              busy_cb;
    gpointer                        busy_ud;
} ExtNotificationCtx;

static void
ext_notification_ctx_free (gpointer data)
{
    g_slice_free (ExtNotificationCtx, data);
}

static void
emit_status (ExtNotificationCtx *ctx, const gchar *msg)
{
    if (ctx->status_cb != NULL)
        ctx->status_cb (msg, ctx->status_ud);
}

/* ------------------------------------------------------------------ */
/*  Sensitivity                                                         */
/* ------------------------------------------------------------------ */

static void
set_writing (ExtNotificationCtx *ctx, gboolean writing)
{
    /* "Form is editable at all": needs a live connection, a streamed
     * config block, and no in-flight write.  The Enabled toggle and
     * Apply both follow this gate. */
    gboolean form_enable = !writing && ctx->connection != NULL && ctx->have_data;
    /* "Detail fields are meaningful": the module also has to be ON.
     * When the user turns Enabled off the device ignores the rest of
     * the block, so paint those fields disabled to match. */
    gboolean detail_enable = form_enable
            && gtk_switch_get_active (ctx->enabled_switch);
    gboolean transition = (ctx->writing != writing);

    ctx->writing = writing;

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->enabled_switch),              form_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->apply_button),                form_enable);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->nag_timeout_spin),            detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->output_ms_spin),              detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->output_spin),                 detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->output_vibra_spin),           detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->output_buzzer_spin),          detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->active_switch),               detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->alert_message_switch),        detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->alert_message_vibra_switch),  detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->alert_message_buzzer_switch), detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->alert_bell_switch),           detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->alert_bell_vibra_switch),     detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->alert_bell_buzzer_switch),    detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->use_pwm_switch),              detail_enable);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->use_i2s_switch),              detail_enable);

    if (transition && ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_ud);
}

/* Re-run the sensitivity calc when the Enabled toggle flips, so the
 * detail fields visibly follow.  We don't enter the writing state
 * here -- this is a pure UI re-evaluation, no I/O. */
static void
on_enabled_switch_toggled (GObject *src, GParamSpec *pspec, gpointer user_data)
{
    ExtNotificationCtx *ctx = user_data;

    (void) src;
    (void) pspec;

    set_writing (ctx, ctx->writing);
}

/* ------------------------------------------------------------------ */
/*  Paint                                                               */
/* ------------------------------------------------------------------ */

static void
repaint (GtkWidget *page, const PnMeshState *state)
{
    ExtNotificationCtx *ctx;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_EXT_NOTIFICATION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    if (state == NULL || !state->have_ext_notification)
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

    gtk_switch_set_active (ctx->enabled_switch,              state->en_enabled);
    gtk_spin_button_set_value (ctx->nag_timeout_spin,        state->en_nag_timeout);
    gtk_spin_button_set_value (ctx->output_ms_spin,          state->en_output_ms);
    gtk_spin_button_set_value (ctx->output_spin,             state->en_output);
    gtk_spin_button_set_value (ctx->output_vibra_spin,       state->en_output_vibra);
    gtk_spin_button_set_value (ctx->output_buzzer_spin,      state->en_output_buzzer);
    gtk_switch_set_active (ctx->active_switch,               state->en_active);
    gtk_switch_set_active (ctx->alert_message_switch,        state->en_alert_message);
    gtk_switch_set_active (ctx->alert_message_vibra_switch,  state->en_alert_message_vibra);
    gtk_switch_set_active (ctx->alert_message_buzzer_switch, state->en_alert_message_buzzer);
    gtk_switch_set_active (ctx->alert_bell_switch,           state->en_alert_bell);
    gtk_switch_set_active (ctx->alert_bell_vibra_switch,     state->en_alert_bell_vibra);
    gtk_switch_set_active (ctx->alert_bell_buzzer_switch,    state->en_alert_bell_buzzer);
    gtk_switch_set_active (ctx->use_pwm_switch,              state->en_use_pwm);
    gtk_switch_set_active (ctx->use_i2s_switch,              state->en_use_i2s_as_buzzer);

    set_writing (ctx, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Apply                                                               */
/* ------------------------------------------------------------------ */

static void
on_apply_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkWidget          *page = user_data;
    ExtNotificationCtx *ctx;
    GError             *error = NULL;
    gboolean            ok;

    (void) source;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_EXT_NOTIFICATION_CTX_QDATA);
    if (ctx == NULL)
    {
        pn_mesh_connection_set_ext_notification_finish (res, &error);
        g_clear_error (&error);
        return;
    }

    ok = pn_mesh_connection_set_ext_notification_finish (res, &error);
    if (!ok)
    {
        gchar *msg = g_strdup_printf (
                "Could not apply External Notification settings: %s",
                error != NULL ? error->message : "(unknown error)");
        emit_status (ctx, msg);
        g_free (msg);
        g_clear_error (&error);
        set_writing (ctx, FALSE);
        return;
    }

    /* Verify-cycle re-handshake refreshed state in place. */
    repaint (page, pn_mesh_connection_get_state (ctx->connection));
    emit_status (ctx, "External Notification settings applied.");
}

static void
on_apply_clicked (GtkButton *button, gpointer user_data)
{
    GtkWidget                  *page = user_data;
    ExtNotificationCtx         *ctx;
    PnMeshExtNotificationWrite  cfg;

    (void) button;

    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_EXT_NOTIFICATION_CTX_QDATA);
    if (ctx == NULL || ctx->connection == NULL || ctx->writing)
        return;

    cfg.enabled              = gtk_switch_get_active (ctx->enabled_switch);
    cfg.nag_timeout          = (guint32) gtk_spin_button_get_value_as_int (ctx->nag_timeout_spin);
    cfg.output_ms            = (guint32) gtk_spin_button_get_value_as_int (ctx->output_ms_spin);
    cfg.output               = (guint32) gtk_spin_button_get_value_as_int (ctx->output_spin);
    cfg.output_vibra         = (guint32) gtk_spin_button_get_value_as_int (ctx->output_vibra_spin);
    cfg.output_buzzer        = (guint32) gtk_spin_button_get_value_as_int (ctx->output_buzzer_spin);
    cfg.active               = gtk_switch_get_active (ctx->active_switch);
    cfg.alert_message        = gtk_switch_get_active (ctx->alert_message_switch);
    cfg.alert_message_vibra  = gtk_switch_get_active (ctx->alert_message_vibra_switch);
    cfg.alert_message_buzzer = gtk_switch_get_active (ctx->alert_message_buzzer_switch);
    cfg.alert_bell           = gtk_switch_get_active (ctx->alert_bell_switch);
    cfg.alert_bell_vibra     = gtk_switch_get_active (ctx->alert_bell_vibra_switch);
    cfg.alert_bell_buzzer    = gtk_switch_get_active (ctx->alert_bell_buzzer_switch);
    cfg.use_pwm              = gtk_switch_get_active (ctx->use_pwm_switch);
    cfg.use_i2s_as_buzzer    = gtk_switch_get_active (ctx->use_i2s_switch);

    emit_status (ctx, "Applying External Notification settings…");
    set_writing (ctx, TRUE);
    pn_mesh_connection_set_ext_notification_async (
            ctx->connection, &cfg, NULL, on_apply_done, page);
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

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
pn_mesh_page_ext_notification_new (void)
{
    ExtNotificationCtx *ctx;
    GtkWidget          *page;
    GtkWidget          *subtitle;
    GtkWidget          *unavailable;
    GtkWidget          *grid;
    GtkWidget          *cell;
    GtkWidget          *w;
    GtkWidget          *apply_box;
    GtkWidget          *apply;
    gint                row = 0;

    ctx = g_slice_new0 (ExtNotificationCtx);

    /* Hosted inside a GtkExpander, so the outer box wears small
     * margins -- the expander body already pads.  The dialog's
     * Notifications tab spaces sibling expanders. */
    page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start  (page, 12);
    gtk_widget_set_margin_end    (page, 12);
    gtk_widget_set_margin_top    (page, 6);
    gtk_widget_set_margin_bottom (page, 6);

    subtitle = gtk_label_new (
            "Drives the device's onboard LED / buzzer / vibration "
            "motor on incoming traffic.  Set Nag timeout to 0 to "
            "stop the device repeating notifications.");
    gtk_label_set_xalign      (GTK_LABEL (subtitle), 0.0);
    gtk_label_set_line_wrap   (GTK_LABEL (subtitle), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (subtitle), 72);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (subtitle);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (page), subtitle, FALSE, FALSE, 0);

    unavailable = gtk_label_new (
            "The device has not reported its External Notification "
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

    cell = add_row (GTK_GRID (grid), row++, "Enabled",
            "Master switch for the module. Turn off to silence the "
            "device entirely.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->enabled_switch = GTK_SWITCH (w);
    /* Detail fields below stay sensitive only while Enabled is on;
     * flipping it greys them out (and re-paints them sensitive when
     * the user turns Enabled back on). */
    g_signal_connect (w, "notify::active",
                      G_CALLBACK (on_enabled_switch_toggled), ctx);

    cell = add_row (GTK_GRID (grid), row++, "Nag timeout",
            "Seconds to keep repeating the notification. 0 fires the "
            "notification once and stops -- the fix for a device that "
            "won't stop beeping.");
    w = gtk_spin_button_new_with_range (0, 3600, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    {
        GtkWidget *suffix = gtk_label_new (" s");
        GtkStyleContext *sc = gtk_widget_get_style_context (suffix);
        gtk_style_context_add_class (sc, "dim-label");
        gtk_box_pack_start (GTK_BOX (cell), suffix, FALSE, FALSE, 0);
    }
    ctx->nag_timeout_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "Output duration",
            "Length of each on/off cycle on the output pin.");
    w = gtk_spin_button_new_with_range (0, 600000, 100);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    {
        GtkWidget *suffix = gtk_label_new (" ms");
        GtkStyleContext *sc = gtk_widget_get_style_context (suffix);
        gtk_style_context_add_class (sc, "dim-label");
        gtk_box_pack_start (GTK_BOX (cell), suffix, FALSE, FALSE, 0);
    }
    ctx->output_ms_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "Active high",
            "When on, the pin idles low and pulses high on alert. "
            "Off swaps the polarity. Wrong polarity = always-on output.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->active_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Output GPIO (LED)",
            "Primary GPIO pin driven on alert. 0 = no LED output.");
    w = gtk_spin_button_new_with_range (0, 64, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->output_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "Vibra GPIO",
            "Secondary GPIO pin for a vibration motor. 0 = none.");
    w = gtk_spin_button_new_with_range (0, 64, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->output_vibra_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "Buzzer GPIO",
            "Third GPIO pin for a buzzer. 0 = none.");
    w = gtk_spin_button_new_with_range (0, 64, 1);
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->output_buzzer_spin = GTK_SPIN_BUTTON (w);

    cell = add_row (GTK_GRID (grid), row++, "Alert on message (LED)",
            "Pulse the LED on any incoming text message.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->alert_message_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Alert on message (vibra)",
            "Pulse the vibration motor on any incoming text message.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->alert_message_vibra_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Alert on message (buzzer)",
            "Sound the buzzer on any incoming text message.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->alert_message_buzzer_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Alert on bell (LED)",
            "Pulse the LED on a bell character (\\007) inside a message.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->alert_bell_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Alert on bell (vibra)",
            "Pulse the vibration motor on a bell character.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->alert_bell_vibra_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Alert on bell (buzzer)",
            "Sound the buzzer on a bell character.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->alert_bell_buzzer_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "PWM output",
            "Drive the output pin with PWM rather than simple on/off "
            "-- useful for analog buzzers tuned to a specific tone.");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->use_pwm_switch = GTK_SWITCH (w);

    cell = add_row (GTK_GRID (grid), row++, "Use I²S as buzzer",
            "Re-purpose the I²S audio output as a buzzer driver "
            "(devices with an I²S DAC but no dedicated buzzer pin).");
    w = make_switch ();
    gtk_box_pack_start (GTK_BOX (cell), w, FALSE, FALSE, 0);
    ctx->use_i2s_switch = GTK_SWITCH (w);

    apply_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top (apply_box, 12);
    gtk_widget_set_halign     (apply_box, GTK_ALIGN_END);

    apply = gtk_button_new_with_mnemonic ("_Apply");
    gtk_widget_set_tooltip_text (apply,
            "Send the values above to the device.  The current "
            "values are read back to confirm the change took.");
    gtk_widget_set_sensitive (apply, FALSE);
    gtk_box_pack_start (GTK_BOX (apply_box), apply, FALSE, FALSE, 0);
    ctx->apply_button = GTK_BUTTON (apply);
    gtk_box_pack_start (GTK_BOX (page), apply_box, FALSE, FALSE, 0);
    ctx->apply_box = apply_box;

    g_object_set_data_full (G_OBJECT (page),
                            PN_MESH_EXT_NOTIFICATION_CTX_QDATA,
                            ctx, ext_notification_ctx_free);

    g_signal_connect (apply, "clicked",
                      G_CALLBACK (on_apply_clicked), page);

    return page;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_ext_notification_set_state (GtkWidget         *page,
                                         const PnMeshState *state,
                                         PnMeshConnection  *connection)
{
    ExtNotificationCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_EXT_NOTIFICATION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->connection = connection;
    repaint (page, state);
}

void
pn_mesh_page_ext_notification_set_status_callback (
        GtkWidget                       *page,
        PnMeshExtNotificationStatusFunc  callback,
        gpointer                         user_data)
{
    ExtNotificationCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_EXT_NOTIFICATION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->status_cb = callback;
    ctx->status_ud = user_data;
}

void
pn_mesh_page_ext_notification_set_busy_callback (
        GtkWidget          *page,
        PnMeshPageBusyFunc  callback,
        gpointer            user_data)
{
    ExtNotificationCtx *ctx;

    g_return_if_fail (GTK_IS_WIDGET (page));
    ctx = g_object_get_data (G_OBJECT (page),
                             PN_MESH_EXT_NOTIFICATION_CTX_QDATA);
    g_return_if_fail (ctx != NULL);

    ctx->busy_cb = callback;
    ctx->busy_ud = user_data;
}
