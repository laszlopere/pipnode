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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-widget-mirror.h"

#include <json-glib/json-glib.h>

#include "pn-injector-widget.h"
#include "pn-led-display.h"
#include "pn-led-lamp.h"
#include "pn-matrix57-display.h"
#include "pn-numeric-display.h"
#include "pn-switch-widget.h"
#include "pn-text-display.h"

/* ------------------------------------------------------------------ */
/*  One mirrored widget                                                 */
/*                                                                     */
/*  @kind is the single-letter code for the node kind behind it:        */
/*    'c' Countdown / Digital Clock -> PnLedDisplay                     */
/*    'l' LED                       -> PnLedLamp                        */
/*    's' Switch                    -> PnSwitchWidget    (interactive)  */
/*    'i' Inject                    -> PnInjectorWidget  (interactive)  */
/*    't' Label                     -> PnTextDisplay                    */
/*    'm' Matrix 5x7                -> PnMatrix57Display                */
/*    'n' Numeric                   -> PnNumericDisplay                 */
/*                                                                     */
/*  The two interactive kinds carry their node UUID as widget data so a */
/*  click can be reported back to the engine by node, not by position.  */
/*  The GtkWidget itself is owned by the mirror's canvas; this struct    */
/*  only tracks it.                                                     */
/* ------------------------------------------------------------------ */
typedef struct
{
    GtkWidget *widget;
    gchar      kind;
    gdouble    x;
    gdouble    y;
} MirrorWidget;

struct _PnWidgetMirror
{
    GtkFixed                   *canvas;      /* borrowed                 */
    PnWidgetMirrorActivateFunc  activate;
    gpointer                    user_data;

    GHashTable *widgets;   /* node UUID (owned) -> MirrorWidget*         */
    GPtrArray  *order;     /* MirrorWidget* (borrowed), layout order     */
};

static void
mirror_widget_free (gpointer data)
{
    /* The GtkWidget is owned by the canvas and torn down with it; free
     * only the tracking struct. */
    g_slice_free (MirrorWidget, data);
}

/* ------------------------------------------------------------------ */
/*  Reading the engine's state JSON                                    */
/* ------------------------------------------------------------------ */

/* Read an [r, g, b] JSON array member into @rgb.  Returns FALSE when the
 * member is missing or malformed, so the caller keeps the widget default. */
static gboolean
read_rgb (JsonObject *obj, const gchar *member, gdouble rgb[3])
{
    JsonArray *arr;

    if (obj == NULL || !json_object_has_member (obj, member))
        return FALSE;

    arr = json_object_get_array_member (obj, member);
    if (arr == NULL || json_array_get_length (arr) < 3)
        return FALSE;

    rgb[0] = json_array_get_double_element (arr, 0);
    rgb[1] = json_array_get_double_element (arr, 1);
    rgb[2] = json_array_get_double_element (arr, 2);
    return TRUE;
}

/* Read an [r, g, b, a] JSON array member into @rgba.  Returns FALSE when
 * the member is missing or malformed, so the caller keeps the default. */
static gboolean
read_rgba (JsonObject *obj, const gchar *member, gdouble rgba[4])
{
    JsonArray *arr;

    if (obj == NULL || !json_object_has_member (obj, member))
        return FALSE;

    arr = json_object_get_array_member (obj, member);
    if (arr == NULL || json_array_get_length (arr) < 4)
        return FALSE;

    rgba[0] = json_array_get_double_element (arr, 0);
    rgba[1] = json_array_get_double_element (arr, 1);
    rgba[2] = json_array_get_double_element (arr, 2);
    rgba[3] = json_array_get_double_element (arr, 3);
    return TRUE;
}

/* Read either an [r, g, b] or [r, g, b, a] member into @rgba, defaulting
 * the alpha to 1.0 when only three components are present.  Older layouts
 * persisted only RGB for the seven-segment colours, so this keeps them
 * loading without forcing every caller to migrate their state JSON. */
static gboolean
read_rgb_or_rgba (JsonObject *obj, const gchar *member, gdouble rgba[4])
{
    JsonArray *arr;
    guint      n;

    if (obj == NULL || !json_object_has_member (obj, member))
        return FALSE;

    arr = json_object_get_array_member (obj, member);
    if (arr == NULL)
        return FALSE;
    n = json_array_get_length (arr);
    if (n < 3)
        return FALSE;

    rgba[0] = json_array_get_double_element (arr, 0);
    rgba[1] = json_array_get_double_element (arr, 1);
    rgba[2] = json_array_get_double_element (arr, 2);
    rgba[3] = n >= 4 ? json_array_get_double_element (arr, 3) : 1.0;
    return TRUE;
}

/* Push a node's render state (the WidgetChanged payload, or a layout
 * widget's inline "state") into its widget. */
static void
apply_widget_state (MirrorWidget *e, JsonObject *state)
{
    gdouble rgb[3];

    if (state == NULL)
        return;

    if (e->kind == 'c')
    {
        PnLedDisplay *led = PN_LED_DISPLAY (e->widget);
        gdouble       rgba[4];

        if (json_object_has_member (state, "seconds"))
            pn_led_display_set_seconds (
                    led, json_object_get_int_member (state, "seconds"));
        if (json_object_has_member (state, "day_digits"))
            pn_led_display_set_day_digits (
                    led, (guint) json_object_get_int_member (state,
                                                             "day_digits"));
        /* RGBA — alpha lets the unlit segments fade into the panel's
         * transparent background instead of painting a solid ghost. */
        if (read_rgb_or_rgba (state, "segment_color", rgba))
            pn_led_display_set_segment_color (led, rgba[0], rgba[1], rgba[2],
                                              rgba[3]);
        if (read_rgb_or_rgba (state, "unlit_color", rgba))
            pn_led_display_set_unlit_color (led, rgba[0], rgba[1], rgba[2],
                                            rgba[3]);
    }
    else if (e->kind == 's')
    {
        PnSwitchWidget *toggle = PN_SWITCH_WIDGET (e->widget);

        if (json_object_has_member (state, "on"))
            pn_switch_widget_set_on (
                    toggle, json_object_get_boolean_member (state, "on"));
    }
    else if (e->kind == 'i')
    {
        PnInjectorWidget *button = PN_INJECTOR_WIDGET (e->widget);

        if (json_object_has_member (state, "icon"))
            pn_injector_widget_set_icon (
                    button, json_object_get_string_member (state, "icon"));
    }
    else if (e->kind == 't')
    {
        PnTextDisplay *text = PN_TEXT_DISPLAY (e->widget);
        gdouble        rgba[4];

        if (json_object_has_member (state, "text"))
            pn_text_display_set_text (
                    text, json_object_get_string_member (state, "text"));
        if (json_object_has_member (state, "lines"))
            pn_text_display_set_lines (
                    text, (gint) json_object_get_int_member (state, "lines"));
        if (json_object_has_member (state, "font")
            || json_object_has_member (state, "scale")
            || json_object_has_member (state, "weight")
            || json_object_has_member (state, "italic"))
        {
            const gchar *family = json_object_has_member (state, "font")
                    ? json_object_get_string_member (state, "font") : "";
            gint scale = json_object_has_member (state, "scale")
                    ? (gint) json_object_get_int_member (state, "scale")
                    : 100;
            gint weight = json_object_has_member (state, "weight")
                    ? (gint) json_object_get_int_member (state, "weight")
                    : 400;
            gboolean italic = json_object_has_member (state, "italic")
                    && json_object_get_boolean_member (state, "italic");
            pn_text_display_set_font (text, family, scale, weight, italic);
        }
        if (json_object_has_member (state, "align"))
            pn_text_display_set_align (
                    text, (gint) json_object_get_int_member (state, "align"));
        if (read_rgb (state, "color", rgb))
            pn_text_display_set_color (text, rgb[0], rgb[1], rgb[2]);
        if (read_rgba (state, "bg", rgba))
            pn_text_display_set_background (text, rgba[0], rgba[1],
                                           rgba[2], rgba[3]);
    }
    else if (e->kind == 'n')
    {
        PnNumericDisplay *display = PN_NUMERIC_DISPLAY (e->widget);
        gdouble           rgba[4];

        if (json_object_has_member (state, "digits"))
            pn_numeric_display_set_digits (
                    display, (guint) json_object_get_int_member (state, "digits"));
        if (json_object_has_member (state, "decimal_places"))
            pn_numeric_display_set_decimal_places (
                    display,
                    (guint) json_object_get_int_member (state, "decimal_places"));
        /* set_value latches has_value to TRUE, so apply value first and
         * let the explicit has_value flag flip it back when there is no
         * reading yet (the pre-first-message blank screen). */
        if (json_object_has_member (state, "value"))
            pn_numeric_display_set_value (
                    display, json_object_get_double_member (state, "value"));
        if (json_object_has_member (state, "has_value"))
            pn_numeric_display_set_has_value (
                    display,
                    json_object_get_boolean_member (state, "has_value"));
        if (read_rgb_or_rgba (state, "segment_color", rgba))
            pn_numeric_display_set_segment_color (display, rgba[0], rgba[1],
                                                  rgba[2], rgba[3]);
        if (read_rgb_or_rgba (state, "unlit_color", rgba))
            pn_numeric_display_set_unlit_color (display, rgba[0], rgba[1],
                                                rgba[2], rgba[3]);
    }
    else if (e->kind == 'm')
    {
        PnMatrix57Display *display = PN_MATRIX57_DISPLAY (e->widget);

        if (json_object_has_member (state, "text"))
            pn_matrix57_display_set_text (
                    display, json_object_get_string_member (state, "text"));
        if (json_object_has_member (state, "cells"))
            pn_matrix57_display_set_cells (
                    display, (guint) json_object_get_int_member (state, "cells"));
        if (json_object_has_member (state, "lines"))
            pn_matrix57_display_set_lines (
                    display, (gint) json_object_get_int_member (state, "lines"));
        if (read_rgb (state, "bg", rgb))
            pn_matrix57_display_set_background_color (display,
                                                      rgb[0], rgb[1], rgb[2]);
        if (read_rgb (state, "pixel", rgb))
            pn_matrix57_display_set_pixel_color (display,
                                                 rgb[0], rgb[1], rgb[2]);
        if (read_rgb (state, "unlit", rgb))
            pn_matrix57_display_set_unlit_pixel_color (display,
                                                       rgb[0], rgb[1], rgb[2]);
    }
    else
    {
        PnLedLamp *lamp = PN_LED_LAMP (e->widget);

        if (json_object_has_member (state, "lit"))
            pn_led_lamp_set_lit (
                    lamp, json_object_get_boolean_member (state, "lit"));
        if (read_rgb (state, "color", rgb))
            pn_led_lamp_set_color (lamp, rgb[0], rgb[1], rgb[2]);
    }
}

/* Size one mirrored widget to @size pixels, per its kind. */
static void
size_widget (MirrorWidget *e, gint size)
{
    if (e->kind == 'c')
        pn_led_display_set_height (PN_LED_DISPLAY (e->widget), size);
    else if (e->kind == 's')
        pn_switch_widget_set_height (PN_SWITCH_WIDGET (e->widget), size);
    else if (e->kind == 'i')
        pn_injector_widget_set_height (PN_INJECTOR_WIDGET (e->widget), size);
    else if (e->kind == 't')
        pn_text_display_set_height (PN_TEXT_DISPLAY (e->widget), size);
    else if (e->kind == 'm')
        pn_matrix57_display_set_height (PN_MATRIX57_DISPLAY (e->widget), size);
    else if (e->kind == 'n')
        pn_numeric_display_set_height (PN_NUMERIC_DISPLAY (e->widget), size);
    else
        pn_led_lamp_set_size (PN_LED_LAMP (e->widget), size);
}

/* ------------------------------------------------------------------ */
/*  Building and destroying widgets                                     */
/* ------------------------------------------------------------------ */

/* A click on an interactive mirrored widget: hand the node's UUID to the
 * host, which asks the engine to act on that one node.  The widget itself
 * never changes state here — it waits for the engine to echo one back, so
 * the display can never drift from the running flow. */
static void
on_widget_activated (GtkWidget *widget, gpointer user_data)
{
    PnWidgetMirror *self = user_data;
    const gchar    *uuid;

    if (self->activate == NULL)
        return;

    uuid = g_object_get_data (G_OBJECT (widget), "pn-uuid");
    if (uuid != NULL)
        self->activate (uuid, self->user_data);
}

/* Build a fresh widget of @kind for the node @uuid and put it on the
 * canvas at the origin; the host places it once positions are known. */
static MirrorWidget *
mirror_widget_new (PnWidgetMirror *self, gchar kind, const gchar *uuid)
{
    MirrorWidget *e = g_slice_new0 (MirrorWidget);

    e->kind = kind;
    switch (kind)
    {
    case 's':
        e->widget = pn_switch_widget_new ();
        /* Make it clickable before it is shown (and thus realized), so the
         * widget grabs its own input window; tag it with the node UUID and
         * route clicks out through the host. */
        pn_switch_widget_set_interactive (PN_SWITCH_WIDGET (e->widget), TRUE);
        g_object_set_data_full (G_OBJECT (e->widget), "pn-uuid",
                                g_strdup (uuid), g_free);
        g_signal_connect (e->widget, "toggled",
                          G_CALLBACK (on_widget_activated), self);
        break;
    case 'i':
        e->widget = pn_injector_widget_new ();
        /* Same pattern as the switch: interactive before realize, tagged
         * with the node UUID, clicks routed out through the host. */
        pn_injector_widget_set_interactive (PN_INJECTOR_WIDGET (e->widget),
                                            TRUE);
        g_object_set_data_full (G_OBJECT (e->widget), "pn-uuid",
                                g_strdup (uuid), g_free);
        g_signal_connect (e->widget, "clicked",
                          G_CALLBACK (on_widget_activated), self);
        break;
    case 'l':
        e->widget = pn_led_lamp_new ();
        break;
    case 't':
        e->widget = pn_text_display_new ();
        break;
    case 'm':
        e->widget = pn_matrix57_display_new ();
        break;
    case 'n':
        e->widget = pn_numeric_display_new ();
        break;
    case 'c':
    default:
        e->widget = pn_led_display_new ();
        break;
    }

    gtk_fixed_put (self->canvas, e->widget, 0, 0);
    gtk_widget_show (e->widget);
    return e;
}

/* The single-letter kind code for the engine's "kind" string. */
static gchar
mirror_kind_from_string (const gchar *kind_s)
{
    return (g_strcmp0 (kind_s, "led")      == 0) ? 'l'
         : (g_strcmp0 (kind_s, "switch")   == 0) ? 's'
         : (g_strcmp0 (kind_s, "injector") == 0) ? 'i'
         : (g_strcmp0 (kind_s, "text")     == 0) ? 't'
         : (g_strcmp0 (kind_s, "matrix57") == 0) ? 'm'
         : (g_strcmp0 (kind_s, "numeric")  == 0) ? 'n'
         : 'c';
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */

PnWidgetMirror *
pn_widget_mirror_new (GtkFixed                   *canvas,
                      PnWidgetMirrorActivateFunc  activate,
                      gpointer                    user_data)
{
    PnWidgetMirror *self;

    g_return_val_if_fail (GTK_IS_FIXED (canvas), NULL);

    self = g_slice_new0 (PnWidgetMirror);
    self->canvas    = canvas;
    self->activate  = activate;
    self->user_data = user_data;
    self->widgets   = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, mirror_widget_free);
    self->order     = g_ptr_array_new ();

    return self;
}

void
pn_widget_mirror_free (PnWidgetMirror *self)
{
    GHashTableIter it;
    gpointer       key, val;

    if (self == NULL)
        return;

    g_hash_table_iter_init (&it, self->widgets);
    while (g_hash_table_iter_next (&it, &key, &val))
        gtk_widget_destroy (((MirrorWidget *) val)->widget);

    g_hash_table_destroy (self->widgets);
    g_ptr_array_unref (self->order);
    g_slice_free (PnWidgetMirror, self);
}

gboolean
pn_widget_mirror_reconcile (PnWidgetMirror *self, const gchar *layout_json)
{
    JsonParser *parser;
    GError     *error = NULL;
    JsonNode   *root;
    JsonObject *obj;
    JsonArray  *widgets;
    GHashTable *desired;
    guint       i, len;

    g_return_val_if_fail (self != NULL, FALSE);
    g_return_val_if_fail (layout_json != NULL, FALSE);

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, layout_json, -1, &error))
    {
        g_warning ("pn-widget-mirror: bad layout JSON: %s", error->message);
        g_clear_error (&error);
        g_object_unref (parser);
        return FALSE;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_object_unref (parser);
        return FALSE;
    }

    obj     = json_node_get_object (root);
    widgets = json_object_has_member (obj, "widgets")
              ? json_object_get_array_member (obj, "widgets") : NULL;
    len     = widgets != NULL ? json_array_get_length (widgets) : 0;

    /* UUIDs present in this layout — keys borrowed from the parser, valid
     * until it is freed below.  @order is rebuilt in array order. */
    desired = g_hash_table_new (g_str_hash, g_str_equal);
    g_ptr_array_set_size (self->order, 0);

    for (i = 0; i < len; i++)
    {
        JsonObject   *w = json_array_get_object_element (widgets, i);
        const gchar  *uuid;
        JsonObject   *state;
        gchar         kind;
        MirrorWidget *e;

        if (w == NULL || !json_object_has_member (w, "uuid"))
            continue;

        uuid  = json_object_get_string_member (w, "uuid");
        state = json_object_has_member (w, "state")
                ? json_object_get_object_member (w, "state") : NULL;
        kind  = mirror_kind_from_string (
                    (state != NULL && json_object_has_member (state, "kind"))
                    ? json_object_get_string_member (state, "kind") : "");

        g_hash_table_add (desired, (gpointer) uuid);

        e = g_hash_table_lookup (self->widgets, uuid);
        if (e != NULL && e->kind != kind)
        {
            gtk_widget_destroy (e->widget);
            g_hash_table_remove (self->widgets, uuid);
            e = NULL;
        }
        if (e == NULL)
        {
            e = mirror_widget_new (self, kind, uuid);
            g_hash_table_insert (self->widgets, g_strdup (uuid), e);
        }

        e->x = json_object_has_member (w, "x")
               ? json_object_get_double_member (w, "x") : 0.0;
        e->y = json_object_has_member (w, "y")
               ? json_object_get_double_member (w, "y") : 0.0;

        apply_widget_state (e, state);
        g_ptr_array_add (self->order, e);
    }

    /* Drop any widget whose node is no longer in the layout. */
    {
        GHashTableIter it;
        gpointer       key, val;
        GList         *stale = NULL, *l;

        g_hash_table_iter_init (&it, self->widgets);
        while (g_hash_table_iter_next (&it, &key, &val))
            if (!g_hash_table_contains (desired, key))
                stale = g_list_prepend (stale, key);

        for (l = stale; l != NULL; l = l->next)
        {
            MirrorWidget *e = g_hash_table_lookup (self->widgets, l->data);
            if (e != NULL)
                gtk_widget_destroy (e->widget);
            g_hash_table_remove (self->widgets, l->data);
        }
        g_list_free (stale);
    }

    g_hash_table_destroy (desired);
    g_object_unref (parser);
    return TRUE;
}

void
pn_widget_mirror_update (PnWidgetMirror *self,
                         const gchar    *uuid,
                         const gchar    *state_json)
{
    MirrorWidget *e;
    JsonParser   *parser;
    JsonNode     *root;

    g_return_if_fail (self != NULL);

    e = g_hash_table_lookup (self->widgets, uuid);
    if (e == NULL)
        return;   /* arrived before its layout entry; next reconcile fixes it */

    parser = json_parser_new ();
    if (json_parser_load_from_data (parser, state_json, -1, NULL))
    {
        root = json_parser_get_root (parser);
        if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
            apply_widget_state (e, json_node_get_object (root));
    }
    g_object_unref (parser);
}

guint
pn_widget_mirror_get_n_widgets (PnWidgetMirror *self)
{
    g_return_val_if_fail (self != NULL, 0);
    return self->order->len;
}

GtkWidget *
pn_widget_mirror_get_widget (PnWidgetMirror *self, guint index)
{
    g_return_val_if_fail (self != NULL, NULL);
    g_return_val_if_fail (index < self->order->len, NULL);

    return ((MirrorWidget *) g_ptr_array_index (self->order, index))->widget;
}

void
pn_widget_mirror_get_position (PnWidgetMirror *self,
                               guint           index,
                               gdouble        *out_x,
                               gdouble        *out_y)
{
    MirrorWidget *e;

    g_return_if_fail (self != NULL);
    g_return_if_fail (index < self->order->len);

    e = g_ptr_array_index (self->order, index);
    if (out_x != NULL)
        *out_x = e->x;
    if (out_y != NULL)
        *out_y = e->y;
}

void
pn_widget_mirror_set_height (PnWidgetMirror *self, guint index, gint height)
{
    g_return_if_fail (self != NULL);
    g_return_if_fail (index < self->order->len);

    size_widget (g_ptr_array_index (self->order, index), height);
}

gboolean
pn_widget_mirror_fills_height (PnWidgetMirror *self, guint index)
{
    MirrorWidget *e;

    g_return_val_if_fail (self != NULL, FALSE);
    g_return_val_if_fail (index < self->order->len, FALSE);

    e = g_ptr_array_index (self->order, index);
    return e->kind == 't' || e->kind == 'm';
}
