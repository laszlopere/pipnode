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
#include "pn-settings-schema.h"

#include <gio/gio.h>

#ifdef HAVE_PN_AUDIO
#include <sndfile.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#endif

#define PN_SOUND_NORMAL_ICON  "\xef\x80\xa8"        /* fa-volume-up U+F028 */

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

    /* %TRUE while a clip is still playing (an in-process worker thread
     * or, in the fallback build, a paplay subprocess).  Cleared by
     * playback_finished() once playback completes. */
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
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.55, 0.42, 0.78, 1.0 };

    /* Keep the healthy violet 🔊 identity at all times; the red body + ❗
     * overlay for the unconfigured state is painted centrally by the
     * worksheet whenever has-error is set. */
    pn_node_set_color     (node, &violet);
    pn_node_set_icon      (node, PN_SOUND_NORMAL_ICON);
    pn_node_set_has_error (node, !configured);
}

/* ------------------------------------------------------------------ */
/*  Playback                                                           */
/* ------------------------------------------------------------------ */

/** Common post-playback bookkeeping, shared by the in-process and
 *  subprocess paths.  Clears the busy flag, arms the dead-period window
 *  so the next message is considered only after dead-period seconds, and
 *  closes the processing glow opened in pn_sound_play().  Main thread. */
static void
playback_finished (PnSound *self)
{
    self->playing     = FALSE;
    self->ready_at_us = g_get_monotonic_time ()
                      + (gint64) self->dead_period * G_TIME_SPAN_SECOND;

    pn_node_processing_end (PN_NODE (self));
}

#ifdef HAVE_PN_AUDIO
/** Decode @path with libsndfile and stream it to the PulseAudio/PipeWire
 *  server with libpulse-simple.  Runs on a GTask worker thread; touches
 *  only the path handed in, never the node's main-thread state.  Both
 *  calls block, which is exactly why this lives off the main loop. */
static void
play_in_thread (
        GTask        *task,
        gpointer      source,
        gpointer      task_data,
        GCancellable *cancellable)
{
    const gchar      *path = task_data;
    SF_INFO           info = { 0 };
    SNDFILE          *snd;
    pa_simple        *pa;
    pa_sample_spec    ss;
    short            *buf;
    sf_count_t        n;
    const sf_count_t  frames_per_chunk = 4096;
    int               paerr = 0;

    (void) source;
    (void) cancellable;

    snd = sf_open (path, SFM_READ, &info);
    if (snd == NULL)
    {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "could not open audio file '%s': %s",
                                 path, sf_strerror (NULL));
        return;
    }
    if (info.channels < 1 || info.samplerate < 1)
    {
        sf_close (snd);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "unsupported audio format in '%s'", path);
        return;
    }

    ss.format   = PA_SAMPLE_S16NE;
    ss.rate     = (uint32_t) info.samplerate;
    ss.channels = (uint8_t)  info.channels;

    pa = pa_simple_new (NULL, "pipnode", PA_STREAM_PLAYBACK, NULL,
                        "Sound node", &ss, NULL, NULL, &paerr);
    if (pa == NULL)
    {
        sf_close (snd);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                 "could not connect to the audio server: %s",
                                 pa_strerror (paerr));
        return;
    }

    buf = g_new (short, (gsize) frames_per_chunk * info.channels);
    while ((n = sf_readf_short (snd, buf, frames_per_chunk)) > 0)
    {
        if (pa_simple_write (pa, buf,
                             (size_t) n * info.channels * sizeof (short),
                             &paerr) < 0)
        {
            g_free (buf);
            pa_simple_free (pa);
            sf_close (snd);
            g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "audio playback failed: %s",
                                     pa_strerror (paerr));
            return;
        }
    }

    g_free (buf);
    pa_simple_drain (pa, &paerr);
    pa_simple_free (pa);
    sf_close (snd);
    g_task_return_boolean (task, TRUE);
}

/** Main-thread completion for the in-process worker: surface any error
 *  and run the shared teardown.  The GTask held the only ref keeping
 *  @self alive across playback; it is released when @result is. */
static void
on_play_ready (
        GObject      *source,
        GAsyncResult *result,
        gpointer      user_data)
{
    PnSound *self  = PN_SOUND (source);
    GError  *error = NULL;

    (void) user_data;

    if (!g_task_propagate_boolean (G_TASK (result), &error))
    {
        if (error != NULL)
        {
            pn_node_log_error (PN_NODE (self), "%s", error->message);
            g_error_free (error);
        }
    }

    playback_finished (self);
}
#else  /* !HAVE_PN_AUDIO */
/** Reap the asynchronous paplay spawn (fallback build).  Releases the
 *  GSubprocess held by @source and runs the shared teardown. */
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
        pn_node_log_error (PN_NODE (self),
                           "paplay failed: %s", error->message);
        g_error_free (error);
    }

    playback_finished (self);

    g_object_unref (sub);
    g_object_unref (self);
}
#endif /* HAVE_PN_AUDIO */

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

/** Start playing @spec in the background, returning once playback has
 *  been kicked off.  With libsndfile + libpulse-simple this decodes and
 *  streams the clip in-process on a worker thread; without them it falls
 *  back to spawning `paplay`.  (We use paplay rather than
 *  canberra-gtk-play because the latter honours GTK's
 *  `gtk-enable-event-sounds` setting, off by default on many desktops,
 *  and silently swallows playback.)  In both cases the processing glow
 *  is opened here and closed by playback_finished() when the clip ends. */
static gboolean
pn_sound_play (
        PnSound     *self,
        const gchar *spec)
{
    gchar *path = resolve_sound_path (spec);

    if (path == NULL)
        return FALSE;

#ifdef HAVE_PN_AUDIO
    {
        /* The GTask is created on the main thread, so its completion
         * callback runs there too; it also holds the only ref keeping
         * @self alive across the worker run. */
        GTask *task = g_task_new (self, NULL, on_play_ready, NULL);

        g_task_set_task_data (task, path, g_free);  /* transfers @path */

        self->playing = TRUE;
        pn_node_processing_begin (PN_NODE (self));

        g_task_run_in_thread (task, play_in_thread);
        g_object_unref (task);
        return TRUE;
    }
#else
    {
        GSubprocess *sub;
        GError      *error = NULL;

        sub = g_subprocess_new (G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &error,
                                "paplay", path, NULL);
        g_free (path);

        if (sub == NULL)
        {
            pn_node_log_error (PN_NODE (self),
                               "Could not start paplay: %s",
                               error ? error->message : "(unknown)");
            g_clear_error (&error);
            return FALSE;
        }

        self->playing = TRUE;
        pn_node_processing_begin (PN_NODE (self));

        g_subprocess_wait_async (sub, NULL,
                                 on_play_done, g_object_ref (self));
        return TRUE;
    }
#endif /* HAVE_PN_AUDIO */
}

const gchar *
pn_sound_backend_description (void)
{
#ifdef HAVE_PN_AUDIO
    return "In-process playback (libsndfile \xe2\x86\x92 PulseAudio)";
#else
    return "External player (paplay)";
#endif
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

static void
pn_sound_class_init (PnSoundClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_sound_get_property;
    object_class->set_property = pn_sound_set_property;
    object_class->finalize     = pn_sound_finalize;
    node_class->receive        = pn_sound_receive;

    /* build_property_editor installed by the gui tier
     * (pn_sound_gui_install). */

    /* Instance icon flips between speaker and ❗ depending on whether
     * a sound has been chosen.  Pin a stable glyph for the palette. */
    node_class->palette_icon   = PN_SOUND_NORMAL_ICON;
    node_class->class_name     = "Sound";
    node_class->icon           = PN_SOUND_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.55, 0.42, 0.78, 1.0 };
    node_class->category       = "Sinks";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);
        pn_node_class_set_settings_schema (node_class, schema);
    }

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
