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

#ifndef PN_CURRENCY_EDITORS_H
#define PN_CURRENCY_EDITORS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Shared settings-dialog editors for the currency nodes.             */
/*                                                                     */
/*  Two property editors are wanted by every node that quotes money:   */
/*  a picker that shows a currency's icon next to its ticker, and a    */
/*  read-only row for the values the node fetches for itself.  They    */
/*  started out inside pn-rate-gui.c; the Bridge Quote node wants the  */
/*  same two, so they live here rather than being copied.              */
/*                                                                     */
/*  Internal to the gui library — not an installed header.             */
/* ------------------------------------------------------------------ */

/**
 * pn_currency_editor_new:
 * @target: the object whose property is being edited
 * @pspec:  the property's #GParamSpec; its value type must be a #GEnum
 *          whose members are #PnCurrency values
 *
 * Builds a combo listing every value of the enum with the currency's
 * bundled icon beside its ticker — faster to scan than 23 bare labels.
 * Icons come from `$datadir/pipnode/icons/<nick>.png` (or the in-tree
 * `data/icons/` when running uninstalled); a missing PNG leaves a blank
 * gap so the remaining rows still line up.  The combo tracks the
 * property both ways, and is insensitive when @pspec is read-only.
 *
 * Returns: (transfer floating): the editor widget.
 */
GtkWidget *pn_currency_editor_new (GObject    *target,
                                   GParamSpec *pspec);

/**
 * pn_readonly_label_editor_new:
 * @target:     the object whose property is being displayed
 * @pspec:      the property's #GParamSpec
 * @empty_text: (nullable): what to show when a string property is empty,
 *              e.g. "Never" for a timestamp that has not been set yet
 *
 * Builds a selectable monospace label bound to @target's property, for
 * values the node writes for itself and the user only reads.  A
 * spinbutton is the wrong editor for these twice over: its default two
 * decimal digits render a rate of 7.5e-06 as a misleading "0.00", and
 * nothing stops the user clobbering a fetched value by accident.
 *
 * Doubles are formatted with %g, strings verbatim, anything else through
 * g_strdup_value_contents().
 *
 * Returns: (transfer floating): the editor widget.
 */
GtkWidget *pn_readonly_label_editor_new (GObject     *target,
                                         GParamSpec  *pspec,
                                         const gchar *empty_text);

G_END_DECLS

#endif /* PN_CURRENCY_EDITORS_H */
