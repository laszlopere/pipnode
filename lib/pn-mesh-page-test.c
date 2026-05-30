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
/*  Test page.                                                          */
/*                                                                      */
/*  Phase 7's "send a test message" feature.  Lives in its own tab     */
/*  ("Test", tab index 7) at the right end of the dialog notebook.    */
/*  Layout:                                                             */
/*                                                                      */
/*    [Channel ▼] [Message entry ........................] [Send]      */
/*    ┌── Incoming traffic ─────────────────────────────────┐          */
/*    │ HH:MM:SS  → "you sent: hello"               (ch#1)  │          */
/*    │ HH:MM:SS  ← !abc12345 "world"               (ch#1)  │          */
/*    │ ...                                                 │          */
/*    └─────────────────────────────────────────────────────┘          */
/*                                                                      */
/*  The page is a pure renderer + Send action.  It does not own the   */
/*  monitor timer -- the dialog drives that, gated on busy_count.     */
/*  This page provides drain_event() so the dialog can hand it events */
/*  one at a time as they come off the connection's queue.            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-page-test.h"

#include "pn-device-combo.h"

#include <time.h>

#define PN_MESH_TEST_CTX_QDATA "pn-mesh-page-test-ctx"

/* Cap the log buffer at a sane size so a chatty mesh doesn't grow it
 * unboundedly.  Trim from the front when we cross the line cap. */
#define PN_MESH_TEST_MAX_LINES 500

typedef struct
{
    GtkWidget          *page;

    GtkComboBoxText    *channel_combo;
    GtkEntry           *message_entry;
    GtkButton          *send_button;
    GtkTextView        *log_view;
    GtkTextBuffer      *log_buffer;
    GtkScrolledWindow  *log_scroller;
    GtkLabel           *placeholder;

    const PnMeshState  *state;        /* borrowed, not owned        */
    PnMeshConnection   *connection;   /* borrowed, not owned        */

    /* Channel-index lookup parallel to the combo's row order.  -1 in
     * a slot means "no real channel" (e.g. the placeholder row when
     * the device hasn't surfaced its channels yet).  Owned by ctx. */
    GArray             *channel_indices;

    PnMeshPageBusyFunc  busy_cb;
    gpointer            busy_cb_data;

    gboolean            writing;       /* TRUE between async send and finish */
    GCancellable       *write_cancel;  /* cancelled in set_state(NULL/diff) */

    /* Line count of the log buffer.  Cheaper than asking GtkTextBuffer
     * every event; we know we add exactly one line per event. */
    guint               line_count;
} TestPageCtx;

static void test_page_ctx_free (gpointer data);
static void set_writing        (TestPageCtx *ctx, gboolean writing);
static void rebuild_combo      (TestPageCtx *ctx);
static void on_send_clicked    (GtkButton *btn, gpointer user_data);
static void on_entry_changed   (GtkEditable *entry, gpointer user_data);
static void on_send_done       (GObject *source, GAsyncResult *res,
                                gpointer user_data);
static void append_log_line    (TestPageCtx *ctx, const gchar *line);
static gchar *format_event_line (TestPageCtx *ctx, PnMeshTextEvent *event);

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_mesh_page_test_new (void)
{
    TestPageCtx *ctx;
    GtkWidget   *top, *row, *scrolled, *view;
    GtkWidget   *label;
    PangoAttrList *attrs;

    ctx = g_slice_new0 (TestPageCtx);
    ctx->channel_indices = g_array_new (FALSE, TRUE, sizeof (gint));
    ctx->line_count = 0;
    ctx->writing = FALSE;

    top = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top    (top, 8);
    gtk_widget_set_margin_bottom (top, 8);
    gtk_widget_set_margin_start  (top, 8);
    gtk_widget_set_margin_end    (top, 8);
    ctx->page = top;

    /* Row 1: channel picker + message entry + send button. */
    row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);

    label = gtk_label_new ("Channel:");
    gtk_box_pack_start (GTK_BOX (row), label, FALSE, FALSE, 0);

    ctx->channel_combo = GTK_COMBO_BOX_TEXT (pn_device_combo_new ());
    gtk_box_pack_start (GTK_BOX (row),
                        GTK_WIDGET (ctx->channel_combo),
                        FALSE, FALSE, 0);

    ctx->message_entry = GTK_ENTRY (gtk_entry_new ());
    gtk_entry_set_placeholder_text (ctx->message_entry,
                                    "Message to broadcast");
    gtk_entry_set_max_length (ctx->message_entry, 220);
    g_signal_connect (ctx->message_entry, "changed",
                      G_CALLBACK (on_entry_changed), ctx);
    g_signal_connect (ctx->message_entry, "activate",
                      G_CALLBACK (on_send_clicked), ctx);
    gtk_box_pack_start (GTK_BOX (row),
                        GTK_WIDGET (ctx->message_entry),
                        TRUE, TRUE, 0);

    ctx->send_button = GTK_BUTTON (gtk_button_new_with_label ("Send"));
    g_signal_connect (ctx->send_button, "clicked",
                      G_CALLBACK (on_send_clicked), ctx);
    gtk_box_pack_start (GTK_BOX (row),
                        GTK_WIDGET (ctx->send_button),
                        FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (top), row, FALSE, FALSE, 0);

    /* Subtitle. */
    label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (label),
                          "<b>Incoming traffic</b>");
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_set_margin_top (label, 4);
    gtk_box_pack_start (GTK_BOX (top), label, FALSE, FALSE, 0);

    /* Row 2: log view in a scrolled window. */
    ctx->log_buffer = gtk_text_buffer_new (NULL);
    view = gtk_text_view_new_with_buffer (ctx->log_buffer);
    ctx->log_view = GTK_TEXT_VIEW (view);
    gtk_text_view_set_editable     (ctx->log_view, FALSE);
    gtk_text_view_set_cursor_visible (ctx->log_view, FALSE);
    gtk_text_view_set_monospace    (ctx->log_view, TRUE);
    gtk_text_view_set_wrap_mode    (ctx->log_view, GTK_WRAP_WORD_CHAR);

    /* Smaller font for the log so a busy mesh fits visibly. */
    attrs = pango_attr_list_new ();
    pango_attr_list_insert (attrs,
                            pango_attr_scale_new (PANGO_SCALE_SMALL));
    gtk_widget_set_name (view, "pn-mesh-test-log");
    (void) attrs;
    pango_attr_list_unref (attrs);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    ctx->log_scroller = GTK_SCROLLED_WINDOW (scrolled);
    gtk_scrolled_window_set_policy (ctx->log_scroller,
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height (ctx->log_scroller, 200);
    gtk_scrolled_window_set_propagate_natural_height (ctx->log_scroller, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), view);
    gtk_widget_set_size_request (view, -1, 200);
    gtk_box_pack_start (GTK_BOX (top), scrolled, TRUE, TRUE, 0);

    /* Placeholder shown when no device is connected (overlaid via
     * box; we just hide the controls and show this instead). */
    ctx->placeholder = GTK_LABEL (gtk_label_new ("No device connected."));
    gtk_widget_set_no_show_all (GTK_WIDGET (ctx->placeholder), TRUE);
    gtk_box_pack_start (GTK_BOX (top),
                        GTK_WIDGET (ctx->placeholder),
                        FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (top), PN_MESH_TEST_CTX_QDATA,
                            ctx, test_page_ctx_free);

    /* Start in disconnected state. */
    pn_mesh_page_test_set_state (top, NULL, NULL);

    gtk_widget_show_all (top);
    return top;
}

static void
test_page_ctx_free (gpointer data)
{
    TestPageCtx *ctx = data;

    if (ctx->write_cancel != NULL)
    {
        g_cancellable_cancel (ctx->write_cancel);
        g_object_unref (ctx->write_cancel);
    }
    if (ctx->channel_indices != NULL)
        g_array_unref (ctx->channel_indices);
    g_slice_free (TestPageCtx, ctx);
}

static TestPageCtx *
get_ctx (GtkWidget *page)
{
    if (page == NULL)
        return NULL;
    return g_object_get_data (G_OBJECT (page), PN_MESH_TEST_CTX_QDATA);
}

/* ------------------------------------------------------------------ */
/*  State                                                               */
/* ------------------------------------------------------------------ */

void
pn_mesh_page_test_set_state (GtkWidget         *page,
                             const PnMeshState *state,
                             PnMeshConnection  *connection)
{
    TestPageCtx *ctx = get_ctx (page);
    gboolean     connected;

    if (ctx == NULL)
        return;

    /* Switching device or going to NULL: cancel any in-flight send
     * and clear the log so the next session starts clean. */
    if (ctx->write_cancel != NULL)
    {
        g_cancellable_cancel (ctx->write_cancel);
        g_clear_object (&ctx->write_cancel);
    }

    ctx->state      = state;
    ctx->connection = connection;
    connected = (state != NULL && connection != NULL);

    if (!connected)
    {
        gtk_text_buffer_set_text (ctx->log_buffer, "", -1);
        ctx->line_count = 0;
        gtk_entry_set_text (ctx->message_entry, "");
    }

    /* Whole row of controls is sensitive only when connected. */
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->channel_combo), connected);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->message_entry), connected);

    /* Send is gated further by "has text" (on_entry_changed handles
     * it normally; here we just enforce the no-connection case). */
    if (!connected)
        gtk_widget_set_sensitive (GTK_WIDGET (ctx->send_button), FALSE);

    gtk_widget_set_visible (GTK_WIDGET (ctx->placeholder), !connected);

    rebuild_combo (ctx);

    /* If newly connected, re-evaluate the entry-changed gate. */
    if (connected)
        on_entry_changed (GTK_EDITABLE (ctx->message_entry), ctx);
}

/* Populate the channel combo from state->channels.  Skips disabled
 * slots (role 0) except for the primary at index 0 (which always
 * exists conceptually even when disabled).  Stores the channel
 * index in a parallel GArray so on_send_clicked can map row -> idx
 * without re-parsing the combo's label. */
static void
rebuild_combo (TestPageCtx *ctx)
{
    guint i;

    gtk_combo_box_text_remove_all (ctx->channel_combo);
    g_array_set_size (ctx->channel_indices, 0);

    if (ctx->state == NULL || ctx->state->channels == NULL
        || ctx->state->channels->len == 0)
    {
        /* No data: stick in a single inert "(no channels)" row. */
        gtk_combo_box_text_append_text (ctx->channel_combo,
                                        "(no channels)");
        {
            gint sentinel = -1;
            g_array_append_val (ctx->channel_indices, sentinel);
        }
        gtk_combo_box_set_active (GTK_COMBO_BOX (ctx->channel_combo), 0);
        return;
    }

    for (i = 0; i < ctx->state->channels->len; i++)
    {
        const PnMeshChannel *ch =
                g_ptr_array_index (ctx->state->channels, i);
        gchar               *label;
        gint                 idx;

        /* Skip disabled channels above the primary -- they cannot be
         * used to broadcast.  Index 0 is always shown even if
         * "disabled" because the device's default channel name
         * machinery treats it as Primary regardless. */
        if (ch->index > 0 && ch->role == 0)
            continue;

        if (ch->name != NULL && *ch->name != '\0')
            label = g_strdup_printf ("%u: %s", ch->index, ch->name);
        else if (ch->index == 0)
            label = g_strdup ("0: Primary");
        else
            label = g_strdup_printf ("%u: (unnamed)", ch->index);

        gtk_combo_box_text_append_text (ctx->channel_combo, label);
        idx = (gint) ch->index;
        g_array_append_val (ctx->channel_indices, idx);
        g_free (label);
    }

    if (ctx->channel_indices->len > 0)
        gtk_combo_box_set_active (GTK_COMBO_BOX (ctx->channel_combo), 0);
}

/* ------------------------------------------------------------------ */
/*  Sending                                                             */
/* ------------------------------------------------------------------ */

static void
on_entry_changed (GtkEditable *entry, gpointer user_data)
{
    TestPageCtx *ctx = user_data;
    const gchar *text;
    gboolean     have_text, have_conn;

    text      = gtk_entry_get_text (GTK_ENTRY (entry));
    have_text = (text != NULL && *text != '\0');
    have_conn = (ctx->connection != NULL && ctx->state != NULL
                 && !ctx->writing);

    gtk_widget_set_sensitive (GTK_WIDGET (ctx->send_button),
                              have_text && have_conn);
}

static void
on_send_clicked (GtkButton *btn, gpointer user_data)
{
    TestPageCtx *ctx = user_data;
    const gchar *text;
    gchar       *text_copy;
    gint         row;
    gint         channel_index;

    (void) btn;

    if (ctx->writing)
        return;
    if (ctx->connection == NULL || ctx->state == NULL)
        return;

    text = gtk_entry_get_text (ctx->message_entry);
    if (text == NULL || *text == '\0')
        return;

    row = gtk_combo_box_get_active (GTK_COMBO_BOX (ctx->channel_combo));
    if (row < 0 || (guint) row >= ctx->channel_indices->len)
        return;
    channel_index = g_array_index (ctx->channel_indices, gint, row);
    if (channel_index < 0)
        return;

    /* Copy text BEFORE clearing the entry (see channels-page Phase 5
     * gotcha (ii) -- gtk_entry_get_text returns a borrowed pointer
     * into the entry's buffer).  In our case the entry isn't being
     * destroyed, but we set its text to "" right after which mutates
     * the buffer; safer to dup here regardless. */
    text_copy = g_strdup (text);

    set_writing (ctx, TRUE);

    if (ctx->write_cancel != NULL)
        g_clear_object (&ctx->write_cancel);
    ctx->write_cancel = g_cancellable_new ();

    pn_mesh_connection_send_text_async (ctx->connection,
                                        (guint32) channel_index,
                                        text_copy,
                                        ctx->write_cancel,
                                        on_send_done,
                                        ctx);
    g_free (text_copy);
    gtk_entry_set_text (ctx->message_entry, "");
}

static void
on_send_done (GObject *source, GAsyncResult *res, gpointer user_data)
{
    TestPageCtx *ctx = user_data;
    GError      *err = NULL;

    (void) source;

    if (!pn_mesh_connection_send_text_finish (res, &err))
    {
        /* On failure surface the message inline at the top of the
         * log -- the user is staring at the log anyway. */
        if (err != NULL)
        {
            gchar *line = g_strdup_printf ("[send failed: %s]",
                                           err->message);
            append_log_line (ctx, line);
            g_free (line);
            g_clear_error (&err);
        }
    }
    /* The synthetic outgoing PnMeshTextEvent the sync path pushed
     * onto the connection queue will surface via drain_event() on
     * the next dialog tick -- no need to render it here. */

    set_writing (ctx, FALSE);
    /* Refresh the send gate now that writing is FALSE. */
    on_entry_changed (GTK_EDITABLE (ctx->message_entry), ctx);
}

static void
set_writing (TestPageCtx *ctx, gboolean writing)
{
    if (ctx->writing == writing)
        return;
    ctx->writing = writing;
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->send_button), !writing);
    gtk_widget_set_sensitive (GTK_WIDGET (ctx->message_entry), !writing);
    if (ctx->busy_cb != NULL)
        ctx->busy_cb (writing, ctx->busy_cb_data);
}

void
pn_mesh_page_test_set_busy_callback (GtkWidget          *page,
                                     PnMeshPageBusyFunc  callback,
                                     gpointer            user_data)
{
    TestPageCtx *ctx = get_ctx (page);
    if (ctx == NULL)
        return;
    ctx->busy_cb      = callback;
    ctx->busy_cb_data = user_data;
}

/* ------------------------------------------------------------------ */
/*  Receive log                                                         */
/* ------------------------------------------------------------------ */

/* Look up a node by id and return its short_name (borrowed pointer
 * into PnMeshState) or NULL when there is no entry yet. */
static const gchar *
short_name_for (TestPageCtx *ctx, guint32 num)
{
    guint i;

    if (ctx->state == NULL || ctx->state->nodes == NULL)
        return NULL;
    for (i = 0; i < ctx->state->nodes->len; i++)
    {
        const PnMeshNode *n = g_ptr_array_index (ctx->state->nodes, i);
        if (n->num == num)
            return n->short_name;
    }
    return NULL;
}

static gchar *
format_event_line (TestPageCtx *ctx, PnMeshTextEvent *event)
{
    gchar       stamp[16];
    time_t      epoch;
    struct tm   tm_buf;
    const gchar *src;
    gchar       *src_alloc = NULL;
    gchar       *line;

    epoch = (time_t) (event->epoch_us / G_USEC_PER_SEC);
    localtime_r (&epoch, &tm_buf);
    strftime (stamp, sizeof stamp, "%H:%M:%S", &tm_buf);

    if (event->outgoing)
    {
        src = "→ (you)";
    }
    else
    {
        const gchar *known = short_name_for (ctx, event->from_node);
        if (known != NULL && *known != '\0')
            src = known;
        else
        {
            src_alloc = g_strdup_printf ("!%08x", event->from_node);
            src = src_alloc;
        }
    }

    line = g_strdup_printf ("%s  %-12s  ch#%u  %s",
                            stamp, src,
                            (unsigned) event->channel,
                            event->text != NULL ? event->text : "");
    g_free (src_alloc);
    return line;
}

void
pn_mesh_page_test_drain_event (GtkWidget       *page,
                               PnMeshTextEvent *event)
{
    TestPageCtx *ctx = get_ctx (page);
    gchar       *line;

    if (ctx == NULL || event == NULL)
        return;

    line = format_event_line (ctx, event);
    append_log_line (ctx, line);
    g_free (line);
    pn_mesh_text_event_free (event);
}

/* Append @line + "\n" to the log buffer, trim the front when we
 * exceed PN_MESH_TEST_MAX_LINES, and auto-scroll the view to the
 * new bottom. */
static void
append_log_line (TestPageCtx *ctx, const gchar *line)
{
    GtkTextIter end;
    GtkTextMark *bottom;

    if (line == NULL)
        return;

    gtk_text_buffer_get_end_iter (ctx->log_buffer, &end);
    if (ctx->line_count > 0)
        gtk_text_buffer_insert (ctx->log_buffer, &end, "\n", 1);
    gtk_text_buffer_insert (ctx->log_buffer, &end, line, -1);
    ctx->line_count++;

    if (ctx->line_count > PN_MESH_TEST_MAX_LINES)
    {
        /* Drop the first 50 lines in one shot rather than one per
         * insert -- amortises the work and keeps the cap from
         * thrashing. */
        GtkTextIter start, cut;
        guint trim = 50;
        gtk_text_buffer_get_start_iter (ctx->log_buffer, &start);
        cut = start;
        gtk_text_iter_forward_lines (&cut, (gint) trim);
        gtk_text_buffer_delete (ctx->log_buffer, &start, &cut);
        ctx->line_count -= trim;
    }

    /* Auto-scroll to the new bottom. */
    gtk_text_buffer_get_end_iter (ctx->log_buffer, &end);
    bottom = gtk_text_buffer_create_mark (ctx->log_buffer,
                                          NULL, &end, FALSE);
    gtk_text_view_scroll_to_mark (ctx->log_view, bottom,
                                  0.0, TRUE, 0.0, 1.0);
    gtk_text_buffer_delete_mark (ctx->log_buffer, bottom);
}
