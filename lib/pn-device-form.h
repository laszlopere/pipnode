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

#ifndef PN_DEVICE_FORM_H
#define PN_DEVICE_FORM_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Free-function form helpers for building device-configuration dialogs
 * -- the read/value rows, key labels, expander sections and enum-combo
 * plumbing the Meshtastic dialog pages grew privately, lifted here so a
 * plugin's GUI companion can build a visually identical dialog without
 * re-implementing the idioms.  (The node-dialog counterpart is
 * pn-node-dialog-helpers.h.)
 *
 * The helpers are deliberately a grab-bag of free functions over plain
 * GtkGrid / GtkBox, not a widget subclass: a page POPULATES a grid with
 * rows in whatever order it likes, mixing read-only value rows, entry
 * rows and caller-built controls (combos, spinners, switches) in one
 * grid.  Combos and spinners should be #PnDeviceCombo / #PnDeviceSpin
 * (scroll-safe) packed into the cell returned by
 * pn_device_form_attach_control_row(). */

/* ------------------------------------------------------------------ */
/*  Section scaffolding                                                 */
/* ------------------------------------------------------------------ */

/* A bold, left-aligned key label with a trailing margin -- the left
 * column of every "key | value" or "key | control" row.  Exposed on
 * its own for callers that lay out a row by hand. */
GtkWidget *pn_device_form_key_label (const gchar *text);

/* Build one scrollable tab body: a vertical box of expander sections
 * inside a vertically-scrolling #GtkScrolledWindow.  Returns the
 * scrolled window (append it to a #GtkNotebook); *@inner_box_out is set
 * to the inner box -- pass it to pn_device_form_add_section() for each
 * sub-section. */
GtkWidget *pn_device_form_new_tab (GtkWidget **inner_box_out);

/* Wrap @child in a #GtkExpander titled @title (rendered bold, expanded
 * by default) and pack it into @parent (an inner box from
 * pn_device_form_new_tab()). */
void       pn_device_form_add_section (GtkWidget   *parent,
                                       const gchar *title,
                                       GtkWidget   *child);

/* ------------------------------------------------------------------ */
/*  Rows                                                                */
/* ------------------------------------------------------------------ */

/* Read-only "key | value" row at @row of @grid: a bold key on the left
 * and a selectable, ellipsizing value label on the right (initialised
 * to the "—" placeholder).  Returns the value label; repaint it with
 * pn_device_form_set_value(). */
GtkLabel  *pn_device_form_attach_label_row (GtkGrid     *grid,
                                            gint         row,
                                            const gchar *key_text);

/* Set a value label's text, substituting the "—" placeholder for a NULL
 * or empty string. */
void       pn_device_form_set_value (GtkLabel    *label,
                                     const gchar *text);

/* "key | control" row at @row of @grid: a bold key on the left and an
 * empty horizontal holder cell on the right.  Pack the control (combo,
 * spinner, switch, entry + suffix label, …) into the returned cell with
 * gtk_box_pack_start().  Non-switch controls realize into a per-grid
 * size group so the value column lines up across rows. */
GtkWidget *pn_device_form_attach_control_row (GtkGrid     *grid,
                                              gint         row,
                                              const gchar *key_text);

/* Convenience "key | entry" row: builds a control row, packs a
 * #GtkEntry into it and returns the entry. */
GtkEntry  *pn_device_form_attach_entry_row (GtkGrid     *grid,
                                            gint         row,
                                            const gchar *key_text);

/* ------------------------------------------------------------------ */
/*  Enum combos                                                         */
/* ------------------------------------------------------------------ */

/* One (enum value, display name) pair for a combo built from a static
 * table.  The combo carries the stringified @id as each row's id, so
 * the firmware enum value round-trips regardless of display order. */
typedef struct
{
    guint32      id;
    const gchar *name;
} PnDeviceEnumEntry;

/* Append every row of @table to @combo (a #GtkComboBoxText, typically a
 * #PnDeviceCombo). */
void       pn_device_form_combo_fill (GtkComboBoxText         *combo,
                                      const PnDeviceEnumEntry *table,
                                      gsize                    n);

/* Select the row whose enum id is @value.  If @value is not in @table,
 * a synthetic "(unknown #N)" row is appended and selected so a value
 * from newer firmware is surfaced rather than silently snapped away. */
void       pn_device_form_combo_select (GtkComboBoxText         *combo,
                                        const PnDeviceEnumEntry *table,
                                        gsize                    n,
                                        guint32                  value);

/* The enum id of the selected row, or @fallback if nothing is
 * selected. */
guint32    pn_device_form_combo_get_id (GtkComboBoxText *combo,
                                        guint32          fallback);

G_END_DECLS

#endif /* PN_DEVICE_FORM_H */
