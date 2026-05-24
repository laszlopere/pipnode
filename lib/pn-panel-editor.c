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
#include "pn-led-display.h"

/* ------------------------------------------------------------------ */
/*  PnPanelEditor                                                      */
/*                                                                     */
/*  Placeholder panel-applet GUI layout editor — the visual            */
/*  counterpart to the node-wiring #PnWorksheet, but for laying out     */
/*  the widgets a flow drives rather than the dataflow itself.  Other   */
/*  GUI-layout editors (desktop, web, mobile) are planned to follow     */
/*  the same shape.                                                     */
/*                                                                     */
/*  At this stage it carries no layout model or editing interactions;   */
/*  it previews the shared panel-applet widget toolkit by embedding a   */
/*  live #PnLedDisplay (the seven-segment deadline readout, the very    */
/*  same widget the XFCE panel applet renders) framed as it appears in  */
/*  the panel, with a caption.  #PnLedDisplay comes from the GTK/Cairo-  */
/*  only panel-widgets convenience library, which both this editor and   */
/*  the applet embed — so the two stay pixel-identical without the       */
/*  applet ever linking the node runtime.                               */
/* ------------------------------------------------------------------ */

/* Pixel height of the previewed readout — a typical XFCE panel icon
 * size, so the preview matches what lands on a real panel. */
#define PN_PE_PREVIEW_HEIGHT 36

/* Sample countdown shown in the preview: 5 days, 13:24:36 remaining, so
 * every field of the "ddd hh:mm:ss" readout shows a non-zero digit. */
#define PN_PE_SAMPLE_SECONDS ((gint64) (5 * 86400 + 13 * 3600 + 24 * 60 + 36))

struct _PnPanelEditor
{
    GtkBox parent_instance;
};

G_DEFINE_TYPE (PnPanelEditor, pn_panel_editor, GTK_TYPE_BOX)

static void
pn_panel_editor_class_init (PnPanelEditorClass *klass)
{
    (void) klass;
}

static void
pn_panel_editor_init (PnPanelEditor *self)
{
    GtkWidget *content;
    GtkWidget *title;
    GtkWidget *frame;
    GtkWidget *led;
    GtkWidget *subtitle;

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_hexpand (GTK_WIDGET (self), TRUE);
    gtk_widget_set_vexpand (GTK_WIDGET (self), TRUE);

    /* Centre the preview both ways: a centred inner column packed with
     * expand=TRUE, fill=FALSE floats in the middle of the canvas. */
    content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_halign (content, GTK_ALIGN_CENTER);
    gtk_box_pack_start (GTK_BOX (self), content, TRUE, FALSE, 0);

    title = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (title),
            "<span size='large' weight='bold'>Panel applet GUI editor</span>");
    gtk_box_pack_start (GTK_BOX (content), title, FALSE, FALSE, 0);

    /* The live applet readout, framed with a bezel so it reads as a
     * panel-mounted display. */
    frame = gtk_frame_new (NULL);
    gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_IN);
    gtk_widget_set_halign (frame, GTK_ALIGN_CENTER);

    led = pn_led_display_new ();
    pn_led_display_set_height  (PN_LED_DISPLAY (led), PN_PE_PREVIEW_HEIGHT);
    pn_led_display_set_seconds (PN_LED_DISPLAY (led), PN_PE_SAMPLE_SECONDS);
    gtk_widget_set_margin_top    (led, 4);
    gtk_widget_set_margin_bottom (led, 4);
    gtk_widget_set_margin_start  (led, 6);
    gtk_widget_set_margin_end    (led, 6);
    gtk_container_add (GTK_CONTAINER (frame), led);
    gtk_box_pack_start (GTK_BOX (content), frame, FALSE, FALSE, 0);

    subtitle = gtk_label_new (NULL);
    gtk_label_set_markup (
            GTK_LABEL (subtitle),
            "<span foreground='#888888'>"
            "Deadline applet preview — layout editing coming soon</span>");
    gtk_box_pack_start (GTK_BOX (content), subtitle, FALSE, FALSE, 0);

    gtk_widget_show_all (content);
}

GtkWidget *
pn_panel_editor_new (void)
{
    return g_object_new (PN_TYPE_PANEL_EDITOR, NULL);
}
