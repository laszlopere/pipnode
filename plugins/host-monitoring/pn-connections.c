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

#include "pn-connections.h"
#include "pn-message.h"
#include "pn-ssh-profile.h"
#include "pn-vault.h"

/* fa-link U+F0C1 — two chain links read as "open connections". */
#define PN_CONNECTIONS_ICON           "\xef\x83\x81"

#define PN_CONNECTIONS_DEFAULT_PERIOD 5u
/* See PN_LOAD_DEFAULT_HOST -- empty is the canonical "this machine"
 * marker; the dialog shows g_get_host_name() as a gray placeholder
 * hint and the trigger emits that same name on data.host. */
#define PN_CONNECTIONS_DEFAULT_HOST   ""

/* ------------------------------------------------------------------ */
/*  Counting script                                                    */
/*                                                                     */
/*  /proc/net/tcp{,6} only show sockets in the *reader's* network      */
/*  namespace, so a host running LXC / Docker containers — each with   */
/*  its own netns — would otherwise look almost empty.  The script    */
/*  below dedupes processes by their network namespace (the inode of  */
/*  /proc/<pid>/ns/net, exposed by stat -L), then counts the TCP      */
/*  sockets visible from a representative pid in each unique namespace */
/*  by reading /proc/<pid>/net/tcp{,6}.                               */
/*                                                                     */
/*  Only sockets in TCP state ESTABLISHED (the kernel's hex code 01,   */
/*  printed in column 4 of every /proc/net/tcp{,6} row) are counted —  */
/*  the figure is meant to represent connections that are *alive*, so  */
/*  TIME_WAIT (06), CLOSE_WAIT (08), LISTEN (0A) and the various       */
/*  half-closed / transient states are excluded.  The header line of   */
/*  each /proc file is skipped via awk's per-file FNR > 1 guard, which */
/*  resets when awk opens the next file in its argv list.              */
/*                                                                     */
/*  The work is done in a fixed number of forks regardless of process  */
/*  count: one `ls`, one `stat` (covering every pid via the shell      */
/*  glob), one `sort`, one `cut`, and one final `awk` covering all     */
/*  unique-namespace files at once.  On a host with thousands of       */
/*  processes this stays well under 100ms.                             */
/*                                                                     */
/*  The script is written using only double quotes so it can be safely */
/*  wrapped in single quotes for the remote shell when run over SSH.   */
/*  Inside double-quoted shell strings, `\$4` is needed so the local   */
/*  shell does not try to expand awk's field reference; the C string   */
/*  in turn escapes that as `\\$4`.                                    */
/* ------------------------------------------------------------------ */

#define PN_CONNECTIONS_SCRIPT \
    "P=$(ls -d /proc/[0-9]*/ns/net 2>/dev/null " \
        "| xargs -r stat -L -c \"%i %n\" 2>/dev/null " \
        "| sort -u -k1,1 " \
        "| cut -d/ -f3); " \
    "F=; " \
    "for p in $P; do " \
        "[ -r \"/proc/$p/net/tcp\" ]  && F=\"$F /proc/$p/net/tcp\"; " \
        "[ -r \"/proc/$p/net/tcp6\" ] && F=\"$F /proc/$p/net/tcp6\"; " \
    "done; " \
    "if [ -z \"$F\" ]; then echo 0; exit 0; fi; " \
    "awk \"FNR > 1 && \\$4 == \\\"01\\\" { c++ } " \
         "END { print c + 0 }\" $F 2>/dev/null"

struct _PnConnections
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads @hostname while the main thread may
     * overwrite it via the property setter, so accesses are
     * serialised through @mutex. */
    GMutex  mutex;
    gchar  *hostname;

    /* Optional SSH Login credential (item 47.4): @auth_profile is the
     * picked profile id (main thread only); @ssh_login is the value
     * snapshot the worker reads under @mutex, refreshed on the main
     * thread when the property or the vault changes. */
    gchar      *auth_profile;
    PnSshLogin  ssh_login;
};

G_DEFINE_TYPE (PnConnections, pn_connections, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOSTNAME,
    PROP_AUTH_PROFILE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Hostname accessor (thread-safe)                                    */
/* ------------------------------------------------------------------ */

static gchar *
conn_dup_hostname_locked (PnConnections *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->hostname);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
conn_set_hostname (
        PnConnections *self,
        const gchar   *hostname)
{
    gchar *old;
    gchar *replacement;

    /* NULL collapses to the default "localhost" so a freshly-loaded
     * node missing the property still behaves sensibly. */
    replacement = g_strdup ((hostname != NULL) ? hostname
                                               : PN_CONNECTIONS_DEFAULT_HOST);

    g_mutex_lock (&self->mutex);
    old = self->hostname;
    self->hostname = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOSTNAME]);
}

static gboolean
hostname_is_local (const gchar *hostname)
{
    if (hostname == NULL || *hostname == '\0')
        return TRUE;
    return g_ascii_strcasecmp (hostname, "localhost") == 0;
}

/* ------------------------------------------------------------------ */
/*  Output parsing                                                     */
/* ------------------------------------------------------------------ */

/** Pull the leading non-negative integer off @text, skipping leading
 *  whitespace.  Returns FALSE when the script gave us back something
 *  unparseable (empty buffer, error chatter, ...). */
gboolean
pn_connections_parse_count (const gchar *text, guint *out_value)
{
    const gchar *p = text;
    guint64      value = 0;
    gboolean     have_digit = FALSE;

    if (p == NULL)
        return FALSE;

    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;

    while (*p >= '0' && *p <= '9')
    {
        value = value * 10 + (guint64) (*p - '0');
        have_digit = TRUE;
        ++p;
    }

    if (!have_digit)
        return FALSE;

    *out_value = (guint) value;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Sample acquisition                                                 */
/* ------------------------------------------------------------------ */

/** Run the counting script in a local sh subprocess.  We use
 *  g_spawn_sync with an explicit argv so the script body never has
 *  to round-trip through GLib's shell-style command-line parser. */
static gboolean
sample_local (
        guint   *out_total,
        gchar  **out_stderr,
        GError **error)
{
    gchar    *argv[] = { "sh", "-c",
                         (gchar *) PN_CONNECTIONS_SCRIPT, NULL };
    gchar    *stdout_text = NULL;
    gint      exit_status = 0;
    gboolean  spawned;
    gboolean  ok = FALSE;

    spawned = g_spawn_sync (NULL, argv, NULL,
                            G_SPAWN_SEARCH_PATH,
                            NULL, NULL,
                            &stdout_text, out_stderr,
                            &exit_status, error);

    if (spawned)
    {
        if (pn_connections_parse_count (stdout_text, out_total))
            ok = TRUE;
        else
            g_spawn_check_wait_status (exit_status, error);
    }

    g_free (stdout_text);
    return ok;
}

/** Run the counting script over SSH.  We pre-quote the script in
 *  single quotes so the remote shell receives `sh -c '<script>'` as
 *  a single command line — the script is intentionally written
 *  without literal single quotes for exactly this reason. */
static gboolean
sample_ssh (
        const gchar      *hostname,
        const PnSshLogin *login,
        guint             timeout,
        guint            *out_total,
        gchar           **out_stderr,
        GError          **error)
{
    gchar       *remote_cmd;
    const gchar *base_argv[2];
    gchar      **argv;
    gchar       *stdout_text = NULL;
    gint         exit_status = 0;
    gboolean     spawned;
    gboolean     ok = FALSE;

    /* The script is pre-wrapped as a single `sh -c '<script>'` word so the
     * remote shell un-quotes it back intact; that one word is the entire
     * ssh tail.  The ssh flags themselves come from the one shared builder
     * (TODO #47.3) — no login profile yet (item 47.4), hence the NULL. */
    remote_cmd   = g_strdup_printf ("sh -c '%s'", PN_CONNECTIONS_SCRIPT);
    base_argv[0] = remote_cmd;
    base_argv[1] = NULL;

    argv = pn_ssh_build_argv (hostname, login, timeout, base_argv);

    spawned = g_spawn_sync (NULL, argv, NULL,
                            G_SPAWN_SEARCH_PATH,
                            NULL, NULL,
                            &stdout_text, out_stderr,
                            &exit_status, error);

    /* Parse first; only fall back to the exit status when we got no
     * usable count out.  Mirrors the load node — the remote script
     * may print a valid number even after a stat / readlink error
     * silenced by 2>/dev/null. */
    if (spawned)
    {
        if (pn_connections_parse_count (stdout_text, out_total))
            ok = TRUE;
        else
            g_spawn_check_wait_status (exit_status, error);
    }

    g_free (stdout_text);
    g_strfreev (argv);
    g_free (remote_cmd);
    return ok;
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/* ------------------------------------------------------------------ */

static void
pn_connections_trigger (PnAutoTrigger *trigger)
{
    PnConnections *self     = PN_CONNECTIONS (trigger);
    PnNode        *node     = PN_NODE (self);
    gchar         *hostname = conn_dup_hostname_locked (self);
    gchar         *errbuf   = NULL;
    GError        *error    = NULL;
    PnMessage     *msg;
    gboolean       success  = FALSE;
    guint          total    = 0;
    PnSshLogin     login    = { 0 };
    gboolean       local    = hostname_is_local (hostname);
    /* Empty / "localhost" both resolve to the actual machine name on
     * the wire and in the summary -- same convention as #PnLoad. */
    const gchar   *display  = local ? g_get_host_name () : hostname;
    guint          period;
    guint          timeout;

    period  = pn_auto_trigger_get_period (trigger);
    timeout = (period > 1u) ? period - 1u : 1u;

    if (local)
    {
        success = sample_local (&total, &errbuf, &error);
    }
    else
    {
        pn_ssh_login_dup_locked (&self->mutex, &self->ssh_login, &login);
        success = sample_ssh (hostname, &login, timeout, &total, &errbuf, &error);
    }

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "host",
                            display);
    pn_message_set_boolean (msg, "success", success);

    if (success)
    {
        gchar *summary;

        pn_message_set_int (msg, "value", (gint) total);

        summary = g_strdup_printf (
                "Host %s has %u connections.",
                display, total);
        pn_message_set_string (msg, "output", summary);
        g_free (summary);
    }
    else
    {
        const gchar *reason = NULL;
        gchar       *summary;

        /* Prefer the remote stderr over the GError text: the GError
         * for a non-zero exit is the generic "Child process exited
         * with code N", whereas stderr typically carries the actual
         * cause ("Permission denied (publickey).",
         * "ssh: connect to host X port 22: Connection refused"). */
        if (errbuf != NULL && *errbuf != '\0')
            reason = g_strchomp (errbuf);
        else if (error != NULL)
            reason = error->message;

        if (reason != NULL)
            summary = g_strdup_printf (
                    "Failed to read connections from %s: %s",
                    display, reason);
        else
            summary = g_strdup_printf (
                    "Failed to read connections from %s.",
                    display);

        pn_message_set_string (msg, "output", summary);
        g_free (summary);
    }

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_clear_error (&error);
    g_free (errbuf);
    g_free (hostname);
    pn_ssh_login_clear (&login);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_connections_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnConnections *self = PN_CONNECTIONS (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        g_value_take_string (value, conn_dup_hostname_locked (self));
        break;
    case PROP_AUTH_PROFILE:
        g_value_set_string (value, self->auth_profile);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_connections_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnConnections *self = PN_CONNECTIONS (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        conn_set_hostname (self, g_value_get_string (value));
        break;
    case PROP_AUTH_PROFILE:
        g_free (self->auth_profile);
        self->auth_profile = g_value_dup_string (value);
        pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                              &self->mutex, &self->ssh_login);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* A profile changed in the vault: re-resolve our login snapshot. */
static void
on_vault_changed (PnConnections *self)
{
    pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                          &self->mutex, &self->ssh_login);
}

static void
pn_connections_finalize (GObject *object)
{
    PnConnections *self = PN_CONNECTIONS (object);

    g_clear_pointer (&self->hostname,     g_free);
    g_clear_pointer (&self->auth_profile, g_free);
    pn_ssh_login_clear (&self->ssh_login);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_connections_parent_class)->finalize (object);
}

static void
pn_connections_class_init (PnConnectionsClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_connections_get_property;
    object_class->set_property = pn_connections_set_property;
    object_class->finalize     = pn_connections_finalize;
    trigger_class->trigger     = pn_connections_trigger;

    node_class->palette_icon   = PN_CONNECTIONS_ICON;
    node_class->class_name     = "Net Connections";
    node_class->icon           = PN_CONNECTIONS_ICON;
    node_class->color          = (PnColor){ 0.62, 0.50, 0.78, 1.0 };
    node_class->category       = "Host monitoring";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOSTNAME] = g_param_spec_string (
            "hostname", "Hostname",
            "Host whose TCP socket count should be sampled.  An "
            "empty string or \"localhost\" runs the counter "
            "locally; any other value is treated as an SSH target "
            "and the count is fetched via passwordless ssh.  The "
            "count covers every network namespace the running user "
            "can see, so containers (LXC / Docker / ...) are "
            "included when the probe runs as root.",
            PN_CONNECTIONS_DEFAULT_HOST,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_hostname_hint (props[PROP_HOSTNAME]);

    /* Optional SSH Login credential picker (item 47.4). */
    props[PROP_AUTH_PROFILE] = pn_ssh_auth_profile_param_spec ();

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_connections_init (PnConnections *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.62, 0.50, 0.78, 1.0 };

    g_mutex_init (&self->mutex);
    self->hostname     = g_strdup (PN_CONNECTIONS_DEFAULT_HOST);
    self->auth_profile = g_strdup ("");
    self->ssh_login    = (PnSshLogin){ 0 };

    g_signal_connect_object (pn_vault_get_default (), "changed",
                             G_CALLBACK (on_vault_changed), self,
                             G_CONNECT_SWAPPED);
    pn_ssh_login_refresh (node, "auth-profile", &self->mutex, &self->ssh_login);

    pn_node_set_class_name (node, "Net Connections");
    pn_node_set_icon       (node, PN_CONNECTIONS_ICON);
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_CONNECTIONS_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnConnections *
pn_connections_new (void)
{
    return g_object_new (PN_TYPE_CONNECTIONS, NULL);
}
