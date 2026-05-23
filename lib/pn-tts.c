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

#include "pn-tts.h"
#include "pn-message.h"
#include "pn-node-dialog-helpers.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Engine table                                                       */
/*                                                                     */
/*  Each row describes one Linux text-to-speech program the node can   */
/*  drive.  `program` is the binary checked via g_find_program_in_path */
/*  when the node decides whether the engine is available; `cmd` is    */
/*  the shell pipeline run for each utterance, with `%s` replaced by   */
/*  the shell-quoted voice-model path for engines that take one, or    */
/*  ignored otherwise.  All listed engines synthesise speech directly  */
/*  to the default audio device themselves; Piper is the only one      */
/*  that needs a separate `aplay` step because it emits raw WAV on     */
/*  stdout.                                                            */
/* ------------------------------------------------------------------ */

/** Forward declarations of per-engine voice enumerators — defined
 *  below the engine table because the table needs their addresses,
 *  while they themselves can stand alone with no other dependencies. */
static gchar **list_piper_voices     (void);
static gchar **list_espeak_ng_voices (void);
static gchar **list_espeak_voices    (void);
static gchar **list_flite_voices     (void);

static gchar  *speed_flag_piper       (double speed);
static gchar  *speed_flag_espeak      (double speed);
static gchar  *speed_flag_flite       (double speed);
static gchar  *speed_flag_none        (double speed);

typedef struct
{
    const gchar *id;              /* persisted in the saved flow */
    const gchar *label;           /* human-readable name in the combo */
    const gchar *program;         /* program name to find on PATH */
    /* Shell pipeline run when no voice is configured.  %NULL means
     * the engine has no usable default and refuses to speak without
     * an explicit voice (piper).  The first %s slot, when present,
     * is the engine's speed-adjust CLI fragment (e.g. "-s 350"); the
     * second is the quoted voice argument. */
    const gchar *cmd_no_voice;
    /* Shell pipeline run when a voice IS configured. */
    const gchar *cmd_with_voice;
    /* Returns a NULL-terminated strv of voice ids, or %NULL if the
     * engine has no enumerable voice selector — in which case the
     * dialog shows a single "Default" entry. */
    gchar **(*list_voices) (void);
    /* Translate a speed multiplier (1.0 = engine default, >1 faster,
     * <1 slower) into a newly-allocated CLI fragment that goes into
     * the first %s slot of the command templates.  Returns "" when
     * the engine has no clean rate knob, so the templates always
     * substitute cleanly. */
    gchar *(*speed_flag) (double speed);
    /* Audio-sink routing.  When NULL, the sink is delivered via the
     * PULSE_SINK environment variable on the subprocess launcher —
     * the right shape for engines that hand audio straight to
     * PulseAudio (espeak family, flite, festival).  Otherwise this
     * is a printf template with one %s slot for the shell-quoted
     * sink name, appended (with a leading space) to the built
     * command line only when a sink is configured — currently only
     * piper, whose `paplay` step takes `-d SINK_NAME`. */
    const gchar *sink_append_template;
} TtsEngine;

static const TtsEngine engines[] = {
    { "piper",     "Piper",     "piper",
      NULL,
      "piper %s -m %s -q -f - | paplay",
      list_piper_voices,
      speed_flag_piper,
      "-d %s" },
    { "espeak-ng", "eSpeak NG", "espeak-ng",
      "espeak-ng %s",
      "espeak-ng %s -v %s",
      list_espeak_ng_voices,
      speed_flag_espeak,
      NULL },
    { "espeak",    "eSpeak",    "espeak",
      "espeak %s",
      "espeak %s -v %s",
      list_espeak_voices,
      speed_flag_espeak,
      NULL },
    { "festival",  "Festival",  "festival",
      "festival %s --tts",
      "festival %s --tts",
      NULL,
      speed_flag_none,
      NULL },
    { "flite",     "Flite",     "flite",
      "flite %s -f -",
      "flite %s -voice %s -f -",
      list_flite_voices,
      speed_flag_flite,
      NULL },
};

#define N_ENGINES G_N_ELEMENTS (engines)

#define PN_TTS_NORMAL_ICON  "\xef\x82\xa1"  /* fa-bullhorn U+F0A1 */
#define PN_TTS_WARNING_ICON "\xe2\x9d\x97"  /* ❗ U+2757 */

static const TtsEngine *
find_engine (const gchar *id)
{
    if (id == NULL)
        return NULL;
    for (gsize i = 0; i < N_ENGINES; i++)
        if (g_strcmp0 (id, engines[i].id) == 0)
            return &engines[i];
    return NULL;
}

/** Cached availability lookup: `g_find_program_in_path()` is a stat
 *  walk over $PATH, and the dialog re-checks the same binaries every
 *  time the combo repopulates — caching the boolean keeps the per-row
 *  cost down to a hash lookup. */
static gboolean
engine_is_installed (const TtsEngine *eng)
{
    static GHashTable *cache = NULL;
    gpointer           hit;

    if (cache == NULL)
        cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                       g_free, NULL);

    if (g_hash_table_lookup_extended (cache, eng->program, NULL, &hit))
        return GPOINTER_TO_INT (hit) != 0;

    gchar    *path  = g_find_program_in_path (eng->program);
    gboolean  found = path != NULL;
    g_free (path);
    g_hash_table_insert (cache, g_strdup (eng->program),
                         GINT_TO_POINTER (found ? 1 : 0));
    return found;
}

struct _PnTts
{
    PnNode parent_instance;

    /* Engine id (one of the `id` fields above).  The setter validates
     * against the table and updates `last_error` plus the visual state
     * if the chosen engine is missing from the host. */
    gchar *engine;

    /* Voice argument passed to the selected engine.  A .onnx file
     * path for piper, an engine-specific short name for the others,
     * or the empty string when the engine has no voice selector. */
    gchar *model;

    /* Speed multiplier.  1.0 is the engine's natural rate; values
     * above 1.0 speak faster, below slower.  Translated to the
     * engine's own knob (length-scale, words-per-minute, duration-
     * stretch) at speak() time via TtsEngine.speed_flag. */
    double speed;

    /* PulseAudio sink name (e.g. "alsa_output.pci-…").  Empty means
     * "let PulseAudio pick its default sink", which is also what
     * happens if pactl/paplay are missing on the host.  Applied at
     * speak() time either as a `paplay -d` argument (piper) or via
     * the PULSE_SINK environment variable on the subprocess
     * launcher (every other engine). */
    gchar *sink;

    /* Sticky last-error string, surfaced both in the settings dialog's
     * status row and as the red ❗ visual state.  NULL when the
     * selected engine is installed and otherwise healthy. */
    gchar *last_error;

    /* When TRUE, the receive path hashes the incoming message's `from`
     * label and uses that hash to pick a voice out of the engine's
     * full voice list -- so messages from different upstream nodes
     * end up in different voices automatically, giving a many-to-one
     * "chat with multiple speakers" feel without per-source wiring.
     * The selection is deterministic per (engine, source-name) pair
     * so the same speaker always gets the same voice across runs.
     * Falls back to the configured `model` whenever the override
     * cannot be applied (no enumerable voices, no source, hash to an
     * empty list, etc.).  Defaults to TRUE. */
    gboolean per_source_voice;

    /* %TRUE while a speak subprocess is still running.  New incoming
     * messages either queue up (see `pending` / `max_queue`) or get
     * dropped on the floor depending on the configured queue depth;
     * the flag flips back when the subprocess finishes via
     * on_speak_done(), which also drains the next pending utterance. */
    gboolean speaking;

    /* Pending utterances that arrived while `speaking` was %TRUE.
     * Drained one at a time from on_speak_done() so the next clip
     * starts as soon as the current one finishes — no overlap, no
     * loss.  Items are PendingUtterance* owned by the queue. */
    GQueue *pending;

    /* Maximum number of utterances allowed to sit in `pending` while
     * one is currently being spoken.  0 reproduces the old "drop
     * while busy" behaviour, -1 means unbounded (never drop), any
     * positive N caps the backlog at N items and drops anything
     * arriving beyond that.  Defaults to 16. */
    gint max_queue;
};

typedef struct
{
    gchar *text;
    gchar *voice;   /* may be NULL — falls back to self->model */
} PendingUtterance;

static void
pending_utterance_free (gpointer data)
{
    PendingUtterance *u = data;
    g_free (u->text);
    g_free (u->voice);
    g_free (u);
}

G_DEFINE_TYPE (PnTts, pn_tts, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_ENGINE,
    PROP_MODEL,
    PROP_SPEED,
    PROP_SINK,
    PROP_LAST_ERROR,
    PROP_PER_SOURCE_VOICE,
    PROP_MAX_QUEUE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void apply_visual_state (PnTts *self);
static void pn_tts_speak         (PnTts       *self,
                                  const gchar *text,
                                  const gchar *voice_override);

/* ------------------------------------------------------------------ */
/*  Audio pipeline                                                     */
/* ------------------------------------------------------------------ */

static void
on_speak_done (
        GObject      *source,
        GAsyncResult *result,
        gpointer      user_data)
{
    GSubprocess *sub   = G_SUBPROCESS (source);
    PnTts       *self  = PN_TTS (user_data);
    GError      *error = NULL;

    g_subprocess_communicate_finish (sub, result, NULL, NULL, &error);
    if (error != NULL)
    {
        g_warning ("pn-tts: pipeline failed: %s", error->message);
        g_error_free (error);
    }

    self->speaking = FALSE;

    g_object_unref (sub);

    /* Pull the next queued utterance, if any, and speak it now that
     * the audio device is free.  The pn_tts_speak() call sets
     * `speaking` back to TRUE and takes its own ref on self for the
     * follow-up callback, so the unref below balances ONLY the ref
     * the just-completed subprocess held. */
    PendingUtterance *next = g_queue_pop_head (self->pending);
    if (next != NULL)
    {
        pn_tts_speak (self, next->text, next->voice);
        pending_utterance_free (next);
    }

    g_object_unref (self);
}

/** Spawn the engine's shell pipeline in the background and feed @text
 *  to its stdin.  Returns immediately so receive() does not block the
 *  main loop.  Refuses to spawn when no engine is selected, when the
 *  selected engine is not on $PATH, or when piper is selected with no
 *  model configured.  When @voice_override is non-NULL and non-empty
 *  it is used in place of the configured `model` for this call only —
 *  used by the per-source-voice receive path to swap voices per
 *  message without disturbing the persisted property. */
static void
pn_tts_speak (
        PnTts       *self,
        const gchar *text,
        const gchar *voice_override)
{
    const TtsEngine *eng = find_engine (self->engine);

    if (eng == NULL)
    {
        g_warning ("pn-tts: no engine selected; refusing to speak");
        return;
    }

    if (!engine_is_installed (eng))
    {
        g_warning ("pn-tts: engine '%s' is not installed", eng->id);
        return;
    }

    const gchar *voice = (voice_override != NULL && *voice_override != '\0')
                             ? voice_override
                             : self->model;
    gboolean have_voice = voice != NULL && *voice != '\0';

    if (!have_voice && eng->cmd_no_voice == NULL)
    {
        g_warning ("pn-tts: %s requires a voice; refusing to speak",
                   eng->label);
        return;
    }

    /* Drop new requests while an earlier clip is still playing so two
     * voices never overlap in the speaker. */
    if (self->speaking)
        return;

    gchar *speed_arg = eng->speed_flag (self->speed);
    gchar *cmd;
    if (have_voice)
    {
        gchar *quoted = g_shell_quote (voice);
        cmd = g_strdup_printf (eng->cmd_with_voice, speed_arg, quoted);
        g_free (quoted);
    }
    else
    {
        cmd = g_strdup_printf (eng->cmd_no_voice, speed_arg);
    }
    g_free (speed_arg);

    /* Audio sink routing.  Two shapes: piper's pipeline ends with
     * paplay, so the sink becomes a `-d SINK` argument tacked onto
     * the command line; every other engine speaks straight to
     * PulseAudio, so we set PULSE_SINK on the subprocess
     * environment and leave the command line alone. */
    gboolean have_sink = self->sink != NULL && *self->sink != '\0';
    if (have_sink && eng->sink_append_template != NULL)
    {
        gchar *qsink   = g_shell_quote (self->sink);
        gchar *suffix  = g_strdup_printf (eng->sink_append_template, qsink);
        gchar *new_cmd = g_strdup_printf ("%s %s", cmd, suffix);
        g_free (qsink);
        g_free (suffix);
        g_free (cmd);
        cmd = new_cmd;
    }

    GSubprocessLauncher *launcher = g_subprocess_launcher_new (
            G_SUBPROCESS_FLAGS_STDIN_PIPE
            | G_SUBPROCESS_FLAGS_STDOUT_SILENCE
            | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    if (have_sink && eng->sink_append_template == NULL)
        g_subprocess_launcher_setenv (launcher, "PULSE_SINK",
                                      self->sink, TRUE);

    GError      *error = NULL;
    GSubprocess *sub   = g_subprocess_launcher_spawn (
            launcher, &error, "sh", "-c", cmd, NULL);
    g_object_unref (launcher);
    g_free (cmd);

    if (sub == NULL)
    {
        g_warning ("pn-tts: failed to spawn pipeline: %s",
                   error ? error->message : "(unknown)");
        g_clear_error (&error);
        return;
    }

    self->speaking = TRUE;

    /* Flatten line-breaks before handing the text to the engine.
     * Piper in particular synthesises one WAV per input line and
     * `paplay` only consumes the first one, so a multi-line LLM
     * reply ("1. foo\n2. bar") would speak just the opening line.
     * Replacing \r / \n / \t with spaces collapses the input into a
     * single utterance every engine speaks straight through, and
     * works for the line-tolerant engines too (espeak / festival /
     * flite) since extra whitespace is silently absorbed. */
    gchar *flat = g_strdup (text);
    for (gchar *p = flat; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';

    GBytes *stdin_bytes = g_bytes_new_take (flat, strlen (flat));
    g_subprocess_communicate_async (sub, stdin_bytes, NULL,
                                    on_speak_done, g_object_ref (self));
    g_bytes_unref (stdin_bytes);
}

/* ------------------------------------------------------------------ */
/*  Per-source voice picker                                            */
/*                                                                     */
/*  Hash the source-node's display label and use the hash modulo the   */
/*  engine's voice-list length to pick a voice deterministically per   */
/*  speaker.  The returned string is a freshly-allocated copy owned by */
/*  the caller (free with g_free); %NULL means "fall back to the       */
/*  configured `model`" -- the engine has no enumerable voices, the    */
/*  message has no source, or anything else that breaks the lookup.    */
/* ------------------------------------------------------------------ */

/* Deterministically map a sender label @who to one of @n_voices slots,
 * so every message from the same source always speaks in the same voice.
 * Pure: the hash makes the choice stable across runs without storing a
 * per-sender table.  Returns 0 when @n_voices is 0. */
guint
pn_tts_voice_index_for (const gchar *who, guint n_voices)
{
    if (who == NULL || n_voices == 0)
        return 0;
    return g_str_hash (who) % n_voices;
}

static gchar *
pick_voice_for_message (
        PnTts     *self,
        PnMessage *message)
{
    if (!self->per_source_voice || message == NULL)
        return NULL;

    PnNode *source = pn_message_get_source (message);
    if (source == NULL)
        return NULL;

    const gchar *who = pn_node_get_name (source);
    if (who == NULL || *who == '\0')
        who = pn_node_get_class_name (source);
    if (who == NULL || *who == '\0')
        return NULL;

    const TtsEngine *eng = find_engine (self->engine);
    if (eng == NULL || eng->list_voices == NULL)
        return NULL;

    gchar **voices = eng->list_voices ();
    if (voices == NULL || voices[0] == NULL)
    {
        g_strfreev (voices);
        return NULL;
    }

    guint n     = g_strv_length (voices);
    gchar *out  = g_strdup (voices[pn_tts_voice_index_for (who, n)]);

    g_strfreev (voices);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_tts_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTts       *self = PN_TTS (node);
    JsonObject  *data = pn_message_get_data (message);
    JsonNode    *n;
    const gchar *text;

    if (data == NULL || !json_object_has_member (data, "output"))
        return;

    n = json_object_get_member (data, "output");
    if (!JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_STRING)
        return;

    text = json_node_get_string (n);
    if (text == NULL || *text == '\0')
        return;

    gchar *voice = pick_voice_for_message (self, message);

    /* Idle device → speak immediately.  Otherwise the new utterance
     * joins the pending queue, capped by `max_queue`: 0 reproduces
     * the original "drop while busy" behaviour, -1 means unbounded,
     * any positive N caps the backlog at N items and warns on
     * overflow so a runaway upstream is visible in the logs. */
    if (!self->speaking)
    {
        pn_tts_speak (self, text, voice);
        g_free (voice);
        return;
    }

    if (self->max_queue == 0 ||
        (self->max_queue > 0 &&
         (gint) g_queue_get_length (self->pending) >= self->max_queue))
    {
        g_warning ("pn-tts: queue full (max-queue=%d), dropping utterance",
                   self->max_queue);
        g_free (voice);
        return;
    }

    PendingUtterance *u = g_new0 (PendingUtterance, 1);
    u->text  = g_strdup (text);
    u->voice = voice;   /* transfers ownership */
    g_queue_push_tail (self->pending, u);
}

/* ------------------------------------------------------------------ */
/*  Error / visual state                                               */
/* ------------------------------------------------------------------ */

static void
set_last_error (PnTts *self, const gchar *reason)
{
    if (g_strcmp0 (self->last_error, reason) == 0)
        return;

    g_free (self->last_error);
    self->last_error = (reason != NULL && *reason != '\0')
                       ? g_strdup (reason)
                       : NULL;

    apply_visual_state (self);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LAST_ERROR]);
}

static void
apply_visual_state (PnTts *self)
{
    PnNode *node = PN_NODE (self);

    if (self->last_error == NULL)
    {
        GdkRGBA violet = { 0.55, 0.42, 0.78, 1.0 };
        pn_node_set_color (node, &violet);
        pn_node_set_icon  (node, PN_TTS_NORMAL_ICON);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_TTS_WARNING_ICON);
    }
}

/** Recompute the error string from the current engine selection: an
 *  unknown engine id or an engine whose program is not installed
 *  yields a descriptive message; otherwise the slot is cleared. */
static void
refresh_engine_error (PnTts *self)
{
    const TtsEngine *eng = find_engine (self->engine);
    gchar           *msg = NULL;

    if (self->engine == NULL || *self->engine == '\0')
        msg = g_strdup ("No engine selected.");
    else if (eng == NULL)
        msg = g_strdup_printf ("Unknown engine '%s'.", self->engine);
    else if (!engine_is_installed (eng))
        msg = g_strdup_printf ("%s is not installed (need '%s' on $PATH).",
                               eng->label, eng->program);

    set_last_error (self, msg);
    g_free (msg);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tts_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTts *self = PN_TTS (object);

    switch (prop_id)
    {
    case PROP_ENGINE:
        g_value_set_string (value, self->engine);
        break;
    case PROP_MODEL:
        g_value_set_string (value, self->model);
        break;
    case PROP_SPEED:
        g_value_set_double (value, self->speed);
        break;
    case PROP_SINK:
        g_value_set_string (value, self->sink);
        break;
    case PROP_LAST_ERROR:
        g_value_set_string (value, self->last_error);
        break;
    case PROP_PER_SOURCE_VOICE:
        g_value_set_boolean (value, self->per_source_voice);
        break;
    case PROP_MAX_QUEUE:
        g_value_set_int (value, self->max_queue);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tts_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTts *self = PN_TTS (object);

    switch (prop_id)
    {
    case PROP_ENGINE:
        {
            const gchar *new_engine = g_value_get_string (value);
            if (g_strcmp0 (self->engine, new_engine) != 0)
            {
                g_free (self->engine);
                self->engine = g_strdup (new_engine);
                refresh_engine_error (self);
                g_object_notify_by_pspec (object, props[PROP_ENGINE]);
            }
        }
        break;
    case PROP_MODEL:
        {
            const gchar *new_model = g_value_get_string (value);
            if (g_strcmp0 (self->model, new_model) != 0)
            {
                g_free (self->model);
                self->model = g_strdup (new_model);
                g_object_notify_by_pspec (object, props[PROP_MODEL]);
            }
        }
        break;
    case PROP_SPEED:
        {
            double new_speed = g_value_get_double (value);
            if (self->speed != new_speed)
            {
                self->speed = new_speed;
                g_object_notify_by_pspec (object, props[PROP_SPEED]);
            }
        }
        break;
    case PROP_SINK:
        {
            const gchar *new_sink = g_value_get_string (value);
            if (g_strcmp0 (self->sink, new_sink) != 0)
            {
                g_free (self->sink);
                self->sink = g_strdup (new_sink != NULL ? new_sink : "");
                g_object_notify_by_pspec (object, props[PROP_SINK]);
            }
        }
        break;
    case PROP_PER_SOURCE_VOICE:
        {
            gboolean v = g_value_get_boolean (value);
            if (self->per_source_voice != v)
            {
                self->per_source_voice = v;
                g_object_notify_by_pspec (object,
                                          props[PROP_PER_SOURCE_VOICE]);
            }
        }
        break;
    case PROP_MAX_QUEUE:
        {
            gint v = g_value_get_int (value);
            if (self->max_queue != v)
            {
                self->max_queue = v;
                g_object_notify_by_pspec (object, props[PROP_MAX_QUEUE]);
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
pn_tts_finalize (GObject *object)
{
    PnTts *self = PN_TTS (object);

    g_clear_pointer (&self->engine,     g_free);
    g_clear_pointer (&self->model,      g_free);
    g_clear_pointer (&self->sink,       g_free);
    g_clear_pointer (&self->last_error, g_free);

    if (self->pending != NULL)
    {
        g_queue_free_full (self->pending, pending_utterance_free);
        self->pending = NULL;
    }

    G_OBJECT_CLASS (pn_tts_parent_class)->finalize (object);
}

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

    g_object_get (target, name, &current, NULL);

    for (gsize i = 0; i < N_ENGINES; i++)
    {
        const TtsEngine *eng = &engines[i];
        gchar           *label;

        /* Suffix unavailable engines so the user can see at a glance
         * which ones are wired up — they remain selectable so picking
         * one surfaces the error message in the status row and the
         * red visual state on the node body. */
        if (engine_is_installed (eng))
            label = g_strdup (eng->label);
        else
            label = g_strdup_printf ("%s (not installed)", eng->label);

        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   eng->id, label);
        g_free (label);

        if (g_strcmp0 (eng->id, current) == 0)
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

/** Derive the human-readable label for a voice id.  Piper voices are
 *  full paths to .onnx files — the same "speaker-quality" collapse
 *  the original dialog used is preserved here.  Every other engine
 *  identifies its voices by short names that are already readable
 *  as-is. */
gchar *
pn_tts_derive_voice_label (const gchar *engine_id, const gchar *id)
{
    if (g_strcmp0 (engine_id, "piper") == 0)
    {
        gchar        *base = g_path_get_basename (id);
        gchar        *dot  = g_strrstr (base, ".onnx");
        gchar       **parts;
        gchar        *out;

        if (dot != NULL)
            *dot = '\0';

        /* Drop a leading "<lang>_<COUNTRY>-" prefix so the label
         * collapses to "speaker-quality".  Anything that does not
         * match the pattern is shown verbatim. */
        parts = g_strsplit (base, "-", 2);
        if (parts[0] != NULL && parts[1] != NULL &&
            strchr (parts[0], '_') != NULL)
            out = g_strdup (parts[1]);
        else
            out = g_strdup (base);

        g_strfreev (parts);
        g_free (base);
        return out;
    }
    return g_strdup (id);
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
    gchar           *engine_id = NULL;
    gchar           *current   = NULL;
    const TtsEngine *eng;
    gchar          **voices    = NULL;

    g_object_get (target, "engine", &engine_id, "model", &current, NULL);
    eng = find_engine (engine_id);

    /* gtk_combo_box_text_remove_all() drops active-id; the property
     * still holds the user's pick so the SYNC_CREATE binding will
     * push it back in once we've finished appending the new rows
     * (or we overwrite the property below). */
    gtk_combo_box_text_remove_all (combo);

    if (eng != NULL && eng->list_voices != NULL)
        voices = eng->list_voices ();

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
            gchar *label = pn_tts_derive_voice_label (eng->id, *p);
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
/*  keep it in sync via notify::last-error so a runtime change (e.g.   */
/*  the user picking an uninstalled engine in the combo above) shows  */
/*  up immediately.                                                    */
/* ------------------------------------------------------------------ */

static void
update_status_label (GObject    *obj,
                     GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer    user_data)
{
    PnTts    *self  = PN_TTS (obj);
    GtkLabel *label = GTK_LABEL (user_data);
    gchar    *markup;

    if (self->last_error != NULL && *self->last_error != '\0')
    {
        gchar *escaped = g_markup_escape_text (self->last_error, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">%s</span>", escaped);
        g_free (escaped);
    }
    else
    {
        const TtsEngine *eng = find_engine (self->engine);
        if (eng != NULL)
            markup = g_strdup_printf ("%s is ready.", eng->label);
        else
            markup = g_strdup ("");
    }

    gtk_label_set_markup (label, markup);
    g_free (markup);
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
    st->source_id = 0;

    /* Skip the playback if the node is in an error state — speaking
     * would just print the same warning to stderr and the dialog's
     * red status row already tells the user why nothing happens. */
    if (st->self->last_error == NULL)
        pn_tts_speak (st->self,
                      "This is a sample of the selected voice.",
                      NULL);
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
    GObject   *target = G_OBJECT (self);
    GtkWidget *grid   = pn_node_dialog_new_property_grid ();
    GtkWidget *engine_editor;
    GtkWidget *model_editor;
    GtkWidget *status_label;

    engine_editor = pn_tts_build_property_editor (
            self, props[PROP_ENGINE], target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 0,
                               g_param_spec_get_nick (props[PROP_ENGINE]),
                               engine_editor);

    model_editor = pn_tts_build_property_editor (
            self, props[PROP_MODEL], target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 1,
                               g_param_spec_get_nick (props[PROP_MODEL]),
                               model_editor);

    /* per-source-voice sits directly under Voice because it modifies
     * how the voice is picked at receive time — keeping the two rows
     * adjacent makes the relationship obvious in the dialog. */
    GtkWidget *psv_editor = pn_node_dialog_default_editor (
            target, props[PROP_PER_SOURCE_VOICE]);
    pn_node_dialog_attach_row (GTK_GRID (grid), 2,
                               g_param_spec_get_nick (
                                   props[PROP_PER_SOURCE_VOICE]),
                               psv_editor);

    /* Speed is a plain double in [0.5, 2.0] — the introspection-
     * driven default editor gives us a spin button with the right
     * range, two-digit precision and 0.1 step out of the box. */
    GtkWidget *speed_editor = pn_node_dialog_default_editor (
            target, props[PROP_SPEED]);
    pn_node_dialog_attach_row (GTK_GRID (grid), 3,
                               g_param_spec_get_nick (props[PROP_SPEED]),
                               speed_editor);

    GtkWidget *sink_editor = pn_tts_build_property_editor (
            self, props[PROP_SINK], target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 4,
                               g_param_spec_get_nick (props[PROP_SINK]),
                               sink_editor);

    GtkWidget *max_queue_editor = pn_node_dialog_default_editor (
            target, props[PROP_MAX_QUEUE]);
    pn_node_dialog_attach_row (GTK_GRID (grid), 5,
                               g_param_spec_get_nick (props[PROP_MAX_QUEUE]),
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

static void
pn_tts_class_init (PnTtsClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_tts_get_property;
    object_class->set_property = pn_tts_set_property;
    object_class->finalize     = pn_tts_finalize;
    node_class->receive        = pn_tts_receive;

    node_class->build_property_editor = pn_tts_build_property_editor;
    node_class->build_class_tab       = pn_tts_build_class_tab;

    node_class->class_name     = "Text to Speech";
    node_class->icon           = PN_TTS_NORMAL_ICON;
    node_class->color          = (GdkRGBA){ 0.55, 0.42, 0.78, 1.0 };
    node_class->category       = "Sinks";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    props[PROP_ENGINE] = g_param_spec_string (
            "engine", "Engine",
            "Text-to-speech program used to synthesise speech "
            "(piper, espeak-ng, espeak, festival, flite)",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Property name stays "model" so existing saved flows keep loading
     * cleanly; nick is "Voice" because that is what the dialog row
     * label and help text now call it. */
    props[PROP_MODEL] = g_param_spec_string (
            "model", "Voice",
            "Voice used by the selected engine — a .onnx file path for "
            "Piper, an engine-specific voice name otherwise, or empty "
            "for engines that have no enumerable voice selector",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* 0.5×–2× span covers the useful range of all four supported-
     * speed engines (piper, espeak, espeak-ng, flite) without
     * pushing them into the unintelligible end of their own scales.
     * 1.0 is the engine's natural rate.  Festival ignores the value
     * since it has no clean CLI knob. */
    props[PROP_SPEED] = g_param_spec_double (
            "speed", "Speed",
            "Speed multiplier applied to the selected engine "
            "(1.0 = natural rate, >1.0 faster, <1.0 slower)",
            0.5, 2.0, 1.0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* PulseAudio sink name (the same string `pactl list short sinks`
     * shows in column 2).  Empty == "default sink", which is what an
     * unconfigured node uses and what hosts without pactl/paplay end
     * up with regardless of the value. */
    props[PROP_SINK] = g_param_spec_string (
            "sink", "Output",
            "PulseAudio sink the speech is routed to "
            "(empty means the system default sink)",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Read-only at the property level so the JSON serialiser skips it
     * — last_error is transient runtime state, not part of the saved
     * flow.  Internal mutation goes through set_last_error(), which
     * writes the field and fires notify directly. */
    props[PROP_LAST_ERROR] = g_param_spec_string (
            "last-error", "Last Error",
            "Last sticky engine-availability error string, or empty",
            NULL,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    props[PROP_PER_SOURCE_VOICE] = g_param_spec_boolean (
            "per-source-voice", "Per-source voice",
            "Pick a voice automatically for each incoming message by "
            "hashing the source node's name -- gives every speaker a "
            "deterministic but distinct voice without per-source "
            "wiring.  Falls back to the configured voice when the "
            "engine has no enumerable voice list",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Backlog cap for utterances that arrive while a previous clip
     * is still playing.  0 reproduces the original "drop while busy"
     * behaviour, -1 means unbounded (never drop, at the risk of a
     * runaway upstream growing the queue without limit), any
     * positive N caps the backlog at N items.  16 is a reasonable
     * chat-sized default. */
    props[PROP_MAX_QUEUE] = g_param_spec_int (
            "max-queue", "Max queue",
            "Maximum number of utterances allowed to wait while one "
            "is already being spoken.  0 drops new messages while "
            "busy (old behaviour), -1 queues without limit, any "
            "positive value caps the backlog at that many items",
            -1, G_MAXINT, 16,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

/** Return the id of the first installed engine in declaration order,
 *  or the first engine in the table if none are installed (the setter
 *  will then mark the node red and surface the missing-engine error). */
static const gchar *
default_engine_id (void)
{
    for (gsize i = 0; i < N_ENGINES; i++)
        if (engine_is_installed (&engines[i]))
            return engines[i].id;
    return engines[0].id;
}

static void
pn_tts_init (PnTts *self)
{
    PnNode *node = PN_NODE (self);

    self->speaking         = FALSE;
    self->speed            = 1.0;
    self->sink             = g_strdup ("");
    self->last_error       = NULL;
    self->per_source_voice = TRUE;
    self->pending          = g_queue_new ();
    self->max_queue        = 16;

    pn_node_set_class_name (node, "Text to Speech");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    /* Default to the Lessac English voice installed under the user's
     * XDG-style data directory; can be overridden via the property. */
    self->model = g_build_filename (
            g_get_home_dir (),
            ".local", "share", "piper", "voices",
            "en_US-lessac-medium.onnx",
            NULL);

    /* Pick the first installed engine so a fresh node out of the
     * palette already works on hosts that have any TTS at all. */
    self->engine = g_strdup (default_engine_id ());
    refresh_engine_error (self);

    /* Seed colour + icon unconditionally: refresh_engine_error above
     * only flips the visual state when last_error actually changes,
     * and on a fresh node the happy path is NULL→NULL — which would
     * leave the body at PnNode's grey default until the first error
     * arrived. */
    apply_visual_state (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTts *
pn_tts_new (void)
{
    return g_object_new (PN_TYPE_TTS, NULL);
}

static int
voice_model_path_compare (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const gchar * const *) a,
                      *(const gchar * const *) b);
}

gchar **
pn_tts_list_voice_models (void)
{
    gchar      *dir_path = g_build_filename (g_get_home_dir (),
                                             ".local", "share",
                                             "piper", "voices",
                                             NULL);
    GPtrArray  *paths    = g_ptr_array_new ();
    GDir       *dir      = g_dir_open (dir_path, 0, NULL);

    if (dir != NULL)
    {
        const gchar *entry;

        while ((entry = g_dir_read_name (dir)) != NULL)
        {
            if (!g_str_has_suffix (entry, ".onnx"))
                continue;

            g_ptr_array_add (paths,
                             g_build_filename (dir_path, entry, NULL));
        }
        g_dir_close (dir);
    }

    g_free (dir_path);

    g_ptr_array_sort (paths, voice_model_path_compare);
    g_ptr_array_add  (paths, NULL);

    return (gchar **) g_ptr_array_free (paths, FALSE);
}

gchar **
pn_tts_list_audio_sinks (void)
{
    GPtrArray *out = g_ptr_array_new ();
    gchar     *raw = NULL;
    gint       status = 0;

    if (!g_spawn_command_line_sync ("pactl list sinks",
                                    &raw, NULL, &status, NULL)
        || !g_spawn_check_wait_status (status, NULL)
        || raw == NULL)
    {
        g_free (raw);
        g_ptr_array_add (out, NULL);
        return (gchar **) g_ptr_array_free (out, FALSE);
    }

    /* Walk the long-form output sink by sink, picking up the
     * "Name:" + "Description:" lines.  `pactl` always prints Name
     * before Description, so a single forward scan with a held-over
     * pending-name string is enough — no need to keep more state. */
    gchar **lines        = g_strsplit (raw, "\n", -1);
    gchar  *pending_name = NULL;

    for (gsize i = 0; lines[i] != NULL; i++)
    {
        const gchar *line = lines[i];
        while (*line == '\t' || *line == ' ')
            line++;

        if (g_str_has_prefix (line, "Name:"))
        {
            const gchar *v = line + strlen ("Name:");
            while (*v == ' ' || *v == '\t')
                v++;
            g_free (pending_name);
            pending_name = g_strdup (v);
        }
        else if (g_str_has_prefix (line, "Description:")
                 && pending_name != NULL)
        {
            const gchar *v = line + strlen ("Description:");
            while (*v == ' ' || *v == '\t')
                v++;
            g_ptr_array_add (out, pending_name);
            g_ptr_array_add (out, g_strdup (v));
            pending_name = NULL;
        }
    }

    g_free (pending_name);
    g_strfreev (lines);
    g_free (raw);

    g_ptr_array_add (out, NULL);
    return (gchar **) g_ptr_array_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Per-engine voice enumeration                                       */
/*                                                                     */
/*  Each function returns a NULL-terminated array of voice ids the     */
/*  spawn pipeline can pass via the engine's `-v` / `-voice` flag.    */
/*  Returning %NULL (or an empty array — both are handled equally     */
/*  upstream) signals "no enumerable voices" and the dialog falls back */
/*  to the single "Default" entry.                                     */
/* ------------------------------------------------------------------ */

static gchar **
list_piper_voices (void)
{
    return pn_tts_list_voice_models ();
}

/* ------------------------------------------------------------------ */
/*  Per-engine speed flags                                             */
/*                                                                     */
/*  Translate the normalised speed multiplier (1.0 = engine default,   */
/*  >1 faster, <1 slower) into the engine's own CLI fragment.  Each    */
/*  helper returns a newly-allocated string the caller frees with      */
/*  g_free(); an empty string means "no flag needed" (the command      */
/*  templates always carry a %s slot for it so the substitution stays  */
/*  uniform).                                                          */
/* ------------------------------------------------------------------ */

static gchar *
speed_flag_none (double speed G_GNUC_UNUSED)
{
    /* Festival has no clean CLI knob for rate — the only way is to
     * inject a Scheme `Parameter.set Duration_Stretch` call into the
     * session, which doesn't fit the `festival --tts` invocation we
     * use.  Quietly ignore the multiplier rather than rebuild the
     * command path for one engine. */
    return g_strdup ("");
}

static gchar *
speed_flag_piper (double speed)
{
    /* piper accepts --length-scale FLOAT.  The flag is the INVERSE
     * of speed: smaller length-scale → faster speech.  A 1.0 speed
     * (== default) skips the flag entirely so users without rate
     * customisation see the unmodified piper command line. */
    if (speed == 1.0)
        return g_strdup ("");
    return g_strdup_printf ("--length-scale %.3f", 1.0 / speed);
}

static gchar *
speed_flag_espeak (double speed)
{
    /* espeak / espeak-ng accept -s WPM (words per minute).  175 is
     * the documented default both binaries fall back to when no -s
     * is passed; multiplying it by the speed factor lands the
     * 0.5×–2× span squarely inside the engine's [80, 450] clamp. */
    if (speed == 1.0)
        return g_strdup ("");
    int wpm = (int) (175.0 * speed + 0.5);
    return g_strdup_printf ("-s %d", wpm);
}

static gchar *
speed_flag_flite (double speed)
{
    /* flite accepts --setf duration_stretch=FLOAT.  Like piper this
     * is the INVERSE of speed.  Skip when speed == 1.0 so the cmd
     * line stays minimal. */
    if (speed == 1.0)
        return g_strdup ("");
    return g_strdup_printf ("--setf duration_stretch=%.3f", 1.0 / speed);
}

/** Run @cmdline, capture its stdout, and return it as a newly-
 *  allocated string — or %NULL if the spawn failed or the program
 *  exited non-zero.  Used by the engine voice-enumeration helpers. */
static gchar *
spawn_capture (const gchar *cmdline)
{
    gchar    *out    = NULL;
    gint      status = 0;
    GError   *error  = NULL;

    if (!g_spawn_command_line_sync (cmdline, &out, NULL, &status, &error))
    {
        g_clear_error (&error);
        g_free (out);
        return NULL;
    }
    if (!g_spawn_check_wait_status (status, NULL))
    {
        g_free (out);
        return NULL;
    }
    return out;
}

/** Parse the second whitespace-separated field of every non-header
 *  line in `<program> --voices` (the language-code column) into a
 *  voice-id strv.  Both espeak and espeak-ng share the same output
 *  shape — only the program name differs. */
static gchar **
list_espeak_family_voices (const gchar *program)
{
    gchar *cmd = g_strdup_printf ("%s --voices", program);
    gchar *out = spawn_capture (cmd);
    g_free (cmd);

    if (out == NULL)
        return NULL;

    GPtrArray  *voices = g_ptr_array_new ();
    gchar     **lines  = g_strsplit (out, "\n", -1);

    /* Skip the header line and any blank lines after it. */
    for (gsize i = 1; lines[i] != NULL; i++)
    {
        if (*lines[i] == '\0')
            continue;

        /* Pull out the 2nd whitespace-separated field — the
         * language-code column (e.g. "en-us", "es"). */
        gchar     **fields = g_strsplit_set (lines[i], " \t", -1);
        gsize       seen   = 0;
        const gchar *picked = NULL;
        for (gsize j = 0; fields[j] != NULL; j++)
        {
            if (*fields[j] == '\0')
                continue;
            if (++seen == 2)
            {
                picked = fields[j];
                break;
            }
        }
        if (picked != NULL && *picked != '\0')
            g_ptr_array_add (voices, g_strdup (picked));
        g_strfreev (fields);
    }

    g_strfreev (lines);
    g_free (out);

    g_ptr_array_sort (voices, voice_model_path_compare);
    g_ptr_array_add  (voices, NULL);
    return (gchar **) g_ptr_array_free (voices, FALSE);
}

static gchar **
list_espeak_ng_voices (void)
{
    return list_espeak_family_voices ("espeak-ng");
}

static gchar **
list_espeak_voices (void)
{
    return list_espeak_family_voices ("espeak");
}

/** Parse `flite -lv`, which prints a single line of the form
 *  "Voices available: kal awb_time kal16 awb rms slt" — turn the
 *  space-separated tail into a voice-id strv. */
static gchar **
list_flite_voices (void)
{
    gchar *out = spawn_capture ("flite -lv");
    if (out == NULL)
        return NULL;

    GPtrArray   *voices = g_ptr_array_new ();
    const gchar *prefix = "Voices available:";
    gchar       *p      = strstr (out, prefix);

    if (p != NULL)
    {
        p += strlen (prefix);
        gchar **toks = g_strsplit_set (p, " \t\r\n", -1);
        for (gsize i = 0; toks[i] != NULL; i++)
        {
            if (*toks[i] == '\0')
                continue;
            g_ptr_array_add (voices, g_strdup (toks[i]));
        }
        g_strfreev (toks);
    }
    g_free (out);

    g_ptr_array_add (voices, NULL);
    return (gchar **) g_ptr_array_free (voices, FALSE);
}
