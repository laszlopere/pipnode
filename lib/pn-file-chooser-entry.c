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

#include "pn-file-chooser-entry.h"

/* Full-colour "open folder" from the freedesktop icon naming spec (the
 * coloured counterpart of document-open-symbolic): a recognisable browse
 * affordance that stands out from the monochrome entry text beside it. */
#define PN_FILE_CHOOSER_ICON "document-open"

struct _PnFileChooserEntry
{
    GtkBox    parent_instance;

    GtkEntry *entry;      /* the path, freely editable                  */
    gchar    *committed;  /* the value last announced via :text/::changed */

    gchar               *title;    /* chooser dialog title (owned, or NULL) */
    GtkFileChooserAction action;   /* OPEN (default) / SAVE                  */
    GPtrArray           *filters;  /* of GtkFileFilter*, refs held (or NULL) */
};

G_DEFINE_TYPE (PnFileChooserEntry, pn_file_chooser_entry, GTK_TYPE_BOX)

enum
{
    PROP_0,
    PROP_TEXT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

enum
{
    SIG_CHANGED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* --- widget callbacks ------------------------------------------------- */

/* Announce the entry's text when it differs from what was last
 * announced.  Edits are committed, not streamed: a bound path property
 * acts on its value (a Pipe node creates a FIFO there, a Logger opens a
 * file), so every keystroke prefix of a typed path must not reach it. */
static void
commit (PnFileChooserEntry *self)
{
    const gchar *text = gtk_entry_get_text (self->entry);

    if (g_strcmp0 (text, self->committed) == 0)
        return;

    g_free (self->committed);
    self->committed = g_strdup (text);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TEXT]);
    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

static void
on_entry_activate (GtkEntry *entry, gpointer user_data)
{
    (void) entry;
    commit (PN_FILE_CHOOSER_ENTRY (user_data));
}

static gboolean
on_entry_focus_out (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void) widget;
    (void) event;
    commit (PN_FILE_CHOOSER_ENTRY (user_data));
    return GDK_EVENT_PROPAGATE;
}

/* Closing the dialog with the caret still in the entry does not always
 * deliver focus-out first; unmap does, while bindings are still alive. */
static void
on_unmap (GtkWidget *widget, gpointer user_data)
{
    (void) user_data;
    commit (PN_FILE_CHOOSER_ENTRY (widget));
}

static void
on_browse_clicked (GtkButton *btn, gpointer user_data)
{
    PnFileChooserEntry *self = PN_FILE_CHOOSER_ENTRY (user_data);
    GtkWidget          *top  = gtk_widget_get_toplevel (GTK_WIDGET (self));
    GtkWindow          *parent = GTK_IS_WINDOW (top) ? GTK_WINDOW (top) : NULL;
    const gchar        *accept = self->action == GTK_FILE_CHOOSER_ACTION_SAVE
                                 ? "_Save" : "_Open";
    const gchar        *cur;
    GtkWidget          *dialog;

    (void) btn;

    dialog = gtk_file_chooser_dialog_new (
            self->title != NULL ? self->title : "Select File",
            parent,
            self->action,
            "_Cancel", GTK_RESPONSE_CANCEL,
            accept,    GTK_RESPONSE_ACCEPT,
            NULL);

    if (self->filters != NULL)
    {
        guint i;
        for (i = 0; i < self->filters->len; i++)
            gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog),
                                         g_ptr_array_index (self->filters, i));
    }

    /* Seed the dialog with the current path so re-browsing starts where the
     * user left off.  An absolute path selects the file; anything else just
     * pre-fills the name field on a save. */
    cur = gtk_entry_get_text (self->entry);
    if (cur != NULL && *cur != '\0')
    {
        if (g_path_is_absolute (cur))
            gtk_file_chooser_set_filename (GTK_FILE_CHOOSER (dialog), cur);
        else if (self->action == GTK_FILE_CHOOSER_ACTION_SAVE)
            gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER (dialog), cur);
    }

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
        gchar *filename =
                gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
        if (filename != NULL)
        {
            gtk_entry_set_text (self->entry, filename);
            commit (self);
        }
        g_free (filename);
    }

    gtk_widget_destroy (dialog);
}

/* --- GObject ---------------------------------------------------------- */

static void
pn_file_chooser_entry_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
    PnFileChooserEntry *self = PN_FILE_CHOOSER_ENTRY (object);

    switch (prop_id) {
    case PROP_TEXT:
        g_value_set_string (value, gtk_entry_get_text (self->entry));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_file_chooser_entry_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
    PnFileChooserEntry *self = PN_FILE_CHOOSER_ENTRY (object);

    switch (prop_id) {
    case PROP_TEXT:
        pn_file_chooser_entry_set_text (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_file_chooser_entry_finalize (GObject *object)
{
    PnFileChooserEntry *self = PN_FILE_CHOOSER_ENTRY (object);

    g_free (self->title);
    g_free (self->committed);
    g_clear_pointer (&self->filters, g_ptr_array_unref);

    G_OBJECT_CLASS (pn_file_chooser_entry_parent_class)->finalize (object);
}

static void
pn_file_chooser_entry_class_init (PnFileChooserEntryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = pn_file_chooser_entry_get_property;
    object_class->set_property = pn_file_chooser_entry_set_property;
    object_class->finalize     = pn_file_chooser_entry_finalize;

    /**
     * PnFileChooserEntry:text:
     *
     * The file path in the entry.  Reading it returns the entry's current
     * text.  It is notified only when an edit is committed (Enter, focus
     * leaving the entry, a pick in the browse dialog, the widget being
     * unmapped) and when it is set programmatically, never per keystroke.
     */
    props[PROP_TEXT] = g_param_spec_string (
            "text", "Text", "The file path in the entry.",
            "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_properties (object_class, N_PROPS, props);

    /**
     * PnFileChooserEntry::changed:
     * @self: the widget
     *
     * Emitted when a changed path is committed: typed or pasted and then
     * confirmed with Enter or by leaving the entry, picked through the
     * browse dialog, or set programmatically.  Read the new value from the
     * #PnFileChooserEntry:text property.
     */
    signals[SIG_CHANGED] = g_signal_new (
            "changed",
            PN_TYPE_FILE_CHOOSER_ENTRY,
            G_SIGNAL_RUN_LAST,
            0,                  /* no class handler vfunc slot */
            NULL, NULL, NULL,   /* no accumulator, no marshaller */
            G_TYPE_NONE,
            0);
}

static void
pn_file_chooser_entry_init (PnFileChooserEntry *self)
{
    GtkWidget *entry, *browse;

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_HORIZONTAL);
    gtk_box_set_spacing (GTK_BOX (self), 4);

    self->action    = GTK_FILE_CHOOSER_ACTION_OPEN;
    self->committed = g_strdup ("");

    entry = gtk_entry_new ();
    self->entry = GTK_ENTRY (entry);
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_box_pack_start (GTK_BOX (self), entry, TRUE, TRUE, 0);

    browse = gtk_button_new_from_icon_name (PN_FILE_CHOOSER_ICON,
                                            GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_focus_on_click (browse, FALSE);
    gtk_widget_set_valign (browse, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (browse, "Browse for a file");
    gtk_box_pack_start (GTK_BOX (self), browse, FALSE, FALSE, 0);

    g_signal_connect (entry, "activate",
                      G_CALLBACK (on_entry_activate), self);
    g_signal_connect (entry, "focus-out-event",
                      G_CALLBACK (on_entry_focus_out), self);
    g_signal_connect (self, "unmap", G_CALLBACK (on_unmap), NULL);
    g_signal_connect (browse, "clicked",
                      G_CALLBACK (on_browse_clicked), self);

    gtk_widget_show_all (GTK_WIDGET (entry));
    gtk_widget_show_all (browse);
}

/* --- public API ------------------------------------------------------- */

GtkWidget *
pn_file_chooser_entry_new (void)
{
    return g_object_new (PN_TYPE_FILE_CHOOSER_ENTRY, NULL);
}

void
pn_file_chooser_entry_set_text (PnFileChooserEntry *self, const gchar *text)
{
    g_return_if_fail (PN_IS_FILE_CHOOSER_ENTRY (self));

    if (text == NULL)
        text = "";
    if (g_strcmp0 (text, gtk_entry_get_text (self->entry)) == 0)
        return;

    gtk_entry_set_text (self->entry, text);
    commit (self);
}

const gchar *
pn_file_chooser_entry_get_text (PnFileChooserEntry *self)
{
    g_return_val_if_fail (PN_IS_FILE_CHOOSER_ENTRY (self), NULL);

    return gtk_entry_get_text (self->entry);
}

void
pn_file_chooser_entry_set_title (PnFileChooserEntry *self, const gchar *title)
{
    g_return_if_fail (PN_IS_FILE_CHOOSER_ENTRY (self));

    g_free (self->title);
    self->title = (title != NULL && *title != '\0') ? g_strdup (title) : NULL;
}

void
pn_file_chooser_entry_set_action (PnFileChooserEntry  *self,
                                  GtkFileChooserAction action)
{
    g_return_if_fail (PN_IS_FILE_CHOOSER_ENTRY (self));

    self->action = action;
}

void
pn_file_chooser_entry_add_filter (PnFileChooserEntry *self,
                                  GtkFileFilter      *filter)
{
    g_return_if_fail (PN_IS_FILE_CHOOSER_ENTRY (self));
    g_return_if_fail (GTK_IS_FILE_FILTER (filter));

    if (self->filters == NULL)
        self->filters = g_ptr_array_new_with_free_func (g_object_unref);
    /* Take ownership: GtkFileFilter is initially floating, so sink it once
     * here and unref on finalize. */
    g_ptr_array_add (self->filters, g_object_ref_sink (filter));
}
