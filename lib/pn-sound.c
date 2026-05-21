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

#include "pn-sound.h"

#define PN_SOUND_NORMAL_ICON  "\xef\x80\xa8"        /* fa-volume-up U+F028 */
#define PN_SOUND_WARNING_ICON "\xe2\x9d\x97"        /* ❗ U+2757 */

#define PN_SOUND_DEAD_PERIOD_MIN 0u
#define PN_SOUND_DEAD_PERIOD_MAX 3600u
#define PN_SOUND_DEAD_PERIOD_DEF 0u

/* The freedesktop XDG sound theme is the most widely-installed source
 * of named sounds on Linux desktops.  We scan only the stereo profile
 * because that is what `canberra-gtk-play -i` resolves by default. */
#define PN_SOUND_THEME_DIR "/usr/share/sounds/freedesktop/stereo"

struct _PnSound
{
    PnNode parent_instance;

    /* Either a freedesktop sound id (e.g. "bell") or an absolute
     * path to an audio file.  Empty/NULL means "not configured" and
     * paints the node with the warning glyph. */
    gchar *sound;

    /* Seconds of mandatory silence that follow each playback.  While
     * the dead window is active incoming messages are dropped. */
    guint dead_period;

    /* %TRUE while a canberra-gtk-play subprocess is still running.
     * Cleared from on_play_done() once the process exits. */
    gboolean playing;

    /* Monotonic timestamp (microseconds) at which the dead period
     * expires; messages arriving before this are dropped.  Set when
     * playback completes; meaningless while @playing is %TRUE. */
    gint64 ready_at_us;
};

G_DEFINE_TYPE (PnSound, pn_sound, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_SOUND,
    PROP_DEAD_PERIOD,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (
        PnSound *self,
        gboolean configured)
{
    PnNode *node = PN_NODE (self);

    if (configured)
    {
        GdkRGBA violet = { 0.55, 0.42, 0.78, 1.0 };
        pn_node_set_color (node, &violet);
        pn_node_set_icon  (node, PN_SOUND_NORMAL_ICON);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_SOUND_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Playback                                                           */
/* ------------------------------------------------------------------ */

/** Reap the asynchronous paplay spawn.  Releases the GSubprocess
 *  held by @source and arms the dead-period window so the next
 *  message can be considered after dead-period seconds. */
static void
on_play_done (
        GObject      *source,
        GAsyncResult *result,
        gpointer      user_data)
{
    GSubprocess *sub   = G_SUBPROCESS (source);
    PnSound     *self  = PN_SOUND (user_data);
    GError      *error = NULL;

    g_subprocess_wait_finish (sub, result, &error);
    if (error != NULL)
    {
        g_warning ("pn-sound: paplay failed: %s", error->message);
        g_error_free (error);
    }

    self->playing     = FALSE;
    self->ready_at_us = g_get_monotonic_time ()
                      + (gint64) self->dead_period * G_TIME_SPAN_SECOND;

    g_object_unref (sub);
    g_object_unref (self);
}

/** Resolve @spec to a concrete file path: an absolute path is used
 *  verbatim, a bare sound id is looked up under the freedesktop
 *  stereo theme as @spec.oga.  Returns a newly-allocated string the
 *  caller must free with g_free(). */
static gchar *
resolve_sound_path (const gchar *spec)
{
    gchar *filename;
    gchar *path;

    if (spec == NULL || *spec == '\0')
        return NULL;

    if (spec[0] == '/')
        return g_strdup (spec);

    filename = g_strconcat (spec, ".oga", NULL);
    path     = g_build_filename (PN_SOUND_THEME_DIR, filename, NULL);
    g_free (filename);
    return path;
}

/** Spawn `paplay PATH` in the background.  We use paplay rather than
 *  canberra-gtk-play because the latter honours GTK's
 *  `gtk-enable-event-sounds` setting, which is off by default on
 *  many desktops and silently swallows playback ("Sound disabled"). */
static gboolean
pn_sound_play (
        PnSound     *self,
        const gchar *spec)
{
    GSubprocess *sub;
    GError      *error = NULL;
    gchar       *path;

    path = resolve_sound_path (spec);
    if (path == NULL)
        return FALSE;

    sub = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                            | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                            &error,
                            "paplay", path, NULL);

    g_free (path);

    if (sub == NULL)
    {
        g_warning ("pn-sound: failed to spawn paplay: %s",
                   error ? error->message : "(unknown)");
        g_clear_error (&error);
        return FALSE;
    }

    self->playing = TRUE;
    g_subprocess_wait_async (sub, NULL,
                             on_play_done, g_object_ref (self));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_sound_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnSound *self = PN_SOUND (node);

    (void) message;

    /* Drop overlapping requests outright: two clips from the same
     * sink would just garble each other in the speaker. */
    if (self->playing)
        return;

    /* Honour the dead-period window even when the previous playback
     * has already finished. */
    if (self->ready_at_us != 0
        && g_get_monotonic_time () < self->ready_at_us)
        return;

    if (self->sound == NULL || *self->sound == '\0')
        return;

    pn_sound_play (self, self->sound);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_sound_set_sound (
        PnSound     *self,
        const gchar *sound)
{
    gboolean configured;

    if (g_strcmp0 (self->sound, sound) == 0)
        return;

    g_free (self->sound);
    self->sound = (sound != NULL && *sound != '\0')
                      ? g_strdup (sound) : NULL;

    configured = (self->sound != NULL);
    apply_visual_state (self, configured);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SOUND]);
}

static void
pn_sound_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnSound *self = PN_SOUND (object);

    switch (prop_id)
    {
    case PROP_SOUND:
        g_value_set_string (value, self->sound);
        break;
    case PROP_DEAD_PERIOD:
        g_value_set_uint (value, self->dead_period);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_sound_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnSound *self = PN_SOUND (object);

    switch (prop_id)
    {
    case PROP_SOUND:
        pn_sound_set_sound (self, g_value_get_string (value));
        break;
    case PROP_DEAD_PERIOD:
        {
            guint v = g_value_get_uint (value);
            if (self->dead_period != v)
            {
                self->dead_period = v;
                g_object_notify_by_pspec (object, props[PROP_DEAD_PERIOD]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_sound_finalize (GObject *object)
{
    PnSound *self = PN_SOUND (object);

    g_clear_pointer (&self->sound, g_free);

    G_OBJECT_CLASS (pn_sound_parent_class)->finalize (object);
}

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
    GtkWidget      *box      = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget      *combo    = gtk_combo_box_text_new ();
    GtkWidget      *chooser  = gtk_file_chooser_button_new (
            "Choose a sound file", GTK_FILE_CHOOSER_ACTION_OPEN);
    PnSoundBinding *bind;
    gchar         **ids;
    gchar         **p;
    gchar          *signal_name;

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

    gtk_box_pack_start (GTK_BOX (box), combo,   TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (box), chooser, TRUE, TRUE, 0);

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

    gtk_widget_set_sensitive (box, writable);

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

static void
pn_sound_class_init (PnSoundClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_sound_get_property;
    object_class->set_property = pn_sound_set_property;
    object_class->finalize     = pn_sound_finalize;
    node_class->receive        = pn_sound_receive;

    node_class->build_property_editor = pn_sound_build_property_editor;

    /* Instance icon flips between speaker and ❗ depending on whether
     * a sound has been chosen.  Pin a stable glyph for the palette. */
    node_class->palette_icon   = PN_SOUND_NORMAL_ICON;
    node_class->class_name     = "Sound";
    node_class->icon           = PN_SOUND_NORMAL_ICON;
    node_class->color          = (GdkRGBA){ 0.55, 0.42, 0.78, 1.0 };
    node_class->category       = "Sinks";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    props[PROP_SOUND] = g_param_spec_string (
            "sound", "Sound",
            "Sound to play.  Either a freedesktop sound-theme id "
            "(e.g. \"bell\") or an absolute path to an audio file",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DEAD_PERIOD] = g_param_spec_uint (
            "dead-period", "Dead period",
            "Seconds of silence enforced after each playback during "
            "which incoming messages are dropped",
            PN_SOUND_DEAD_PERIOD_MIN,
            PN_SOUND_DEAD_PERIOD_MAX,
            PN_SOUND_DEAD_PERIOD_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_sound_init (PnSound *self)
{
    PnNode *node = PN_NODE (self);

    self->sound       = NULL;
    self->dead_period = PN_SOUND_DEAD_PERIOD_DEF;
    self->playing     = FALSE;
    self->ready_at_us = 0;

    pn_node_set_class_name (node, "Sound");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    apply_visual_state (self, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnSound *
pn_sound_new (void)
{
    return g_object_new (PN_TYPE_SOUND, NULL);
}

void
pn_sound_preview (PnSound *self)
{
    g_return_if_fail (PN_IS_SOUND (self));

    if (self->playing)
        return;

    if (self->sound == NULL || *self->sound == '\0')
        return;

    pn_sound_play (self, self->sound);
}

static int
sound_id_compare (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const gchar * const *) a,
                      *(const gchar * const *) b);
}

gchar **
pn_sound_list_system_sounds (void)
{
    GPtrArray *ids = g_ptr_array_new ();
    GDir      *dir = g_dir_open (PN_SOUND_THEME_DIR, 0, NULL);

    if (dir != NULL)
    {
        const gchar *entry;

        while ((entry = g_dir_read_name (dir)) != NULL)
        {
            const gchar *dot = g_strrstr (entry, ".oga");
            gsize        len;

            if (dot == NULL || dot[4] != '\0')
                continue;

            len = (gsize) (dot - entry);
            g_ptr_array_add (ids, g_strndup (entry, len));
        }
        g_dir_close (dir);
    }

    g_ptr_array_sort (ids, sound_id_compare);
    g_ptr_array_add  (ids, NULL);

    return (gchar **) g_ptr_array_free (ids, FALSE);
}
