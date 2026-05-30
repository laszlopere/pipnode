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

#include "pn-device-combo.h"

struct _PnDeviceCombo
{
    GtkComboBoxText parent_instance;
};

G_DEFINE_TYPE (PnDeviceCombo, pn_device_combo, GTK_TYPE_COMBO_BOX_TEXT)

/* Swallow scroll without touching the selection.  GtkComboBox's own
 * scroll_event handler steps the active item on every wheel-tick; by
 * overriding it and NOT chaining up we leave the value alone.
 * Returning GDK_EVENT_PROPAGATE lets the event bubble to the enclosing
 * scrolled window, so wheel-scrolling the dialog still works with the
 * pointer over the combo. */
static gboolean
pn_device_combo_scroll_event (GtkWidget *widget, GdkEventScroll *event)
{
    (void) widget;
    (void) event;
    return GDK_EVENT_PROPAGATE;
}

static void
pn_device_combo_class_init (PnDeviceComboClass *klass)
{
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

    widget_class->scroll_event = pn_device_combo_scroll_event;
}

static void
pn_device_combo_init (PnDeviceCombo *self)
{
    (void) self;
}

GtkWidget *
pn_device_combo_new (void)
{
    /* The inherited GtkComboBoxText constructor wires up the text
     * model + cell renderer, so this is a straight swap for
     * gtk_combo_box_text_new (). */
    return GTK_WIDGET (g_object_new (PN_TYPE_DEVICE_COMBO, NULL));
}
