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

#include "pn-shell-command.h"
#include "pn-message.h"
#include "pn-shell-host.h"
#include "pn-shell-output.h"
#include "pn-ssh-profile.h"
#include "pn-vault.h"

/* Visual states.  The icon panel renders in white, so the body colour
 * carries the alert for the warning state. */
#define PN_SHELL_COMMAND_NORMAL_ICON  "\xef\x84\xa0"   /* fa-terminal U+F120 */

#define PN_SHELL_COMMAND_DEFAULT_PERIOD 5u

struct _PnShellCommand
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads @shell_command and @host while the main
     * thread may overwrite either via a property setter, so accesses
     * are serialised through @mutex.  Both strings are owned by the
     * node and freed in finalize. */
    GMutex  mutex;
    gchar  *shell_command;
    gchar  *host;

    /* How the command's output becomes the message: Text (output+success)
     * or JSON (parse stdout into the data bag).  Read on the worker
     * thread under @mutex, written on the main thread. */
    PnShellOutputFormat output_format;

    /* The optional SSH Login credential (item 47.4).  @auth_profile is the
     * picked profile id and is touched only on the main thread; @ssh_login
     * is the value snapshot resolved from it, read by the worker under
     * @mutex and refreshed on the main thread when the property or the
     * vault changes. */
    gchar      *auth_profile;
    PnSshLogin  ssh_login;
};

G_DEFINE_TYPE (PnShellCommand, pn_shell_command, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_SHELL_COMMAND,
    PROP_AUTH_PROFILE,
    PROP_HOST,
    PROP_OUTPUT_FORMAT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (
        PnShellCommand *self,
        gboolean        configured)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.42, 0.62, 0.86, 1.0 };

    /* Keep the healthy blue identity at all times; the red body + ❗
     * overlay for the unconfigured state is painted centrally by the
     * worksheet whenever has-error is set. */
    pn_node_set_color     (node, &blue);
    pn_node_set_icon      (node, PN_SHELL_COMMAND_NORMAL_ICON);
    pn_node_set_has_error (node, !configured);
}

/* ------------------------------------------------------------------ */
/*  Command accessor (thread-safe)                                     */
/* ------------------------------------------------------------------ */

static gchar *
shell_dup_command_locked (PnShellCommand *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->shell_command);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
shell_set_command (
        PnShellCommand *self,
        const gchar    *command)
{
    gchar    *old;
    gchar    *replacement;
    gboolean  configured;

    replacement = (command != NULL) ? g_strdup (command) : NULL;
    configured  = (command != NULL && *command != '\0');

    g_mutex_lock (&self->mutex);
    old = self->shell_command;
    self->shell_command = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    apply_visual_state (self, configured);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SHELL_COMMAND]);
}

static gchar *
shell_dup_host_locked (PnShellCommand *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->host);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
shell_set_host (
        PnShellCommand *self,
        const gchar    *host)
{
    gchar *old;
    gchar *replacement;

    /* Keep an empty entry empty — an empty host means "local machine"
     * (see pn_shell_host_is_local) and the settings dialog shows the
     * real local name as a grey hint, so coercing "" to "localhost"
     * here would only fight the hint and discard the user's clear. */
    replacement = g_strdup ((host != NULL) ? host : PN_SHELL_HOST_DEFAULT);

    g_mutex_lock (&self->mutex);
    old = self->host;
    self->host = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOST]);
}

static PnShellOutputFormat
shell_get_output_format_locked (PnShellCommand *self)
{
    PnShellOutputFormat fmt;

    g_mutex_lock (&self->mutex);
    fmt = self->output_format;
    g_mutex_unlock (&self->mutex);

    return fmt;
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger.  Spawn   */
/*  /bin/sh -c <command> so the user's one-liner can use pipes,       */
/*  redirections, variable expansion, etc., without us having to      */
/*  tokenise it ourselves.                                             */
/* ------------------------------------------------------------------ */

static void
pn_shell_command_trigger (PnAutoTrigger *trigger)
{
    PnShellCommand     *self        = PN_SHELL_COMMAND (trigger);
    PnNode             *node        = PN_NODE (self);
    gchar              *command_raw = shell_dup_command_locked (self);
    gchar              *command;
    gchar              *host        = shell_dup_host_locked (self);
    PnShellOutputFormat fmt         = shell_get_output_format_locked (self);
    gchar              *stdout_text = NULL;
    gchar              *stderr_text = NULL;
    gchar              *spawn_err   = NULL;
    gint                exit_status = 0;
    GError             *error       = NULL;
    PnMessage          *msg;
    gboolean            spawned;
    gboolean            success     = FALSE;
    GSpawnFlags         spawn_flags;
    const gchar        *base_argv[4];
    gchar             **argv;
    PnSshLogin          login       = { 0 };

    /* Skip work entirely while the node is in its "configuration
     * required" state.  The visual marker on the canvas already tells
     * the user; emitting nothing keeps downstream nodes idle. */
    if (command_raw == NULL || *command_raw == '\0')
    {
        g_free (command_raw);
        g_free (host);
        return;
    }

    /* Interpolate ${nodeclass} / ${nodename} / ${hostname}; any other
     * ${...} is left for the login shell to expand (e.g. ${HOME}). */
    command = pn_node_expand_vars (node, command_raw);
    g_free (command_raw);

    /* Local: spawn /bin/sh -c <command> directly so pipes, redirections
     * and variable expansion work without us tokenising the one-liner
     * ourselves.  Remote: hand the command string to ssh as a single
     * argv element so the remote login shell (not us) does the
     * interpretation, and so embedded spaces survive sshd's
     * argv-join-with-spaces behaviour intact. */
    if (pn_shell_host_is_local (host))
    {
        base_argv[0] = "/bin/sh";
        base_argv[1] = "-c";
        base_argv[2] = command;
        base_argv[3] = NULL;
        spawn_flags  = G_SPAWN_DEFAULT;
    }
    else
    {
        base_argv[0] = command;
        base_argv[1] = NULL;
        spawn_flags  = G_SPAWN_SEARCH_PATH;
    }

    /* Lift the resolved login snapshot for the worker (no-op for a local
     * host, where wrap_argv ignores it). */
    pn_ssh_login_dup_locked (&self->mutex, &self->ssh_login, &login);
    argv = pn_shell_wrap_argv (host, &login, base_argv);

    spawned = g_spawn_sync (NULL,
                            argv,
                            NULL,
                            spawn_flags,
                            NULL, NULL,
                            &stdout_text,
                            &stderr_text,
                            &exit_status,
                            &error);

    if (!spawned)
    {
        /* No streams on spawn failure: route the error text through the
         * stderr slot so Text mode shows it and JSON mode reports the
         * failure with that text. */
        spawn_err = g_strdup (error ? error->message : "spawn failed");
        g_clear_error (&error);
    }
    else
    {
        success = g_spawn_check_wait_status (exit_status, NULL);
    }

    msg = pn_message_new (node, NULL);
    pn_shell_output_apply (msg, fmt, success,
                           spawned ? stdout_text : NULL,
                           spawned ? stderr_text : spawn_err);

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_free (spawn_err);
    g_free (stdout_text);
    g_free (stderr_text);
    g_free (command);
    g_free (host);
    g_strfreev (argv);
    pn_ssh_login_clear (&login);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_shell_command_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnShellCommand *self = PN_SHELL_COMMAND (object);

    switch (prop_id)
    {
    case PROP_SHELL_COMMAND:
        g_value_take_string (value, shell_dup_command_locked (self));
        break;
    case PROP_HOST:
        g_value_take_string (value, shell_dup_host_locked (self));
        break;
    case PROP_AUTH_PROFILE:
        g_value_set_string (value, self->auth_profile);
        break;
    case PROP_OUTPUT_FORMAT:
        g_value_set_enum (value, shell_get_output_format_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_shell_command_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnShellCommand *self = PN_SHELL_COMMAND (object);

    switch (prop_id)
    {
    case PROP_SHELL_COMMAND:
        shell_set_command (self, g_value_get_string (value));
        break;
    case PROP_HOST:
        shell_set_host (self, g_value_get_string (value));
        break;
    case PROP_AUTH_PROFILE:
        g_free (self->auth_profile);
        self->auth_profile = g_value_dup_string (value);
        pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                              &self->mutex, &self->ssh_login);
        break;
    case PROP_OUTPUT_FORMAT:
        g_mutex_lock (&self->mutex);
        self->output_format = g_value_get_enum (value);
        g_mutex_unlock (&self->mutex);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* A profile was added / edited / removed in the vault: re-resolve our
 * login snapshot so the next sample uses the current credentials. */
static void
on_vault_changed (PnShellCommand *self)
{
    pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                          &self->mutex, &self->ssh_login);
}

static void
pn_shell_command_finalize (GObject *object)
{
    PnShellCommand *self = PN_SHELL_COMMAND (object);

    g_clear_pointer (&self->shell_command, g_free);
    g_clear_pointer (&self->host,          g_free);
    g_clear_pointer (&self->auth_profile,  g_free);
    pn_ssh_login_clear (&self->ssh_login);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_shell_command_parent_class)->finalize (object);
}

static void
pn_shell_command_class_init (PnShellCommandClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_shell_command_get_property;
    object_class->set_property = pn_shell_command_set_property;
    object_class->finalize     = pn_shell_command_finalize;
    trigger_class->trigger     = pn_shell_command_trigger;

    node_class->palette_icon   = PN_SHELL_COMMAND_NORMAL_ICON;
    node_class->class_name     = "Shell Command";
    node_class->icon           = PN_SHELL_COMMAND_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.42, 0.62, 0.86, 1.0 };
    node_class->category       = "Shell";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_SHELL_COMMAND] = g_param_spec_string (
            "shell-command", "Shell Command",
            "One-line shell command to execute periodically; while "
            "empty the node is marked as needing configuration and "
            "emits nothing",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HOST] = g_param_spec_string (
            "host", "Host",
            "Hostname (or user@host) to run the command on.  An empty "
            "string (the default) or \"localhost\" runs locally; any "
            "other value routes the command through passwordless ssh "
            "(BatchMode=yes — a pre-installed key is required).",
            PN_SHELL_HOST_DEFAULT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    /* Show the local machine's name as a grey hint while the field is
     * blank, so the user sees where an empty host actually runs. */
    pn_param_spec_set_hostname_hint (props[PROP_HOST]);

    /* Optional SSH Login credential picker (item 47.4); sits next to the
     * host field — host says WHERE, this says HOW to log in. */
    props[PROP_AUTH_PROFILE] = pn_ssh_auth_profile_param_spec ();

    props[PROP_OUTPUT_FORMAT] = g_param_spec_enum (
            "output-format", "Output Format",
            "How the command's output becomes the message.  Text: the "
            "combined stdout+stderr lands in data.output.  JSON: stdout "
            "is parsed — a JSON object merges into the data bag (the "
            "command defines the payload), a bare number/value lands in "
            "data.value.",
            PN_TYPE_SHELL_OUTPUT_FORMAT, PN_SHELL_OUTPUT_TEXT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_shell_command_init (PnShellCommand *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->shell_command = NULL;
    self->host          = g_strdup (PN_SHELL_HOST_DEFAULT);
    self->auth_profile  = g_strdup ("");
    self->ssh_login     = (PnSshLogin){ 0 };
    self->output_format = PN_SHELL_OUTPUT_TEXT;

    /* Keep the login snapshot current as the user edits credentials, and
     * seed it now (default ref "" -> primary SSH Login profile, or ambient
     * when none is configured). */
    g_signal_connect_object (pn_vault_get_default (), "changed",
                             G_CALLBACK (on_vault_changed), self,
                             G_CONNECT_SWAPPED);
    pn_ssh_login_refresh (node, "auth-profile", &self->mutex, &self->ssh_login);

    pn_node_set_class_name (node, "Shell Command");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_SHELL_COMMAND_DEFAULT_PERIOD);

    apply_visual_state (self, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnShellCommand *
pn_shell_command_new (void)
{
    return g_object_new (PN_TYPE_SHELL_COMMAND, NULL);
}
