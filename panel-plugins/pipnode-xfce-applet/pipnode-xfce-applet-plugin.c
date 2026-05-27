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
/*  Pipnode Panel — XFCE panel applet                                 */
/*                                                                     */
/*  A panel button that *runs* a pipnode worksheet, not just a         */
/*  launcher.  Each applet instance owns one worksheet (a .json        */
/*  document) under ~/.config/pipnode/panel/<unique-id>.json, executed */
/*  by the background engine — pipnode-editor running as a D-Bus       */
/*  service (org.pipas.pipnode), auto-started on first use.            */
/*                                                                     */
/*  The applet is a live mirror of the worksheet's panel layout: every */
/*  Countdown / LED node the user snapped onto the panel band in the   */
/*  editor shows here as a counterpart widget — a seven-segment        */
/*  PnLedDisplay readout or a PnLedLamp indicator.  The applet asks    */
/*  the engine for the widget set and order (GetLayout) and follows it */
/*  (LayoutChanged); each node's live value arrives keyed by node UUID */
/*  (WidgetChanged) as JSON and is pushed into its widget.             */
/*                                                                     */
/*  It is a thin D-Bus client: it links GTK / GLib / json-glib /       */
/*  libxfce4panel only (GDBus comes via gio) and NO pipnode runtime    */
/*  library, so an engine/node crash can never take down xfce4-panel.  */
/*  The only pipnode code it embeds is the GTK/Cairo-only panel-widgets */
/*  convenience library (PnLedDisplay, PnLedLamp), statically linked.   */
/*    - a mouse click drives the PnPanelInput node(s): the click is     */
/*      forwarded (SendEvent) tagged with the button;                  */
/*    - a click on a Switch toggle flips that one node, addressed by    */
/*      UUID (ActivateWidget), instead of the generic input click;      */
/*    - right-click "Properties" opens the worksheet for editing       */
/*      (PresentEditor) — the same running flow, shown.                */
/* ------------------------------------------------------------------ */

#include <gtk/gtk.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <libxfce4panel/libxfce4panel.h>

#include "pn-injector-widget.h"
#include "pn-led-display.h"
#include "pn-led-lamp.h"
#include "pn-matrix57-display.h"
#include "pn-numeric-display.h"
#include "pn-switch-widget.h"
#include "pn-text-display.h"

/* The freedesktop/themed icon name shipped by the main app
 * (data/icons/hicolor/.../org.pipas.pipnode.png). */
#define PIPNODE_ICON_NAME "org.pipas.pipnode"

/* Flatten the panel button in every state: no border, background or
 * box-shadow on hover/focus/active, so the applet shows just its widgets
 * with no frame or highlight when the mouse is over it. */
static const gchar BUTTON_CSS[] =
    "button, button:hover, button:active, button:checked, button:focus {"
    "  background: none;"
    "  background-image: none;"
    "  border: none;"
    "  box-shadow: none;"
    "  outline: none;"
    "  padding: 0;"
    "}";

static void
flatten_button (GtkWidget *button)
{
    GtkCssProvider  *provider;
    GtkStyleContext *context;

    provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_data (provider, BUTTON_CSS, -1, NULL);

    context = gtk_widget_get_style_context (button);
    gtk_style_context_add_provider (context,
                                    GTK_STYLE_PROVIDER (provider),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (provider);
}

/* The background engine's well-known bus name, object path and the
 * panel control interface (see src/pn-application.c).  The name is
 * D-Bus-activatable, so the first call auto-starts the engine. */
#define PN_ENGINE_BUS    "org.pipas.pipnode"
#define PN_ENGINE_OBJECT "/org/pipas/pipnode"
#define PN_ENGINE_IFACE  "org.pipas.pipnode.Engine"

/* Starter worksheet shipped with the applet: a new applet instance is
 * seeded with a copy of this so the user begins with a working flow
 * rather than a blank sheet.  Installed to pipnode's datadir; see
 * ensure_worksheet(). */
#define PN_DEFAULT_WORKSHEET  PKGDATADIR "/pipnode-xfce-applet-default.json"

/* A blank pipnode document, matching the on-disk format produced by
 * lib/pn-flow.c (PN_FLOW_FILE_VERSION 1).  Used as a fallback when the
 * shipped starter worksheet is missing.  The engine reparses and
 * rewrites this on first save, so only the structure has to be valid. */
static const gchar EMPTY_WORKSHEET[] =
    "{\n"
    "  \"format\" : \"pipnode\",\n"
    "  \"version\" : 1,\n"
    "  \"nodes\" : [],\n"
    "  \"connections\" : [],\n"
    "  \"sheets\" : [\n"
    "    \"Worksheet\"\n"
    "  ],\n"
    "  \"active_sheet\" : \"Worksheet\"\n"
    "}\n";

/* One mirrored widget: a panel-widgets drawing area plus the kind of node
 * it stands for ('c' = Countdown -> PnLedDisplay, 'l' = LED -> PnLedLamp,
 * 's' = Switch -> PnSwitchWidget, 'm' = Matrix57 -> PnMatrix57Display,
 * 'i' = Injector -> PnInjectorWidget).
 * Switch and injector are the interactive kinds: their widgets are made
 * clickable and the click is reported back to the engine by the node's
 * UUID, stashed on the widget as "pn-uuid".  The widget itself is owned by
 * the applet's @fixed; this struct only tracks the node.
 * Row order is the engine's layout order, held by @order. */
typedef struct
{
    GtkWidget *widget;
    gchar      kind;
} AppletWidget;

typedef struct
{
    XfcePanelPlugin *plugin;
    GtkWidget       *button;   /* panel button (owned by the plugin)     */
    GtkWidget       *fixed;    /* free-positioning row of widgets + icon  */
    GtkWidget       *image;    /* icon, shown only when no widgets        */
    gint             icon_size;/* panel icon size, applied to new widgets */
    gint             row_h;    /* fixed's allocated height; centres the row */

    GHashTable      *widgets;  /* node UUID (owned) -> AppletWidget*      */
    GPtrArray       *order;    /* AppletWidget* (borrowed), row order     */

    gchar           *path;     /* this instance's worksheet file          */
    GDBusProxy      *engine;   /* org.pipas.pipnode.Engine proxy          */
    GCancellable    *cancel;   /* cancels in-flight async D-Bus work      */
} PipnodeDeadline;

/* ------------------------------------------------------------------ */
/*  Worksheet location / creation                                      */
/* ------------------------------------------------------------------ */

/* ~/.config/pipnode/panel/<unique-id>.json — one file per applet
 * instance, so two panel buttons keep distinct worksheets. */
static gchar *
worksheet_path (PipnodeDeadline *self)
{
    gchar    *base;
    gchar    *path;
    gint      id;

    id = xfce_panel_plugin_get_unique_id (self->plugin);
    base = g_strdup_printf ("%d.json", id);
    path = g_build_filename (g_get_user_config_dir (),
                             "pipnode", "panel", base, NULL);
    g_free (base);

    return path;
}

/* Make sure the worksheet file exists, seeding a new one (and its parent
 * directory) on first use.  The seed is a copy of the shipped starter
 * worksheet (PN_DEFAULT_WORKSHEET) so the user begins with a working flow;
 * if that file is missing we fall back to the embedded empty document.
 * Returns FALSE and sets *error on failure. */
static gboolean
ensure_worksheet (const gchar *path,
                  GError     **error)
{
    gchar  *dir;
    gchar  *seed     = NULL;
    gsize   seed_len = 0;
    gboolean ok;

    if (g_file_test (path, G_FILE_TEST_EXISTS))
        return TRUE;

    dir = g_path_get_dirname (path);
    if (g_mkdir_with_parents (dir, 0700) != 0)
    {
        g_set_error (error, G_FILE_ERROR,
                     g_file_error_from_errno (errno),
                     "Could not create %s: %s",
                     dir, g_strerror (errno));
        g_free (dir);
        return FALSE;
    }
    g_free (dir);

    /* Prefer the shipped starter worksheet; the embedded empty document is
     * the fallback when it is not installed. */
    if (g_file_get_contents (PN_DEFAULT_WORKSHEET, &seed, &seed_len, NULL))
    {
        ok = g_file_set_contents (path, seed, seed_len, error);
        g_free (seed);
    }
    else
    {
        ok = g_file_set_contents (path, EMPTY_WORKSHEET,
                                  sizeof EMPTY_WORKSHEET - 1, error);
    }

    return ok;
}

/* ------------------------------------------------------------------ */
/*  Mirrored widgets                                                    */
/* ------------------------------------------------------------------ */

static void
applet_widget_free (gpointer data)
{
    /* The GtkWidget is owned by @box and torn down with it; free only the
     * tracking struct. */
    g_slice_free (AppletWidget, data);
}

/* Show the icon only while the worksheet contributes no widgets. */
static void
update_empty_state (PipnodeDeadline *self)
{
    gtk_widget_set_visible (self->image,
                            g_hash_table_size (self->widgets) == 0);
}

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
apply_widget_state (AppletWidget *e, JsonObject *state)
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

/* Mirrored readouts render half again the panel's icon size: a panel-height
 * countdown is hard to read, so we trade a little vertical slack for legibility
 * (the row centres the taller widget, see relayout()). */
#define PN_APPLET_WIDGET_SIZE(self) ((self)->icon_size * 3 / 2)

/* Size one mirrored widget to @size pixels, per its kind. */
static void
size_widget (AppletWidget *e, gint size)
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

/* Size one widget the right way for its kind: the text label takes the full
 * panel row height so its font fills the panel like a clock applet; the
 * other readouts keep the legibility-tuned icon×3/2 size. */
static void
apply_widget_size (PipnodeDeadline *self, AppletWidget *e)
{
    gint size = ((e->kind == 't' || e->kind == 'm') && self->row_h > 0)
                ? self->row_h : PN_APPLET_WIDGET_SIZE (self);
    size_widget (e, size);
}

/* A click on a mirrored interactive widget: ask the engine to act on that
 * one node, addressed by the UUID stashed on the widget.  For a switch the
 * engine toggles it and echoes the new "on" state back as a WidgetChanged,
 * which update_widget() pushes into the toggle.  For an injector the engine
 * fires the node (a one-shot message); there is no state to echo back.
 * Either way, the widget follows the node — it never tries to guess. */
static void
on_widget_activated (GtkWidget *widget, PipnodeDeadline *self)
{
    const gchar *uuid;

    if (self->engine == NULL)
        return;

    uuid = g_object_get_data (G_OBJECT (widget), "pn-uuid");
    if (uuid == NULL)
        return;

    g_dbus_proxy_call (self->engine, "ActivateWidget",
                       g_variant_new ("(ss)", self->path, uuid),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* Build a fresh widget of @kind for the node @uuid, sized to the panel and
 * placed on the row.  The interactive kinds (switch toggle, injector
 * button) are made clickable and tagged with their UUID so
 * on_widget_activated() can address them.  relayout() moves the widget to
 * its final spot once positions are known. */
static AppletWidget *
applet_widget_new (PipnodeDeadline *self, gchar kind, const gchar *uuid)
{
    AppletWidget *e = g_slice_new0 (AppletWidget);

    e->kind = kind;
    switch (kind)
    {
    case 's':
        e->widget = pn_switch_widget_new ();
        /* Make it clickable before it is shown (and thus realized), so the
         * widget grabs its own input window; tag it with the node UUID and
         * route clicks to the engine. */
        pn_switch_widget_set_interactive (PN_SWITCH_WIDGET (e->widget), TRUE);
        g_object_set_data_full (G_OBJECT (e->widget), "pn-uuid",
                                g_strdup (uuid), g_free);
        g_signal_connect (e->widget, "toggled",
                          G_CALLBACK (on_widget_activated), self);
        break;
    case 'i':
        e->widget = pn_injector_widget_new ();
        /* Same pattern as the switch: make the tab interactive before
         * realize, tag it with the node UUID, route clicks to the engine. */
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
    apply_widget_size (self, e);

    gtk_fixed_put (GTK_FIXED (self->fixed), e->widget, 0, 0);
    gtk_widget_show (e->widget);
    return e;
}

/* Vertically centre @child within the row, returning the y it was placed at.
 * Shorter-than-row widgets sit mid-band rather than clinging to the top. */
static void
center_in_row (PipnodeDeadline *self, GtkWidget *child, gint x)
{
    gint h = 0;

    gtk_widget_get_preferred_height (child, NULL, &h);
    gtk_fixed_move (GTK_FIXED (self->fixed), child, x,
                    self->row_h > h ? (self->row_h - h) / 2 : 0);
}

/* Small horizontal gap between adjacent mirrored widgets, so the readouts
 * sit beside each other with a little breathing room rather than touching. */
#define PN_APPLET_WIDGET_GAP 4

/* Place the row's widgets in layout order, each a few pixels past where the
 * previous ended (PN_APPLET_WIDGET_GAP), so the panel shows one strip with a
 * little space between widgets, however the editor spaced or grouped them on
 * the band.  The applet mirrors only the order, not the editor's spacing.
 * Each widget — and the fallback icon — is centred vertically in the row's
 * allocated height (see on_fixed_allocate). */
static void
relayout (PipnodeDeadline *self)
{
    guint i;
    gint  pack_x = 0;

    for (i = 0; i < self->order->len; i++)
    {
        AppletWidget *e   = g_ptr_array_index (self->order, i);
        gint          nat = 0;

        if (i > 0)
            pack_x += PN_APPLET_WIDGET_GAP;

        gtk_widget_get_preferred_width (e->widget, NULL, &nat);
        center_in_row (self, e->widget, pack_x);
        pack_x += nat;
    }

    center_in_row (self, self->image, 0);
}

/* Reconcile the live widget row against the engine's layout JSON:
 *   { "widgets": [ { "uuid", "order", "state": { "kind", … } }, … ] }
 * Create widgets that are new, drop widgets that vanished, recreate any
 * whose kind changed, apply each one's inline state, and order them to
 * match the array (the hidden icon floats harmlessly among them). */
static void
reconcile_layout (PipnodeDeadline *self, const gchar *layout_json)
{
    JsonParser *parser = json_parser_new ();
    GError     *error  = NULL;
    JsonNode   *root;
    JsonObject *obj;
    JsonArray  *widgets;
    GHashTable *desired;
    guint       i, len;

    if (!json_parser_load_from_data (parser, layout_json, -1, &error))
    {
        g_warning ("pipnode-xfce-applet: bad layout JSON: %s", error->message);
        g_clear_error (&error);
        g_object_unref (parser);
        return;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_object_unref (parser);
        return;
    }

    obj     = json_node_get_object (root);
    widgets = json_object_has_member (obj, "widgets")
              ? json_object_get_array_member (obj, "widgets") : NULL;
    len     = widgets != NULL ? json_array_get_length (widgets) : 0;

    /* uuids present in this layout — keys borrowed from the parser, valid
     * until it is freed below.  @order is rebuilt in array order. */
    desired = g_hash_table_new (g_str_hash, g_str_equal);
    g_ptr_array_set_size (self->order, 0);

    for (i = 0; i < len; i++)
    {
        JsonObject   *w = json_array_get_object_element (widgets, i);
        const gchar  *uuid;
        JsonObject   *state;
        const gchar  *kind_s;
        gchar         kind;
        AppletWidget *e;

        if (w == NULL || !json_object_has_member (w, "uuid"))
            continue;

        uuid  = json_object_get_string_member (w, "uuid");
        state = json_object_has_member (w, "state")
                ? json_object_get_object_member (w, "state") : NULL;
        kind_s = (state != NULL && json_object_has_member (state, "kind"))
                 ? json_object_get_string_member (state, "kind") : "";
        kind   = (g_strcmp0 (kind_s, "led")      == 0) ? 'l'
               : (g_strcmp0 (kind_s, "switch")   == 0) ? 's'
               : (g_strcmp0 (kind_s, "injector") == 0) ? 'i'
               : (g_strcmp0 (kind_s, "text")     == 0) ? 't'
               : (g_strcmp0 (kind_s, "matrix57") == 0) ? 'm'
               : (g_strcmp0 (kind_s, "numeric")  == 0) ? 'n'
               : 'c';

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
            e = applet_widget_new (self, kind, uuid);
            g_hash_table_insert (self->widgets, g_strdup (uuid), e);
        }

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
            AppletWidget *e = g_hash_table_lookup (self->widgets, l->data);
            if (e != NULL)
                gtk_widget_destroy (e->widget);
            g_hash_table_remove (self->widgets, l->data);
        }
        g_list_free (stale);
    }

    g_hash_table_destroy (desired);
    g_object_unref (parser);

    relayout (self);
    update_empty_state (self);
}

/* Apply a single WidgetChanged state payload to the addressed widget. */
static void
update_widget (PipnodeDeadline *self, const gchar *uuid, const gchar *state_json)
{
    AppletWidget *e = g_hash_table_lookup (self->widgets, uuid);
    JsonParser   *parser;
    JsonNode     *root;

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

/* ------------------------------------------------------------------ */
/*  Engine D-Bus calls                                                  */
/* ------------------------------------------------------------------ */

static void
on_get_layout_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
    PipnodeDeadline *self  = user_data;
    GVariant        *reply;
    GError          *error = NULL;

    reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
    if (reply == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pipnode-xfce-applet: GetLayout failed: %s",
                       error->message);
        g_clear_error (&error);
        return;
    }

    {
        const gchar *layout = NULL;
        g_variant_get (reply, "(&s)", &layout);
        reconcile_layout (self, layout);
    }
    g_variant_unref (reply);
}

static void
on_run_worksheet_done (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
    PipnodeDeadline *self = user_data;
    GVariant         *reply;
    GError           *error = NULL;

    reply = g_dbus_proxy_call_finish (G_DBUS_PROXY (source), result, &error);
    if (reply == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pipnode-xfce-applet: RunWorksheet failed: %s",
                       error->message);
        g_clear_error (&error);
        return;
    }
    g_variant_unref (reply);   /* legacy display value — unused now */

    /* The worksheet is running; pull its panel layout to build the row. */
    g_dbus_proxy_call (self->engine, "GetLayout",
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       on_get_layout_done, self);
}

/* Engine signals arrive here; the engine multiplexes every applet on one
 * interface, so the path argument disambiguates which worksheet they
 * concern.  LayoutChanged rebuilds the widget row; WidgetChanged pushes a
 * single node's fresh state into its widget. */
static void
on_engine_signal (GDBusProxy *proxy,
                  const gchar *sender,
                  const gchar *signal_name,
                  GVariant    *parameters,
                  gpointer     user_data)
{
    PipnodeDeadline *self = user_data;

    (void) proxy;
    (void) sender;

    if (g_strcmp0 (signal_name, "LayoutChanged") == 0)
    {
        const gchar *path   = NULL;
        const gchar *layout = NULL;
        g_variant_get (parameters, "(&s&s)", &path, &layout);
        if (g_strcmp0 (path, self->path) == 0)
            reconcile_layout (self, layout);
    }
    else if (g_strcmp0 (signal_name, "WidgetChanged") == 0)
    {
        const gchar *path  = NULL;
        const gchar *uuid  = NULL;
        const gchar *state = NULL;
        g_variant_get (parameters, "(&s&s&s)", &path, &uuid, &state);
        if (g_strcmp0 (path, self->path) == 0)
            update_widget (self, uuid, state);
    }
    /* ValueChanged is the legacy single-value channel — ignored here, the
     * per-widget WidgetChanged path carries displays now. */
}

/* Start (or restart) the worksheet on the engine.  A call on a proxy whose
 * name has no owner auto-activates the service file, so this both kicks off
 * the first run and respawns a freshly installed engine after a Quit.  The
 * reply callback then pulls the layout. */
static void
engine_run_worksheet (PipnodeDeadline *self)
{
    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, "RunWorksheet",
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       on_run_worksheet_done, self);
}

/* The engine owns org.pipas.pipnode.  When that owner vanishes — it quit
 * (our "Reload Worksheet" item), was killed, or crashed — re-run the
 * worksheet: the method call auto-activates the service, starting whatever
 * pipnode-editor binary is now installed, and the reply rebuilds our row.
 * When the owner instead (re)appears there is nothing to do; the pending
 * RunWorksheet already covers it.  This makes every applet self-heal across
 * an engine restart without touching the panel. */
static void
on_engine_owner_changed (GObject    *proxy,
                         GParamSpec *pspec,
                         gpointer    user_data)
{
    PipnodeDeadline *self  = user_data;
    gchar            *owner;

    (void) pspec;

    owner = g_dbus_proxy_get_name_owner (G_DBUS_PROXY (proxy));
    if (owner == NULL)
        engine_run_worksheet (self);
    g_free (owner);
}

static void
on_engine_ready (GObject      *source,
                 GAsyncResult *result,
                 gpointer      user_data)
{
    PipnodeDeadline *self = user_data;
    GError           *error = NULL;

    (void) source;

    self->engine = g_dbus_proxy_new_for_bus_finish (result, &error);
    if (self->engine == NULL)
    {
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("pipnode-xfce-applet: cannot reach the pipnode engine: %s",
                       error->message);
        g_clear_error (&error);
        return;
    }

    g_signal_connect (self->engine, "g-signal",
                      G_CALLBACK (on_engine_signal), self);
    g_signal_connect (self->engine, "notify::g-name-owner",
                      G_CALLBACK (on_engine_owner_changed), self);

    /* Start the worksheet running (auto-activates the engine); the reply
     * callback then pulls the layout. */
    engine_run_worksheet (self);
}

/* Fire-and-forget engine call carrying just the worksheet path. */
static void
engine_call_path (PipnodeDeadline *self, const gchar *method)
{
    if (self->engine == NULL)
        return;

    g_dbus_proxy_call (self->engine, method,
                       g_variant_new ("(s)", self->path),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Interaction                                                         */
/* ------------------------------------------------------------------ */

/* A mouse click on the applet: forward it to the worksheet's Panel Input
 * node(s), tagged with the button, as one "click" event.  Fired on button
 * press (which reaches us for every button, before a right-click hands off
 * to the panel's context menu).  Returns FALSE so the panel still gets the
 * event (the button's own click visuals, and the right-click menu). */
static gboolean
on_button_press (GtkWidget        *button,
                 GdkEventButton   *event,
                 PipnodeDeadline *self)
{
    (void) button;

    if (self->engine == NULL)
        return FALSE;

    /* One click per press; ignore the synthetic GDK_2BUTTON_PRESS /
     * GDK_3BUTTON_PRESS so a double-click counts as two clicks, not four. */
    if (event->type != GDK_BUTTON_PRESS)
        return FALSE;

    g_dbus_proxy_call (self->engine, "SendEvent",
                       g_variant_new ("(ssu)", self->path, "click",
                                      (guint32) event->button),
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
    return FALSE;
}

/* Right-click "Properties": open the running worksheet for editing. */
static void
on_configure_plugin (XfcePanelPlugin  *plugin,
                     PipnodeDeadline *self)
{
    (void) plugin;
    engine_call_path (self, "PresentEditor");
}

/* Right-click "Reload Worksheet": ask the shared engine to quit.  A panel
 * restart reloads the applet but never the long-lived engine, so after
 * `make install` the old binary keeps running; this bounces it.  Every
 * applet watches the bus name (on_engine_owner_changed) and re-runs its
 * worksheet when the owner drops, so one quit cleanly respawns the new
 * binary and reconnects all of them. */
static void
on_restart_engine (GtkMenuItem      *item,
                   PipnodeDeadline *self)
{
    (void) item;

    /* Not connected yet (or a past connect failed): just (re)connect, which
     * auto-activates the engine. */
    if (self->engine == NULL)
    {
        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                                  G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                                  NULL, PN_ENGINE_BUS, PN_ENGINE_OBJECT,
                                  PN_ENGINE_IFACE, self->cancel,
                                  on_engine_ready, self);
        return;
    }

    g_dbus_proxy_call (self->engine, "Quit", NULL,
                       G_DBUS_CALL_FLAGS_NONE, -1, self->cancel,
                       NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Panel lifecycle                                                    */
/* ------------------------------------------------------------------ */

/* Apply the panel's icon size to every mirrored widget. */
static void
resize_widgets (PipnodeDeadline *self)
{
    GHashTableIter it;
    gpointer       key, val;

    g_hash_table_iter_init (&it, self->widgets);
    while (g_hash_table_iter_next (&it, &key, &val))
        apply_widget_size (self, val);
}

/* The row's height is only known once the panel allocates it, so re-centre
 * the widgets here whenever it changes.  gtk_fixed_move() queues a resize, so
 * we must act only on a genuine height change or we would loop forever. */
static void
on_fixed_allocate (GtkWidget     *fixed,
                   GtkAllocation *alloc,
                   PipnodeDeadline *self)
{
    (void) fixed;

    if (alloc->height == self->row_h)
        return;

    self->row_h = alloc->height;
    /* The text label is sized to the full row height, so re-apply sizes now
     * that the row height is known, then re-centre. */
    resize_widgets (self);
    relayout (self);
}

static gboolean
on_size_changed (XfcePanelPlugin  *plugin,
                 gint              size,
                 PipnodeDeadline *self)
{
    (void) size;
    self->icon_size = xfce_panel_plugin_get_icon_size (plugin);
    gtk_image_set_pixel_size (GTK_IMAGE (self->image), self->icon_size);
    resize_widgets (self);
    relayout (self);   /* widths changed → repack / refit positions */
    return TRUE;       /* handled */
}

static void
on_free_data (XfcePanelPlugin  *plugin,
              PipnodeDeadline *self)
{
    (void) plugin;

    g_cancellable_cancel (self->cancel);
    g_clear_object (&self->cancel);
    g_clear_object (&self->engine);
    g_clear_pointer (&self->order, g_ptr_array_unref);
    g_clear_pointer (&self->widgets, g_hash_table_destroy);
    g_clear_pointer (&self->path, g_free);

    g_slice_free (PipnodeDeadline, self);
}

static void
pipnode_xfce_applet_construct (XfcePanelPlugin *plugin)
{
    PipnodeDeadline *self;
    GError           *error = NULL;

    self = g_slice_new0 (PipnodeDeadline);
    self->plugin    = plugin;
    self->cancel    = g_cancellable_new ();
    self->path      = worksheet_path (self);
    self->icon_size = xfce_panel_plugin_get_icon_size (plugin);
    self->widgets   = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, applet_widget_free);
    self->order     = g_ptr_array_new ();

    /* Flat, panel-styled button holding a free-positioned row: the icon
     * (shown only when the worksheet has no panel widgets) plus one widget
     * per mirrored Countdown / LED node, placed by relayout() to mirror the
     * editor's band spacing. */
    self->button = xfce_panel_create_button ();
    flatten_button (self->button);
    self->fixed = gtk_fixed_new ();
    self->image = gtk_image_new_from_icon_name (PIPNODE_ICON_NAME,
                                                GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size (GTK_IMAGE (self->image), self->icon_size);
    gtk_fixed_put (GTK_FIXED (self->fixed), self->image, 0, 0);
    gtk_container_add (GTK_CONTAINER (self->button), self->fixed);

    gtk_widget_set_tooltip_text (self->button,
                                 "Pipnode panel — click to send, "
                                 "right-click → Properties to edit");

    gtk_container_add (GTK_CONTAINER (plugin), self->button);

    /* Let the panel's own context menu pop up over the button. */
    xfce_panel_plugin_add_action_widget (plugin, self->button);

    /* Add the "Properties" entry to the right-click menu (the settings
     * menu) — fires "configure-plugin", which opens the editor. */
    xfce_panel_plugin_menu_show_configure (plugin);

    /* Custom "Reload Worksheet" entry: bounce the shared background engine so
     * a freshly installed pipnode-editor binary takes over (a panel restart
     * reloads only the applet, never the engine).  "Engine" is internal
     * jargon, so the user-facing label talks about the worksheet instead. */
    {
        /* Use a GtkImageMenuItem so the icon sits in the same left-hand
         * column as the panel's own items (Properties, Move, Remove…) and
         * lines up with them — a hand-built box would indent the icon to
         * where their labels start.  The type is deprecated and GTK hides
         * menu images by default, so guard the warning and force the icon
         * on with set_always_show_image.  "view-refresh" is the standard
         * two-arrows-in-a-circle reload glyph. */
        GtkWidget *restart;
        GtkWidget *icon = gtk_image_new_from_icon_name (
                              "view-refresh", GTK_ICON_SIZE_MENU);

        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        restart = gtk_image_menu_item_new_with_label ("Reload Worksheet");
        gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (restart), icon);
        gtk_image_menu_item_set_always_show_image (
                GTK_IMAGE_MENU_ITEM (restart), TRUE);
        G_GNUC_END_IGNORE_DEPRECATIONS

        gtk_widget_show (restart);
        g_signal_connect (restart, "activate",
                          G_CALLBACK (on_restart_engine), self);
        xfce_panel_plugin_menu_insert_item (plugin, GTK_MENU_ITEM (restart));
    }

    g_signal_connect (self->button, "button-press-event",
                      G_CALLBACK (on_button_press), self);
    g_signal_connect (plugin, "configure-plugin",
                      G_CALLBACK (on_configure_plugin), self);
    g_signal_connect (plugin, "size-changed",
                      G_CALLBACK (on_size_changed), self);
    g_signal_connect (self->fixed, "size-allocate",
                      G_CALLBACK (on_fixed_allocate), self);
    g_signal_connect (plugin, "free-data",
                      G_CALLBACK (on_free_data), self);

    gtk_widget_show_all (self->button);  /* image hidden again below */
    update_empty_state (self);

    /* Make sure the file exists, then connect to the engine
     * asynchronously so the panel never blocks on D-Bus. */
    if (!ensure_worksheet (self->path, &error))
    {
        g_warning ("pipnode-xfce-applet: could not create worksheet '%s': %s",
                   self->path, error->message);
        g_clear_error (&error);
        return;
    }

    g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
                              G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
                              NULL,
                              PN_ENGINE_BUS, PN_ENGINE_OBJECT, PN_ENGINE_IFACE,
                              self->cancel, on_engine_ready, self);
}

/* Generates the xfce_panel_module_* entry points the panel dlopen()s. */
XFCE_PANEL_PLUGIN_REGISTER (pipnode_xfce_applet_construct)
