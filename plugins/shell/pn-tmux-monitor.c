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

#include "pn-tmux-monitor.h"
#include "pn-message.h"
#include "pn-node-dialog-helpers.h"
#include "pn-shell-host.h"

/* fa-television U+F26C — close enough to a terminal-y "session" glyph
 * within the FA-4 set the canvas font ships. */
#define PN_TMUX_MONITOR_NORMAL_ICON  "\xef\x89\xac"
#define PN_TMUX_MONITOR_WARNING_ICON "\xe2\x9d\x97"   /* ❗ U+2757 */

#define PN_TMUX_MONITOR_DEFAULT_PERIOD     5u
#define PN_TMUX_MONITOR_DEFAULT_LINE_LIMIT 50u
#define PN_TMUX_MONITOR_LINE_LIMIT_MAX     100000u

struct _PnTmuxMonitor
{
    PnAutoTrigger parent_instance;

    /* All four scalar settings, the runtime state, and the cached
     * session list ride on @mutex.  The session-enumerator worker
     * and the periodic auto-trigger worker both touch this struct
     * from non-main threads while property setters can fire on the
     * main thread, so every read or write goes through it. */
    GMutex      mutex;
    gchar      *host;
    gchar      *tmux_session;
    guint       line_limit;

    /* Latest sessions list captured by the enumerator thread.  NULL
     * means "no enumeration has finished yet" (the dialog falls back
     * to showing only the currently-configured value); a non-NULL
     * empty strv (just a trailing NULL) means "the enumerator ran
     * but the host has no sessions" — distinct so the combobox can
     * surface the empty state without flicker. */
    gchar     **sessions_cache;

    /* Settings-dialog runtime state.  `busy` flips while an
     * enumerator thread is in flight (the host dialog watches
     * notify::busy and overlays its spinner accordingly).  `last_error`
     * is non-empty when the most recent enumeration failed and feeds
     * the red status row in the per-class tab. */
    gboolean    busy;
    gchar      *last_error;

    /* Detached one-shot enumerator generation counter.  Every
     * `host` change bumps it; the enumerator captures the value it
     * was started with and its idle callback drops its result on the
     * floor when a newer enumeration has already taken over.  Stays
     * outside the mutex so the idle callback can compare without
     * waiting on whichever critical section is in progress. */
    gint        enum_gen;

    /* Previous tick's capture-pane output, kept so each emitted
     * message carries only the *new* lines since the last tick (the
     * delta), rather than the full pane the tmux command keeps
     * re-handing us.  Touched from the auto-trigger worker thread
     * via @mutex; reset to NULL whenever the host or session is
     * changed so a switch doesn't diff the new target's content
     * against unrelated history. */
    gchar      *prev_output;
};

G_DEFINE_TYPE (PnTmuxMonitor, pn_tmux_monitor, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOST,
    PROP_TMUX_SESSION,
    PROP_LINE_LIMIT,
    PROP_BUSY,
    PROP_LAST_ERROR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (PnTmuxMonitor *self,
                    gboolean       configured)
{
    PnNode *node = PN_NODE (self);

    if (configured)
    {
        GdkRGBA blue = { 0.42, 0.62, 0.86, 1.0 };
        pn_node_set_color (node, &blue);
        pn_node_set_icon  (node, PN_TMUX_MONITOR_NORMAL_ICON);
    }
    else
    {
        GdkRGBA red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_TMUX_MONITOR_WARNING_ICON);
    }
}

/* Forward declaration — the delta-baseline reset is needed from the
 * host and session setters but is defined near the trigger that
 * actually uses it. */
static void tm_reset_prev_output (PnTmuxMonitor *self);

/* ------------------------------------------------------------------ */
/*  Thread-safe accessors                                              */
/* ------------------------------------------------------------------ */

static gchar *
tm_dup_host_locked (PnTmuxMonitor *self)
{
    gchar *copy;
    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->host);
    g_mutex_unlock (&self->mutex);
    return copy;
}

static gchar *
tm_dup_session_locked (PnTmuxMonitor *self)
{
    gchar *copy;
    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->tmux_session);
    g_mutex_unlock (&self->mutex);
    return copy;
}

static guint
tm_get_line_limit_locked (PnTmuxMonitor *self)
{
    guint n;
    g_mutex_lock (&self->mutex);
    n = self->line_limit;
    g_mutex_unlock (&self->mutex);
    return n;
}

/* ------------------------------------------------------------------ */
/*  Argv wrapper that survives the remote shell                        */
/*                                                                     */
/*  pn_shell_wrap_argv() drops the literal argv after `host` for ssh   */
/*  to forward, and ssh joins those tokens with single spaces before   */
/*  handing the result to the remote login shell.  That means any      */
/*  argv element containing a character the receiving shell treats as  */
/*  syntax — for tmux's format strings the killer is `#` (line         */
/*  comment) — is re-interpreted on the other side: e.g. our argv      */
/*  ["tmux","list-sessions","-F","#S"] turns into `tmux list-sessions  */
/*  -F #S`, the remote shell trims everything from the `#` onward,     */
/*  and tmux reports "command list-sessions: -F expects an argument".  */
/*                                                                     */
/*  Wrap each element with g_shell_quote() and concatenate into one    */
/*  string, then pass *that* as the single argv element to                */
/*  pn_shell_wrap_argv: the remote shell un-quotes it back into the    */
/*  argv we built locally.  The local branch stays on the literal      */
/*  argv since g_spawn_sync() does not go through any shell.           */
/* ------------------------------------------------------------------ */

static gchar **
tm_build_argv (const gchar *host, const gchar * const *base_argv)
{
    if (pn_shell_host_is_local (host))
        return pn_shell_wrap_argv (host, base_argv);

    {
        GString     *cmd = g_string_new (NULL);
        const gchar *wrap[2];
        gchar      **out;
        guint        i;

        for (i = 0; base_argv[i] != NULL; i++)
        {
            gchar *q = g_shell_quote (base_argv[i]);
            if (i > 0)
                g_string_append_c (cmd, ' ');
            g_string_append (cmd, q);
            g_free (q);
        }

        wrap[0] = cmd->str;
        wrap[1] = NULL;
        out = pn_shell_wrap_argv (host, wrap);
        g_string_free (cmd, TRUE);
        return out;
    }
}

/* ------------------------------------------------------------------ */
/*  Session enumerator                                                 */
/*                                                                     */
/*  One-shot worker started from the `host` setter (so editing a       */
/*  remote host re-fetches its session list, and so the combobox       */
/*  refresh that follows a worksheet load happens automatically the    */
/*  first time the deserialiser writes the property).  The worker      */
/*  runs `tmux list-sessions -F '#S'` either locally or via ssh,       */
/*  parses the output into a strv, and posts a result back to the      */
/*  main loop where the property notifies fire on the right thread.    */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnTmuxMonitor  *self;       /* strong ref */
    gint            gen;
    gchar         **sessions;   /* NULL when the enumeration failed */
    gchar          *error;      /* NULL on success */
} PnTmuxEnumResult;

static void
enum_result_free (gpointer data)
{
    PnTmuxEnumResult *r = data;
    if (r == NULL)
        return;
    g_clear_pointer (&r->sessions, g_strfreev);
    g_clear_pointer (&r->error,    g_free);
    g_clear_object  (&r->self);
    g_free (r);
}

static gboolean
enum_result_deliver (gpointer data)
{
    PnTmuxEnumResult *r           = data;
    PnTmuxMonitor    *self        = r->self;
    gboolean          want_notify_error = FALSE;
    gboolean          want_notify_busy  = FALSE;

    /* A newer enumeration has already started — drop this stale
     * result rather than letting it overwrite the fresher one's
     * cache or status. */
    if (g_atomic_int_get (&self->enum_gen) != r->gen)
        return G_SOURCE_REMOVE;

    g_mutex_lock (&self->mutex);

    if (r->sessions != NULL)
    {
        g_clear_pointer (&self->sessions_cache, g_strfreev);
        self->sessions_cache = r->sessions;
        r->sessions = NULL;          /* ownership transferred */
    }
    else
    {
        /* Failure: forget any older cache so the combo doesn't show
         * sessions that may not exist on the new host. */
        g_clear_pointer (&self->sessions_cache, g_strfreev);
    }

    {
        const gchar *new_err = r->error != NULL ? r->error : "";
        const gchar *old_err = self->last_error != NULL
                               ? self->last_error : "";
        if (g_strcmp0 (new_err, old_err) != 0)
        {
            g_free (self->last_error);
            self->last_error = (r->error != NULL)
                               ? g_strdup (r->error) : NULL;
            want_notify_error = TRUE;
        }
    }

    if (self->busy)
    {
        self->busy = FALSE;
        want_notify_busy = TRUE;
    }

    g_mutex_unlock (&self->mutex);

    if (want_notify_error)
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LAST_ERROR]);
    if (want_notify_busy)
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BUSY]);

    return G_SOURCE_REMOVE;
}

static gchar *
classify_enum_failure (const gchar *host,
                       gboolean     spawned,
                       const gchar *spawn_err,
                       gboolean     succeeded,
                       const gchar *stdout_text,
                       const gchar *stderr_text)
{
    const gchar *where = (host != NULL && *host != '\0') ? host : "localhost";
    gchar       *trimmed_err = NULL;
    const gchar *reason;

    if (!spawned)
    {
        return g_strdup_printf ("Cannot run tmux on %s: %s",
                                where,
                                spawn_err != NULL ? spawn_err
                                                  : "spawn failed");
    }

    /* tmux list-sessions exits 1 when no server is running and prints
     * "no server running on /tmp/tmux-NNN/default" on stderr.  Surface
     * that verbatim — it is exactly what the user needs to see. */
    if (!succeeded)
    {
        if (stderr_text != NULL && *stderr_text != '\0')
        {
            trimmed_err = g_strdup (stderr_text);
            g_strstrip (trimmed_err);
            /* Take only the first line so a multi-line error doesn't
             * blow the status row up vertically. */
            {
                gchar *nl = strchr (trimmed_err, '\n');
                if (nl != NULL)
                    *nl = '\0';
            }
            reason = (*trimmed_err != '\0')
                     ? trimmed_err
                     : "tmux exited with non-zero status";
        }
        else
        {
            reason = "tmux exited with non-zero status";
        }

        {
            gchar *msg = g_strdup_printf ("%s: %s", where, reason);
            g_free (trimmed_err);
            return msg;
        }
    }

    /* Exit 0 with no output is the "tmux server is up but has no
     * sessions" case — surface it as a deliberate red message so the
     * combobox's emptiness is explained rather than mysterious. */
    if (stdout_text == NULL || *stdout_text == '\0')
        return g_strdup_printf ("No tmux sessions on %s.", where);

    return NULL;
}

static gpointer
enum_worker (gpointer data)
{
    PnTmuxEnumResult  *r           = data;
    PnTmuxMonitor     *self        = r->self;
    gchar             *host;
    gchar             *stdout_text = NULL;
    gchar             *stderr_text = NULL;
    gint               exit_status = 0;
    GError            *error       = NULL;
    gboolean           spawned;
    gboolean           succeeded   = FALSE;
    const gchar       *base_argv[] = {
        "tmux", "list-sessions", "-F", "#S", NULL
    };
    gchar            **argv;

    host = tm_dup_host_locked (self);
    argv = tm_build_argv (host, base_argv);

    spawned = g_spawn_sync (NULL,
                            argv,
                            NULL,
                            G_SPAWN_SEARCH_PATH,
                            NULL, NULL,
                            &stdout_text,
                            &stderr_text,
                            &exit_status,
                            &error);

    if (spawned)
        succeeded = g_spawn_check_wait_status (exit_status, NULL);

    r->error = classify_enum_failure (host,
                                      spawned,
                                      (error != NULL) ? error->message : NULL,
                                      succeeded,
                                      stdout_text,
                                      stderr_text);

    if (r->error == NULL && stdout_text != NULL)
    {
        /* Split on '\n', drop empty entries, allocate a fresh strv. */
        GPtrArray *out = g_ptr_array_new ();
        gchar    **raw = g_strsplit (stdout_text, "\n", -1);
        guint      i;

        for (i = 0; raw[i] != NULL; i++)
        {
            gchar *t = g_strdup (raw[i]);
            g_strstrip (t);
            if (*t != '\0')
                g_ptr_array_add (out, t);
            else
                g_free (t);
        }
        g_ptr_array_add (out, NULL);
        g_strfreev (raw);
        r->sessions = (gchar **) g_ptr_array_free (out, FALSE);
    }

    g_clear_error (&error);
    g_free (stdout_text);
    g_free (stderr_text);
    g_strfreev (argv);
    g_free (host);

    /* Hand the result back to the main thread.  The destroy notify
     * frees the closure (including the strong ref to @self) once the
     * idle runs — or, if the main loop is being torn down, once it
     * is dropped. */
    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                enum_result_deliver,
                                r,
                                enum_result_free);
    return NULL;
}

static void
tm_kick_enumerator (PnTmuxMonitor *self)
{
    PnTmuxEnumResult *r;
    GThread          *thread;
    gboolean          want_notify_busy = FALSE;

    g_mutex_lock (&self->mutex);
    if (!self->busy)
    {
        self->busy = TRUE;
        want_notify_busy = TRUE;
    }
    /* Clear any stale error eagerly so the red row vanishes the
     * moment a fresh attempt is in flight.  The deliver-idle re-
     * populates it if the new attempt also fails. */
    if (self->last_error != NULL)
    {
        g_free (self->last_error);
        self->last_error = NULL;
    }
    g_mutex_unlock (&self->mutex);

    if (want_notify_busy)
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_BUSY]);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LAST_ERROR]);

    r          = g_new0 (PnTmuxEnumResult, 1);
    r->self    = g_object_ref (self);
    r->gen     = g_atomic_int_add (&self->enum_gen, 1) + 1;

    /* Detached one-shot: g_thread_unref releases our reference to
     * the GThread struct, the thread keeps running and self-cleans
     * when its function returns.  No need for a list of in-flight
     * threads — the generation counter discards stale results. */
    thread = g_thread_new ("pn-tmux-enum", enum_worker, r);
    g_thread_unref (thread);
}

/* ------------------------------------------------------------------ */
/*  Property setters                                                   */
/* ------------------------------------------------------------------ */

static void
tm_set_host (PnTmuxMonitor *self, const gchar *host)
{
    gchar    *old;
    gchar    *replacement;
    gboolean  host_changed;

    /* Keep an empty entry empty — an empty host means "local machine"
     * (see pn_shell_host_is_local) and the entry shows the real local
     * name as a grey hint, so coercing "" to "localhost" here would
     * only fight the hint and discard the user's clear. */
    replacement = g_strdup ((host != NULL) ? host : PN_SHELL_HOST_DEFAULT);

    g_mutex_lock (&self->mutex);
    host_changed = (g_strcmp0 (self->host, replacement) != 0);
    old = self->host;
    self->host = replacement;
    /* Drop the previous host's session list under the same lock as
     * the host swap.  notify::host fires synchronously below; if the
     * cache still held the old host's sessions when the combobox
     * repopulate handler reached for it, the dropdown would briefly
     * show stale entries from the previous host (and on the load
     * order the user reported, the new enumeration's later
     * notify::busy didn't suffice to flush them visually).  Clearing
     * eagerly means the transient state between "host changed" and
     * "new sessions arrived" is the empty cache, not the previous
     * one. */
    if (host_changed)
        g_clear_pointer (&self->sessions_cache, g_strfreev);
    g_mutex_unlock (&self->mutex);

    g_free (old);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOST]);

    /* Skip the enumerator restart when the property write was a no-op
     * (the JSON deserialiser sometimes echoes the same value back, and
     * a synthetic dialog refresh would spawn a spurious thread). */
    if (host_changed)
    {
        /* Different host means the next capture's diff baseline must
         * be empty — we'd otherwise subtract the prior host's pane
         * from the new host's pane and emit gibberish for the first
         * tick on the new host. */
        tm_reset_prev_output (self);
        tm_kick_enumerator   (self);
    }
}

static void
tm_set_session (PnTmuxMonitor *self, const gchar *session)
{
    gchar    *old;
    gchar    *replacement;
    gboolean  configured;
    gboolean  session_changed;

    replacement = g_strdup (session != NULL ? session : "");
    configured  = (session != NULL && *session != '\0');

    g_mutex_lock (&self->mutex);
    session_changed = (g_strcmp0 (self->tmux_session, replacement) != 0);
    old = self->tmux_session;
    self->tmux_session = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    apply_visual_state (self, configured);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TMUX_SESSION]);

    /* Same reasoning as the host setter: a session swap means the
     * delta baseline must start fresh, otherwise the first tick on
     * the new session emits "what the new session shows minus what
     * the old session showed", which is meaningless. */
    if (session_changed)
        tm_reset_prev_output (self);
}

static void
tm_set_line_limit (PnTmuxMonitor *self, guint n)
{
    guint clamped = n;

    if (clamped < 1)
        clamped = 1;
    if (clamped > PN_TMUX_MONITOR_LINE_LIMIT_MAX)
        clamped = PN_TMUX_MONITOR_LINE_LIMIT_MAX;

    g_mutex_lock (&self->mutex);
    self->line_limit = clamped;
    g_mutex_unlock (&self->mutex);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_LINE_LIMIT]);
}

/* ------------------------------------------------------------------ */
/*  Delta between successive captures                                  */
/*                                                                     */
/*  tmux capture-pane re-emits the whole visible pane (plus the        */
/*  scrollback window the -S flag asked for) every tick, so a naive    */
/*  emitter would hand the downstream chain the same lines over and    */
/*  over and pretend a static screen is fresh traffic.  Split both     */
/*  the previous tick's text and the new one into lines and find the   */
/*  largest k where the previous tick's last k lines exactly match     */
/*  the new tick's first k lines — that's the overlap the user has     */
/*  already seen.  Everything after that k is the genuinely-new tail   */
/*  and is what the message carries.  If there is no overlap (the      */
/*  pane was cleared, the session was switched in tmux, etc.) the      */
/*  whole new capture is emitted verbatim.                             */
/*                                                                     */
/*  Complexity is O(min(M,N)·L) where M,N are the line counts and L    */
/*  the average line length.  M,N are bounded by :line-limit (50 by    */
/*  default), so the worst case is well under a millisecond.           */
/* ------------------------------------------------------------------ */

static gchar *
tm_compute_delta (const gchar *prev, const gchar *curr)
{
    gchar    **prev_lines;
    gchar    **curr_lines;
    guint      m, n, kmax, k, best;
    GString   *out;

    if (curr == NULL || *curr == '\0')
        return g_strdup ("");

    if (prev == NULL || *prev == '\0')
        return g_strdup (curr);

    prev_lines = g_strsplit (prev, "\n", -1);
    curr_lines = g_strsplit (curr, "\n", -1);
    m = g_strv_length (prev_lines);
    n = g_strv_length (curr_lines);

    kmax = (m < n) ? m : n;
    best = 0;
    for (k = kmax; k > 0; k--)
    {
        gboolean match = TRUE;
        guint    i;
        for (i = 0; i < k; i++)
        {
            if (g_strcmp0 (prev_lines[m - k + i], curr_lines[i]) != 0)
            {
                match = FALSE;
                break;
            }
        }
        if (match)
        {
            best = k;
            break;
        }
    }

    out = g_string_new (NULL);
    for (k = best; k < n; k++)
    {
        if (k > best)
            g_string_append_c (out, '\n');
        g_string_append (out, curr_lines[k]);
    }

    g_strfreev (prev_lines);
    g_strfreev (curr_lines);
    return g_string_free (out, FALSE);
}

/* Swap the worker-side previous-capture cache atomically, returning
 * the *old* value so the caller (on the worker thread) can diff
 * against it.  The setter takes ownership of @new_value. */
static gchar *
tm_swap_prev_output (PnTmuxMonitor *self, gchar *new_value)
{
    gchar *prev;
    g_mutex_lock (&self->mutex);
    prev = self->prev_output;
    self->prev_output = new_value;
    g_mutex_unlock (&self->mutex);
    return prev;
}

static void
tm_reset_prev_output (PnTmuxMonitor *self)
{
    gchar *old;
    g_mutex_lock (&self->mutex);
    old = self->prev_output;
    self->prev_output = NULL;
    g_mutex_unlock (&self->mutex);
    g_free (old);
}

/* ------------------------------------------------------------------ */
/*  Trigger (periodic capture)                                         */
/* ------------------------------------------------------------------ */

static void
pn_tmux_monitor_trigger (PnAutoTrigger *trigger)
{
    PnTmuxMonitor   *self        = PN_TMUX_MONITOR (trigger);
    PnNode          *node        = PN_NODE (self);
    gchar           *host        = tm_dup_host_locked (self);
    gchar           *session     = tm_dup_session_locked (self);
    guint            line_limit  = tm_get_line_limit_locked (self);
    gchar           *stdout_text = NULL;
    gchar           *stderr_text = NULL;
    gchar           *combined;
    gint             exit_status = 0;
    GError          *error       = NULL;
    PnMessage       *msg;
    gboolean         spawned;
    gboolean         success     = FALSE;
    gchar           *start_arg;
    const gchar     *base_argv[8];
    gchar          **argv;

    /* Without a session selected the trigger has nothing meaningful
     * to emit — the visual state already marks the node as needing
     * configuration. */
    if (session == NULL || *session == '\0')
    {
        g_free (host);
        g_free (session);
        return;
    }

    /* tmux capture-pane -p          → stdout instead of a buffer
     *                  -S -<N>      → start N lines back into history
     *                  -E -         → end at the bottom of the visible
     *                                 area
     *                  -t <session> → target the session by name */
    start_arg = g_strdup_printf ("-%u", line_limit);

    base_argv[0] = "tmux";
    base_argv[1] = "capture-pane";
    base_argv[2] = "-p";
    base_argv[3] = "-S";
    base_argv[4] = start_arg;
    base_argv[5] = "-t";
    base_argv[6] = session;
    base_argv[7] = NULL;

    argv = tm_build_argv (host, base_argv);

    spawned = g_spawn_sync (NULL,
                            argv,
                            NULL,
                            G_SPAWN_SEARCH_PATH,
                            NULL, NULL,
                            &stdout_text,
                            &stderr_text,
                            &exit_status,
                            &error);

    if (!spawned)
    {
        combined = g_strdup (error ? error->message : "spawn failed");
        g_clear_error (&error);
    }
    else
    {
        success  = g_spawn_check_wait_status (exit_status, NULL);
        combined = g_strconcat (stdout_text ? stdout_text : "",
                                stderr_text ? stderr_text : "",
                                NULL);
    }

    /* Emit only the new tail since the previous tick.  On the
     * success path the previous capture becomes the new baseline;
     * on a failure (spawn refused, ssh down) leave the baseline
     * alone so the next successful capture diffs against the last
     * good one rather than against the error message we synthesised
     * into `combined`. */
    {
        gchar *delta;

        if (success)
        {
            gchar *prev = tm_swap_prev_output (self, g_strdup (combined));
            delta = tm_compute_delta (prev, combined);
            g_free (prev);
        }
        else
        {
            delta = g_strdup (combined);
        }

        /* An empty host means "local machine"; report its real name in
         * the message rather than an empty string, matching the local
         * monitoring nodes (PnCpu, PnMemory, …). */
        msg = pn_message_new (node, NULL);
        pn_message_set_boolean (msg, "success", success);
        pn_message_set_string  (msg, "output",  delta);
        pn_message_set_string  (msg, "host",
                                pn_shell_host_is_local (host)
                                ? g_get_host_name () : host);
        pn_message_set_string  (msg, "session", session);

        pn_auto_trigger_emit_on_main (trigger, msg);

        g_free (delta);
    }

    g_free (combined);
    g_free (stdout_text);
    g_free (stderr_text);
    g_free (start_arg);
    g_free (host);
    g_free (session);
    g_strfreev (argv);
}

/* ------------------------------------------------------------------ */
/*  Settings dialog                                                    */
/* ------------------------------------------------------------------ */

static gchar **
tm_dup_sessions_cache (PnTmuxMonitor *self)
{
    gchar **copy;
    g_mutex_lock (&self->mutex);
    copy = (self->sessions_cache != NULL)
           ? g_strdupv (self->sessions_cache)
           : NULL;
    g_mutex_unlock (&self->mutex);
    return copy;
}

static void
session_combo_repopulate (GObject         *target,
                          GtkComboBoxText *combo)
{
    PnTmuxMonitor  *node     = PN_TMUX_MONITOR (target);
    gchar          *current  = NULL;
    gchar         **cache;
    gboolean        listed   = FALSE;

    g_object_get (target, "tmux-session", &current, NULL);

    gtk_combo_box_text_remove_all (combo);

    /* Sentinel empty entry mirrors the Meshtastic device combo — gives
     * the active-id binding something to land on for a brand-new node
     * with no session picked yet, and lets the user explicitly
     * unselect by picking it back. */
    gtk_combo_box_text_append (combo, "", "(no session)");

    cache = tm_dup_sessions_cache (node);
    if (cache != NULL)
    {
        for (gsize i = 0; cache[i] != NULL; i++)
        {
            gtk_combo_box_text_append (combo, cache[i], cache[i]);
            if (g_strcmp0 (cache[i], current) == 0)
                listed = TRUE;
        }
    }

    /* Preserve a previously-configured session that the host no longer
     * lists (e.g. the worksheet was authored against a host whose
     * session was since killed): keep showing it so the user can see
     * what the saved value was and pick a replacement deliberately. */
    if (!listed && current != NULL && *current != '\0')
        gtk_combo_box_text_append (combo, current, current);

    if (current != NULL)
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), current);

    g_strfreev (cache);
    g_free     (current);
}

static void
on_tmux_host_or_busy_notify (GObject    *target,
                             GParamSpec *pspec G_GNUC_UNUSED,
                             gpointer    user_data)
{
    session_combo_repopulate (target, GTK_COMBO_BOX_TEXT (user_data));
}

/* ------------------------------------------------------------------ */
/*  Deferred-commit host entry                                         */
/*                                                                     */
/*  The default introspection-driven string editor binds entry.text    */
/*  <-> target.host bidirectionally, so every keystroke writes the     */
/*  property — which kicks the session enumerator, flips `busy` to     */
/*  TRUE, and triggers #PnNodeDialog::sync_busy_state to call          */
/*  gtk_widget_set_sensitive(notebook, FALSE) on the form containing   */
/*  the entry.  Desensitising the entry's ancestor pulls focus off     */
/*  the entry the user is typing into — the symptom the user          */
/*  reported as "the entry looses the focus for every entered          */
/*  character".  Plus we'd be firing one ssh hop per keystroke.        */
/*                                                                     */
/*  This editor commits the entry's text to the host property only     */
/*  on `activate` (Enter) and `focus-out-event` (the user moved on),   */
/*  and refreshes the entry text on `notify::host` so an external      */
/*  write (e.g. the worksheet loader) still flows in.  The notify      */
/*  handler skips the write when the entry already shows the new      */
/*  value, so a self-triggered notify after our own commit doesn't    */
/*  fight the user's cursor position.                                  */
/* ------------------------------------------------------------------ */

static void
host_entry_commit (GtkEntry *entry, GObject *target)
{
    const gchar *text    = gtk_entry_get_text (entry);
    gchar       *current = NULL;

    g_object_get (target, "host", &current, NULL);
    if (g_strcmp0 (current, text) != 0)
        g_object_set (target, "host", text, NULL);
    g_free (current);
}

static gboolean
on_host_entry_focus_out (GtkWidget *widget,
                         GdkEvent  *event   G_GNUC_UNUSED,
                         gpointer   user_data)
{
    host_entry_commit (GTK_ENTRY (widget), G_OBJECT (user_data));
    return GDK_EVENT_PROPAGATE;
}

static void
on_host_entry_activate (GtkEntry *entry, gpointer user_data)
{
    host_entry_commit (entry, G_OBJECT (user_data));
}

static void
on_host_property_notify (GObject    *target,
                         GParamSpec *pspec G_GNUC_UNUSED,
                         gpointer    user_data)
{
    GtkEntry *entry   = GTK_ENTRY (user_data);
    gchar    *current = NULL;

    g_object_get (target, "host", &current, NULL);
    if (g_strcmp0 (gtk_entry_get_text (entry),
                   current != NULL ? current : "") != 0)
        gtk_entry_set_text (entry, current != NULL ? current : "");
    g_free (current);
}

static GtkWidget *
pn_tmux_monitor_build_property_editor (PnNode     *self      G_GNUC_UNUSED,
                                       GParamSpec *pspec,
                                       GObject    *target,
                                       GtkWindow  *parent    G_GNUC_UNUSED)
{
    const gchar  *name     = pspec->name;
    gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);

    if (g_strcmp0 (name, "host") == 0)
    {
        GtkWidget *entry   = gtk_entry_new ();
        gchar     *current = NULL;

        g_object_get (target, name, &current, NULL);
        gtk_entry_set_text (GTK_ENTRY (entry),
                            current != NULL ? current : "");
        g_free (current);

        gtk_widget_set_hexpand   (entry, TRUE);
        gtk_widget_set_sensitive (entry, writable);

        /* Mirror the default string editor's grey local-hostname hint
         * (this entry is hand-rolled for deferred commit, so it does
         * not go through pn_node_dialog_default_editor()).  Empty host
         * == run locally, and the hint shows which machine that is. */
        pn_node_dialog_attach_hostname_hint (GTK_ENTRY (entry));

        if (writable)
        {
            g_signal_connect_object (entry, "activate",
                                     G_CALLBACK (on_host_entry_activate),
                                     target, 0);
            g_signal_connect_object (entry, "focus-out-event",
                                     G_CALLBACK (on_host_entry_focus_out),
                                     target, 0);
            g_signal_connect_object (target, "notify::host",
                                     G_CALLBACK (on_host_property_notify),
                                     entry, 0);
        }
        return entry;
    }

    if (g_strcmp0 (name, "tmux-session") == 0)
    {
        GtkWidget *combo = gtk_combo_box_text_new ();

        session_combo_repopulate (target, GTK_COMBO_BOX_TEXT (combo));

        /* Repopulate when the enumerator finishes (notify::busy
         * transitions to FALSE just after the cache update lands)
         * and when the user picks a new host (notify::host) so the
         * combo follows the user's host edit without a dialog
         * reopen.  g_signal_connect_object ties both handlers to
         * the combo's lifetime so the dialog can close cleanly. */
        g_signal_connect_object (target, "notify::busy",
                                 G_CALLBACK (on_tmux_host_or_busy_notify),
                                 combo, 0);
        g_signal_connect_object (target, "notify::host",
                                 G_CALLBACK (on_tmux_host_or_busy_notify),
                                 combo, 0);

        gtk_widget_set_hexpand   (combo, TRUE);
        gtk_widget_set_sensitive (combo, writable);
        g_object_bind_property (target, name, combo, "active-id", flags);
        return combo;
    }

    return NULL;
}

static void
update_status_label (GObject    *obj,
                     GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer    user_data)
{
    PnTmuxMonitor *self  = PN_TMUX_MONITOR (obj);
    GtkLabel      *label = GTK_LABEL (user_data);
    gchar         *snapshot;
    gchar         *markup;

    g_mutex_lock (&self->mutex);
    snapshot = (self->last_error != NULL && *self->last_error != '\0')
               ? g_strdup (self->last_error) : NULL;
    g_mutex_unlock (&self->mutex);

    if (snapshot != NULL)
    {
        gchar *escaped = g_markup_escape_text (snapshot, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">%s</span>", escaped);
        g_free (escaped);
    }
    else
    {
        markup = g_strdup ("");
    }

    gtk_label_set_markup (label, markup);
    g_free (markup);
    g_free (snapshot);
}

static GtkWidget *
pn_tmux_monitor_build_class_tab (PnNode    *self,
                                 GtkWindow *parent)
{
    GObject   *target = G_OBJECT (self);
    GtkWidget *grid   = pn_node_dialog_new_property_grid ();
    GtkWidget *host_editor;
    GtkWidget *session_editor;
    GtkWidget *line_limit_editor;
    GtkWidget *status_label;

    host_editor = pn_tmux_monitor_build_property_editor (
            self, props[PROP_HOST], target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 0,
                               g_param_spec_get_nick (props[PROP_HOST]),
                               host_editor);

    session_editor = pn_tmux_monitor_build_property_editor (
            self, props[PROP_TMUX_SESSION], target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 1,
                               g_param_spec_get_nick (props[PROP_TMUX_SESSION]),
                               session_editor);

    line_limit_editor = pn_node_dialog_default_editor (
            target, props[PROP_LINE_LIMIT]);
    pn_node_dialog_attach_row (GTK_GRID (grid), 2,
                               g_param_spec_get_nick (props[PROP_LINE_LIMIT]),
                               line_limit_editor);

    status_label = gtk_label_new (NULL);
    gtk_label_set_xalign     (GTK_LABEL (status_label), 0.0);
    gtk_label_set_use_markup (GTK_LABEL (status_label), TRUE);
    pn_node_dialog_attach_row (GTK_GRID (grid), 3, "Status", status_label);

    update_status_label (target, NULL, status_label);
    g_signal_connect_object (target, "notify::last-error",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    /* busy transitions don't change last-error directly, but the
     * `busy → idle` flip is the exact moment a freshly-finished
     * enumeration's status has just landed in the field — wire it
     * too so a slow ssh that ends up clearing the error redraws the
     * row immediately rather than waiting for the next notify. */
    g_signal_connect_object (target, "notify::busy",
                             G_CALLBACK (update_status_label),
                             status_label, 0);

    return grid;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tmux_monitor_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTmuxMonitor *self = PN_TMUX_MONITOR (object);

    switch (prop_id)
    {
    case PROP_HOST:
        g_value_take_string (value, tm_dup_host_locked (self));
        break;
    case PROP_TMUX_SESSION:
        g_value_take_string (value, tm_dup_session_locked (self));
        break;
    case PROP_LINE_LIMIT:
        g_value_set_uint (value, tm_get_line_limit_locked (self));
        break;
    case PROP_BUSY:
        {
            gboolean b;
            g_mutex_lock (&self->mutex);
            b = self->busy;
            g_mutex_unlock (&self->mutex);
            g_value_set_boolean (value, b);
            break;
        }
    case PROP_LAST_ERROR:
        {
            gchar *copy;
            g_mutex_lock (&self->mutex);
            copy = g_strdup (self->last_error);
            g_mutex_unlock (&self->mutex);
            g_value_take_string (value, copy);
            break;
        }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_tmux_monitor_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTmuxMonitor *self = PN_TMUX_MONITOR (object);

    switch (prop_id)
    {
    case PROP_HOST:
        tm_set_host (self, g_value_get_string (value));
        break;
    case PROP_TMUX_SESSION:
        tm_set_session (self, g_value_get_string (value));
        break;
    case PROP_LINE_LIMIT:
        tm_set_line_limit (self, g_value_get_uint (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_tmux_monitor_finalize (GObject *object)
{
    PnTmuxMonitor *self = PN_TMUX_MONITOR (object);

    /* Detached enumerator threads still in flight hold a ref on us
     * (taken in tm_kick_enumerator) so finalize cannot run while one
     * is alive — by the time we reach this point the only owner is
     * gone, which means every previously-spawned enumerator has
     * already dropped its ref via enum_result_free. */

    g_clear_pointer (&self->host,           g_free);
    g_clear_pointer (&self->tmux_session,   g_free);
    g_clear_pointer (&self->last_error,     g_free);
    g_clear_pointer (&self->sessions_cache, g_strfreev);
    g_clear_pointer (&self->prev_output,    g_free);
    g_mutex_clear   (&self->mutex);

    G_OBJECT_CLASS (pn_tmux_monitor_parent_class)->finalize (object);
}

static void
pn_tmux_monitor_class_init (PnTmuxMonitorClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_tmux_monitor_get_property;
    object_class->set_property = pn_tmux_monitor_set_property;
    object_class->finalize     = pn_tmux_monitor_finalize;
    trigger_class->trigger     = pn_tmux_monitor_trigger;

    node_class->build_property_editor = pn_tmux_monitor_build_property_editor;
    node_class->build_class_tab       = pn_tmux_monitor_build_class_tab;

    node_class->palette_icon   = PN_TMUX_MONITOR_NORMAL_ICON;
    node_class->class_name     = "Tmux Monitor";
    node_class->icon           = PN_TMUX_MONITOR_NORMAL_ICON;
    node_class->color          = (GdkRGBA){ 0.42, 0.62, 0.86, 1.0 };
    node_class->category       = "Shell";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOST] = g_param_spec_string (
            "host", "Host",
            "Hostname (or user@host) the tmux server runs on.  An empty "
            "string (the default) or \"localhost\" runs locally; any "
            "other value routes the tmux invocation through passwordless "
            "ssh (BatchMode=yes — a pre-installed key is required).",
            PN_SHELL_HOST_DEFAULT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TMUX_SESSION] = g_param_spec_string (
            "tmux-session", "Session",
            "Name of the tmux session to capture from.  The settings "
            "dialog populates a combobox from the live "
            "`tmux list-sessions` result on the configured host.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LINE_LIMIT] = g_param_spec_uint (
            "line-limit", "Line Limit",
            "Maximum number of trailing lines to capture from the "
            "session's scrollback per tick (tmux capture-pane -S "
            "-<N>).",
            1, PN_TMUX_MONITOR_LINE_LIMIT_MAX,
            PN_TMUX_MONITOR_DEFAULT_LINE_LIMIT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Read-only — transient runtime state, not part of the saved
     * worksheet (pn-flow's introspection-driven serialiser skips
     * non-writable properties).  The host dialog reacts to
     * notify::busy by overlaying its spinner — see
     * lib/pn-node-dialog.c::maybe_install_busy_watcher. */
    props[PROP_BUSY] = g_param_spec_boolean (
            "busy", "Busy",
            "TRUE while the session enumerator thread is in flight",
            FALSE,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    /* Read-only for the same reason as PROP_BUSY.  The per-class
     * status row in the settings dialog renders this in red when
     * non-empty. */
    props[PROP_LAST_ERROR] = g_param_spec_string (
            "last-error", "Last Error",
            "Failure reason from the most recent session enumeration, "
            "or empty when the last enumeration succeeded",
            NULL,
            G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_tmux_monitor_init (PnTmuxMonitor *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->host           = g_strdup (PN_SHELL_HOST_DEFAULT);
    self->tmux_session   = g_strdup ("");
    self->line_limit     = PN_TMUX_MONITOR_DEFAULT_LINE_LIMIT;
    self->sessions_cache = NULL;
    self->busy           = FALSE;
    self->last_error     = NULL;
    self->enum_gen       = 0;
    self->prev_output    = NULL;

    pn_node_set_class_name (node, "Tmux Monitor");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_TMUX_MONITOR_DEFAULT_PERIOD);

    apply_visual_state (self, FALSE);

    /* Kick the enumerator once at construct time so the dialog opens
     * with a populated combo for the default (localhost) host without
     * the user having to nudge the property first. */
    tm_kick_enumerator (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTmuxMonitor *
pn_tmux_monitor_new (void)
{
    return g_object_new (PN_TYPE_TMUX_MONITOR, NULL);
}
