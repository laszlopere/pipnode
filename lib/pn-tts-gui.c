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
/*  PnTts — gui tier.                                                  */
/*                                                                     */
/*  The settings-dialog customisation for the Tts node.  The node's    */
/*  GType, properties, the engine table + speak()/receive() spawn      */
/*  path, the GTK-free voice/sink enumeration and the exported          */
/*  selection parsers (pn_tts_derive_voice_label / voice_index_for)    */
/*  all live in the core file pn-tts.c; this file installs the          */
/*  build_property_editor + build_class_tab vfuncs onto that class at   */
/*  editor startup (pn_tts_gui_install).  The dialog enumerates the     */
/*  engine table through the GTK-free pn_tts_engine_* accessors, reads  */
/*  the runtime fields through their GObject properties, and auditions  */
/*  via the public pn_tts_speak — so it needs no struct seam.  The      */
/*  headless runtime never loads this half, so the synthesis logic      */
/*  runs without GTK.                                                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-tts-gui.h"
#include "pn-tts.h"
#include "pn-node-dialog-helpers.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog: per-property editor overrides                     */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_tts_engine_editor (GObject    *target,
                         GParamSpec *pspec)
{
    const gchar     *name     = pspec->name;
    gboolean         writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags    flags    = G_BINDING_SYNC_CREATE
                                | (writable ? G_BINDING_BIDIRECTIONAL : 0);
    GtkWidget       *combo    = gtk_combo_box_text_new ();
    gchar           *current  = NULL;
    gboolean         listed   = FALSE;
    guint            n        = pn_tts_n_engines ();
    guint            i;

    g_object_get (target, name, &current, NULL);

    for (i = 0; i < n; i++)
    {
        const gchar *eng_id    = pn_tts_engine_id (i);
        const gchar *eng_label = pn_tts_engine_label (i);
        gchar       *label;

        /* Suffix unavailable engines so the user can see at a glance
         * which ones are wired up — they remain selectable so picking
         * one surfaces the error message in the status row and the
         * red visual state on the node body. */
        if (pn_tts_engine_installed (i))
            label = g_strdup (eng_label);
        else
            label = g_strdup_printf ("%s (not installed)", eng_label);

        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   eng_id, label);
        g_free (label);

        if (g_strcmp0 (eng_id, current) == 0)
            listed = TRUE;
    }

    /* Saved flows may carry an engine id we've since dropped from the
     * table — keep it in the combo so the user can see what is set
     * before they pick something else. */
    if (!listed && current != NULL && *current != '\0')
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   current, current);

    g_free (current);

    gtk_widget_set_hexpand   (combo, TRUE);
    gtk_widget_set_sensitive (combo, writable);
    g_object_bind_property (target, name, combo, "active-id", flags);
    return combo;
}

/** Rebuild the voice combo from the engine currently selected on
 *  @target.  Called once at editor-build time and again on every
 *  notify::engine.  When the previously-selected voice is missing
 *  from the new engine's list, the first available voice is selected
 *  so the user lands on a working configuration without having to
 *  re-pick.  Engines with no enumerable voice list (festival) show a
 *  single "Default" entry whose id is the empty string. */
static void
voice_repopulate (GObject *target, GtkComboBoxText *combo)
{
    gchar  *engine_id = NULL;
    gchar  *current   = NULL;
    gchar **voices    = NULL;

    g_object_get (target, "engine", &engine_id, "model", &current, NULL);

    /* gtk_combo_box_text_remove_all() drops active-id; the property
     * still holds the user's pick so the SYNC_CREATE binding will
     * push it back in once we've finished appending the new rows
     * (or we overwrite the property below). */
    gtk_combo_box_text_remove_all (combo);

    voices = pn_tts_engine_list_voices (engine_id);

    if (voices == NULL || voices[0] == NULL)
    {
        /* Engine has no enumerable voices — surface a single
         * "Default" entry and zero out the persisted voice so the
         * spawn path falls through to cmd_no_voice. */
        gtk_combo_box_text_append (combo, "", "Default");
        if (current == NULL || *current != '\0')
            g_object_set (target, "model", "", NULL);
    }
    else
    {
        gboolean listed = FALSE;
        for (gchar **p = voices; *p != NULL; p++)
        {
            gchar *label = pn_tts_derive_voice_label (engine_id, *p);
            gtk_combo_box_text_append (combo, *p, label);
            g_free (label);
            if (g_strcmp0 (*p, current) == 0)
                listed = TRUE;
        }

        /* When the current voice doesn't belong to the freshly-
         * picked engine, auto-select the first one so the dialog
         * always lands on a runnable configuration.  Note we do
         * *not* append the orphan voice as a verbatim fallback the
         * way the piper-only editor used to — that fallback was
         * also reached when the user picked a different engine,
         * leaving the previous engine's voice stuck in the combo
         * alongside the new list. */
        if (!listed)
            g_object_set (target, "model", voices[0], NULL);
    }

    g_strfreev (voices);
    g_free (engine_id);
    g_free (current);
}

static void
on_engine_notify_repopulate (GObject    *target,
                             GParamSpec *pspec G_GNUC_UNUSED,
                             gpointer    user_data)
{
    voice_repopulate (target, GTK_COMBO_BOX_TEXT (user_data));
}

static GtkWidget *
build_tts_model_editor (GObject    *target,
                        GParamSpec *pspec)
{
    const gchar  *name     = pspec->name;
    gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);
    GtkWidget    *combo    = gtk_combo_box_text_new ();

    voice_repopulate (target, GTK_COMBO_BOX_TEXT (combo));

    /* Refresh the voice list whenever the engine changes so the user
     * sees the new engine's voices without reopening the dialog.
     * Tied to the combo's lifetime via g_signal_connect_object so the
     * handler auto-disconnects when the dialog closes. */
    g_signal_connect_object (target, "notify::engine",
                             G_CALLBACK (on_engine_notify_repopulate),
                             combo, 0);

    gtk_widget_set_hexpand   (combo, TRUE);
    gtk_widget_set_sensitive (combo, writable);
    g_object_bind_property (target, name, combo, "active-id", flags);
    return combo;
}

static GtkWidget *
build_tts_sink_editor (GObject    *target,
                       GParamSpec *pspec)
{
    const gchar  *name     = pspec->name;
    gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);
    GtkWidget    *combo    = gtk_combo_box_text_new ();
    gchar        *current  = NULL;
    gchar       **pairs;
    gboolean      listed   = FALSE;

    g_object_get (target, name, &current, NULL);

    /* Sentinel "Default" entry whose id is the empty string — the
     * speak path treats an empty sink as "don't set PULSE_SINK and
     * don't pass paplay -d", which is what an unconfigured node and
     * a host without pactl/paplay should both fall back to. */
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                               "", "Default");

    pairs = pn_tts_list_audio_sinks ();
    for (gsize i = 0; pairs[i] != NULL && pairs[i + 1] != NULL; i += 2)
    {
        const gchar *sink_name = pairs[i];
        const gchar *sink_desc = pairs[i + 1];
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   sink_name, sink_desc);
        if (g_strcmp0 (sink_name, current) == 0)
            listed = TRUE;
    }

    /* Saved sink that's no longer present on this host — keep it in
     * the dropdown so the user sees what they configured before they
     * pick a substitute, the same shape the engine combo uses. */
    if (!listed && current != NULL && *current != '\0')
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   current, current);

    g_strfreev (pairs);
    g_free (current);

    gtk_widget_set_hexpand   (combo, TRUE);
    gtk_widget_set_sensitive (combo, writable);
    g_object_bind_property (target, name, combo, "active-id", flags);
    return combo;
}

static GtkWidget *
pn_tts_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                              GParamSpec  *pspec,
                              GObject     *target,
                              GtkWindow   *parent    G_GNUC_UNUSED)
{
    if (g_strcmp0 (pspec->name, "engine") == 0)
        return build_tts_engine_editor (target, pspec);
    if (g_strcmp0 (pspec->name, "model") == 0)
        return build_tts_model_editor (target, pspec);
    if (g_strcmp0 (pspec->name, "sink") == 0)
        return build_tts_sink_editor (target, pspec);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: status row                                        */
/*                                                                     */
/*  The auto-generated tab would show the engine and model rows but    */
/*  not the sticky last-error string the node uses to flip itself red. */
/*  Mirror PnMeshtastic's pattern: hand-roll the tab so we can attach  */
/*  a third "Status" row that surfaces last_error in red markup, and   */
/*  read the runtime fields through their GObject properties.          */
/* ------------------------------------------------------------------ */

static void
update_status_label (GObject    *obj,
                     GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer    user_data)
{
    GtkLabel *label = GTK_LABEL (user_data);
    gchar    *engine     = NULL;
    gchar    *last_error = NULL;
    gchar    *markup;

    g_object_get (obj,
                  "engine",     &engine,
                  "last-error", &last_error,
                  NULL);

    if (last_error != NULL && *last_error != '\0')
    {
        gchar *escaped = g_markup_escape_text (last_error, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">%s</span>", escaped);
        g_free (escaped);
    }
    else
    {
        const gchar *eng_label = pn_tts_engine_label_for_id (engine);
        if (eng_label != NULL)
            markup = g_strdup_printf ("%s is ready.", eng_label);
        else
            markup = g_strdup ("");
    }

    gtk_label_set_markup (label, markup);
    g_free (markup);
    g_free (engine);
    g_free (last_error);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: audio preview                                     */
/*                                                                     */
/*  Whenever the user changes the engine or the voice in the dialog,  */
/*  fire a short sample utterance so they hear what they just picked  */
/*  without having to wire up an inject node first.  Rapid combo      */
/*  changes are debounced through a single timeout source so the user */
/*  hears the *settled* selection — and the source is anchored to a   */
/*  dialog widget so it gets cancelled when the dialog closes.        */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnTts *self;       /* owns a strong ref so the timeout can fire
                        * even if the node is briefly detached */
    guint  source_id;  /* 0 when no preview is pending */
} TtsPreviewState;

static void
tts_preview_state_free (gpointer data)
{
    TtsPreviewState *st = data;
    if (st->source_id != 0)
        g_source_remove (st->source_id);
    g_object_unref (st->self);
    g_free (st);
}

static gboolean
tts_preview_fire (gpointer user_data)
{
    TtsPreviewState *st = user_data;
    gchar           *last_error = NULL;

    st->source_id = 0;

    /* Skip the playback if the node is in an error state — speaking
     * would just print the same warning to stderr and the dialog's
     * red status row already tells the user why nothing happens. */
    g_object_get (st->self, "last-error", &last_error, NULL);
    if (last_error == NULL)
        pn_tts_speak (st->self,
                      "This is a sample of the selected voice.",
                      NULL);
    g_free (last_error);
    return G_SOURCE_REMOVE;
}

static void
on_setting_changed_preview (GObject    *obj,
                            GParamSpec *pspec G_GNUC_UNUSED,
                            gpointer    user_data)
{
    GtkWidget       *anchor = GTK_WIDGET (user_data);
    TtsPreviewState *st     = g_object_get_data (G_OBJECT (anchor),
                                                 "pn-tts-preview");

    if (st == NULL)
    {
        st = g_new0 (TtsPreviewState, 1);
        st->self      = g_object_ref (PN_TTS (obj));
        st->source_id = 0;
        g_object_set_data_full (G_OBJECT (anchor), "pn-tts-preview",
                                st, tts_preview_state_free);
    }

    /* Coalesce: a single 300 ms window means switching engine and
     * then having voice_repopulate auto-swap the voice produces one
     * playback, not two — and the user dragging through several combo
     * entries only hears the one they land on. */
    if (st->source_id != 0)
        g_source_remove (st->source_id);
    st->source_id = g_timeout_add (300, tts_preview_fire, st);
}

static GtkWidget *
pn_tts_build_class_tab (PnNode    *self,
                        GtkWindow *parent)
{
    GObject    *target = G_OBJECT (self);
    GtkWidget  *grid   = pn_node_dialog_new_property_grid ();
    GtkWidget  *engine_editor;
    GtkWidget  *model_editor;
    GtkWidget  *status_label;
    GObjectClass *klass = G_OBJECT_GET_CLASS (self);
    GParamSpec *engine_pspec = g_object_class_find_property (klass, "engine");
    GParamSpec *model_pspec  = g_object_class_find_property (klass, "model");
    GParamSpec *psv_pspec    = g_object_class_find_property (klass,
                                                            "per-source-voice");
    GParamSpec *speed_pspec  = g_object_class_find_property (klass, "speed");
    GParamSpec *sink_pspec   = g_object_class_find_property (klass, "sink");
    GParamSpec *mq_pspec     = g_object_class_find_property (klass, "max-queue");

    engine_editor = pn_tts_build_property_editor (
            self, engine_pspec, target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 0,
                               g_param_spec_get_nick (engine_pspec),
                               engine_editor);

    model_editor = pn_tts_build_property_editor (
            self, model_pspec, target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 1,
                               g_param_spec_get_nick (model_pspec),
                               model_editor);

    /* per-source-voice sits directly under Voice because it modifies
     * how the voice is picked at receive time — keeping the two rows
     * adjacent makes the relationship obvious in the dialog. */
    GtkWidget *psv_editor = pn_node_dialog_default_editor (
            target, psv_pspec);
    pn_node_dialog_attach_row (GTK_GRID (grid), 2,
                               g_param_spec_get_nick (psv_pspec),
                               psv_editor);

    /* Speed is a plain double in [0.5, 2.0] — the introspection-
     * driven default editor gives us a spin button with the right
     * range, two-digit precision and 0.1 step out of the box. */
    GtkWidget *speed_editor = pn_node_dialog_default_editor (
            target, speed_pspec);
    pn_node_dialog_attach_row (GTK_GRID (grid), 3,
                               g_param_spec_get_nick (speed_pspec),
                               speed_editor);

    GtkWidget *sink_editor = pn_tts_build_property_editor (
            self, sink_pspec, target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 4,
                               g_param_spec_get_nick (sink_pspec),
                               sink_editor);

    GtkWidget *max_queue_editor = pn_node_dialog_default_editor (
            target, mq_pspec);
    pn_node_dialog_attach_row (GTK_GRID (grid), 5,
                               g_param_spec_get_nick (mq_pspec),
                               max_queue_editor);

    status_label = gtk_label_new (NULL);
    gtk_label_set_xalign     (GTK_LABEL (status_label), 0.0);
    gtk_label_set_use_markup (GTK_LABEL (status_label), TRUE);
    pn_node_dialog_attach_row (GTK_GRID (grid), 6, "Status", status_label);

    update_status_label (target, NULL, status_label);
    g_signal_connect_object (target, "notify::engine",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::last-error",
                             G_CALLBACK (update_status_label),
                             status_label, 0);

    /* Audio preview hooks are connected AFTER the editors have been
     * built so the initial voice_repopulate sync — which can write
     * "model" back to the property if the saved voice doesn't fit
     * the current engine's list — doesn't trigger a spurious sample
     * the instant the dialog opens.  The anchor is status_label
     * because it outlives the editors but is destroyed with the
     * dialog, giving the timeout source a clean lifetime. */
    g_signal_connect_object (target, "notify::engine",
                             G_CALLBACK (on_setting_changed_preview),
                             status_label, 0);
    g_signal_connect_object (target, "notify::model",
                             G_CALLBACK (on_setting_changed_preview),
                             status_label, 0);
    g_signal_connect_object (target, "notify::speed",
                             G_CALLBACK (on_setting_changed_preview),
                             status_label, 0);
    g_signal_connect_object (target, "notify::sink",
                             G_CALLBACK (on_setting_changed_preview),
                             status_label, 0);

    return grid;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_tts_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_TTS));

    node_class->build_property_editor = pn_tts_build_property_editor;
    node_class->build_class_tab       = pn_tts_build_class_tab;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid. */
}
