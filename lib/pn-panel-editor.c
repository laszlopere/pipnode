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

#include "pn-panel-editor.h"
#include "pn-preferences.h"

#include <pango/pangocairo.h>

/* ------------------------------------------------------------------ */
/*  PnPanelEditor                                                      */
/*                                                                     */
/*  Placeholder GtkDrawingArea-derived widget that will eventually     */
/*  host the panel-applet GUI layout editor — the visual counterpart   */
/*  to the node-wiring #PnWorksheet, but for laying out the widgets a   */
/*  flow drives rather than the dataflow itself.  Other GUI-layout      */
/*  editors (desktop, web, mobile) are planned to follow the same       */
/*  shape.  At this stage the widget only paints a mock of an XFCE      */
/*  panel with an empty applet slot and a caption; it owns no model     */
/*  and carries no editing interactions.                               */
/* ------------------------------------------------------------------ */

/* Mock panel geometry, in widget pixels.  The strip is centred in the
 * drawing area; the slot is the highlighted "drop here" cell inside it. */
#define PN_PE_PANEL_WIDTH   420.0
#define PN_PE_PANEL_HEIGHT   48.0
#define PN_PE_SLOT_SIZE      36.0
#define PN_PE_SLOT_PAD        6.0

struct _PnPanelEditor
{
    GtkDrawingArea parent_instance;
};

G_DEFINE_TYPE (PnPanelEditor, pn_panel_editor, GTK_TYPE_DRAWING_AREA)

/* Lay a Pango layout out centred horizontally at (cx) with its top at
 * (top_y); returns the height consumed so the caller can stack lines. */
static double
draw_centered_text (cairo_t      *cr,
                    double        cx,
                    double        top_y,
                    const gchar  *markup)
{
    PangoLayout *layout;
    int          tw = 0, th = 0;

    layout = pango_cairo_create_layout (cr);
    pango_layout_set_markup (layout, markup, -1);
    pango_layout_set_alignment (layout, PANGO_ALIGN_CENTER);
    pango_layout_get_pixel_size (layout, &tw, &th);

    cairo_move_to (cr, cx - tw / 2.0, top_y);
    pango_cairo_show_layout (cr, layout);

    g_object_unref (layout);
    return th;
}

static gboolean
pn_panel_editor_draw (GtkWidget *widget,
                      cairo_t   *cr)
{
    PnPreferences *prefs  = pn_preferences_get_default ();
    const int      width  = gtk_widget_get_allocated_width  (widget);
    const int      height = gtk_widget_get_allocated_height (widget);
    const double   cx     = width  / 2.0;
    const double   cy     = height / 2.0;
    GdkRGBA        bg;
    double         px, py, slot_x, slot_y;
    double         caption_y;
    int            i;

    /* Background, matching the node worksheet so the two editors feel
     * like part of the same canvas family. */
    pn_preferences_get_background_color (prefs, &bg);
    cairo_set_source_rgba (cr, bg.red, bg.green, bg.blue, bg.alpha);
    cairo_paint (cr);

    /* --- Mock XFCE panel strip, centred --- */
    px = cx - PN_PE_PANEL_WIDTH  / 2.0;
    py = cy - PN_PE_PANEL_HEIGHT / 2.0;

    cairo_set_source_rgba (cr, 0.20, 0.20, 0.22, 0.92);
    cairo_rectangle (cr, px, py, PN_PE_PANEL_WIDTH, PN_PE_PANEL_HEIGHT);
    cairo_fill (cr);

    /* A couple of greyed-out placeholder applets on the left, then the
     * highlighted dashed slot where the pipnode applet would land. */
    slot_y = py + (PN_PE_PANEL_HEIGHT - PN_PE_SLOT_SIZE) / 2.0;
    slot_x = px + PN_PE_SLOT_PAD;

    cairo_set_source_rgba (cr, 0.45, 0.45, 0.48, 1.0);
    for (i = 0; i < 3; i++)
    {
        cairo_rectangle (cr, slot_x, slot_y, PN_PE_SLOT_SIZE, PN_PE_SLOT_SIZE);
        cairo_fill (cr);
        slot_x += PN_PE_SLOT_SIZE + PN_PE_SLOT_PAD;
    }

    /* Highlighted "drop your applet here" slot, dashed outline. */
    cairo_set_source_rgba (cr, 0.30, 0.60, 0.95, 0.25);
    cairo_rectangle (cr, slot_x, slot_y, PN_PE_SLOT_SIZE, PN_PE_SLOT_SIZE);
    cairo_fill (cr);

    cairo_set_source_rgba (cr, 0.30, 0.60, 0.95, 1.0);
    cairo_set_line_width (cr, 2.0);
    {
        const double dashes[] = { 4.0, 3.0 };
        cairo_set_dash (cr, dashes, 2, 0.0);
    }
    cairo_rectangle (cr, slot_x + 1.0, slot_y + 1.0,
                     PN_PE_SLOT_SIZE - 2.0, PN_PE_SLOT_SIZE - 2.0);
    cairo_stroke (cr);
    cairo_set_dash (cr, NULL, 0, 0.0);

    /* --- Caption beneath the strip --- */
    caption_y = py + PN_PE_PANEL_HEIGHT + 24.0;
    caption_y += draw_centered_text (
            cr, cx, caption_y,
            "<span size='large' weight='bold'>Panel applet GUI editor</span>");
    caption_y += 6.0;
    (void) draw_centered_text (
            cr, cx, caption_y,
            "<span foreground='#888888'>"
            "Placeholder — layout editing coming soon</span>");

    return FALSE;
}

static void
pn_panel_editor_get_preferred_width (GtkWidget *widget,
                                     gint      *minimum,
                                     gint      *natural)
{
    (void) widget;
    *minimum = 320;
    *natural = (gint) (PN_PE_PANEL_WIDTH + 120.0);
}

static void
pn_panel_editor_get_preferred_height (GtkWidget *widget,
                                      gint      *minimum,
                                      gint      *natural)
{
    (void) widget;
    *minimum = 200;
    *natural = 320;
}

static void
pn_panel_editor_class_init (PnPanelEditorClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    widget_class->draw                 = pn_panel_editor_draw;
    widget_class->get_preferred_width  = pn_panel_editor_get_preferred_width;
    widget_class->get_preferred_height = pn_panel_editor_get_preferred_height;
}

static void
pn_panel_editor_init (PnPanelEditor *self)
{
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);
}

GtkWidget *
pn_panel_editor_new (void)
{
    return g_object_new (PN_TYPE_PANEL_EDITOR, NULL);
}
