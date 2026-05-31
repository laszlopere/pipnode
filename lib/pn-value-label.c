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
/*  PnValueLabel -- a standalone read-only "key: value" field.         */
/*                                                                     */
/*  The same look pn_device_form_attach_label_row() bakes into a grid  */
/*  row, lifted into a packable widget so a plugin can drop a single   */
/*  read-out anywhere without owning a grid.                           */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-value-label.h"

struct _PnValueLabel
{
    GtkBox     parent_instance;
    GtkWidget *value;   /* the right-hand value label (owned by the box) */
};

G_DEFINE_TYPE (PnValueLabel, pn_value_label, GTK_TYPE_BOX)

static void
pn_value_label_class_init (PnValueLabelClass *klass)
{
    (void) klass;
}

static void
pn_value_label_init (PnValueLabel *self)
{
    GtkStyleContext *sc;

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_HORIZONTAL);
    sc = gtk_widget_get_style_context (GTK_WIDGET (self));
    gtk_style_context_add_class (sc, "pn-value-label");
}

GtkWidget *
pn_value_label_new (const gchar *key)
{
    PnValueLabel  *self  = g_object_new (PN_TYPE_VALUE_LABEL, NULL);
    GtkWidget     *klabel = gtk_label_new (key);
    PangoAttrList *attrs  = pango_attr_list_new ();

    /* Bold, left-aligned key with a trailing gap -- matches
     * pn_device_form_key_label() so a PnValueLabel reads identically to a
     * grid value row. */
    gtk_label_set_xalign (GTK_LABEL (klabel), 0.0);
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes (GTK_LABEL (klabel), attrs);
    pango_attr_list_unref (attrs);
    gtk_widget_set_margin_end (klabel, 16);
    gtk_box_pack_start (GTK_BOX (self), klabel, FALSE, FALSE, 0);

    /* Selectable, ellipsizing value that fills the rest of the row,
     * starting on the "—" placeholder. */
    self->value = gtk_label_new ("—");
    gtk_label_set_xalign     (GTK_LABEL (self->value), 0.0);
    gtk_label_set_selectable (GTK_LABEL (self->value), TRUE);
    gtk_label_set_ellipsize  (GTK_LABEL (self->value), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand   (self->value, TRUE);
    gtk_box_pack_start (GTK_BOX (self), self->value, TRUE, TRUE, 0);

    return GTK_WIDGET (self);
}

void
pn_value_label_set_value (PnValueLabel *self,
                          const gchar  *text)
{
    g_return_if_fail (PN_IS_VALUE_LABEL (self));
    gtk_label_set_text (GTK_LABEL (self->value),
                        (text != NULL && *text != '\0') ? text : "—");
}

void
pn_value_label_set_markup (PnValueLabel *self,
                           const gchar  *markup)
{
    g_return_if_fail (PN_IS_VALUE_LABEL (self));
    gtk_label_set_markup (GTK_LABEL (self->value), markup != NULL ? markup : "");
}

const gchar *
pn_value_label_get_value (PnValueLabel *self)
{
    g_return_val_if_fail (PN_IS_VALUE_LABEL (self), NULL);
    return gtk_label_get_text (GTK_LABEL (self->value));
}
