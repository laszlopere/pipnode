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

#include "pn-foldable.h"

struct _PnFoldable
{
    GtkExpander  parent_instance;

    /* The folded body.  The expander's single child; its first child is
     * the mandatory description label, with caller content packed after.
     * Owned via the expander (its GtkBin child); not a separate ref. */
    GtkWidget   *content;
};

G_DEFINE_TYPE (PnFoldable, pn_foldable, GTK_TYPE_EXPANDER)

/* Forward gtk_container_add() into the body so caller content lands after
 * the description, not as the expander's (single) child.  During
 * construction self->content is still NULL and the body box is attached
 * through the parent class vfunc, so that initial add bypasses this. */
static void
pn_foldable_add (GtkContainer *container, GtkWidget *child)
{
    PnFoldable *self = PN_FOLDABLE (container);

    if (self->content != NULL)
        gtk_container_add (GTK_CONTAINER (self->content), child);
    else
        GTK_CONTAINER_CLASS (pn_foldable_parent_class)->add (container,
                                                             child);
}

/* The mirror of pn_foldable_add(): body children are removed from the
 * body; the body box itself (e.g. during destroy) falls through to the
 * expander. */
static void
pn_foldable_remove (GtkContainer *container, GtkWidget *child)
{
    PnFoldable *self = PN_FOLDABLE (container);

    if (self->content != NULL && child != self->content)
        gtk_container_remove (GTK_CONTAINER (self->content), child);
    else
        GTK_CONTAINER_CLASS (pn_foldable_parent_class)->remove (container,
                                                                child);
}

static void
pn_foldable_class_init (PnFoldableClass *klass)
{
    GtkContainerClass *container_class = GTK_CONTAINER_CLASS (klass);

    container_class->add    = pn_foldable_add;
    container_class->remove = pn_foldable_remove;
}

static void
pn_foldable_init (PnFoldable *self)
{
    self->content = NULL;
    gtk_style_context_add_class (gtk_widget_get_style_context (GTK_WIDGET (self)),
                                 "pn-foldable");
}

GtkWidget *
pn_foldable_new (const gchar *title,
                 const gchar *description)
{
    PnFoldable *self;
    GtkWidget  *heading;
    GtkWidget  *desc;
    gchar      *markup;

    g_return_val_if_fail (description != NULL, NULL);

    self = g_object_new (PN_TYPE_FOLDABLE, NULL);

    /* Bold heading reads as a section title rather than a plain expander
     * caption, and stays legible next to the disclosure arrow. */
    heading = gtk_label_new (NULL);
    markup  = g_markup_printf_escaped ("<b>%s</b>", title != NULL ? title : "");
    gtk_label_set_markup (GTK_LABEL (heading), markup);
    g_free (markup);
    gtk_expander_set_label_widget (GTK_EXPANDER (self), heading);
    gtk_expander_set_expanded (GTK_EXPANDER (self), TRUE);

    /* Body box.  Its first child is the mandatory description line. */
    self->content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

    desc = gtk_label_new (description);
    gtk_label_set_xalign    (GTK_LABEL (desc), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (desc), TRUE);
    gtk_style_context_add_class (gtk_widget_get_style_context (desc),
                                 "pn-foldable-description");
    gtk_style_context_add_class (gtk_widget_get_style_context (desc),
                                 "dim-label");
    gtk_box_pack_start (GTK_BOX (self->content), desc, FALSE, FALSE, 0);

    /* Attach the body through the parent vfunc so pn_foldable_add() does
     * not redirect it into itself. */
    GTK_CONTAINER_CLASS (pn_foldable_parent_class)->add (
            GTK_CONTAINER (self), self->content);

    return GTK_WIDGET (self);
}

GtkWidget *
pn_foldable_get_content_area (PnFoldable *self)
{
    g_return_val_if_fail (PN_IS_FOLDABLE (self), NULL);
    return self->content;
}
