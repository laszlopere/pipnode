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

#include "pn-device-spin.h"

#include <math.h>

/* GtkSpinButton caps the displayed digits at 20 (its private
 * MAX_DIGITS); mirror that so derived ranges match GTK exactly. */
#define PN_DEVICE_SPIN_MAX_DIGITS 20

struct _PnDeviceSpin
{
    GtkSpinButton parent_instance;
};

G_DEFINE_TYPE (PnDeviceSpin, pn_device_spin, GTK_TYPE_SPIN_BUTTON)

/* Swallow scroll without stepping the value.  GtkSpinButton's own
 * scroll_event handler increments/decrements on every wheel-tick; by
 * overriding it and NOT chaining up we leave the value alone.
 * Returning GDK_EVENT_PROPAGATE lets the event bubble to the enclosing
 * scrolled window, so wheel-scrolling the dialog still works with the
 * pointer over the spinner. */
static gboolean
pn_device_spin_scroll_event (GtkWidget *widget, GdkEventScroll *event)
{
    (void) widget;
    (void) event;
    return GDK_EVENT_PROPAGATE;
}

static void
pn_device_spin_class_init (PnDeviceSpinClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    widget_class->scroll_event = pn_device_spin_scroll_event;
}

static void
pn_device_spin_init (PnDeviceSpin *self)
{
    (void) self;
}

GtkWidget *
pn_device_spin_new_with_range (gdouble min, gdouble max, gdouble step)
{
    GtkSpinButton *spin;
    GtkAdjustment *adjustment;
    gint           digits;

    g_return_val_if_fail (min <= max, NULL);
    g_return_val_if_fail (step != 0.0, NULL);

    spin = g_object_new (PN_TYPE_DEVICE_SPIN, NULL);

    adjustment = gtk_adjustment_new (min, min, max, step, 10 * step, 0);

    /* Same digit-count derivation as gtk_spin_button_new_with_range (). */
    if (fabs (step) >= 1.0 || step == 0.0)
        digits = 0;
    else
    {
        digits = abs ((gint) floor (log10 (fabs (step))));
        if (digits > PN_DEVICE_SPIN_MAX_DIGITS)
            digits = PN_DEVICE_SPIN_MAX_DIGITS;
    }

    gtk_spin_button_configure (spin, adjustment, step, digits);
    gtk_spin_button_set_numeric (spin, TRUE);

    return GTK_WIDGET (spin);
}
