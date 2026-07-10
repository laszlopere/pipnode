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
/*  PnDailyTimer — gui tier.                                           */
/*                                                                     */
/*  The settings-dialog customisation for the Daily Timer node.  The   */
/*  node's GType, the "schedule" string property, the compiled         */
/*  interval cache and the poll-and-emit-on-edge trigger live in the   */
/*  GTK-free core file pn-daily-timer.c; this file installs the        */
/*  build_class_tab vfunc onto that class at editor startup            */
/*  (pn_daily_timer_gui_install).  The per-interval row editor reads   */
/*  and writes the node's "schedule" property via g_object_get /       */
/*  g_object_set, so it needs no core seam -- the same shape as the    */
/*  Set and Filter rule editors.                                       */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-daily-timer-gui.h"
#include "pn-daily-timer.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

/* Combo id for the "Every day" row, kept out of the 1..7 range the
 * core uses for real weekdays. */
#define PN_DAILY_TIMER_ANY_ID  "any"

typedef struct _PnDailyTimerBinding PnDailyTimerBinding;

typedef struct
{
    PnDailyTimerBinding *binding;    /* borrowed */
    GtkWidget           *row_box;    /* outer GtkBox for this row */
    GtkComboBox         *day_combo;
    GtkSpinButton       *on_hour;
    GtkSpinButton       *on_minute;
    GtkSpinButton       *off_hour;
    GtkSpinButton       *off_minute;
    gboolean             building;   /* skip per-widget signals */
} PnDailyTimerRow;

struct _PnDailyTimerBinding
{
    GObject  *target;                /* borrowed PnDailyTimer */
    GtkBox   *list;                  /* vbox of PnDailyTimerRow.row_box */
    gulong    notify_handler;
    gboolean  updating;
};

static void       dt_serialise             (PnDailyTimerBinding *bind);
static void       dt_rebuild_from_property (PnDailyTimerBinding *bind);
static GtkWidget *dt_build_row             (PnDailyTimerBinding *bind,
                                            gint                 day,
                                            gint                 on_h,
                                            gint                 on_m,
                                            gint                 off_h,
                                            gint                 off_m);

static void
dt_binding_free (gpointer data)
{
    PnDailyTimerBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

/* ------------------------------------------------------------------ */
/*  Row widgets                                                        */
/* ------------------------------------------------------------------ */

static void
dt_row_emit_change (PnDailyTimerRow *row)
{
    if (row->building || row->binding->updating)
        return;

    dt_serialise (row->binding);
}

static void
on_dt_row_changed (GtkWidget *w, gpointer user_data)
{
    (void) w;
    dt_row_emit_change (user_data);
}

/* Render 7 as "07".  A schedule reads as a column of times, and a
 * ragged "7:5" / "22:30" mix is markedly harder to scan than the
 * zero-padded clock notation the times are written in everywhere else
 * in the dialog. */
static gboolean
on_dt_spin_output (GtkSpinButton *spin, gpointer user_data)
{
    gchar *text;

    (void) user_data;

    text = g_strdup_printf ("%02d", gtk_spin_button_get_value_as_int (spin));
    gtk_entry_set_text (GTK_ENTRY (spin), text);
    g_free (text);

    return TRUE;
}

static GtkWidget *
dt_make_time_spin (PnDailyTimerRow *row, gint max, gint value)
{
    GtkWidget *spin = gtk_spin_button_new_with_range (0, max, 1);

    gtk_spin_button_set_numeric (GTK_SPIN_BUTTON (spin), TRUE);
    /* Wrapping turns the hour field into a clock: stepping down from 00
     * lands on 23 rather than sticking at the floor. */
    gtk_spin_button_set_wrap  (GTK_SPIN_BUTTON (spin), TRUE);
    gtk_spin_button_set_value (GTK_SPIN_BUTTON (spin), value);
    gtk_entry_set_width_chars (GTK_ENTRY (spin), 2);

    g_signal_connect (spin, "output",
                      G_CALLBACK (on_dt_spin_output), NULL);
    g_signal_connect (spin, "value-changed",
                      G_CALLBACK (on_dt_row_changed), row);

    return spin;
}

static void
on_dt_remove_clicked (GtkButton *btn, gpointer user_data)
{
    PnDailyTimerRow     *row  = user_data;
    PnDailyTimerBinding *bind = row->binding;

    (void) btn;

    gtk_widget_destroy (row->row_box);

    if (!bind->updating)
        dt_serialise (bind);
}

static void
on_dt_add_clicked (GtkButton *btn, gpointer user_data)
{
    PnDailyTimerBinding *bind = user_data;
    GtkWidget           *row;

    (void) btn;

    /* A fresh row defaults to every day, 07:00-09:00: a plausible
     * morning heating window, so the common case is two spin-button
     * edits rather than five. */
    row = dt_build_row (bind, -1, 7, 0, 9, 0);
    gtk_box_pack_start (bind->list, row, FALSE, FALSE, 0);
    gtk_widget_show_all (row);

    if (!bind->updating)
        dt_serialise (bind);
}

static GtkWidget *
dt_build_row (
        PnDailyTimerBinding *bind,
        gint                 day,
        gint                 on_h,
        gint                 on_m,
        gint                 off_h,
        gint                 off_m)
{
    PnDailyTimerRow *row = g_new0 (PnDailyTimerRow, 1);
    GtkWidget       *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget       *day_combo;
    GtkWidget       *on_hour;
    GtkWidget       *on_minute;
    GtkWidget       *off_hour;
    GtkWidget       *off_minute;
    GtkWidget       *remove_btn;
    static const gchar * const day_names[7] = {
        "Monday", "Tuesday", "Wednesday", "Thursday",
        "Friday", "Saturday", "Sunday"
    };
    gint i;

    row->binding  = bind;
    row->row_box  = box;
    row->building = TRUE;

    /* --- Day combo ----------------------------------------------- */
    day_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (day_combo),
                               PN_DAILY_TIMER_ANY_ID, "Every day");
    for (i = 0; i < 7; i++)
    {
        gchar id[2] = { (gchar) ('1' + i), '\0' };
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (day_combo),
                                   id, day_names[i]);
    }
    row->day_combo = GTK_COMBO_BOX (day_combo);

    /* --- Times ---------------------------------------------------- */
    on_hour    = dt_make_time_spin (row, 23, on_h);
    on_minute  = dt_make_time_spin (row, 59, on_m);
    off_hour   = dt_make_time_spin (row, 23, off_h);
    off_minute = dt_make_time_spin (row, 59, off_m);

    row->on_hour    = GTK_SPIN_BUTTON (on_hour);
    row->on_minute  = GTK_SPIN_BUTTON (on_minute);
    row->off_hour   = GTK_SPIN_BUTTON (off_hour);
    row->off_minute = GTK_SPIN_BUTTON (off_minute);

    remove_btn = gtk_button_new_from_icon_name ("list-remove-symbolic",
                                                GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (remove_btn, "Remove this interval");

    gtk_box_pack_start (GTK_BOX (box), day_combo,            FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), gtk_label_new ("on"), FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), on_hour,              FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), gtk_label_new (":"),  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), on_minute,            FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), gtk_label_new ("off"), FALSE, FALSE, 6);
    gtk_box_pack_start (GTK_BOX (box), off_hour,             FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), gtk_label_new (":"),  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), off_minute,           FALSE, FALSE, 0);
    gtk_box_pack_end   (GTK_BOX (box), remove_btn,           FALSE, FALSE, 0);

    if (day >= 1 && day <= 7)
    {
        gchar id[2] = { (gchar) ('0' + day), '\0' };
        gtk_combo_box_set_active_id (row->day_combo, id);
    }
    else
    {
        gtk_combo_box_set_active_id (row->day_combo, PN_DAILY_TIMER_ANY_ID);
    }

    g_object_set_data_full (G_OBJECT (box),
                            "pn-daily-timer-row", row, g_free);

    g_signal_connect (day_combo,  "changed",
                      G_CALLBACK (on_dt_row_changed), row);
    g_signal_connect (remove_btn, "clicked",
                      G_CALLBACK (on_dt_remove_clicked), row);

    row->building = FALSE;
    return box;
}

/* ------------------------------------------------------------------ */
/*  Serialise / rebuild                                                */
/* ------------------------------------------------------------------ */

static gint
dt_row_get_day (PnDailyTimerRow *row)
{
    const gchar *id = gtk_combo_box_get_active_id (row->day_combo);

    if (id == NULL || g_strcmp0 (id, PN_DAILY_TIMER_ANY_ID) == 0)
        return -1;

    return (gint) g_ascii_strtoll (id, NULL, 10);
}

static void
dt_serialise (PnDailyTimerBinding *bind)
{
    JsonArray *arr = json_array_new ();
    JsonNode  *root;
    GList     *children;
    GList     *iter;
    gchar     *json_string;

    if (bind->updating)
    {
        json_array_unref (arr);
        return;
    }

    children = gtk_container_get_children (GTK_CONTAINER (bind->list));
    for (iter = children; iter != NULL; iter = iter->next)
    {
        GtkWidget       *row_box = iter->data;
        PnDailyTimerRow *row     = g_object_get_data (G_OBJECT (row_box),
                                                      "pn-daily-timer-row");
        JsonObject      *obj;

        if (row == NULL)
            continue;

        obj = json_object_new ();
        json_object_set_int_member (obj, "day", dt_row_get_day (row));
        json_object_set_int_member (
                obj, "on_hour",
                gtk_spin_button_get_value_as_int (row->on_hour));
        json_object_set_int_member (
                obj, "on_minute",
                gtk_spin_button_get_value_as_int (row->on_minute));
        json_object_set_int_member (
                obj, "off_hour",
                gtk_spin_button_get_value_as_int (row->off_hour));
        json_object_set_int_member (
                obj, "off_minute",
                gtk_spin_button_get_value_as_int (row->off_minute));

        json_array_add_object_element (arr, obj);
    }
    g_list_free (children);

    root = json_node_new (JSON_NODE_ARRAY);
    json_node_take_array (root, arr);
    json_string = json_to_string (root, FALSE);
    json_node_unref (root);

    bind->updating = TRUE;
    g_object_set (bind->target, "schedule", json_string, NULL);
    bind->updating = FALSE;

    g_free (json_string);
}

static void
dt_clear_rows (PnDailyTimerBinding *bind)
{
    GList *children = gtk_container_get_children (GTK_CONTAINER (bind->list));
    GList *iter;

    for (iter = children; iter != NULL; iter = iter->next)
        gtk_widget_destroy (GTK_WIDGET (iter->data));

    g_list_free (children);
}

static gint
dt_object_get_int (JsonObject *obj, const gchar *member)
{
    return json_object_has_member (obj, member)
               ? (gint) json_object_get_int_member (obj, member)
               : 0;
}

static void
dt_rebuild_from_property (PnDailyTimerBinding *bind)
{
    gchar      *json_string = NULL;
    JsonParser *parser;

    if (bind->updating)
        return;

    g_object_get (bind->target, "schedule", &json_string, NULL);

    bind->updating = TRUE;
    dt_clear_rows (bind);

    parser = json_parser_new ();
    if (json_string != NULL && *json_string != '\0' &&
        json_parser_load_from_data (parser, json_string, -1, NULL))
    {
        JsonNode *root = json_parser_get_root (parser);

        if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
        {
            JsonArray *arr = json_node_get_array (root);
            guint      n   = json_array_get_length (arr);
            guint      i;

            for (i = 0; i < n; i++)
            {
                JsonNode   *item = json_array_get_element (arr, i);
                JsonObject *obj;
                GtkWidget  *row;

                if (item == NULL || !JSON_NODE_HOLDS_OBJECT (item))
                    continue;

                obj = json_node_get_object (item);
                row = dt_build_row (bind,
                                    json_object_has_member (obj, "day")
                                        ? (gint) json_object_get_int_member (
                                              obj, "day")
                                        : -1,
                                    dt_object_get_int (obj, "on_hour"),
                                    dt_object_get_int (obj, "on_minute"),
                                    dt_object_get_int (obj, "off_hour"),
                                    dt_object_get_int (obj, "off_minute"));
                gtk_box_pack_start (bind->list, row, FALSE, FALSE, 0);
                gtk_widget_show_all (row);
            }
        }
    }
    g_object_unref (parser);
    g_free (json_string);

    bind->updating = FALSE;
}

static void
on_dt_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    dt_rebuild_from_property (user_data);
}

/* ------------------------------------------------------------------ */
/*  PnNodeClass.build_class_tab override                               */
/* ------------------------------------------------------------------ */

static GtkWidget *
pn_daily_timer_build_class_tab (
        PnNode    *self,
        GtkWindow *parent G_GNUC_UNUSED)
{
    GObject             *target   = G_OBJECT (self);
    GtkWidget           *outer    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget           *hint;
    GtkWidget           *scrolled = gtk_scrolled_window_new (NULL, NULL);
    GtkWidget           *list     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget           *add_btn  = gtk_button_new_with_label ("Add interval");
    GtkWidget           *btn_row  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    PnDailyTimerBinding *bind;

    g_object_set (outer,
                  "margin-start",  12,
                  "margin-end",    12,
                  "margin-top",    12,
                  "margin-bottom", 12,
                  NULL);

    /* The midnight-wrap rule is not discoverable from the widgets --
     * nothing about two pairs of spin buttons says what 22:00-06:00
     * means -- so it is spelled out where the user is typing it. */
    hint = gtk_label_new ("Sends value = 1 when the time enters an "
                          "interval, 0 when it leaves.\n"
                          "An off time at or before the on time runs "
                          "past midnight into the next day.");
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_set_sensitive (hint, FALSE);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    gtk_widget_set_size_request (scrolled, -1, 200);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);

    gtk_container_add (GTK_CONTAINER (scrolled), list);

    gtk_box_pack_start (GTK_BOX (outer),   hint,     FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (outer),   scrolled, TRUE,  TRUE,  0);

    gtk_widget_set_halign (add_btn, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (btn_row), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (outer),   btn_row, FALSE, FALSE, 0);

    bind = g_new0 (PnDailyTimerBinding, 1);
    bind->target = target;
    bind->list   = GTK_BOX (list);

    bind->notify_handler = g_signal_connect (
            target, "notify::schedule",
            G_CALLBACK (on_dt_target_notify), bind);

    g_signal_connect (add_btn, "clicked",
                      G_CALLBACK (on_dt_add_clicked), bind);

    g_object_set_data_full (G_OBJECT (outer),
                            "pn-daily-timer-binding",
                            bind, dt_binding_free);

    dt_rebuild_from_property (bind);

    return outer;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_daily_timer_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_DAILY_TIMER));

    node_class->build_class_tab = pn_daily_timer_build_class_tab;

    /* The class ref is intentionally held for the process lifetime --
     * the same lifetime the factory keeps it alive for -- so the slot
     * we just wrote stays valid. */
}
