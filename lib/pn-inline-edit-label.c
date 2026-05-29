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

#include "pn-inline-edit-label.h"

/* Standard symbolic "pencil"; present in the symbolic icon set shipped with
 * GTK.  Reads as a subtle grey glyph beside the label. */
#define PN_INLINE_EDIT_ICON "document-edit-symbolic"

struct _PnInlineEditLabel
{
    GtkBox    parent_instance;

    GtkStack *stack;   /* swaps between the two pages below */
    GtkLabel *label;   /* display mode -- the single source of truth */
    GtkEntry *entry;   /* edit mode */
    gboolean  editing;
};

G_DEFINE_TYPE (PnInlineEditLabel, pn_inline_edit_label, GTK_TYPE_BOX)

enum
{
    PROP_0,
    PROP_TEXT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

enum
{
    SIG_COMMITTED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* --- edit-mode transitions -------------------------------------------- */

static void
begin_edit (PnInlineEditLabel *self)
{
    if (self->editing)
        return;
    self->editing = TRUE;

    gtk_entry_set_text (self->entry, gtk_label_get_text (self->label));
    gtk_stack_set_visible_child_name (self->stack, "edit");
    gtk_widget_grab_focus (GTK_WIDGET (self->entry));
    gtk_editable_select_region (GTK_EDITABLE (self->entry), 0, -1);
}

static void
do_commit (PnInlineEditLabel *self)
{
    gchar    *text;
    gboolean  changed;

    if (!self->editing)
        return;
    self->editing = FALSE;   /* set first: switching pages re-enters focus-out */

    text    = g_strdup (gtk_entry_get_text (self->entry));
    changed = g_strcmp0 (text, gtk_label_get_text (self->label)) != 0;

    gtk_stack_set_visible_child_name (self->stack, "label");

    if (changed) {
        gtk_label_set_text (self->label, text);
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TEXT]);
    }
    g_signal_emit (self, signals[SIG_COMMITTED], 0, text);
    g_free (text);
}

static void
do_cancel (PnInlineEditLabel *self)
{
    if (!self->editing)
        return;
    self->editing = FALSE;
    gtk_stack_set_visible_child_name (self->stack, "label");
}

/* --- widget callbacks ------------------------------------------------- */

static void
on_pencil_clicked (GtkButton *btn, gpointer user_data)
{
    (void) btn;
    begin_edit (PN_INLINE_EDIT_LABEL (user_data));
}

static void
on_entry_activate (GtkEntry *entry, gpointer user_data)
{
    (void) entry;
    do_commit (PN_INLINE_EDIT_LABEL (user_data));
}

static gboolean
on_entry_key_press (GtkWidget *widget, GdkEventKey *ev, gpointer user_data)
{
    (void) widget;
    if (ev->keyval == GDK_KEY_Escape) {
        do_cancel (PN_INLINE_EDIT_LABEL (user_data));
        return GDK_EVENT_STOP;
    }
    return GDK_EVENT_PROPAGATE;
}

static gboolean
on_entry_focus_out (GtkWidget *widget, GdkEventFocus *ev, gpointer user_data)
{
    (void) widget;
    (void) ev;
    /* Clicking away from the editor commits.  do_commit() no-ops when Enter or
     * Escape already left edit mode, so this never double-fires. */
    do_commit (PN_INLINE_EDIT_LABEL (user_data));
    return GDK_EVENT_PROPAGATE;
}

/* --- GObject ---------------------------------------------------------- */

static void
pn_inline_edit_label_get_property (GObject    *object,
                                   guint       prop_id,
                                   GValue     *value,
                                   GParamSpec *pspec)
{
    PnInlineEditLabel *self = PN_INLINE_EDIT_LABEL (object);

    switch (prop_id) {
    case PROP_TEXT:
        g_value_set_string (value, gtk_label_get_text (self->label));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_inline_edit_label_set_property (GObject      *object,
                                   guint         prop_id,
                                   const GValue *value,
                                   GParamSpec   *pspec)
{
    PnInlineEditLabel *self = PN_INLINE_EDIT_LABEL (object);

    switch (prop_id) {
    case PROP_TEXT:
        pn_inline_edit_label_set_text (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_inline_edit_label_class_init (PnInlineEditLabelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = pn_inline_edit_label_get_property;
    object_class->set_property = pn_inline_edit_label_set_property;

    /**
     * PnInlineEditLabel:text:
     *
     * The displayed value, and the text seeded into the editor when the user
     * starts editing.  Setting it programmatically updates the label but does
     * not emit #PnInlineEditLabel::committed.
     */
    props[PROP_TEXT] = g_param_spec_string (
            "text", "Text", "The displayed and edited text.",
            "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties (object_class, N_PROPS, props);

    /**
     * PnInlineEditLabel::committed:
     * @self: the widget
     * @text: the value after the edit was confirmed
     *
     * Emitted when the user confirms an edit, either by pressing Enter or by
     * moving focus away from the editor.  It is not emitted when the edit is
     * cancelled with Escape.
     */
    signals[SIG_COMMITTED] = g_signal_new (
            "committed",
            PN_TYPE_INLINE_EDIT_LABEL,
            G_SIGNAL_RUN_LAST,
            0,                  /* no class handler vfunc slot */
            NULL, NULL, NULL,   /* no accumulator, no marshaller */
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);
}

static void
pn_inline_edit_label_init (PnInlineEditLabel *self)
{
    GtkWidget *stack, *row, *label, *pencil, *entry;

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_HORIZONTAL);

    stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (stack),
                                   GTK_STACK_TRANSITION_TYPE_NONE);
    /* Reserve the tallest page's height so the row does not jump on edit. */
    gtk_stack_set_hhomogeneous (GTK_STACK (stack), FALSE);
    self->stack = GTK_STACK (stack);

    /* display page: label + flat pencil button */
    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

    label = gtk_label_new ("");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_widget_set_halign (label, GTK_ALIGN_START);
    self->label = GTK_LABEL (label);
    gtk_box_pack_start (GTK_BOX (row), label, TRUE, TRUE, 0);

    pencil = gtk_button_new_from_icon_name (PN_INLINE_EDIT_ICON,
                                            GTK_ICON_SIZE_MENU);
    gtk_button_set_relief (GTK_BUTTON (pencil), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (pencil, FALSE);
    gtk_widget_set_can_focus (pencil, FALSE);
    gtk_widget_set_valign (pencil, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (pencil, "Edit");
    gtk_box_pack_start (GTK_BOX (row), pencil, FALSE, FALSE, 0);

    /* edit page: the entry */
    entry = gtk_entry_new ();
    self->entry = GTK_ENTRY (entry);

    gtk_stack_add_named (GTK_STACK (stack), row,   "label");
    gtk_stack_add_named (GTK_STACK (stack), entry, "edit");
    gtk_stack_set_visible_child_name (GTK_STACK (stack), "label");

    gtk_box_pack_start (GTK_BOX (self), stack, TRUE, TRUE, 0);
    gtk_widget_show_all (stack);

    g_signal_connect (pencil, "clicked",
                      G_CALLBACK (on_pencil_clicked), self);
    g_signal_connect (entry, "activate",
                      G_CALLBACK (on_entry_activate), self);
    g_signal_connect (entry, "key-press-event",
                      G_CALLBACK (on_entry_key_press), self);
    g_signal_connect (entry, "focus-out-event",
                      G_CALLBACK (on_entry_focus_out), self);
}

/* --- public API ------------------------------------------------------- */

GtkWidget *
pn_inline_edit_label_new (void)
{
    return g_object_new (PN_TYPE_INLINE_EDIT_LABEL, NULL);
}

void
pn_inline_edit_label_set_text (PnInlineEditLabel *self, const gchar *text)
{
    g_return_if_fail (PN_IS_INLINE_EDIT_LABEL (self));

    if (text == NULL)
        text = "";
    if (g_strcmp0 (text, gtk_label_get_text (self->label)) == 0)
        return;

    gtk_label_set_text (self->label, text);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TEXT]);
}

const gchar *
pn_inline_edit_label_get_text (PnInlineEditLabel *self)
{
    g_return_val_if_fail (PN_IS_INLINE_EDIT_LABEL (self), NULL);

    return gtk_label_get_text (self->label);
}

void
pn_inline_edit_label_set_max_length (PnInlineEditLabel *self, gint max_length)
{
    g_return_if_fail (PN_IS_INLINE_EDIT_LABEL (self));

    gtk_entry_set_max_length (self->entry, max_length);
}
