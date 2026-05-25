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
/*  PnSwitchWidget — a panel-sized slide toggle                         */
/*                                                                     */
/*  A miniature of the Switch node's header slider: a rounded "track"   */
/*  with a circular "thumb", green/right when on and grey/left when     */
/*  off.  The painter is a port of pn-switch-gui.c's paint_switch,      */
/*  restated with plain GTK/cairo so the applet never links a pipnode   */
/*  library.                                                           */
/*                                                                     */
/*  Unlike the other panel widgets it can be made interactive: an       */
/*  interactive toggle owns an input window and emits ::toggled on a    */
/*  primary click (the applet turns that into a node toggle).  A        */
/*  non-interactive toggle is windowless and passive, so in the panel   */
/*  editor clicks fall through to the drag handle exactly as the LED /   */
/*  clock widgets do.                                                   */
/* ------------------------------------------------------------------ */

#include "pn-switch-widget.h"

/* A hair of inset so the pill never sits flush against the widget edge. */
#define SWITCH_EDGE_PAD 2.0

/* Pill aspect: the node slider is 38x20, so width is ~1.9x the height. */
#define SWITCH_ASPECT 1.9

enum
{
    SIGNAL_TOGGLED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _PnSwitchWidget
{
    GtkDrawingArea parent_instance;

    gboolean on;          /* latch position drawn                       */
    gint     height;      /* requested overall pixel height             */
    gboolean interactive; /* whether a primary click emits ::toggled    */
};

G_DEFINE_TYPE (PnSwitchWidget, pn_switch_widget, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/*  Painting (ported from pn-switch-gui.c's paint_switch)              */
/* ------------------------------------------------------------------ */

/** Trace a horizontal pill (rounded-end rectangle). */
static void
pill_path (cairo_t *cr, double x, double y, double w, double h)
{
    const double r = h * 0.5;

    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r, r, -G_PI_2,  G_PI_2);
    cairo_arc (cr, x + r,     y + r, r,  G_PI_2,  3.0 * G_PI_2);
    cairo_close_path (cr);
}

/** Paint a slide switch concentric on (@cx,@cy) with size @w x @h.  On
 *  shows the thumb right with a green track; off parks it left with a dark
 *  grey track. */
static void
paint_switch (cairo_t *cr,
              double   cx,
              double   cy,
              double   w,
              double   h,
              gboolean on)
{
    const double tx = cx - w * 0.5;
    const double ty = cy - h * 0.5;
    const double thumb_r = h * 0.5 - 2.0;
    const double thumb_cx = on ? (tx + w - h * 0.5)
                               : (tx + h * 0.5);

    /* Track fill.  Green when on, dark grey when off. */
    pill_path (cr, tx, ty, w, h);
    if (on)
        cairo_set_source_rgb (cr, 0.25, 0.72, 0.35);
    else
        cairo_set_source_rgb (cr, 0.28, 0.30, 0.32);
    cairo_fill_preserve (cr);

    /* Track outline — a thin dark ring so the pill reads as a moulded part. */
    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.55);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    /* Inset highlight on the upper rim sells the chamfered bevel. */
    cairo_save (cr);
    pill_path (cr, tx, ty, w, h);
    cairo_clip (cr);
    pill_path (cr, tx + 1.0, ty + 1.0, w - 2.0, h - 2.0);
    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.18);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* Thumb — a near-white disc lit from the upper-left so it reads as a
     * physical knob, with a thin dark hairline sealing it to the track. */
    {
        cairo_pattern_t *grad = cairo_pattern_create_radial (
                thumb_cx - thumb_r * 0.30,
                cy        - thumb_r * 0.30,
                thumb_r * 0.10,
                thumb_cx, cy, thumb_r);
        cairo_pattern_add_color_stop_rgb (grad, 0.0, 1.00, 1.00, 1.00);
        cairo_pattern_add_color_stop_rgb (grad, 1.0, 0.80, 0.80, 0.82);
        cairo_set_source (cr, grad);
        cairo_arc (cr, thumb_cx, cy, thumb_r, 0.0, 2.0 * G_PI);
        cairo_fill (cr);
        cairo_pattern_destroy (grad);

        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.45);
        cairo_set_line_width (cr, 0.8);
        cairo_arc (cr, thumb_cx, cy, thumb_r - 0.4, 0.0, 2.0 * G_PI);
        cairo_stroke (cr);
    }
}

static gboolean
pn_switch_widget_draw (GtkWidget *widget, cairo_t *cr)
{
    PnSwitchWidget *self = PN_SWITCH_WIDGET (widget);
    GtkAllocation   alloc;
    double          cx, cy, w, h;

    gtk_widget_get_allocation (widget, &alloc);

    /* No frame or background: the pill is drawn straight onto the
     * transparent allocation, so the panel shows through behind it. */
    cx = alloc.width  * 0.5;
    cy = alloc.height * 0.5;

    h = alloc.height - 2.0 * SWITCH_EDGE_PAD;
    w = alloc.width  - 2.0 * SWITCH_EDGE_PAD;
    if (h < 6.0) h = 6.0;
    /* Keep the pill at least as wide as it is tall so it reads as a track. */
    if (w < h) w = h;

    paint_switch (cr, cx, cy, w, h, self->on);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Interaction                                                        */
/* ------------------------------------------------------------------ */

/* A primary press on an interactive toggle requests a flip: emit ::toggled
 * and consume the event so the parent (the applet's flat panel button) does
 * not also fire its own generic click.  Non-primary buttons propagate so
 * the panel's right-click context menu still works. */
static gboolean
pn_switch_widget_button_press (GtkWidget      *widget,
                               GdkEventButton *event)
{
    PnSwitchWidget *self = PN_SWITCH_WIDGET (widget);

    if (!self->interactive || event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;

    g_signal_emit (self, signals[SIGNAL_TOGGLED], 0);
    return GDK_EVENT_STOP;
}

/* ------------------------------------------------------------------ */
/*  GtkWidget size vfuncs                                              */
/* ------------------------------------------------------------------ */

static void
pn_switch_widget_get_preferred_width (GtkWidget *widget,
                                      gint      *minimum,
                                      gint      *natural)
{
    PnSwitchWidget *self = PN_SWITCH_WIDGET (widget);

    *minimum = *natural = (gint) (self->height * SWITCH_ASPECT + 0.5);
}

static void
pn_switch_widget_get_preferred_height (GtkWidget *widget,
                                       gint      *minimum,
                                       gint      *natural)
{
    *minimum = *natural = PN_SWITCH_WIDGET (widget)->height;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_switch_widget_class_init (PnSwitchWidgetClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    widget_class->draw                 = pn_switch_widget_draw;
    widget_class->button_press_event   = pn_switch_widget_button_press;
    widget_class->get_preferred_width  = pn_switch_widget_get_preferred_width;
    widget_class->get_preferred_height = pn_switch_widget_get_preferred_height;

    /* Emitted on a primary click when the toggle is interactive.  The
     * widget does not change its own state — the consumer flips the real
     * Switch node and pushes the authoritative state back via set_on(). */
    signals[SIGNAL_TOGGLED] =
            g_signal_new ("toggled",
                          G_TYPE_FROM_CLASS (klass),
                          G_SIGNAL_RUN_LAST,
                          0, NULL, NULL, NULL,
                          G_TYPE_NONE, 0);
}

static void
pn_switch_widget_init (PnSwitchWidget *self)
{
    self->on          = FALSE;
    self->height      = 24;
    self->interactive = FALSE;

    /* Windowless by default, like the other panel widgets, so clicks fall
     * through to the parent (e.g. the editor's drag handle).  Going
     * interactive flips this on before realize. */
    gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_switch_widget_new (void)
{
    return g_object_new (PN_TYPE_SWITCH_WIDGET, NULL);
}

void
pn_switch_widget_set_on (PnSwitchWidget *self, gboolean on)
{
    g_return_if_fail (PN_IS_SWITCH_WIDGET (self));

    on = !!on;
    if (on == self->on)
        return;

    self->on = on;
    gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_switch_widget_set_height (PnSwitchWidget *self, gint height)
{
    g_return_if_fail (PN_IS_SWITCH_WIDGET (self));

    if (height < 8)
        height = 8;
    if (height == self->height)
        return;

    self->height = height;
    gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
pn_switch_widget_set_interactive (PnSwitchWidget *self, gboolean interactive)
{
    g_return_if_fail (PN_IS_SWITCH_WIDGET (self));

    interactive = !!interactive;
    if (interactive == self->interactive)
        return;

    self->interactive = interactive;

    /* An interactive toggle needs its own input window to receive the
     * press (a windowless widget's events go to its parent).  Toggling
     * has_window only takes effect before realize, which the applet
     * guarantees by setting this right after construction. */
    if (!gtk_widget_get_realized (GTK_WIDGET (self)))
        gtk_widget_set_has_window (GTK_WIDGET (self), interactive);

    if (interactive)
        gtk_widget_add_events (GTK_WIDGET (self), GDK_BUTTON_PRESS_MASK);
}
