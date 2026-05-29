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

#ifndef PN_INLINE_EDIT_LABEL_H
#define PN_INLINE_EDIT_LABEL_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * PnInlineEditLabel:
 *
 * A drop-in widget that looks like a plain label with a small grey pencil
 * button beside it.  Clicking the pencil turns the label into a #GtkEntry
 * with the text selected; pressing Enter (or moving focus away) commits the
 * change, while Escape cancels it.
 *
 * Internally it swaps a real #GtkLabel and a #GtkEntry inside a #GtkStack --
 * showing a genuine label in display mode means there is no stray text caret
 * or entry focus-frame, which a read-only #GtkEntry cannot reliably suppress.
 *
 * Read the current value with the #PnInlineEditLabel:text property and react
 * to user edits via the #PnInlineEditLabel::committed signal.
 */

#define PN_TYPE_INLINE_EDIT_LABEL (pn_inline_edit_label_get_type ())
G_DECLARE_FINAL_TYPE (PnInlineEditLabel, pn_inline_edit_label,
                      PN, INLINE_EDIT_LABEL, GtkBox)

GtkWidget   *pn_inline_edit_label_new      (void);

void         pn_inline_edit_label_set_text (PnInlineEditLabel *self,
                                            const gchar       *text);
const gchar *pn_inline_edit_label_get_text (PnInlineEditLabel *self);

G_END_DECLS

#endif /* PN_INLINE_EDIT_LABEL_H */
