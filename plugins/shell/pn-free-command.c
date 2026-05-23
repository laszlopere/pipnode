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

#include "pn-free-command.h"
#include "pn-message.h"
#include "pn-shell-host.h"

#include <json-glib/json-glib.h>

/* fa-microchip U+F2DB — the bundled icon font is Font Awesome 4.x
 * (no glyph past U+F2FF), so fa-memory U+F538 from FA 5 renders as
 * the missing-character box; microchip is the conventional FA-4
 * stand-in for RAM. */
#define PN_FREE_COMMAND_ICON  "\xef\x8b\x9b"

#define PN_FREE_COMMAND_DEFAULT_PERIOD 5u

struct _PnFreeCommand
{
    PnAutoTrigger parent_instance;

    /* The "host" property; serialised through @mutex so the worker
     * thread's trigger can read it while the main thread writes it
     * from the inspector. */
    GMutex  mutex;
    gchar  *host;
};

G_DEFINE_TYPE (PnFreeCommand, pn_free_command, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_HOST,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Host accessor (thread-safe)                                        */
/* ------------------------------------------------------------------ */

static gchar *
free_dup_host_locked (PnFreeCommand *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->host);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static void
free_set_host (PnFreeCommand *self, const gchar *host)
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
/*  Parsing                                                            */
/*                                                                     */
/*  The default output of `free` (no flags) is well-defined:           */
/*                                                                     */
/*    "<spaces>total used free shared buff/cache available"            */
/*    "Mem:     <total> <used> <free> <shared> <buff/cache> <avail>"   */
/*    "Swap:    <total> <used> <free>"                                 */
/*                                                                     */
/*  so we don't need PnTableModel's column-position detector; we just  */
/*  whitespace-split every line.  Trailing data columns the Swap line  */
/*  doesn't carry (shared, buff/cache, available) are emitted as empty */
/*  cells so a downstream consumer can address them by name without    */
/*  guarding against ragged rows.                                      */
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

    while (out->len > 0 && out->str[out->len - 1] == '_')
        g_string_truncate (out, out->len - 1);

    return g_string_free (out, FALSE);
}

static gchar **
split_whitespace (const gchar *line)
{
    /* g_strsplit_set yields empty tokens between consecutive
     * delimiters; collapse them so consecutive spaces don't introduce
     * phantom columns. */
    GPtrArray *out  = g_ptr_array_new ();
    gchar    **raw  = g_strsplit_set (line, " \t", -1);
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

/* Exposed (non-static) for unit testing — see tests/unit/test-pn-free-command.c.
 * Kept out of the public header to keep the plugin's surface clean; the test
 * declares a matching extern prototype.  Renamed from the file-local
 * build_table_from_output so it no longer collides with the same-named static
 * helpers in the sibling pn-df-command.c / pn-lxc-ls-command.c, which all link
 * into the one pipnode_shell.la. */
JsonNode *
pn_free_command_build_table (const gchar *output)
{
    JsonObject  *table        = json_object_new ();
    JsonArray   *rows         = json_array_new ();
    gchar      **raw_lines    = NULL;
    GPtrArray   *cleaned;
    gchar      **header_words = NULL;
    gchar      **column_ids   = NULL;
    guint        n_cols       = 0;   /* row-label column + data columns */
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
        guint        h;

        header_words = split_whitespace (header_line);
        n_cols = 1u;
        for (h = 0; header_words[h] != NULL; h++)
            n_cols++;

        /* Column ids:
         *   col 0 -> "name" (the row-label column; the header carries
         *                    no word for it)
         *   col k -> sanitised header_words[k-1] */
        column_ids = g_new0 (gchar *, n_cols + 1);
        column_ids[0] = g_strdup ("name");
        for (h = 0; header_words[h] != NULL; h++)
        {
            gchar *id = sanitise_id (header_words[h]);
            if (*id == '\0')
            {
                g_free (id);
                id = g_strdup_printf ("col%u", h + 1);
            }
            column_ids[h + 1] = id;
        }

        {
            JsonArray *header_cells = json_array_new ();
            json_array_add_object_element (header_cells,
                                           make_cell ("", "header.name"));
            for (h = 0; header_words[h] != NULL; h++)
            {
                gchar *name = g_strdup_printf ("header.%s",
                                               column_ids[h + 1]);
                json_array_add_object_element (header_cells,
                        make_cell (header_words[h], name));
                g_free (name);
            }
            json_object_set_object_member (table, "header",
                                           wrap_cells (header_cells));
        }

        for (i = 1; i < cleaned->len; i++)
        {
            const gchar  *line  = g_ptr_array_index (cleaned, i);
            gchar       **words = split_whitespace (line);
            guint         n_words;
            gchar        *row_id;
            JsonArray    *cells;
            guint         c;

            for (n_words = 0; words[n_words] != NULL; n_words++)
                ;

            if (n_words == 0)
            {
                g_strfreev (words);
                continue;
            }

            row_id = sanitise_id (words[0]);
            if (*row_id == '\0')
            {
                g_free (row_id);
                row_id = g_strdup_printf ("row%u", i);
            }

            cells = json_array_new ();
            for (c = 0; c < n_cols; c++)
            {
                const gchar *text = (c < n_words) ? words[c] : "";
                gchar       *name = g_strdup_printf ("%s.%s",
                                                     row_id, column_ids[c]);
                json_array_add_object_element (cells,
                                               make_cell (text, name));
                g_free (name);
            }
            json_array_add_object_element (rows, wrap_cells (cells));

            g_free (row_id);
            g_strfreev (words);
        }
    }

    json_object_set_array_member (table, "rows", rows);

    if (header_words != NULL)
        g_strfreev (header_words);
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
pn_free_command_trigger (PnAutoTrigger *trigger)
{
    PnFreeCommand     *self        = PN_FREE_COMMAND (trigger);
    PnNode            *node        = PN_NODE (self);
    gchar             *host        = free_dup_host_locked (self);
    gchar             *stdout_text = NULL;
    gchar             *stderr_text = NULL;
    gchar             *combined;
    gint               exit_status = 0;
    GError            *error       = NULL;
    PnMessage         *msg;
    gboolean           spawned;
    gboolean           success     = FALSE;
    const gchar       *base_argv[] = { "free", NULL };
    gchar            **argv;

    argv = pn_shell_wrap_argv (host, base_argv);

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
                            pn_free_command_build_table (success ? combined
                                                                 : NULL));

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_free (combined);
    g_free (stdout_text);
    g_free (stderr_text);
    g_free (host);
    g_strfreev (argv);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_free_command_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnFreeCommand *self = PN_FREE_COMMAND (object);

    switch (prop_id)
    {
    case PROP_HOST:
        g_value_take_string (value, free_dup_host_locked (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_free_command_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnFreeCommand *self = PN_FREE_COMMAND (object);

    switch (prop_id)
    {
    case PROP_HOST:
        free_set_host (self, g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_free_command_finalize (GObject *object)
{
    PnFreeCommand *self = PN_FREE_COMMAND (object);

    g_clear_pointer (&self->host, g_free);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_free_command_parent_class)->finalize (object);
}

static void
pn_free_command_class_init (PnFreeCommandClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_free_command_get_property;
    object_class->set_property = pn_free_command_set_property;
    object_class->finalize     = pn_free_command_finalize;
    trigger_class->trigger     = pn_free_command_trigger;

    node_class->palette_icon   = PN_FREE_COMMAND_ICON;
    node_class->class_name     = "Free Command";
    node_class->icon           = PN_FREE_COMMAND_ICON;
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

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_free_command_init (PnFreeCommand *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.42, 0.62, 0.86, 1.0 };

    g_mutex_init (&self->mutex);
    self->host = g_strdup (PN_SHELL_HOST_DEFAULT);

    pn_node_set_class_name (node, "Free Command");
    pn_node_set_icon       (node, PN_FREE_COMMAND_ICON);
    pn_node_set_color      (node, &blue);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_FREE_COMMAND_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnFreeCommand *
pn_free_command_new (void)
{
    return g_object_new (PN_TYPE_FREE_COMMAND, NULL);
}
