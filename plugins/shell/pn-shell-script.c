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
/*  PnShellScript — the multi-line sibling of PnShellCommand.          */
/*                                                                     */
/*  Runtime behaviour is identical to PnShellCommand (periodically     */
/*  spawn `/bin/sh -c <body>` on the worker thread, local or ssh,      */
/*  emit data.success + data.output); the only differences are         */
/*  cosmetic — its own palette name and icon — and that the body is    */
/*  edited as a whole multi-line SCRIPT in a full-width GtkSourceView   */
/*  code editor with `sh` highlighting.  That editor is declared        */
/*  purely through the GTK-free PnSettingsSchema (PN_EDITOR_CODE +      */
/*  the new code-language hint), so this node — like the Rewrite       */
/*  node it borrows the pattern from — needs no GUI companion and       */
/*  runs headless under pipnode-run.                                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-shell-script.h"
#include "pn-message.h"
#include "pn-settings-schema.h"
#include "pn-shell-host.h"
#include "pn-ssh-profile.h"
#include "pn-vault.h"

/* fa-file-code-o (U+F120's script cousin) keeps the steel-blue shell
 * family look while reading as "a script file" rather than a prompt. */
#define PN_SHELL_SCRIPT_NORMAL_ICON  "\xef\x87\x89"   /* fa-file-code-o U+F1C9 */

#define PN_SHELL_SCRIPT_DEFAULT_PERIOD 5u

struct _PnShellScript
{
    PnAutoTrigger parent_instance;

    /* The worker thread reads @shell_script and @host while the main
     * thread may overwrite either via a property setter, so accesses
     * are serialised through @mutex.  Both strings are owned by the
     * node and freed in finalize. */
    GMutex  mutex;
    gchar  *shell_script;
    gchar  *host;

    /* The optional SSH Login credential (item 47.4).  @auth_profile is the
     * picked profile id and is touched only on the main thread; @ssh_login
     * is the value snapshot resolved from it, read by the worker under
     * @mutex and refreshed on the main thread when the property or the
     * vault changes. */
    gchar      *auth_profile;
    PnSshLogin  ssh_login;
};

G_DEFINE_TYPE (PnShellScript, pn_shell_script, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_SHELL_SCRIPT,
    PROP_AUTH_PROFILE,
    PROP_HOST,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (
        PnShellScript *self,
        gboolean       configured)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.42, 0.62, 0.86, 1.0 };

    /* Keep the healthy blue identity at all times; the red body + ❗
     * overlay for the unconfigured state is painted centrally by the
     * worksheet whenever has-error is set. */
    pn_node_set_color     (node, &blue);
    pn_node_set_icon      (node, PN_SHELL_SCRIPT_NORMAL_ICON);
    pn_node_set_has_error (node, !configured);
}

/* ------------------------------------------------------------------ */
/*  Script accessor (thread-safe)                                      */
/* ------------------------------------------------------------------ */

static gchar *
shell_dup_script_locked (PnShellScript *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->shell_script);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
shell_set_script (
        PnShellScript *self,
        const gchar   *script)
{
    gchar    *old;
    gchar    *replacement;
    gboolean  configured;

    replacement = (script != NULL) ? g_strdup (script) : NULL;
    configured  = (script != NULL && *script != '\0');

    g_mutex_lock (&self->mutex);
    old = self->shell_script;
    self->shell_script = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    apply_visual_state (self, configured);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_SHELL_SCRIPT]);
}

static gchar *
shell_dup_host_locked (PnShellScript *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->host);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
shell_set_host (
        PnShellScript *self,
        const gchar   *host)
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

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger.  Spawn    */
/*  /bin/sh -c <script> so the user's whole multi-line script can use  */
/*  functions, loops, pipes, redirections and variable expansion       */
/*  without us having to tokenise it ourselves.                        */
/* ------------------------------------------------------------------ */

static void
pn_shell_script_trigger (PnAutoTrigger *trigger)
{
    PnShellScript      *self        = PN_SHELL_SCRIPT (trigger);
    PnNode             *node        = PN_NODE (self);
    gchar              *script_raw  = shell_dup_script_locked (self);
    gchar              *script;
    gchar              *host        = shell_dup_host_locked (self);
    gchar              *stdout_text = NULL;
    gchar              *stderr_text = NULL;
    gchar              *combined;
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
    if (script_raw == NULL || *script_raw == '\0')
    {
        g_free (script_raw);
        g_free (host);
        return;
    }

    /* Interpolate ${nodeclass} / ${nodename} / ${hostname}; any other
     * ${...} is left for the login shell to expand (e.g. ${HOME}). */
    script = pn_node_expand_vars (node, script_raw);
    g_free (script_raw);

    /* Local: spawn /bin/sh -c <script> directly so the whole script —
     * pipes, redirections, loops, here-docs and variable expansion —
     * runs as one shell program without us tokenising it.  Remote: hand
     * the script string to ssh as a single argv element so the remote
     * login shell (not us) interprets it, and so embedded spaces and
     * newlines survive sshd's argv-join-with-spaces behaviour intact. */
    if (pn_shell_host_is_local (host))
    {
        base_argv[0] = "/bin/sh";
        base_argv[1] = "-c";
        base_argv[2] = script;
        base_argv[3] = NULL;
        spawn_flags  = G_SPAWN_DEFAULT;
    }
    else
    {
        base_argv[0] = script;
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

    msg = pn_message_new (node, NULL);
    pn_message_set_boolean (msg, "success", success);
    pn_message_set_string  (msg, "output",  combined);

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_free (combined);
    g_free (stdout_text);
    g_free (stderr_text);
    g_free (script);
    g_free (host);
    g_strfreev (argv);
    pn_ssh_login_clear (&login);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_shell_script_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnShellScript *self = PN_SHELL_SCRIPT (object);

    switch (prop_id)
    {
    case PROP_SHELL_SCRIPT:
        g_value_take_string (value, shell_dup_script_locked (self));
        break;
    case PROP_HOST:
        g_value_take_string (value, shell_dup_host_locked (self));
        break;
    case PROP_AUTH_PROFILE:
        g_value_set_string (value, self->auth_profile);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_shell_script_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnShellScript *self = PN_SHELL_SCRIPT (object);

    switch (prop_id)
    {
    case PROP_SHELL_SCRIPT:
        shell_set_script (self, g_value_get_string (value));
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
on_vault_changed (PnShellScript *self)
{
    pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                          &self->mutex, &self->ssh_login);
}

static void
pn_shell_script_finalize (GObject *object)
{
    PnShellScript *self = PN_SHELL_SCRIPT (object);

    g_clear_pointer (&self->shell_script, g_free);
    g_clear_pointer (&self->host,         g_free);
    g_clear_pointer (&self->auth_profile, g_free);
    pn_ssh_login_clear (&self->ssh_login);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_shell_script_parent_class)->finalize (object);
}

static void
pn_shell_script_class_init (PnShellScriptClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_shell_script_get_property;
    object_class->set_property = pn_shell_script_set_property;
    object_class->finalize     = pn_shell_script_finalize;
    trigger_class->trigger     = pn_shell_script_trigger;

    node_class->palette_icon   = PN_SHELL_SCRIPT_NORMAL_ICON;
    node_class->class_name     = "Shell Script";
    node_class->icon           = PN_SHELL_SCRIPT_NORMAL_ICON;
    node_class->color          = (PnColor){ 0.42, 0.62, 0.86, 1.0 };
    node_class->category       = "Shell";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_SHELL_SCRIPT] = g_param_spec_string (
            "shell-script", "Shell Script",
            "Multi-line shell script to execute periodically via "
            "/bin/sh -c; while empty the node is marked as needing "
            "configuration and emits nothing",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HOST] = g_param_spec_string (
            "host", "Host",
            "Hostname (or user@host) to run the script on.  An empty "
            "string (the default) or \"localhost\" runs locally; any "
            "other value routes the script through passwordless ssh "
            "(BatchMode=yes — a pre-installed key is required).",
            PN_SHELL_HOST_DEFAULT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    /* Show the local machine's name as a grey hint while the field is
     * blank, so the user sees where an empty host actually runs. */
    pn_param_spec_set_hostname_hint (props[PROP_HOST]);

    /* Optional SSH Login credential picker (item 47.4); sits next to the
     * host field — host says WHERE, this says HOW to log in. */
    props[PROP_AUTH_PROFILE] = pn_ssh_auth_profile_param_spec ();

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings schema, split across two tabs: a "Host" tab
     * with the where/how-to-connect fields (host + SSH login), then a
     * "Script" tab that is the script body alone in a full-width
     * GtkSourceView code editor with `sh` syntax highlighting.  The
     * editor gets its own last tab so it can use the whole page; the
     * tabs render in declaration order, after PnAutoTrigger's own
     * `period` tab, so "Script" is the rightmost.
     *
     * Declaring named tabs makes the renderer own this class's settings
     * pages (the auto tab is skipped), so host + auth-profile are listed
     * explicitly here.  The editor lives entirely in the GUI tier's
     * renderer, so this GTK-free description keeps the node headless —
     * no companion. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Host");
        pn_settings_schema_row (schema, "host",         PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "auth-profile", PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Script");
        pn_settings_schema_row (schema, "shell-script", PN_EDITOR_CODE);
        pn_settings_schema_code_language (schema, "shell-script", "sh");
        pn_settings_schema_row_flags (schema, "shell-script",
                                      PN_ROW_FLAG_FULL_WIDTH);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_shell_script_init (PnShellScript *self)
{
    PnNode *node = PN_NODE (self);

    g_mutex_init (&self->mutex);
    self->shell_script = NULL;
    self->host         = g_strdup (PN_SHELL_HOST_DEFAULT);
    self->auth_profile = g_strdup ("");
    self->ssh_login    = (PnSshLogin){ 0 };

    /* Keep the login snapshot current as the user edits credentials, and
     * seed it now (default ref "" -> primary SSH Login profile, or ambient
     * when none is configured). */
    g_signal_connect_object (pn_vault_get_default (), "changed",
                             G_CALLBACK (on_vault_changed), self,
                             G_CONNECT_SWAPPED);
    pn_ssh_login_refresh (node, "auth-profile", &self->mutex, &self->ssh_login);

    pn_node_set_class_name (node, "Shell Script");
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_SHELL_SCRIPT_DEFAULT_PERIOD);

    apply_visual_state (self, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnShellScript *
pn_shell_script_new (void)
{
    return g_object_new (PN_TYPE_SHELL_SCRIPT, NULL);
}
