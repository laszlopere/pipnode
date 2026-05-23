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
/*  PnTmuxMonitor — companion GUI module for the shell plugin.         */
/*                                                                     */
/*  Built as pipnode_shell-gui.so, the sibling of the logic module     */
/*  pipnode_shell.so (TODO #23, Phase 8).  The editor loads the logic  */
/*  .so first (registering every shell #GType), then finds this        */
/*  companion next to it and calls pn_plugin_gui_init(), which installs */
/*  the Tmux Monitor node's settings-dialog vfunc slots onto the       */
/*  already-registered class.  pipnode-run never loads a companion, so  */
/*  the shell logic runs on a server with no GTK while the editor       */
/*  still shows this dialog.                                           */
/*                                                                     */
/*  Tmux Monitor is the only shell node with a custom dialog: a live   */
/*  session combobox (populated from the node's GTK-free `sessions`    */
/*  read-seam property and the `tmux list-sessions` enumeration the     */
/*  logic half runs on the configured host), a deferred-commit host     */
/*  entry, and a red runtime status row driven by `last-error`.  None   */
/*  of that fits the declarative settings schema, so it ships as an     */
/*  imperative companion (decision D2's escape hatch).                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gmodule.h>
#include <gtk/gtk.h>

#include "pn-node.h"
#include "pn-node-factory.h"
#include "pn-node-dialog-helpers.h"
#include "pn-plugin.h"

/* The companion resolves the node #GType by its registered name rather
 * than calling pn_tmux_monitor_get_type().  The host loads the logic
 * plugin with G_MODULE_BIND_LOCAL, so its symbols (the type getter, the
 * tm_dup_sessions_cache helper, the private struct) are NOT visible
 * here — linking the logic header would only yield "undefined symbol"
 * at companion-load time.  This file therefore drives the node through
 * public API only: GObject properties ("host", "tmux-session",
 * "line-limit", "sessions", "last-error", "busy") and the PnNode vfunc
 * table.  The session list crosses the barrier as the read-only
 * `sessions` property the logic half exposes for exactly this reason. */
#define PN_TMUX_MONITOR_TYPE_NAME "PnTmuxMonitor"

/* ------------------------------------------------------------------ */
/*  Session combobox                                                   */
/* ------------------------------------------------------------------ */

static void
session_combo_repopulate (GObject         *target,
                          GtkComboBoxText *combo)
{
    gchar          *current  = NULL;
    gchar         **cache    = NULL;
    gboolean        listed   = FALSE;

    g_object_get (target, "tmux-session", &current, NULL);
    /* The GTK-free read seam — a copy of the latest enumerated session
     * list, or NULL until the first `tmux list-sessions` finished. */
    g_object_get (target, "sessions", &cache, NULL);

    gtk_combo_box_text_remove_all (combo);

    /* Sentinel empty entry mirrors the Meshtastic device combo — gives
     * the active-id binding something to land on for a brand-new node
     * with no session picked yet, and lets the user explicitly
     * unselect by picking it back. */
    gtk_combo_box_text_append (combo, "", "(no session)");

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
on_tmux_sessions_or_host_notify (GObject    *target,
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
/*  the entry the user is typing into.  Plus we'd be firing one ssh    */
/*  hop per keystroke.                                                 */
/*                                                                     */
/*  This editor commits the entry's text to the host property only     */
/*  on `activate` (Enter) and `focus-out-event` (the user moved on),   */
/*  and refreshes the entry text on `notify::host` so an external      */
/*  write (e.g. the worksheet loader) still flows in.  The notify      */
/*  handler skips the write when the entry already shows the new       */
/*  value, so a self-triggered notify after our own commit doesn't     */
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

/* ------------------------------------------------------------------ */
/*  Per-property editors                                               */
/* ------------------------------------------------------------------ */

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

        /* Repopulate when a fresh enumeration lands (notify::sessions,
         * the logic half's GTK-free read seam) and when the user picks
         * a new host (notify::host) so the combo follows the host edit
         * without a dialog reopen.  g_signal_connect_object ties both
         * handlers to the combo's lifetime so the dialog can close
         * cleanly. */
        g_signal_connect_object (target, "notify::sessions",
                                 G_CALLBACK (on_tmux_sessions_or_host_notify),
                                 combo, 0);
        g_signal_connect_object (target, "notify::host",
                                 G_CALLBACK (on_tmux_sessions_or_host_notify),
                                 combo, 0);

        gtk_widget_set_hexpand   (combo, TRUE);
        gtk_widget_set_sensitive (combo, writable);
        g_object_bind_property (target, name, combo, "active-id", flags);
        return combo;
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Runtime status row                                                 */
/* ------------------------------------------------------------------ */

static void
update_status_label (GObject    *obj,
                     GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer    user_data)
{
    GtkLabel *label    = GTK_LABEL (user_data);
    gchar    *snapshot = NULL;
    gchar    *markup;

    /* Read the failure text through the public property — the companion
     * cannot reach the node's private last_error field across the
     * BIND_LOCAL barrier. */
    g_object_get (obj, "last-error", &snapshot, NULL);

    if (snapshot != NULL && *snapshot != '\0')
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

/* ------------------------------------------------------------------ */
/*  Per-class settings tab                                             */
/* ------------------------------------------------------------------ */

static GtkWidget *
pn_tmux_monitor_build_class_tab (PnNode    *self,
                                 GtkWindow *parent)
{
    GObject      *target = G_OBJECT (self);
    GObjectClass *klass  = G_OBJECT_GET_CLASS (target);
    GtkWidget    *grid   = pn_node_dialog_new_property_grid ();
    GParamSpec   *host_pspec;
    GParamSpec   *session_pspec;
    GParamSpec   *line_limit_pspec;
    GtkWidget    *host_editor;
    GtkWidget    *session_editor;
    GtkWidget    *line_limit_editor;
    GtkWidget    *status_label;

    /* The companion has no access to the logic half's static props[]
     * array, so resolve each pspec by name off the registered class. */
    host_pspec       = g_object_class_find_property (klass, "host");
    session_pspec    = g_object_class_find_property (klass, "tmux-session");
    line_limit_pspec = g_object_class_find_property (klass, "line-limit");

    host_editor = pn_tmux_monitor_build_property_editor (
            self, host_pspec, target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 0,
                               g_param_spec_get_nick (host_pspec),
                               host_editor);

    session_editor = pn_tmux_monitor_build_property_editor (
            self, session_pspec, target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 1,
                               g_param_spec_get_nick (session_pspec),
                               session_editor);

    line_limit_editor = pn_node_dialog_default_editor (
            target, line_limit_pspec);
    pn_node_dialog_attach_row (GTK_GRID (grid), 2,
                               g_param_spec_get_nick (line_limit_pspec),
                               line_limit_editor);

    status_label = gtk_label_new (NULL);
    gtk_label_set_xalign     (GTK_LABEL (status_label), 0.0);
    gtk_label_set_use_markup (GTK_LABEL (status_label), TRUE);
    /* This row is the read-only display for the `last-error` property, so
     * tag it with the standard pn-prop-<name> handle.  The auto dialog
     * skips non-writable properties, so this widget exists only when the
     * companion's build_class_tab ran — which is exactly the positive
     * signal tests/test_node_dialog_shell_companion.py probes to confirm
     * pn_plugin_gui_init() loaded this module. */
    gtk_widget_set_name (status_label, "pn-prop-last-error");
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
/*  Companion entry point                                              */
/* ------------------------------------------------------------------ */

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_gui_init (PnNodeFactory *factory G_GNUC_UNUSED)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "pipnode-shell (GUI)",
        .version     = "1.5.0",
        .description = "Companion GUI module: the Tmux Monitor node's "
                       "settings dialog (live session combobox, "
                       "deferred-commit host entry, runtime status row).",
    };
    GType        type = g_type_from_name (PN_TMUX_MONITOR_TYPE_NAME);
    PnNodeClass *node_class;

    if (type == 0)
    {
        /* The logic half did not register the type — nothing to dress
         * up.  Returning the descriptor still counts as a clean load. */
        g_warning ("pipnode-shell (GUI): %s is not registered; is the "
                   "logic plugin loaded?", PN_TMUX_MONITOR_TYPE_NAME);
        return &info;
    }

    /* Install the dialog vfunc slots onto the class the logic .so
     * already registered.  The class ref is intentionally held for the
     * process lifetime so the slots stay valid — the same one-leaked-
     * ref-on-a-singleton-class pattern pn_<node>_gui_install() uses. */
    node_class = PN_NODE_CLASS (g_type_class_ref (type));

    node_class->build_property_editor = pn_tmux_monitor_build_property_editor;
    node_class->build_class_tab       = pn_tmux_monitor_build_class_tab;

    return &info;
}
