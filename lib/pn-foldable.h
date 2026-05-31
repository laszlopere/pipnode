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

#ifndef PN_FOLDABLE_H
#define PN_FOLDABLE_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A collapsible section ("foldable") with one consistent look across
 * every pipnode dialog.
 *
 * Device dialogs -- and the Meshtastic dialog before it -- grew a private
 * copy of the same idiom over and over: a #GtkExpander with a bold title
 * label and, as the first thing inside the folded body, a dim, wrapping
 * line of explanatory text describing what the section is for.  This
 * widget is that idiom promoted to a single reusable type so host and
 * plugin dialogs share one style (controlled here, not re-tuned per call
 * site) and so the explanatory line is no longer optional.
 *
 * The @description passed to pn_foldable_new() is MANDATORY: it becomes
 * the very first widget inside the folded area -- a dim, line-wrapped
 * label above whatever content the caller packs in afterwards.  This
 * keeps every foldable self-documenting.
 *
 * #PnFoldable is itself the content container: gtk_container_add() (and
 * gtk_container_remove()) forward into the body so additions land *after*
 * the description.  For control over packing options use
 * pn_foldable_get_content_area() and gtk_box_pack_start() on the returned
 * vertical #GtkBox directly.
 *
 * The widget carries the "pn-foldable" CSS class and the description label
 * the "pn-foldable-description" class (alongside "dim-label"), so the look
 * can be themed centrally. */

#define PN_TYPE_FOLDABLE (pn_foldable_get_type ())
G_DECLARE_FINAL_TYPE (PnFoldable, pn_foldable, PN, FOLDABLE, GtkExpander)

/* Create a foldable titled @title (rendered bold; may be %NULL for no
 * heading text) whose folded body opens with the mandatory @description
 * line.  Expanded by default; flip with gtk_expander_set_expanded().
 * Pack content in with gtk_container_add() or via the content area. */
GtkWidget *pn_foldable_new (const gchar *title,
                            const gchar *description);

/* The vertical #GtkBox holding the body.  Its first child is the
 * description label; pack further content after it with
 * gtk_box_pack_start().  Owned by the foldable -- do not unref. */
GtkWidget *pn_foldable_get_content_area (PnFoldable *self);

G_END_DECLS

#endif /* PN_FOLDABLE_H */
