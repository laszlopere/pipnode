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
/*  PnSound — gui tier.                                                */
/*                                                                     */
/*  The settings-dialog customisation for the Sound node.  The node's  */
/*  GType, properties, the sound-playback receive() path and the       */
/*  GTK-free helpers pn_sound_list_system_sounds / pn_sound_preview    */
/*  live in the core file pn-sound.c; this file installs the           */
/*  build_property_editor vfunc onto that class at editor startup       */
/*  (pn_sound_gui_install).  The composite combo/file-chooser editor   */
/*  reads and writes the node's "sound" property via                   */
/*  g_object_get/g_object_set and calls the public list/preview        */
/*  accessors, so it needs no extra core seam.  The headless runtime   */
/*  never loads this half, so the playback logic runs without GTK.     */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-sound-gui.h"
#include "pn-sound.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_property_editor override        */
/*                                                                     */
/*  Composite editor for #PnSound:sound: a combo of freedesktop sound  */
/*  ids next to a #GtkFileChooserButton.  Both halves drive the same   */
/*  underlying string property — picking a combo entry stores the      */
/*  bare id, picking a file stores its absolute path.  Both halves     */
/*  also listen for external changes so the visible state always       */
/*  reflects the property.  Pre-2.0 this lived as a PN_IS_SOUND        */
/*  branch in the host's pn-node-dialog.c; the per-class vfunc keeps   */
/*  it next to the rest of the node and lets the host dialog remain    */
/*  ignorant of every concrete subclass.                               */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject              *target;       /* borrowed */
    const gchar          *property;     /* pspec->name */
    GtkComboBox          *combo;
    GtkFileChooserButton *chooser;
    gulong                notify_handler;
    gboolean              updating;
} PnSoundBinding;

static void
sound_binding_free (gpointer data)
{
    PnSoundBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
sound_binding_pull (PnSoundBinding *bind)
{
    gchar *value = NULL;

    if (bind->updating)
        return;

    g_object_get (bind->target, bind->property, &value, NULL);

    bind->updating = TRUE;

    if (value != NULL && value[0] == '/')
    {
        gtk_combo_box_set_active_id (bind->combo, NULL);
        gtk_file_chooser_set_filename (
                GTK_FILE_CHOOSER (bind->chooser), value);
    }
    else
    {
        /* Clearing the chooser through the public API is awkward; the
         * chooser keeps showing the previously-picked file label even
         * after we deselect it, but that does not affect the property
         * state — the combo is the authority while a system id is in
         * effect. */
        gtk_file_chooser_unselect_all (
                GTK_FILE_CHOOSER (bind->chooser));
        gtk_combo_box_set_active_id (bind->combo,
                                     (value != NULL && *value != '\0')
                                         ? value : NULL);
    }

    bind->updating = FALSE;
    g_free (value);
}

static void
on_sound_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    sound_binding_pull (user_data);
}

static void
on_sound_combo_changed (
        GtkComboBox *combo,
        gpointer     user_data)
{
    PnSoundBinding *bind = user_data;
    const gchar    *id;

    if (bind->updating)
        return;

    id = gtk_combo_box_get_active_id (combo);
    if (id == NULL)
        return;

    bind->updating = TRUE;
    g_object_set (bind->target, bind->property, id, NULL);
    /* The chooser may still show a previously-picked path; clear it
     * so the visible state matches the system-id we just selected. */
    gtk_file_chooser_unselect_all (GTK_FILE_CHOOSER (bind->chooser));
    bind->updating = FALSE;

    /* Audition the freshly-picked clip so the user can hear what
     * they just chose without wiring up a message source. */
    if (PN_IS_SOUND (bind->target))
        pn_sound_preview (PN_SOUND (bind->target));
}

static void
on_sound_file_set (
        GtkFileChooserButton *chooser,
        gpointer              user_data)
{
    PnSoundBinding *bind = user_data;
    gchar          *path;

    if (bind->updating)
        return;

    path = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (chooser));
    if (path == NULL)
        return;

    bind->updating = TRUE;
    g_object_set (bind->target, bind->property, path, NULL);
    /* A picked file overrides the combo selection; clear it so the
     * UI does not look like both are simultaneously active. */
    gtk_combo_box_set_active_id (bind->combo, NULL);
    bind->updating = FALSE;

    g_free (path);
}

static GtkWidget *
build_sound_editor (
        GObject    *target,
        GParamSpec *pspec)
{
    const gchar    *name     = pspec->name;
    gboolean        writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GtkWidget      *box      = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget      *row      = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget      *combo    = gtk_combo_box_text_new ();
    GtkWidget      *chooser  = gtk_file_chooser_button_new (
            "Choose a sound file", GTK_FILE_CHOOSER_ACTION_OPEN);
    GtkWidget      *status   = gtk_label_new (NULL);
    PnSoundBinding *bind;
    gchar         **ids;
    gchar         **p;
    gchar          *signal_name;
    gchar          *status_markup;

    ids = pn_sound_list_system_sounds ();
    for (p = ids; *p != NULL; p++)
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   *p, *p);
    g_strfreev (ids);

    {
        GtkFileFilter *filter = gtk_file_filter_new ();
        gtk_file_filter_set_name (filter, "Audio files");
        gtk_file_filter_add_mime_type (filter, "audio/*");
        gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (chooser), filter);
    }

    gtk_widget_set_hexpand (combo,   TRUE);
    gtk_widget_set_hexpand (chooser, TRUE);

    gtk_box_pack_start (GTK_BOX (row), combo,   TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (row), chooser, TRUE, TRUE, 0);

    /* Show which playback path this build uses (in-process vs the paplay
     * fallback) so the choice is visible without reading the build log. */
    status_markup = g_markup_printf_escaped (
            "<small>Playback: %s</small>",
            pn_sound_backend_description ());
    gtk_label_set_markup (GTK_LABEL (status), status_markup);
    g_free (status_markup);
    gtk_widget_set_halign (status, GTK_ALIGN_START);
    gtk_style_context_add_class (gtk_widget_get_style_context (status),
                                 "dim-label");

    gtk_box_pack_start (GTK_BOX (box), row,    FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), status, FALSE, FALSE, 0);

    bind = g_new0 (PnSoundBinding, 1);
    bind->target   = target;
    bind->property = name;
    bind->combo    = GTK_COMBO_BOX (combo);
    bind->chooser  = GTK_FILE_CHOOSER_BUTTON (chooser);

    sound_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_sound_target_notify), bind);
    g_free (signal_name);

    if (writable)
    {
        g_signal_connect (combo,   "changed",
                          G_CALLBACK (on_sound_combo_changed), bind);
        g_signal_connect (chooser, "file-set",
                          G_CALLBACK (on_sound_file_set),      bind);
    }

    gtk_widget_set_sensitive (row, writable);

    g_object_set_data_full (G_OBJECT (box),
                            "pn-sound-binding",
                            bind, sound_binding_free);

    return box;
}

static GtkWidget *
pn_sound_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                                GParamSpec  *pspec,
                                GObject     *target,
                                GtkWindow   *parent    G_GNUC_UNUSED)
{
    if (g_strcmp0 (pspec->name, "sound") == 0)
        return build_sound_editor (target, pspec);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_sound_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_SOUND));

    node_class->build_property_editor = pn_sound_build_property_editor;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid. */
}
