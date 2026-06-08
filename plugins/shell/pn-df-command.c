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

#include "pn-df-command.h"
#include "pn-message.h"
#include "pn-shell-host.h"
#include "pn-ssh-profile.h"
#include "pn-vault.h"

#include <json-glib/json-glib.h>

/* fa-hdd-o U+F0A0 — disk glyph in the FA-4 range the bundled icon
 * font supports.  Same FA-4 ceiling (no codepoints past U+F2FF) the
 * Free Command node's icon comment spells out applies here. */
#define PN_DF_COMMAND_ICON  "\xef\x82\xa0"

#define PN_DF_COMMAND_DEFAULT_PERIOD 5u

/* Default `df` output is six logical columns:
 *
 *   Filesystem  1K-blocks  Used  Available  Use%  Mounted on
 *
 * The first five are whitespace-free tokens; the sixth is the
 * mount path, which may itself contain whitespace (a header word
 * "Mounted on" with an internal space, and mount points like
 * "/mnt/My Documents") -- splitting line-wise on whitespace and
 * then joining the tail tokens back together for the last column
 * handles both gracefully.  Counted as a constant rather than
 * derived from the header so the parser is robust against a header
 * line whose internal-space header ("Mounted on") would otherwise
 * be tokenised into two separate column slots. */
#define PN_DF_COMMAND_COLUMNS 6u

struct _PnDfCommand
{
    PnAutoTrigger parent_instance;

    /* The "host" property; serialised through @mutex so the worker
     * thread's trigger can read it while the main thread writes it
     * from the inspector. */
    GMutex  mutex;
    gchar  *host;

    /* Optional SSH Login credential (item 47.4): @auth_profile is the
     * picked profile id (main thread only); @ssh_login is the value
     * snapshot the worker reads under @mutex, refreshed on the main
     * thread when the property or the vault changes. */
    gchar      *auth_profile;
    PnSshLogin  ssh_login;
};

G_DEFINE_TYPE (PnDfCommand, pn_df_command, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOST,
    PROP_AUTH_PROFILE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Host accessor (thread-safe)                                        */
/* ------------------------------------------------------------------ */

static gchar *
df_dup_host_locked (PnDfCommand *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->host);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
df_set_host (PnDfCommand *self, const gchar *host)
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
/*  Identifier sanitisation                                            */
/* ------------------------------------------------------------------ */

static gchar *
sanitise_id (const gchar *src)
{
    GString     *out;
    const gchar *p;

    if (src == NULL || *src == '\0')
        return g_strdup ("");

    out = g_string_new (NULL);
    for (p = src; *p != '\0'; p++)
    {
        gchar c = *p;
        if (c >= 'A' && c <= 'Z')
            c = (gchar) (c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            g_string_append_c (out, c);
        else if (c == ':')
            continue;
        else
            g_string_append_c (out, '_');
    }

    /* Strip leading and trailing underscores so a filesystem path like
     * "/dev/nvme0n1p2" reads as "dev_nvme0n1p2" rather than the noisier
     * "_dev_nvme0n1p2". */
    while (out->len > 0 && out->str[out->len - 1] == '_')
        g_string_truncate (out, out->len - 1);
    while (out->len > 0 && out->str[0] == '_')
        g_string_erase (out, 0, 1);

    return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Whitespace tokenisation                                            */
/* ------------------------------------------------------------------ */

static gchar **
split_whitespace (const gchar *line)
{
    GPtrArray *out = g_ptr_array_new ();
    gchar    **raw = g_strsplit_set (line, " \t", -1);
    guint      i;

    for (i = 0; raw[i] != NULL; i++)
    {
        if (*raw[i] != '\0')
            g_ptr_array_add (out, g_strdup (raw[i]));
    }
    g_ptr_array_add (out, NULL);
    g_strfreev (raw);
    return (gchar **) g_ptr_array_free (out, FALSE);
}

/* Return tokens[start..end-1] joined with single spaces; if @start is
 * past the end of @tokens, returns an empty string.  Used to fold the
 * tail tokens of every line back into the single "Mounted on" /
 * mount-path column. */
static gchar *
join_tail (gchar **tokens, guint start)
{
    GString *out = g_string_new (NULL);
    guint    i;

    for (i = start; tokens[i] != NULL; i++)
    {
        if (out->len > 0)
            g_string_append_c (out, ' ');
        g_string_append (out, tokens[i]);
    }

    return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Cell helpers                                                       */
/* ------------------------------------------------------------------ */

static JsonObject *
make_cell (const gchar *text, const gchar *name)
{
    JsonObject *cell = json_object_new ();
    json_object_set_string_member (cell, "text", text != NULL ? text : "");
    json_object_set_string_member (cell, "name", name != NULL ? name : "");
    return cell;
}

static JsonObject *
wrap_cells (JsonArray *cells)
{
    JsonObject *row = json_object_new ();
    json_object_set_array_member (row, "cells", cells);
    return row;
}

/* ------------------------------------------------------------------ */
/*  Table build                                                        */
/* ------------------------------------------------------------------ */

/* Exposed (non-static) for unit testing — see tests/unit/test-pn-df-command.c.
 * Kept out of the public header to keep the plugin's surface clean; the test
 * declares a matching extern prototype.  Renamed from the file-local
 * build_table_from_output so it no longer collides with the same-named static
 * helpers in the sibling pn-free-command.c / pn-lxc-ls-command.c, which all
 * link into the one pipnode_shell.la. */
JsonNode *
pn_df_command_build_table (const gchar *output)
{
    JsonObject  *table     = json_object_new ();
    JsonArray   *rows      = json_array_new ();
    gchar      **raw_lines = NULL;
    GPtrArray   *cleaned;
    gchar      **column_ids = NULL;
    JsonNode    *node;
    guint        i;

    if (output != NULL)
        raw_lines = g_strsplit (output, "\n", -1);

    cleaned = g_ptr_array_new_with_free_func (g_free);
    if (raw_lines != NULL)
    {
        for (i = 0; raw_lines[i] != NULL; i++)
        {
            gchar *trimmed = g_strdup (raw_lines[i]);
            g_strstrip (trimmed);
            if (*trimmed != '\0')
                g_ptr_array_add (cleaned, trimmed);
            else
                g_free (trimmed);
        }
    }

    if (cleaned->len >= 2)
    {
        const gchar *header_line = g_ptr_array_index (cleaned, 0);
        gchar      **header_words = split_whitespace (header_line);
        guint        c;

        /* Build per-column header text + sanitised id.  Columns 0..4
         * are single-token headers; column 5 (the "Mounted on" slot)
         * absorbs every remaining header token so a header that came
         * in as "... Use% Mounted on" still produces the single
         * "Mounted on" / "mounted_on" pair. */
        column_ids = g_new0 (gchar *, PN_DF_COMMAND_COLUMNS + 1);
        {
            JsonArray *header_cells = json_array_new ();
            for (c = 0; c < PN_DF_COMMAND_COLUMNS; c++)
            {
                gchar *text;
                gchar *id;
                gchar *cell_name;

                if (c + 1 == PN_DF_COMMAND_COLUMNS)
                    text = join_tail (header_words, c);
                else
                    text = g_strdup (header_words[c] != NULL
                                         ? header_words[c]
                                         : "");

                id = sanitise_id (text);
                if (*id == '\0')
                {
                    g_free (id);
                    id = g_strdup_printf ("col%u", c);
                }
                column_ids[c] = id;

                cell_name = g_strdup_printf ("header.%s", id);
                json_array_add_object_element (header_cells,
                                               make_cell (text, cell_name));
                g_free (cell_name);
                g_free (text);
            }
            json_object_set_object_member (table, "header",
                                           wrap_cells (header_cells));
        }
        g_strfreev (header_words);

        for (i = 1; i < cleaned->len; i++)
        {
            const gchar *line  = g_ptr_array_index (cleaned, i);
            gchar      **words = split_whitespace (line);
            JsonArray   *cells;
            gchar       *row_id;
            guint        n_words;

            if (words[0] == NULL)
            {
                g_strfreev (words);
                continue;
            }

            for (n_words = 0; words[n_words] != NULL; n_words++)
                ;

            row_id = sanitise_id (words[0]);
            if (*row_id == '\0')
            {
                g_free (row_id);
                row_id = g_strdup_printf ("row%u", i);
            }

            cells = json_array_new ();
            for (c = 0; c < PN_DF_COMMAND_COLUMNS; c++)
            {
                gchar *text;
                gchar *cell_name;

                /* A row shorter than six columns pads the missing trailing
                 * cells with "".  Guard on n_words so we never index
                 * words[] past its NULL terminator (nor hand join_tail a
                 * @start beyond the token run). */
                if (c >= n_words)
                    text = g_strdup ("");
                else if (c + 1 == PN_DF_COMMAND_COLUMNS)
                    text = join_tail (words, c);
                else
                    text = g_strdup (words[c]);

                cell_name = g_strdup_printf ("%s.%s", row_id, column_ids[c]);
                json_array_add_object_element (cells,
                                               make_cell (text, cell_name));
                g_free (cell_name);
                g_free (text);
            }
            json_array_add_object_element (rows, wrap_cells (cells));

            g_free (row_id);
            g_strfreev (words);
        }
    }

    json_object_set_array_member (table, "rows", rows);

    if (column_ids != NULL)
        g_strfreev (column_ids);
    g_ptr_array_free (cleaned, TRUE);
    g_strfreev (raw_lines);

    node = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (node, table);
    return node;
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/* ------------------------------------------------------------------ */

static void
pn_df_command_trigger (PnAutoTrigger *trigger)
{
    PnDfCommand       *self        = PN_DF_COMMAND (trigger);
    PnNode            *node        = PN_NODE (self);
    gchar             *host        = df_dup_host_locked (self);
    gchar             *stdout_text = NULL;
    gchar             *stderr_text = NULL;
    gchar             *combined;
    gint               exit_status = 0;
    GError            *error       = NULL;
    PnMessage         *msg;
    gboolean           spawned;
    gboolean           success     = FALSE;
    const gchar       *base_argv[] = { "df", NULL };
    gchar            **argv;
    PnSshLogin         login       = { 0 };

    pn_ssh_login_dup_locked (&self->mutex, &self->ssh_login, &login);
    argv = pn_shell_wrap_argv (host, &login, base_argv);

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

    msg = pn_message_new (node, NULL);
    pn_message_set_boolean (msg, "success", success);
    pn_message_set_string  (msg, "output",  combined);
    pn_message_set_member  (msg, "table",
                            pn_df_command_build_table (success ? combined
                                                               : NULL));

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_free (combined);
    g_free (stdout_text);
    g_free (stderr_text);
    g_free (host);
    g_strfreev (argv);
    pn_ssh_login_clear (&login);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_df_command_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnDfCommand *self = PN_DF_COMMAND (object);

    switch (prop_id)
    {
    case PROP_HOST:
        g_value_take_string (value, df_dup_host_locked (self));
        break;
    case PROP_AUTH_PROFILE:
        g_value_set_string (value, self->auth_profile);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_df_command_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnDfCommand *self = PN_DF_COMMAND (object);

    switch (prop_id)
    {
    case PROP_HOST:
        df_set_host (self, g_value_get_string (value));
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

/* A profile changed in the vault: re-resolve our login snapshot. */
static void
on_vault_changed (PnDfCommand *self)
{
    pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                          &self->mutex, &self->ssh_login);
}

static void
pn_df_command_finalize (GObject *object)
{
    PnDfCommand *self = PN_DF_COMMAND (object);

    g_clear_pointer (&self->host,         g_free);
    g_clear_pointer (&self->auth_profile, g_free);
    pn_ssh_login_clear (&self->ssh_login);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_df_command_parent_class)->finalize (object);
}

static void
pn_df_command_class_init (PnDfCommandClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_df_command_get_property;
    object_class->set_property = pn_df_command_set_property;
    object_class->finalize     = pn_df_command_finalize;
    trigger_class->trigger     = pn_df_command_trigger;

    node_class->palette_icon   = PN_DF_COMMAND_ICON;
    node_class->class_name     = "Df Command";
    node_class->icon           = PN_DF_COMMAND_ICON;
    node_class->color          = (PnColor){ 0.42, 0.62, 0.86, 1.0 };
    node_class->category       = "Shell";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

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

    /* Optional SSH Login credential picker (item 47.4). */
    props[PROP_AUTH_PROFILE] = pn_ssh_auth_profile_param_spec ();

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_df_command_init (PnDfCommand *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.42, 0.62, 0.86, 1.0 };

    g_mutex_init (&self->mutex);
    self->host         = g_strdup (PN_SHELL_HOST_DEFAULT);
    self->auth_profile = g_strdup ("");
    self->ssh_login    = (PnSshLogin){ 0 };

    g_signal_connect_object (pn_vault_get_default (), "changed",
                             G_CALLBACK (on_vault_changed), self,
                             G_CONNECT_SWAPPED);
    pn_ssh_login_refresh (node, "auth-profile", &self->mutex, &self->ssh_login);

    pn_node_set_class_name (node, "Df Command");
    pn_node_set_icon       (node, PN_DF_COMMAND_ICON);
    pn_node_set_color      (node, &blue);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_DF_COMMAND_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDfCommand *
pn_df_command_new (void)
{
    return g_object_new (PN_TYPE_DF_COMMAND, NULL);
}
