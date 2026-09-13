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
/*  PnTopicDemux — gui tier.                                           */
/*                                                                     */
/*  Replaces the auto-generated tab (an "outputs" spin and a raw JSON  */
/*  "topics" entry) with the output-count spin followed by one topic   */
/*  entry per output.  The entries are rebuilt when the count changes, */
/*  so there are always exactly as many as the node has outputs, and   */
/*  refreshed in place when "topics" changes from elsewhere (undo,     */
/*  D-Bus).  Every keystroke goes straight to the node through         */
/*  pn_topic_demux_set_topic(); the dialog edits the live node.        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-topic-demux-gui.h"
#include "pn-topic-demux.h"
#include "pn-node-dialog-helpers.h"

#include <gtk/gtk.h>

typedef struct
{
    PnTopicDemux *node;          /* borrowed; outlives the dialog        */
    GtkWidget    *entries_box;   /* refilled with a fresh grid           */
    GPtrArray    *entries;       /* borrowed GtkEntry per output         */
    gulong        outputs_handler;
    gulong        topics_handler;
    gboolean      updating;      /* our own writes; skip our handlers    */
} TopicDemuxTab;

static void
tab_free (gpointer data)
{
    TopicDemuxTab *tab = data;

    g_signal_handler_disconnect (tab->node, tab->outputs_handler);
    g_signal_handler_disconnect (tab->node, tab->topics_handler);
    g_ptr_array_unref (tab->entries);
    g_free (tab);
}

static void
on_entry_changed (GtkEditable *editable, gpointer user_data)
{
    TopicDemuxTab *tab   = user_data;
    gint           index = GPOINTER_TO_INT (
            g_object_get_data (G_OBJECT (editable), "pn-topic-index"));

    if (tab->updating)
        return;

    /* The notify::topics this raises must not rewrite the entry being
     * typed into (it would move the cursor, and strip trailing spaces
     * mid-word). */
    tab->updating = TRUE;
    pn_topic_demux_set_topic (tab->node, index,
                              gtk_entry_get_text (GTK_ENTRY (editable)));
    tab->updating = FALSE;
}

static void
rebuild_entries (TopicDemuxTab *tab)
{
    GtkWidget *grid = pn_node_dialog_new_property_grid ();
    gint       n    = pn_node_get_n_outputs (PN_NODE (tab->node));
    gint       i;

    gtk_container_foreach (GTK_CONTAINER (tab->entries_box),
                           (GtkCallback) gtk_widget_destroy, NULL);
    g_ptr_array_set_size (tab->entries, 0);

    /* The grid's own margins would double the tab's; the rows line up
     * with the spin row above without them. */
    g_object_set (grid, "margin", 0, NULL);

    for (i = 0; i < n; i++)
    {
        GtkWidget *entry = gtk_entry_new ();
        gchar     *label = g_strdup_printf ("Output %d", i + 1);

        gtk_entry_set_text (GTK_ENTRY (entry),
                            pn_topic_demux_get_topic (tab->node, i));
        gtk_entry_set_placeholder_text (GTK_ENTRY (entry),
                                        "unused \xe2\x80\x94 matches nothing");
        g_object_set_data (G_OBJECT (entry), "pn-topic-index",
                           GINT_TO_POINTER (i));
        g_signal_connect (entry, "changed",
                          G_CALLBACK (on_entry_changed), tab);

        pn_node_dialog_attach_row (GTK_GRID (grid), i, label, entry);
        g_ptr_array_add (tab->entries, entry);
        g_free (label);
    }

    gtk_container_add (GTK_CONTAINER (tab->entries_box), grid);
    gtk_widget_show_all (tab->entries_box);
}

static void
on_outputs_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void) object;
    (void) pspec;
    rebuild_entries (user_data);
}

static void
on_topics_notify (GObject *object, GParamSpec *pspec, gpointer user_data)
{
    TopicDemuxTab *tab = user_data;
    guint          i;

    (void) object;
    (void) pspec;

    if (tab->updating)
        return;

    tab->updating = TRUE;
    for (i = 0; i < tab->entries->len; i++)
    {
        GtkEntry    *entry = g_ptr_array_index (tab->entries, i);
        const gchar *want  = pn_topic_demux_get_topic (tab->node, (gint) i);

        if (g_strcmp0 (gtk_entry_get_text (entry), want) != 0)
            gtk_entry_set_text (entry, want);
    }
    tab->updating = FALSE;
}

static GtkWidget *
pn_topic_demux_build_class_tab (
        PnNode    *self,
        GtkWindow *parent G_GNUC_UNUSED)
{
    GtkWidget     *outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget     *top   = pn_node_dialog_new_property_grid ();
    GtkWidget     *hint;
    GtkWidget     *scrolled;
    TopicDemuxTab *tab   = g_new0 (TopicDemuxTab, 1);
    GParamSpec    *pspec;

    tab->node    = PN_TOPIC_DEMUX (self);
    tab->entries = g_ptr_array_new ();

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (self), "outputs");
    pn_node_dialog_attach_row (GTK_GRID (top), 0, "Outputs",
                               pn_node_dialog_default_editor (G_OBJECT (self),
                                                              pspec));
    gtk_box_pack_start (GTK_BOX (outer), top, FALSE, FALSE, 0);

    /* Matching rules are not discoverable from a bare entry, so they are
     * spelled out where the user types the patterns. */
    hint = gtk_label_new (
            "A message leaves by the first output whose topic matches; "
            "unmatched messages are dropped.\n"
            "MQTT wildcards: + matches one level, a trailing # the rest. "
            "Globs: * and ? (when either is present).");
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (hint), TRUE);
    gtk_widget_set_sensitive (hint, FALSE);
    g_object_set (hint, "margin-start", 12, "margin-end", 12, NULL);
    gtk_box_pack_start (GTK_BOX (outer), hint, FALSE, FALSE, 0);

    tab->entries_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    g_object_set (tab->entries_box,
                  "margin-start",  12,
                  "margin-end",    12,
                  "margin-bottom", 12,
                  NULL);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (
            GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), tab->entries_box);
    gtk_box_pack_start (GTK_BOX (outer), scrolled, TRUE, TRUE, 0);

    tab->outputs_handler = g_signal_connect (
            self, "notify::outputs", G_CALLBACK (on_outputs_notify), tab);
    tab->topics_handler = g_signal_connect (
            self, "notify::topics", G_CALLBACK (on_topics_notify), tab);

    g_object_set_data_full (G_OBJECT (outer), "pn-topic-demux-tab",
                            tab, tab_free);

    rebuild_entries (tab);
    return outer;
}

void
pn_topic_demux_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_TOPIC_DEMUX));

    node_class->build_class_tab = pn_topic_demux_build_class_tab;

    /* Class ref held for the process lifetime, like the factory's. */
}
