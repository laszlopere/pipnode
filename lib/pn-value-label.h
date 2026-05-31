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

#ifndef PN_VALUE_LABEL_H
#define PN_VALUE_LABEL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* A read-only "key: value" field as a single packable widget.
 *
 * Device dialogs show a configuration value the user cannot edit -- a
 * firmware version, a coordinator model, a connection state -- as a bold
 * key beside a selectable value that falls back to the "—" placeholder
 * when empty.  pn_device_form_attach_label_row() bakes exactly this row,
 * but only into a #GtkGrid at a given row.  #PnValueLabel is the same
 * field as a standalone #GtkWidget you can pack anywhere -- a #GtkBox, a
 * #PnFoldable body, an action bar -- without owning a grid, so a plugin
 * can drop a single read-out next to a combo or button.
 *
 * It is a #GtkBox (horizontal) carrying a "pn-value-label" CSS class, with
 * a bold key label and a left-aligned, selectable, ellipsizing value
 * label that hexpands to fill the row. */

#define PN_TYPE_VALUE_LABEL (pn_value_label_get_type ())
G_DECLARE_FINAL_TYPE (PnValueLabel, pn_value_label, PN, VALUE_LABEL, GtkBox)

/* A field labelled @key (bold), with its value initialised to the "—"
 * placeholder. */
GtkWidget   *pn_value_label_new (const gchar *key);

/* Set the value text, substituting the "—" placeholder for a NULL or
 * empty string (matches pn_device_form_set_value()). */
void         pn_value_label_set_value (PnValueLabel *self,
                                       const gchar  *text);

/* Set the value from a Pango-markup string verbatim (no placeholder
 * substitution) -- for a coloured state bullet or other inline markup. */
void         pn_value_label_set_markup (PnValueLabel *self,
                                        const gchar  *markup);

/* The current value text (the raw label text, i.e. "—" when empty). */
const gchar *pn_value_label_get_value (PnValueLabel *self);

G_END_DECLS

#endif /* PN_VALUE_LABEL_H */
