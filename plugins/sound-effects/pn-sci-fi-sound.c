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
/*  PnSciFiSound — logic half (GTK-free, core only).                   */
/*                                                                     */
/*  This file holds the sink node's runtime behaviour: it resolves the */
/*  configured clip out of the per-user cache and spawns a media       */
/*  player on every incoming message, with an optional dead period     */
/*  between playbacks.  It links libpipnode-core ALONE and pulls no    */
/*  GTK, so a headless server runs it under pipnode-run (TODO #23,      */
/*  Phase 8).                                                          */
/*                                                                     */
/*  The clip-download manager and the settings dialog that drives it   */
/*  (a per-pack checkbox grid, a category/clip combo, an audition      */
/*  button) cannot be expressed as a declarative schema, so they ship  */
/*  separately in the companion module pn-sci-fi-sound-gui.c            */
/*  (pipnode_sound_effects-gui.so), which the editor loads next to     */
/*  this .so and a server never installs.  The two halves share the    */
/*  cache-path / player helpers in pn-sci-fi-clips.c.                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gio/gio.h>

#include "pn-settings-schema.h"
#include "pn-sci-fi-clips.h"
#include "pn-sci-fi-sound.h"

/* fa-rocket U+F135 — a recognisable sci-fi glyph for the palette.  */
#define PN_SCI_FI_SOUND_NORMAL_ICON  "\xef\x84\xb5"

#define PN_SCI_FI_SOUND_DEAD_PERIOD_MIN 0u
#define PN_SCI_FI_SOUND_DEAD_PERIOD_MAX 3600u
#define PN_SCI_FI_SOUND_DEAD_PERIOD_DEF 0u

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnSciFiSound
{
    PnNode parent_instance;

    /* Clip identifier of the form "<Category>/<basename>", e.g.
     * "Romulan/romulan_disruptor.mp3".  Stored as a relative path so a
     * worksheet saved on one machine still resolves when reopened on
     * another whose cache happens to live under a different $HOME. */
    gchar *clip;

    guint    dead_period;
    gboolean playing;
    gint64   ready_at_us;

    /* The running media player and the cancellable for its wait_async,
     * kept so dispose can force the player to exit and cancel the wait.
     * Both non-NULL only while @playing; cleared in on_play_done. */
    GSubprocess  *sub;
    GCancellable *cancellable;
};

G_DEFINE_TYPE (PnSciFiSound, pn_sci_fi_sound, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_CLIP,
    PROP_DEAD_PERIOD,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (PnSciFiSound *self, gboolean configured)
{
    PnNode  *node = PN_NODE (self);
    PnColor  warp = { 0.20, 0.55, 0.85, 1.0 };

    /* Keep the healthy warp-blue identity at all times; the red body + ❗
     * overlay for the unconfigured state is painted centrally by the
     * worksheet whenever has-error is set. */
    pn_node_set_color     (node, &warp);
    pn_node_set_icon      (node, PN_SCI_FI_SOUND_NORMAL_ICON);
    pn_node_set_has_error (node, !configured);
}

/* ------------------------------------------------------------------ */
/*  Playback                                                           */
/* ------------------------------------------------------------------ */

static void
on_play_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    GSubprocess  *sub   = G_SUBPROCESS (source);
    PnSciFiSound *self  = PN_SCI_FI_SOUND (user_data);
    GError       *error = NULL;

    g_subprocess_wait_finish (sub, result, &error);
    if (error != NULL)
    {
        /* A dispose-time cancellation lands here too; that is an orderly
         * teardown, not a playback failure, so don't log it. */
        if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            pn_node_log_error (PN_NODE (self),
                               "Playback failed: %s", error->message);
        g_error_free (error);
    }

    self->playing     = FALSE;
    self->ready_at_us = g_get_monotonic_time ()
                      + (gint64) self->dead_period * G_TIME_SPAN_SECOND;

    /* Playback finished: close the processing glow opened in
     * sci_fi_sound_play_file(). */
    pn_node_processing_end (PN_NODE (self));

    g_clear_object (&self->cancellable);
    g_clear_object (&self->sub);   /* drops the g_subprocess_newv ref */
    g_object_unref (self);
}

static gboolean
sci_fi_sound_play_file (PnSciFiSound *self, const gchar *path)
{
    gchar       *player;
    gchar      **argv;
    GSubprocess *sub;
    GError      *error = NULL;

    player = pn_sci_fi_find_player ();
    if (player == NULL)
    {
        pn_node_log_error (PN_NODE (self),
                           "No media player found "
                           "(install mpv, ffplay, or gst-play-1.0).");
        return FALSE;
    }

    argv = pn_sci_fi_build_argv (player, path);
    g_free (player);

    sub  = g_subprocess_newv ((const gchar * const *) argv,
                              G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                              | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                              &error);
    g_strfreev (argv);

    if (sub == NULL)
    {
        pn_node_log_error (PN_NODE (self),
                           "Could not start the media player: %s",
                           error ? error->message : "(unknown)");
        g_clear_error (&error);
        return FALSE;
    }

    self->playing = TRUE;

    /* Hold the player's ref and a fresh cancellable so dispose can stop
     * it; a GCancellable stays cancelled once tripped, so recreate it per
     * play rather than reuse. */
    g_clear_object (&self->cancellable);
    self->cancellable = g_cancellable_new ();
    self->sub         = sub;   /* transfers the g_subprocess_newv ref */

    /* Light the processing glow for the whole playback run; the
     * synchronous receive() wrap only covers the kickoff.  Balanced by
     * the pn_node_processing_end() in on_play_done(). */
    pn_node_processing_begin (PN_NODE (self));

    g_subprocess_wait_async (sub, self->cancellable,
                             on_play_done, g_object_ref (self));
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_sci_fi_sound_receive (PnNode *node, PnMessage *message)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (node);
    gchar        *path;

    (void) message;

    if (self->playing)
        return;
    if (self->ready_at_us != 0
        && g_get_monotonic_time () < self->ready_at_us)
        return;
    if (self->clip == NULL || *self->clip == '\0')
        return;

    path = pn_sci_fi_resolve_path (self->clip);
    if (path == NULL)
        return;

    if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        sci_fi_sound_play_file (self, path);

    g_free (path);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_sci_fi_sound_set_clip (PnSciFiSound *self, const gchar *clip)
{
    if (g_strcmp0 (self->clip, clip) == 0)
        return;

    g_free (self->clip);
    self->clip = (clip != NULL && *clip != '\0') ? g_strdup (clip) : NULL;

    apply_visual_state (self, self->clip != NULL);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CLIP]);
}

static void
pn_sci_fi_sound_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (object);

    switch (prop_id)
    {
    case PROP_CLIP:
        g_value_set_string (value, self->clip);
        break;
    case PROP_DEAD_PERIOD:
        g_value_set_uint (value, self->dead_period);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_sci_fi_sound_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (object);

    switch (prop_id)
    {
    case PROP_CLIP:
        pn_sci_fi_sound_set_clip (self, g_value_get_string (value));
        break;
    case PROP_DEAD_PERIOD:
        {
            guint v = g_value_get_uint (value);
            if (self->dead_period != v)
            {
                self->dead_period = v;
                g_object_notify_by_pspec (object,
                                          props[PROP_DEAD_PERIOD]);
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
pn_sci_fi_sound_dispose (GObject *object)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (object);

    /* Stop a clip still playing when the node is torn down: cancel the
     * wait and force the media player to exit so it doesn't keep playing
     * after the worksheet closes.
     *
     * Caveat (same as Ollama #39.11): node teardown in this tree is
     * refcount-only -- there is no g_object_run_dispose anywhere, so this
     * runs at refcount 0, which the on_play_done strong ref from
     * wait_async holds off until the player exits.  So mid-playback this
     * is teardown hygiene (the right place to release the player + wait)
     * rather than a true early abort; a real abort would need the store
     * to run_dispose nodes on removal. */
    if (self->cancellable != NULL)
        g_cancellable_cancel (self->cancellable);
    if (self->sub != NULL)
        g_subprocess_force_exit (self->sub);

    g_clear_object (&self->cancellable);
    g_clear_object (&self->sub);

    G_OBJECT_CLASS (pn_sci_fi_sound_parent_class)->dispose (object);
}

static void
pn_sci_fi_sound_finalize (GObject *object)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (object);

    g_clear_pointer (&self->clip, g_free);

    G_OBJECT_CLASS (pn_sci_fi_sound_parent_class)->finalize (object);
}

/* ------------------------------------------------------------------ */
/*  GType                                                              */
/* ------------------------------------------------------------------ */

static void
pn_sci_fi_sound_class_init (PnSciFiSoundClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_sci_fi_sound_get_property;
    object_class->set_property = pn_sci_fi_sound_set_property;
    object_class->dispose      = pn_sci_fi_sound_dispose;
    object_class->finalize     = pn_sci_fi_sound_finalize;

    /* No build_class_tabs here — the settings dialog ships in the
     * companion GUI module (pn-sci-fi-sound-gui.c), which installs the
     * vfunc slot on this class via pn_plugin_gui_init() when the editor
     * loads it.  pipnode-run leaves the slot NULL and runs headless. */
    node_class->receive = pn_sci_fi_sound_receive;

    node_class->palette_icon = PN_SCI_FI_SOUND_NORMAL_ICON;
    node_class->class_name   = "SciFi Sound";
    node_class->icon         = PN_SCI_FI_SOUND_NORMAL_ICON;
    node_class->color        = (PnColor){ 0.20, 0.55, 0.85, 1.0 };
    node_class->category     = "Sinks";
    node_class->has_input    = TRUE;
    node_class->has_output   = FALSE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_CLIP] = g_param_spec_string (
            "clip", "Clip",
            "Relative path of the clip in the per-user sound-effects "
            "cache, of the form \"<Pack>/<basename>\", e.g. "
            "\"Romulan/romulan_disruptor.mp3\"",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DEAD_PERIOD] = g_param_spec_uint (
            "dead-period", "Dead period",
            "Seconds of silence enforced after each playback during "
            "which incoming messages are dropped",
            PN_SCI_FI_SOUND_DEAD_PERIOD_MIN,
            PN_SCI_FI_SOUND_DEAD_PERIOD_MAX,
            PN_SCI_FI_SOUND_DEAD_PERIOD_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_sci_fi_sound_init (PnSciFiSound *self)
{
    PnNode *node = PN_NODE (self);

    self->clip        = NULL;
    self->dead_period = PN_SCI_FI_SOUND_DEAD_PERIOD_DEF;
    self->playing     = FALSE;
    self->ready_at_us = 0;
    self->sub         = NULL;
    self->cancellable = NULL;

    pn_node_set_class_name (node, "SciFi Sound");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    apply_visual_state (self, FALSE);
}

PnSciFiSound *
pn_sci_fi_sound_new (void)
{
    return g_object_new (PN_TYPE_SCI_FI_SOUND, NULL);
}
