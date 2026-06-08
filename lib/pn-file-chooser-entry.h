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

#ifndef PN_FILE_CHOOSER_ENTRY_H
#define PN_FILE_CHOOSER_ENTRY_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * PnFileChooserEntry:
 *
 * A drop-in widget for editing a file path: an ordinary, freely editable
 * #GtkEntry with a small "open folder" button to its right.  Clicking the
 * button pops up a #GtkFileChooserDialog seeded with the entry's current
 * value; picking a file writes its path back into the entry.  The user can
 * also type or paste a path directly, so the widget works just as well when
 * the file lives on a remote/headless machine the chooser cannot browse.
 *
 * It is the file-path counterpart to #PnInlineEditLabel: a reusable editor
 * the host's Credentials dialog uses for %PN_FIELD_FILE fields, and which
 * plugins may drop into their own dialogs.  Read the value through the
 * #PnFileChooserEntry:text property and react to edits via
 * #PnFileChooserEntry::changed.
 */

#define PN_TYPE_FILE_CHOOSER_ENTRY (pn_file_chooser_entry_get_type ())
G_DECLARE_FINAL_TYPE (PnFileChooserEntry, pn_file_chooser_entry,
                      PN, FILE_CHOOSER_ENTRY, GtkBox)

GtkWidget   *pn_file_chooser_entry_new      (void);

void         pn_file_chooser_entry_set_text (PnFileChooserEntry *self,
                                             const gchar        *text);
const gchar *pn_file_chooser_entry_get_text (PnFileChooserEntry *self);

/* Title shown on the file-chooser dialog the browse button opens
 * (default "Select File").  The string is copied; %NULL restores the
 * default. */
void         pn_file_chooser_entry_set_title (PnFileChooserEntry *self,
                                              const gchar        *title);

/* Which kind of chooser the browse button opens — %GTK_FILE_CHOOSER_ACTION_OPEN
 * (the default) to pick an existing file, or %GTK_FILE_CHOOSER_ACTION_SAVE to
 * name one to write. */
void         pn_file_chooser_entry_set_action (PnFileChooserEntry  *self,
                                               GtkFileChooserAction action);

/* Offer @filter as a selectable name-filter in the chooser dialog (e.g. one
 * matching "*.pem").  Add several to let the user switch between them; the
 * first added is selected initially.  The widget takes ownership of @filter
 * (it sinks the floating reference), so callers need not unref it. */
void         pn_file_chooser_entry_add_filter (PnFileChooserEntry *self,
                                               GtkFileFilter      *filter);

G_END_DECLS

#endif /* PN_FILE_CHOOSER_ENTRY_H */
