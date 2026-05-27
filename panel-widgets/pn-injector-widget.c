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
/*  PnInjectorWidget — a panel-sized fire-button                       */
/*                                                                     */
/*  A miniature of the PnInject fire tab drawn by pn-worksheet.c: a    */
/*  grey rounded rectangle holding either a themed icon (when one is    */
/*  configured) or a cairo-drawn play triangle, sitting above a soft    */
/*  drop shadow.  A primary press sinks the tab onto the shadow for     */
/*  tactile feedback; the release inside the widget emits ::clicked so  */
/*  the applet can fire the underlying inject node.                     */
/*                                                                     */
/*  Cairo-only, no pipnode library: the applet embeds it directly.     */
/* ------------------------------------------------------------------ */

#include "pn-injector-widget.h"

/* A hair of inset so the shadow has room without clipping at the row edge. */
#define INJECTOR_EDGE_PAD 2.0

/* Corner radius — matches PN_NODE_RADIUS in the worksheet. */
#define INJECTOR_RADIUS   4.0

/* The icon-bearing tab is roughly square; the play-triangle tab is half
 * that wide, just like the worksheet's compact fallback. */
#define INJECTOR_ICON_ASPECT     1.0
#define INJECTOR_TRIANGLE_ASPECT 0.5

/* Press feedback offset — matches the worksheet's PN_INJECT_BUTTON_PRESS_OFFSET. */
#define INJECTOR_PRESS_OFFSET    2.0

enum
{
    SIGNAL_CLICKED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

struct _PnInjectorWidget
{
    GtkDrawingArea parent_instance;

    gchar    *icon_name;   /* themed icon to render, or NULL/"" for triangle */
    gint      height;      /* requested overall pixel height                 */
    gboolean  interactive; /* whether clicks emit ::clicked                  */
    gboolean  pressed;     /* primary button currently held inside the tab   */
};

G_DEFINE_TYPE (PnInjectorWidget, pn_injector_widget, GTK_TYPE_DRAWING_AREA)

/* ------------------------------------------------------------------ */
/*  Painting (ported from pn-worksheet.c's draw_node fire-button path)  */
/* ------------------------------------------------------------------ */

static gboolean
icon_configured (PnInjectorWidget *self)
{
    return self->icon_name != NULL && *self->icon_name != '\0';
}

/** Trace a rounded-rectangle subpath. */
static void
rounded_rect_path (cairo_t *cr,
                   double   x,
                   double   y,
                   double   w,
                   double   h,
                   double   r)
{
    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r,     r, -G_PI_2, 0);
    cairo_arc (cr, x + w - r, y + h - r, r,  0,       G_PI_2);
    cairo_arc (cr, x + r,     y + h - r, r,  G_PI_2,  G_PI);
    cairo_arc (cr, x + r,     y + r,     r,  G_PI,    3.0 * G_PI_2);
    cairo_close_path (cr);
}

/** Soft drop shadow under a rounded rectangle, stacked semi-transparent
 *  silhouettes feathering down-right.  Same recipe as the worksheet's
 *  paint_drop_shadow so the panel button reads the same way. */
static void
paint_drop_shadow (cairo_t *cr, double x, double y, double w, double h, double r)
{
    int i;

    for (i = 0; i < 6; i++)
    {
        const double off = 1.2 + i * 0.72;
        rounded_rect_path (cr, x + off, y + off, w, h, r);
        cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.088);
        cairo_fill (cr);
    }
}

/** Paint a cairo play-triangle centred in (@bx,@by,@bw,@bh).  Used as the
 *  default look when no themed icon is configured. */
static void
paint_play_triangle (cairo_t *cr,
                     double   bx,
                     double   by,
                     double   bw,
                     double   bh)
{
    const double cx = bx + bw * 0.5;
    const double cy = by + bh * 0.5;
    /* A modest triangle so the narrow tab is mostly grey with a small glyph. */
    const double s  = MIN (bw, bh) * 0.40;

    cairo_move_to (cr, cx - s * 0.5, cy - s * 0.6);
    cairo_line_to (cr, cx + s * 0.6, cy);
    cairo_line_to (cr, cx - s * 0.5, cy + s * 0.6);
    cairo_close_path (cr);

    cairo_set_source_rgb (cr, 0.95, 0.95, 0.95);
    cairo_fill (cr);
}

/** Load the configured icon as a pixbuf sized to fit @bw x @bh, or %NULL
 *  if the theme has no such icon (the caller falls back to the triangle). */
static GdkPixbuf *
load_icon_pixbuf (PnInjectorWidget *self, double bw, double bh)
{
    GtkIconTheme *theme;
    /* Slight inset so the icon does not crowd the tab's rounded corners. */
    gint          px = (gint) (MIN (bw, bh) - 6.0);

    if (px < 8)
        px = 8;

    theme = gtk_icon_theme_get_default ();
    return gtk_icon_theme_load_icon (theme, self->icon_name, px,
                                     GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
}

static gboolean
pn_injector_widget_draw (GtkWidget *widget, cairo_t *cr)
{
    PnInjectorWidget *self = PN_INJECTOR_WIDGET (widget);
    GtkAllocation     alloc;
    double            bx, by, bw, bh;
    double            shift;
    GdkPixbuf        *pixbuf = NULL;

    gtk_widget_get_allocation (widget, &alloc);

    /* Lay the tab out inside the allocation, leaving room for the shadow's
     * down-right feather and the press shift so neither gets clipped. */
    bw = alloc.width  - 2.0 * INJECTOR_EDGE_PAD - INJECTOR_PRESS_OFFSET;
    bh = alloc.height - 2.0 * INJECTOR_EDGE_PAD - INJECTOR_PRESS_OFFSET;
    if (bw < 8.0) bw = 8.0;
    if (bh < 8.0) bh = 8.0;

    /* Centre the tab in the allocation rather than anchoring to the
     * top-left: the panel row often gives us more vertical slack than the
     * tab needs (the row is sized to the tallest sibling — usually a
     * legibility-tuned 1.5× icon-size readout), and a top-anchored tab
     * would sit visibly high in that band.  We still reserve press_offset
     * of slack on the bottom-right so the pressed translate has somewhere
     * to land without clipping. */
    shift = self->pressed ? INJECTOR_PRESS_OFFSET : 0.0;
    bx    = (alloc.width  - bw - INJECTOR_PRESS_OFFSET) * 0.5 + shift;
    by    = (alloc.height - bh - INJECTOR_PRESS_OFFSET) * 0.5 + shift;

    if (!self->pressed)
        paint_drop_shadow (cr, bx, by, bw, bh, INJECTOR_RADIUS);

    rounded_rect_path (cr, bx, by, bw, bh, INJECTOR_RADIUS);
    cairo_set_source_rgb (cr, 0.55, 0.55, 0.58);
    cairo_fill_preserve (cr);
    cairo_set_source_rgb (cr, 0.20, 0.20, 0.22);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);

    if (icon_configured (self))
        pixbuf = load_icon_pixbuf (self, bw, bh);

    if (pixbuf != NULL)
    {
        const double iw = (double) gdk_pixbuf_get_width  (pixbuf);
        const double ih = (double) gdk_pixbuf_get_height (pixbuf);
        const double ix = bx + (bw - iw) / 2.0;
        const double iy = by + (bh - ih) / 2.0;

        cairo_save (cr);
        gdk_cairo_set_source_pixbuf (cr, pixbuf, ix, iy);
        cairo_paint (cr);
        cairo_restore (cr);

        g_object_unref (pixbuf);
    }
    else
    {
        /* No icon configured, or the theme refused to load it — fall back
         * to the cairo-drawn play triangle so the button is never blank. */
        paint_play_triangle (cr, bx, by, bw, bh);
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Interaction                                                        */
/* ------------------------------------------------------------------ */

/* A primary press on an interactive tab arms the click: drop into the
 * "pressed" look and consume the event so the panel's flat button does
 * not also send its generic click.  Non-primary buttons propagate so the
 * panel's right-click context menu still works. */
static gboolean
pn_injector_widget_button_press (GtkWidget      *widget,
                                 GdkEventButton *event)
{
    PnInjectorWidget *self = PN_INJECTOR_WIDGET (widget);

    if (!self->interactive || event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;

    self->pressed = TRUE;
    gtk_widget_queue_draw (widget);
    return GDK_EVENT_STOP;
}

/* A primary release inside the tab while pressed fires the click.  A
 * release elsewhere (the pointer drifted off) is just a cancel — the
 * pressed flag is cleared on leave-notify below. */
static gboolean
pn_injector_widget_button_release (GtkWidget      *widget,
                                   GdkEventButton *event)
{
    PnInjectorWidget *self = PN_INJECTOR_WIDGET (widget);
    gboolean          was_pressed;

    if (!self->interactive || event->button != GDK_BUTTON_PRIMARY)
        return GDK_EVENT_PROPAGATE;

    was_pressed = self->pressed;
    self->pressed = FALSE;
    gtk_widget_queue_draw (widget);

    if (was_pressed)
        g_signal_emit (self, signals[SIGNAL_CLICKED], 0);

    return GDK_EVENT_STOP;
}

/* The pointer left the tab while held: drop the press look so the user
 * can see they have moved off and a release here would cancel the fire. */
static gboolean
pn_injector_widget_leave_notify (GtkWidget        *widget,
                                 GdkEventCrossing *event)
{
    PnInjectorWidget *self = PN_INJECTOR_WIDGET (widget);

    (void) event;

    if (self->pressed)
    {
        self->pressed = FALSE;
        gtk_widget_queue_draw (widget);
    }
    return GDK_EVENT_PROPAGATE;
}

/* ------------------------------------------------------------------ */
/*  GtkWidget size vfuncs                                              */
/* ------------------------------------------------------------------ */

static void
pn_injector_widget_get_preferred_width (GtkWidget *widget,
                                        gint      *minimum,
                                        gint      *natural)
{
    PnInjectorWidget *self = PN_INJECTOR_WIDGET (widget);
    gdouble           aspect = icon_configured (self)
                                  ? INJECTOR_ICON_ASPECT
                                  : INJECTOR_TRIANGLE_ASPECT;

    *minimum = *natural = (gint) (self->height * aspect + 0.5);
}

static void
pn_injector_widget_get_preferred_height (GtkWidget *widget,
                                         gint      *minimum,
                                         gint      *natural)
{
    *minimum = *natural = PN_INJECTOR_WIDGET (widget)->height;
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_injector_widget_finalize (GObject *object)
{
    PnInjectorWidget *self = PN_INJECTOR_WIDGET (object);

    g_clear_pointer (&self->icon_name, g_free);

    G_OBJECT_CLASS (pn_injector_widget_parent_class)->finalize (object);
}

static void
pn_injector_widget_class_init (PnInjectorWidgetClass *klass)
{
    GObjectClass   *object_class = G_OBJECT_CLASS (klass);
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    object_class->finalize             = pn_injector_widget_finalize;

    widget_class->draw                 = pn_injector_widget_draw;
    widget_class->button_press_event   = pn_injector_widget_button_press;
    widget_class->button_release_event = pn_injector_widget_button_release;
    widget_class->leave_notify_event   = pn_injector_widget_leave_notify;
    widget_class->get_preferred_width  = pn_injector_widget_get_preferred_width;
    widget_class->get_preferred_height = pn_injector_widget_get_preferred_height;

    /* Emitted on a primary release inside an interactive tab.  The widget
     * does not act on its own — the consumer fires the inject node. */
    signals[SIGNAL_CLICKED] =
            g_signal_new ("clicked",
                          G_TYPE_FROM_CLASS (klass),
                          G_SIGNAL_RUN_LAST,
                          0, NULL, NULL, NULL,
                          G_TYPE_NONE, 0);
}

static void
pn_injector_widget_init (PnInjectorWidget *self)
{
    self->icon_name   = NULL;
    self->height      = 24;
    self->interactive = FALSE;
    self->pressed     = FALSE;

    /* Windowless by default, like the other panel widgets, so clicks fall
     * through to the parent (e.g. the editor's drag handle).  Going
     * interactive flips this on before realize. */
    gtk_widget_set_has_window (GTK_WIDGET (self), FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_injector_widget_new (void)
{
    return g_object_new (PN_TYPE_INJECTOR_WIDGET, NULL);
}

void
pn_injector_widget_set_icon (PnInjectorWidget *self, const gchar *icon_name)
{
    gboolean was_configured;

    g_return_if_fail (PN_IS_INJECTOR_WIDGET (self));

    if (g_strcmp0 (self->icon_name, icon_name) == 0)
        return;

    was_configured = icon_configured (self);

    g_free (self->icon_name);
    self->icon_name = (icon_name != NULL) ? g_strdup (icon_name) : NULL;

    /* Toggling between the triangle (narrow) and icon (square) tab changes
     * the natural width — request a re-layout, not just a repaint. */
    if (was_configured != icon_configured (self))
        gtk_widget_queue_resize (GTK_WIDGET (self));
    else
        gtk_widget_queue_draw (GTK_WIDGET (self));
}

void
pn_injector_widget_set_height (PnInjectorWidget *self, gint height)
{
    g_return_if_fail (PN_IS_INJECTOR_WIDGET (self));

    if (height < 8)
        height = 8;
    if (height == self->height)
        return;

    self->height = height;
    gtk_widget_queue_resize (GTK_WIDGET (self));
}

void
pn_injector_widget_set_interactive (PnInjectorWidget *self, gboolean interactive)
{
    g_return_if_fail (PN_IS_INJECTOR_WIDGET (self));

    interactive = !!interactive;
    if (interactive == self->interactive)
        return;

    self->interactive = interactive;

    /* An interactive tab needs its own input window to receive presses;
     * toggling has_window only takes effect before realize, which the
     * applet guarantees by setting this right after construction. */
    if (!gtk_widget_get_realized (GTK_WIDGET (self)))
        gtk_widget_set_has_window (GTK_WIDGET (self), interactive);

    if (interactive)
        gtk_widget_add_events (GTK_WIDGET (self),
                               GDK_BUTTON_PRESS_MASK
                               | GDK_BUTTON_RELEASE_MASK
                               | GDK_LEAVE_NOTIFY_MASK);
}
