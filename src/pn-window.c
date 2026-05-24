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

#include "pn-window.h"
#include "pn-help-browser.h"
#include "pn-palette.h"
#include "pn-panel-editor.h"
#include "pn-preferences.h"
#include "pn-preferences-dialog.h"
#include "pn-document-settings-dialog.h"
#include "pn-node-dialog.h"

#include <json-glib/json-glib.h>

struct _PnWindow
{
    GtkApplicationWindow parent_instance;

    /* Panel-backed mode (set at construction by pn_window_new_panel):
     * the window is built hidden and its flow runs for an XFCE panel
     * applet via the background engine.  Closing it autosaves and hides
     * rather than prompting and destroying, so the worksheet keeps
     * running after the editor window is dismissed. */
    gboolean panel_backed;

    GtkWidget *menubar;
    GtkWidget *toolbar;

    /* Shared headless model.  Owns every node and wire across every
     * sheet in the file; per-sheet PnWorksheet widgets in @notebook
     * each render a filtered view of the same flow.  Listeners on
     * the flow's sheet-added / sheet-removed / sheet-renamed /
     * sheets-reordered signals keep @notebook tabs in sync; the
     * "modified-changed" signal drives the Save UI for the whole
     * file, which is one save and one path regardless of how many
     * sheets it contains. */
    PnFlow      *flow;
    GtkWidget   *notebook;
    GtkWidget   *worksheet;       /* alias for the active page's PnWorksheet */
    GtkWidget   *statusbar;

    /* The single panel-applet GUI layout editor tab, or %NULL when none
     * is open.  Unlike the worksheet tabs this page is not backed by a
     * #PnFlow sheet — it is a standalone #PnPanelEditor the window owns
     * directly — so the sheet-list signals leave it untouched and the
     * pointer is the one-instance guard: the "+" tab menu greys out its
     * "Add Panel Applet Layout" entry while it is non-%NULL, and the
     * tab's close button clears it so a fresh one can be created. */
    GtkWidget   *panel_editor_page;

    GtkWidget *menu_save;
    GtkWidget *menu_undo;
    GtkWidget *menu_redo;
    GtkWidget *menu_cut;
    GtkWidget *menu_copy;
    GtkWidget *menu_paste;
    GtkWidget *menu_debug_view;

    GtkWidget *btn_save;
    GtkWidget *btn_undo;
    GtkWidget *btn_redo;
    GtkWidget *btn_cut;
    GtkWidget *btn_copy;
    GtkWidget *btn_paste;

    /* The collapsible debug pane on the right side of the editor.
     * The scrolled wrapper is what we show/hide; debug_list is a
     * GtkListBox of custom expander rows — each row parses a
     * #PnDebug JSON-formatted message and renders it as a
     * structured envelope plus a collapsible data tree, with the
     * source-node label rendered as a clickable button that focuses
     * the originating node on the worksheet via its `from_id` UUID.
     * Rows whose payload is not parseable JSON (or where the format
     * is text/oneliner rather than JSON) fall back to a plain label
     * showing the raw text so the pane still works as a log view. */
    GtkWidget     *debug_paned;
    GtkWidget     *debug_pane;
    GtkWidget     *debug_list;

    /* Quick-search above the debug list: as the user types we cache the
     * case-folded needle here and refilter the list, hiding rows whose
     * raw message text does not contain the needle and highlighting the
     * matched substrings in the rows that remain.  %NULL when the search
     * box is empty, in which case every row is shown unhighlighted. */
    GtkWidget     *debug_search_entry;
    gchar         *debug_search_casefold;

    /* Index of the currently-focused search match among the rows that
     * match the needle, or -1 when there is no current match (empty
     * search, no hits, or the needle just changed).  The Next/Previous
     * buttons step this index and scroll the corresponding row into
     * view; it is recomputed against the live match list on every step
     * so appended or trimmed rows can never leave it dangling. */
    gint           debug_match_index;

    /* On-screen width (in px) of the debug pane the last time it was
     * visible.  Restored on the next toggle-on and persisted to
     * preferences so it also survives a restart.  <= 0 means "no
     * width recorded yet, use the default slice".
     *
     * We track a width rather than the raw #GtkPaned divider position
     * because the position is measured from the left and so depends on
     * the window width: a divider position captured on a wide window
     * would place the pane off-screen on a narrow one.  A width is
     * window-size independent and is also what the user means by "the
     * size of the debug view".
     *
     * Why we cannot just rely on #GtkPaned remembering its split
     * across hide/show: while pack2 is hidden GtkPaned drifts the
     * divider toward the right edge as the window widens (pack1 has
     * resize=TRUE and absorbs every new pixel), so a user who toggled
     * the pane off, widened the window, and toggled it back on would
     * find pack2 allocated outside the paned's visible area at INT_MIN
     * coordinates — exactly the regression this field exists to
     * prevent. */
    gint           debug_pane_width;

    /* TRUE while restore_debug_pane_position is applying the saved
     * width.  Suppresses the notify::position bookkeeping so the
     * transient natural width the paned hands out before (and during)
     * the restore does not overwrite the width we are about to set. */
    gboolean       debug_pane_restoring;

    /** Path of the worksheet currently associated with the
     *  window, or %NULL if it has never been saved. */
    gchar *current_path;

    /** Live #PnNodeDialog opened by the DBus test surface
     *  (pn_window_open_node_dialog), or %NULL when no test dialog is
     *  open.  Production menu-driven dialogs are not tracked here —
     *  this exists only so a functional test can open a node's
     *  settings dialog, introspect its tabs, drive its editors, and
     *  close it again over D-Bus.  A weak pointer so the field clears
     *  itself if the dialog is destroyed by other means. */
    GtkWidget *test_node_dialog;
};

G_DEFINE_TYPE (PnWindow, pn_window, GTK_TYPE_APPLICATION_WINDOW)

/* Forward declarations. */
static GtkWidget *create_menubar (PnWindow *self);
static GtkWidget *create_toolbar (PnWindow *self);
static gchar     *resolve_help_path (const gchar *filename);
static void       update_window_title (PnWindow *self);
static gboolean   confirm_discard_changes (PnWindow *self);
static void       action_toggle_debug_view (GtkCheckMenuItem *item,
                                            gpointer          user_data);
static GtkWidget *pn_window_append_sheet_tab    (PnWindow    *self,
                                                 const gchar *sheet_name);
static GtkWidget *pn_window_find_sheet_page     (PnWindow    *self,
                                                 const gchar *sheet_name);
static PnWorksheet *pn_window_active_worksheet  (PnWindow    *self);
static void       on_flow_sheet_added           (PnFlow      *flow,
                                                 const gchar *name,
                                                 gpointer     user_data);
static void       on_flow_sheet_removed         (PnFlow      *flow,
                                                 const gchar *name,
                                                 gpointer     user_data);
static void       on_flow_sheet_renamed         (PnFlow      *flow,
                                                 const gchar *from,
                                                 const gchar *to,
                                                 gpointer     user_data);
static void       on_flow_sheets_reordered      (PnFlow      *flow,
                                                 gpointer     user_data);
static void       on_flow_active_sheet_changed  (PnFlow      *flow,
                                                 const gchar *name,
                                                 gpointer     user_data);
static void       on_flow_modified_changed      (PnFlow      *flow,
                                                 gboolean     modified,
                                                 gpointer     user_data);
static void       on_notebook_switch_page       (GtkNotebook *notebook,
                                                 GtkWidget   *page,
                                                 guint        page_num,
                                                 gpointer     user_data);
static void       on_new_sheet_clicked          (GtkButton   *button,
                                                 gpointer     user_data);
static void       on_add_tab_button_clicked     (GtkButton   *button,
                                                 gpointer     user_data);
static void       on_add_panel_editor_activate  (GtkMenuItem *item,
                                                 gpointer     user_data);
static void       pn_window_add_panel_editor_tab (PnWindow   *self);
static void       on_panel_editor_close_clicked (GtkButton   *button,
                                                 gpointer     user_data);
static gboolean   on_tab_label_button_press     (GtkWidget   *event_box,
                                                 GdkEventButton *event,
                                                 gpointer     user_data);

/* ------------------------------------------------------------------ */
/*  Worksheet → status bar forwarder                                   */
/* ------------------------------------------------------------------ */

static void status_set (PnWindow *self, const gchar *text);

static void
on_worksheet_status_message (
        PnWorksheet *worksheet,
        const gchar *text,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) worksheet;

    status_set (self, text ? text : "");
}

/* ------------------------------------------------------------------ */
/*  Worksheet → debug pane forwarder                                   */
/* ------------------------------------------------------------------ */

/* Maximum number of entries kept in the debug list.  When the cap
 * is reached the oldest rows are dropped on each append so the
 * pane stays bounded under sustained traffic. */
#define PN_DEBUG_PANE_MAX_ROWS 500

/* Syntax-highlighting palette for the debug data tree.  Names use one
 * fixed colour; each scalar value type gets its own so the kind of a
 * value is readable at a glance without relying on bold weight. */
#define PN_DBG_COLOR_KEY     "#3465a4"   /* member / index names    */
#define PN_DBG_COLOR_STRING  "#4e9a06"   /* "quoted" string values  */
#define PN_DBG_COLOR_NUMBER  "#ce5c00"   /* integer / double values */
#define PN_DBG_COLOR_BOOL    "#75507b"   /* true / false            */
#define PN_DBG_COLOR_NULL    "#888a85"   /* null + count badges     */

/* Search-match highlight: matched substrings are drawn white on a dark
 * blue background so a hit stands out against the type-coloured text. */
#define PN_DBG_COLOR_MATCH_BG "#204a87"  /* dark blue match background */
#define PN_DBG_COLOR_MATCH_FG "#ffffff"  /* white match text           */

/* One coloured run of text inside a debug label.  A debug label keeps
 * its segments around (as object data) so it can rebuild its Pango
 * markup whenever the search needle changes — wrapping any matched
 * substring in a highlight span without losing the per-type colours. */
typedef struct {
    gchar    *text;        /* display text, unescaped                */
    gchar    *color;       /* foreground colour, or %NULL for default */
    gboolean  small;       /* render at size="small"                 */
    gboolean  italic;      /* render italic                          */
    gboolean  monospace;   /* wrap in <tt>                           */
    gboolean  searchable;  /* eligible for search-match highlighting  */
} PnDbgSeg;

static void
pn_dbg_seg_free (gpointer data)
{
    PnDbgSeg *s = data;
    g_free (s->text);
    g_free (s->color);
    g_free (s);
}

/** Escape @text for Pango markup, wrapping every case-insensitive
 *  occurrence of @needle_cf (already case-folded; may be %NULL/empty)
 *  in a highlight span.  Returns newly allocated markup.  Matching is
 *  exact for ASCII — the common case for debug keys, UUIDs and topics —
 *  and a close approximation for text whose case-folding changes its
 *  character count. */
static gchar *
pn_dbg_highlight_escape (
        const gchar *text,
        const gchar *needle_cf)
{
    GString     *out;
    const gchar *p;
    glong        needle_len;

    if (text == NULL)
        text = "";
    if (needle_cf == NULL || *needle_cf == '\0')
        return g_markup_escape_text (text, -1);

    out        = g_string_new (NULL);
    needle_len = g_utf8_strlen (needle_cf, -1);
    p          = text;

    while (*p != '\0')
    {
        const gchar *end     = p;
        glong        k;
        gchar       *run_cf;
        gboolean     matched = FALSE;

        /* Case-fold the run of needle_len characters starting at @p and
         * compare it to the folded needle. */
        for (k = 0; k < needle_len && *end != '\0'; k++)
            end = g_utf8_next_char (end);

        run_cf = g_utf8_casefold (p, end - p);
        if (g_strcmp0 (run_cf, needle_cf) == 0)
        {
            gchar *esc = g_markup_escape_text (p, end - p);
            g_string_append_printf (
                    out,
                    "<span background=\"%s\" foreground=\"%s\">%s</span>",
                    PN_DBG_COLOR_MATCH_BG, PN_DBG_COLOR_MATCH_FG, esc);
            g_free (esc);
            p       = end;
            matched = TRUE;
        }
        g_free (run_cf);

        if (!matched)
        {
            const gchar *next = g_utf8_next_char (p);
            gchar       *esc  = g_markup_escape_text (p, next - p);
            g_string_append (out, esc);
            g_free (esc);
            p = next;
        }
    }

    return g_string_free (out, FALSE);
}

/** Create an empty debug label: left-aligned, selectable, wrapping, and
 *  carrying an (initially empty) segment list.  Append runs with
 *  pn_dbg_label_add_seg, then call pn_dbg_label_render. */
static GtkWidget *
pn_dbg_label_new (void)
{
    GtkWidget *lbl  = gtk_label_new (NULL);
    GPtrArray *segs = g_ptr_array_new_with_free_func (pn_dbg_seg_free);

    gtk_label_set_xalign     (GTK_LABEL (lbl), 0.0);
    gtk_label_set_selectable (GTK_LABEL (lbl), TRUE);
    gtk_label_set_line_wrap  (GTK_LABEL (lbl), TRUE);
    g_object_set_data_full (G_OBJECT (lbl), "pn-segs", segs,
                            (GDestroyNotify) g_ptr_array_unref);
    return lbl;
}

static void
pn_dbg_label_add_seg (
        GtkWidget   *label,
        const gchar *text,
        const gchar *color,
        gboolean     small,
        gboolean     italic,
        gboolean     monospace,
        gboolean     searchable)
{
    GPtrArray *segs = g_object_get_data (G_OBJECT (label), "pn-segs");
    PnDbgSeg  *s    = g_new0 (PnDbgSeg, 1);

    s->text       = g_strdup (text != NULL ? text : "");
    s->color      = g_strdup (color);   /* g_strdup(NULL) -> NULL */
    s->small      = small;
    s->italic     = italic;
    s->monospace  = monospace;
    s->searchable = searchable;
    g_ptr_array_add (segs, s);
}

/** Rebuild @label's markup from its segments, highlighting @needle_cf
 *  (case-folded; %NULL/empty disables highlighting). */
static void
pn_dbg_label_render (
        GtkWidget   *label,
        const gchar *needle_cf)
{
    GPtrArray *segs = g_object_get_data (G_OBJECT (label), "pn-segs");
    GString   *out;
    guint      i;

    if (segs == NULL)
        return;

    out = g_string_new (NULL);
    for (i = 0; i < segs->len; i++)
    {
        PnDbgSeg *s     = g_ptr_array_index (segs, i);
        gchar    *inner = pn_dbg_highlight_escape (
                s->text, s->searchable ? needle_cf : NULL);

        if (s->monospace)
        {
            gchar *tmp = g_strdup_printf ("<tt>%s</tt>", inner);
            g_free (inner);
            inner = tmp;
        }

        if (s->color != NULL || s->small || s->italic)
        {
            GString *attrs = g_string_new (NULL);
            if (s->color != NULL)
                g_string_append_printf (attrs, " foreground=\"%s\"",
                                        s->color);
            if (s->small)
                g_string_append (attrs, " size=\"small\"");
            if (s->italic)
                g_string_append (attrs, " style=\"italic\"");
            g_string_append_printf (out, "<span%s>%s</span>",
                                    attrs->str, inner);
            g_string_free (attrs, TRUE);
        }
        else
        {
            g_string_append (out, inner);
        }
        g_free (inner);
    }

    gtk_label_set_markup (GTK_LABEL (label), out->str);
    g_string_free (out, TRUE);
}

/** Pick the highlight colour for a scalar (or null) JSON node based on
 *  its value type.  Returns a static string; never freed. */
static const gchar *
json_scalar_color (JsonNode *node)
{
    if (node == NULL || JSON_NODE_HOLDS_NULL (node))
        return PN_DBG_COLOR_NULL;

    if (JSON_NODE_HOLDS_VALUE (node))
    {
        GType vt = json_node_get_value_type (node);

        if (vt == G_TYPE_STRING)  return PN_DBG_COLOR_STRING;
        if (vt == G_TYPE_BOOLEAN) return PN_DBG_COLOR_BOOL;
        if (vt == G_TYPE_INT64 || vt == G_TYPE_DOUBLE)
            return PN_DBG_COLOR_NUMBER;
    }

    return PN_DBG_COLOR_NULL;
}

/** Render a scalar (or null) JSON node as a short display string.
 *  Strings come back quoted; numbers, booleans and null use their
 *  JSON spelling.  Returns a newly allocated string. */
static gchar *
json_scalar_to_display (JsonNode *node)
{
    if (node == NULL || JSON_NODE_HOLDS_NULL (node))
        return g_strdup ("null");

    if (JSON_NODE_HOLDS_VALUE (node))
    {
        GType vt = json_node_get_value_type (node);

        if (vt == G_TYPE_STRING)
            return g_strdup_printf ("\"%s\"",
                                    json_node_get_string (node));
        if (vt == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (node)
                                 ? "true" : "false");
        if (vt == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT,
                                    json_node_get_int (node));
        if (vt == G_TYPE_DOUBLE)
            return g_strdup_printf ("%g", json_node_get_double (node));
    }

    /* Anything unexpected: fall back to its JSON serialisation. */
    {
        gchar *s = json_to_string (node, FALSE);
        return s != NULL ? s : g_strdup ("");
    }
}

/** Recursively build a foldable widget tree for one JSON @node,
 *  labelled @key (which may be %NULL/empty for an unlabelled root).
 *
 *  Objects and arrays become #GtkExpander rows whose child is an
 *  indented column of their members / elements; scalars become a
 *  single selectable "key: value" label.  Nesting the expanders is
 *  what turns a previously flat multi-line JSON dump into individually
 *  collapsible items. */
static GtkWidget *
build_json_tree_widget (
        const gchar *key,
        JsonNode    *node)
{
    if (node != NULL && JSON_NODE_HOLDS_OBJECT (node))
    {
        JsonObject *obj      = json_node_get_object (node);
        GList      *members  = json_object_get_members (obj);
        GList      *l;
        GtkWidget  *expander;
        GtkWidget  *title;
        GtkWidget  *box;

        title = pn_dbg_label_new ();
        gtk_label_set_line_wrap (GTK_LABEL (title), FALSE);
        pn_dbg_label_add_seg (title, key != NULL ? key : "",
                              PN_DBG_COLOR_KEY, FALSE, FALSE, FALSE, TRUE);
        pn_dbg_label_add_seg (title, "  ", NULL,
                              FALSE, FALSE, FALSE, FALSE);
        pn_dbg_label_add_seg (title, "object", PN_DBG_COLOR_NULL,
                              FALSE, TRUE, FALSE, FALSE);
        pn_dbg_label_render (title, NULL);

        expander = gtk_expander_new (NULL);
        gtk_expander_set_label_widget (GTK_EXPANDER (expander), title);
        gtk_expander_set_expanded     (GTK_EXPANDER (expander), TRUE);
        g_object_set_data (G_OBJECT (expander), "pn-default-expanded",
                           GINT_TO_POINTER (TRUE));

        box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start (box, 16);
        for (l = members; l != NULL; l = l->next)
        {
            const gchar *m     = l->data;
            GtkWidget   *child = build_json_tree_widget (
                    m, json_object_get_member (obj, m));
            gtk_box_pack_start (GTK_BOX (box), child, FALSE, FALSE, 0);
        }
        g_list_free (members);

        gtk_container_add (GTK_CONTAINER (expander), box);
        return expander;
    }

    if (node != NULL && JSON_NODE_HOLDS_ARRAY (node))
    {
        JsonArray *arr = json_node_get_array (node);
        guint      n   = json_array_get_length (arr);
        guint      i;
        GtkWidget *expander;
        GtkWidget *title;
        GtkWidget *box;

        title = pn_dbg_label_new ();
        gtk_label_set_line_wrap (GTK_LABEL (title), FALSE);
        pn_dbg_label_add_seg (title, key != NULL ? key : "",
                              PN_DBG_COLOR_KEY, FALSE, FALSE, FALSE, TRUE);
        pn_dbg_label_add_seg (title, "  ", NULL,
                              FALSE, FALSE, FALSE, FALSE);
        pn_dbg_label_add_seg (title, "array", PN_DBG_COLOR_NULL,
                              FALSE, TRUE, FALSE, FALSE);
        pn_dbg_label_render (title, NULL);

        expander = gtk_expander_new (NULL);
        gtk_expander_set_label_widget (GTK_EXPANDER (expander), title);
        gtk_expander_set_expanded     (GTK_EXPANDER (expander), TRUE);
        g_object_set_data (G_OBJECT (expander), "pn-default-expanded",
                           GINT_TO_POINTER (TRUE));

        box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start (box, 16);
        for (i = 0; i < n; i++)
        {
            gchar     *idx   = g_strdup_printf ("[%u]", i);
            GtkWidget *child = build_json_tree_widget (
                    idx, json_array_get_element (arr, i));
            gtk_box_pack_start (GTK_BOX (box), child, FALSE, FALSE, 0);
            g_free (idx);
        }

        gtk_container_add (GTK_CONTAINER (expander), box);
        return expander;
    }

    /* Scalar / null leaf: a single "key: value" line.  The name is
     * drawn in the fixed key colour and the value in a colour chosen
     * by its type, so structure reads through colour rather than weight. */
    {
        GtkWidget   *lbl    = pn_dbg_label_new ();
        gchar       *val    = json_scalar_to_display (node);
        const gchar *vcolor = json_scalar_color (node);

        if (key != NULL && *key != '\0')
        {
            pn_dbg_label_add_seg (lbl, key, PN_DBG_COLOR_KEY,
                                  FALSE, FALSE, FALSE, TRUE);
            pn_dbg_label_add_seg (lbl, ": ", NULL,
                                  FALSE, FALSE, FALSE, FALSE);
        }
        pn_dbg_label_add_seg (lbl, val, vcolor,
                              FALSE, FALSE, TRUE, TRUE);
        pn_dbg_label_render (lbl, NULL);

        g_free (val);
        return lbl;
    }
}

/** Click handler attached to a row's `from`-button.  The button
 *  carries the source node's UUID as object data; this callback
 *  forwards it to the worksheet so the originating node is scrolled
 *  into view and pulses briefly. */
static void
on_debug_from_clicked (
        GtkButton *button,
        gpointer   user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    const gchar *uuid;
    PnNodeStore *store;
    guint        i, n;

    uuid = g_object_get_data (G_OBJECT (button), "pn-from-id");
    if (uuid == NULL || *uuid == '\0' || self->flow == NULL)
        return;

    /* Walk the shared flow to find the node carrying @uuid; the node
     * may live on a different sheet from the one currently shown, so
     * switch to its sheet first before asking the matching worksheet
     * to scroll it into view and pulse the focus highlight. */
    store = pn_flow_get_nodes (self->flow);
    n = pn_node_store_get_length (store);
    for (i = 0; i < n; i++)
    {
        PnNode      *node = pn_node_store_get_node (store, i);
        const gchar *u    = pn_node_get_uuid (node);
        if (u == NULL || g_strcmp0 (u, uuid) != 0)
            continue;

        pn_flow_set_active_sheet (self->flow,
                                  pn_node_get_worksheet (node));
        {
            PnWorksheet *ws = pn_window_active_worksheet (self);
            if (ws != NULL)
                pn_worksheet_focus_node_by_uuid (ws, uuid);
        }
        return;
    }
}

/** Build one "name: value" line for an envelope metadata field, in the
 *  same colour scheme as the data tree: the name in the key colour and
 *  the value (always a string here) in the string colour.  The value is
 *  shown quoted, matching how the data tree renders string scalars. */
static GtkWidget *
build_envelope_field (
        const gchar *key,
        const gchar *value)
{
    GtkWidget *lbl    = pn_dbg_label_new ();
    gchar     *quoted = g_strdup_printf ("\"%s\"",
                                         value != NULL ? value : "");

    pn_dbg_label_add_seg (lbl, key, PN_DBG_COLOR_KEY,
                          FALSE, FALSE, FALSE, TRUE);
    pn_dbg_label_add_seg (lbl, ": ", NULL,
                          FALSE, FALSE, FALSE, FALSE);
    pn_dbg_label_add_seg (lbl, quoted, PN_DBG_COLOR_STRING,
                          FALSE, FALSE, TRUE, TRUE);
    pn_dbg_label_render (lbl, NULL);

    g_free (quoted);
    return lbl;
}

/** Build a structured row widget for a JSON-shaped debug message.
 *
 *  The whole message folds under an opening line that names its
 *  concrete class:
 *
 *      ▸ Message PnMessage          (italic, grey — the fold handle)
 *        ├── [from-button]          (jumps to the source node)
 *        ├── from_id / topic / id / created   (envelope fields)
 *        └── data {N}                 (foldable; the message payload)
 *
 *  The `from`-button lives inside the expander body (not its label
 *  widget), so its click always reaches the button's "clicked" handler
 *  instead of being absorbed by the expander's title-area press
 *  handler. */
static GtkWidget *
build_envelope_row (
        PnWindow    *self,
        JsonObject  *envelope)
{
    GtkWidget   *expander;
    GtkWidget   *heading;
    GtkWidget   *body;
    GtkWidget   *from_btn;
    GtkWidget   *from_lbl;
    const gchar *type;
    const gchar *from;
    const gchar *from_id;
    const gchar *topic;
    const gchar *id;
    const gchar *created;
    gchar       *msg;

    type    = json_object_has_member (envelope, "type")
                ? json_object_get_string_member (envelope, "type")    : "";
    from    = json_object_has_member (envelope, "from")
                ? json_object_get_string_member (envelope, "from")    : "";
    from_id = json_object_has_member (envelope, "from_id")
                ? json_object_get_string_member (envelope, "from_id") : "";
    topic   = json_object_has_member (envelope, "topic")
                ? json_object_get_string_member (envelope, "topic")   : "";
    id      = json_object_has_member (envelope, "id")
                ? json_object_get_string_member (envelope, "id")      : "";
    created = json_object_has_member (envelope, "created")
                ? json_object_get_string_member (envelope, "created") : "";

    /* Opening line names the message's class and doubles as the fold
     * handle for the whole entry.  Italic + grey so it reads as a quiet
     * heading rather than as data. */
    heading = pn_dbg_label_new ();
    gtk_label_set_line_wrap (GTK_LABEL (heading), FALSE);
    msg = g_strdup_printf ("Message %s",
            (type != NULL && *type != '\0') ? type : "PnMessage");
    pn_dbg_label_add_seg (heading, msg, PN_DBG_COLOR_NULL,
                          FALSE, TRUE, FALSE, TRUE);
    pn_dbg_label_render (heading, NULL);
    g_free (msg);

    expander = gtk_expander_new (NULL);
    gtk_expander_set_label_widget (GTK_EXPANDER (expander), heading);
    gtk_expander_set_expanded     (GTK_EXPANDER (expander), FALSE);
    g_object_set_data (G_OBJECT (expander), "pn-default-expanded",
                       GINT_TO_POINTER (FALSE));

    body = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start (body, 12);

    /* From button: its label is the message's `from`, and clicking it
     * forwards `from_id` to the worksheet focus API.  Disabled
     * (insensitive) when the producer left from_id empty so the user
     * gets clear visual feedback that the shortcut is unavailable.  The
     * label is a segment-backed debug label so the `from` name takes
     * part in search highlighting like every other field. */
    from_lbl = pn_dbg_label_new ();
    gtk_label_set_selectable (GTK_LABEL (from_lbl), FALSE);
    pn_dbg_label_add_seg (from_lbl,
                          from != NULL && *from != '\0' ? from : "(none)",
                          NULL, FALSE, FALSE, FALSE, TRUE);
    pn_dbg_label_render (from_lbl, NULL);
    from_btn = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (from_btn), from_lbl);
    gtk_button_set_relief (GTK_BUTTON (from_btn), GTK_RELIEF_NONE);
    gtk_widget_set_halign (from_btn, GTK_ALIGN_START);
    if (from_id != NULL && *from_id != '\0')
    {
        g_object_set_data_full (G_OBJECT (from_btn), "pn-from-id",
                                g_strdup (from_id), g_free);
        g_signal_connect (from_btn, "clicked",
                          G_CALLBACK (on_debug_from_clicked), self);
        gtk_widget_set_tooltip_text (
                from_btn,
                "Click to focus this node on the worksheet");
    }
    else
    {
        gtk_widget_set_sensitive (from_btn, FALSE);
        gtk_widget_set_tooltip_text (
                from_btn,
                "Source node has no UUID (older worksheet?)");
    }
    gtk_box_pack_start (GTK_BOX (body), from_btn, FALSE, FALSE, 0);

    /* Show the rest of the envelope in full.  `from` is already carried
     * by the button label above, so it is not repeated here. */
    gtk_box_pack_start (GTK_BOX (body),
                        build_envelope_field ("from_id", from_id),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (body),
                        build_envelope_field ("topic", topic),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (body),
                        build_envelope_field ("id", id),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (body),
                        build_envelope_field ("created", created),
                        FALSE, FALSE, 0);

    /* Data bag as its own "data" foldable, matching the look of every
     * nested object inside it — collapse the whole payload with one
     * click, or drill into it.  When the envelope carries no data
     * object we still render an (empty) "data {0}" node so the foldable
     * is always present. */
    {
        JsonNode *data_node = json_object_has_member (envelope, "data")
                ? json_object_get_member (envelope, "data") : NULL;
        JsonNode *placeholder = NULL;

        if (data_node == NULL || !JSON_NODE_HOLDS_OBJECT (data_node))
        {
            placeholder = json_node_new (JSON_NODE_OBJECT);
            json_node_take_object (placeholder, json_object_new ());
            data_node = placeholder;
        }

        gtk_box_pack_start (GTK_BOX (body),
                            build_json_tree_widget ("data", data_node),
                            FALSE, FALSE, 0);

        /* build_json_tree_widget only reads the node, so the synthesised
         * empty placeholder can be freed immediately. */
        if (placeholder != NULL)
            json_node_free (placeholder);
    }

    gtk_container_add (GTK_CONTAINER (expander), body);
    return expander;
}

/** TRUE if any searchable segment of @label contains @needle_cf
 *  (case-folded).  Mirrors the matching done by pn_dbg_highlight_escape
 *  so "shows a highlight" and "counts as a match" stay in lockstep. */
static gboolean
debug_label_has_match (
        GtkWidget   *label,
        const gchar *needle_cf)
{
    GPtrArray *segs = g_object_get_data (G_OBJECT (label), "pn-segs");
    guint      i;

    if (segs == NULL || needle_cf == NULL || *needle_cf == '\0')
        return FALSE;

    for (i = 0; i < segs->len; i++)
    {
        PnDbgSeg *s = g_ptr_array_index (segs, i);
        gchar    *fold;
        gboolean  hit;

        if (!s->searchable)
            continue;
        fold = g_utf8_casefold (s->text, -1);
        hit  = (g_strstr_len (fold, -1, needle_cf) != NULL);
        g_free (fold);
        if (hit)
            return TRUE;
    }
    return FALSE;
}

/** Re-render every segment-backed label under @widget for the current
 *  @needle_cf and reconcile each expander's fold state with the search:
 *  an expander whose subtree contains a match is opened so the highlight
 *  is visible, while one with no match (including when the needle is
 *  cleared) falls back to the default state captured when it was built.
 *  Returns TRUE if @widget's subtree contains at least one match.
 *
 *  Expander label widgets are internal children, so the title and body
 *  are descended explicitly rather than via gtk_container_get_children. */
static gboolean
debug_highlight_apply (
        GtkWidget   *widget,
        const gchar *needle_cf)
{
    gboolean match = FALSE;

    if (g_object_get_data (G_OBJECT (widget), "pn-segs") != NULL)
    {
        pn_dbg_label_render (widget, needle_cf);
        if (debug_label_has_match (widget, needle_cf))
            match = TRUE;
    }

    if (GTK_IS_EXPANDER (widget))
    {
        GtkWidget *title = gtk_expander_get_label_widget (
                GTK_EXPANDER (widget));
        GtkWidget *body  = gtk_bin_get_child (GTK_BIN (widget));
        gboolean   def;

        if (title != NULL && debug_highlight_apply (title, needle_cf))
            match = TRUE;
        if (body != NULL && debug_highlight_apply (body, needle_cf))
            match = TRUE;

        def = GPOINTER_TO_INT (g_object_get_data (
                G_OBJECT (widget), "pn-default-expanded"));
        gtk_expander_set_expanded (GTK_EXPANDER (widget),
                                   match ? TRUE : def);
    }
    else if (GTK_IS_CONTAINER (widget))
    {
        GList *kids = gtk_container_get_children (GTK_CONTAINER (widget));
        GList *l;

        for (l = kids; l != NULL; l = l->next)
            if (debug_highlight_apply (GTK_WIDGET (l->data), needle_cf))
                match = TRUE;
        g_list_free (kids);
    }

    return match;
}

/** #GtkListBoxFilterFunc for the debug list.  A row is shown when its
 *  raw message text contains the current (case-folded) search needle;
 *  the matching rows have their matched substrings highlighted, while an
 *  empty needle shows every row with highlighting cleared. */
static gboolean
debug_row_filter (
        GtkListBoxRow *row,
        gpointer       user_data)
{
    PnWindow    *self   = PN_WINDOW (user_data);
    GtkWidget   *child  = gtk_bin_get_child (GTK_BIN (row));
    const gchar *needle = self->debug_search_casefold;
    const gchar *raw;
    gchar       *hay;
    gboolean     match;

    if (child == NULL)
        return TRUE;

    if (needle == NULL || *needle == '\0')
    {
        debug_highlight_apply (child, NULL);
        return TRUE;
    }

    raw = g_object_get_data (G_OBJECT (child), "pn-search-text");
    if (raw == NULL)
        return FALSE;

    hay   = g_utf8_casefold (raw, -1);
    match = (g_strstr_len (hay, -1, needle) != NULL);
    g_free (hay);

    if (match)
        debug_highlight_apply (child, needle);
    return match;
}

/** Drop the "debug-current-match" style class from every row, clearing
 *  any focused-match highlight left by the Next/Previous buttons. */
static void
debug_clear_current_match (PnWindow *self)
{
    GList *kids;
    GList *l;

    if (self->debug_list == NULL)
        return;

    kids = gtk_container_get_children (GTK_CONTAINER (self->debug_list));
    for (l = kids; l != NULL; l = l->next)
        gtk_style_context_remove_class (
                gtk_widget_get_style_context (GTK_WIDGET (l->data)),
                "debug-current-match");
    g_list_free (kids);
}

/** Refresh the cached case-folded needle from the search entry and
 *  re-evaluate the list filter on every keystroke. */
static void
on_debug_search_changed (
        GtkEditable *editable,
        gpointer     user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (editable));

    g_clear_pointer (&self->debug_search_casefold, g_free);
    if (text != NULL && *text != '\0')
        self->debug_search_casefold = g_utf8_casefold (text, -1);

    /* The match set just changed, so any focused match is stale. */
    self->debug_match_index = -1;
    if (self->debug_list != NULL)
    {
        debug_clear_current_match (self);
        gtk_list_box_invalidate_filter (GTK_LIST_BOX (self->debug_list));
    }
}

/** Collect, in display order, the list-box rows that match the current
 *  needle.  The returned list holds #GtkListBoxRow widgets and must be
 *  freed with g_list_free (the rows themselves are owned by the list). */
static GList *
debug_matching_rows (PnWindow *self)
{
    const gchar *needle = self->debug_search_casefold;
    GList       *kids;
    GList       *l;
    GList       *out = NULL;

    if (self->debug_list == NULL || needle == NULL || *needle == '\0')
        return NULL;

    kids = gtk_container_get_children (GTK_CONTAINER (self->debug_list));
    for (l = kids; l != NULL; l = l->next)
    {
        GtkWidget   *row   = GTK_WIDGET (l->data);
        GtkWidget   *child = gtk_bin_get_child (GTK_BIN (row));
        const gchar *raw;
        gchar       *hay;
        gboolean     match;

        if (child == NULL)
            continue;
        raw = g_object_get_data (G_OBJECT (child), "pn-search-text");
        if (raw == NULL)
            continue;
        hay   = g_utf8_casefold (raw, -1);
        match = (g_strstr_len (hay, -1, needle) != NULL);
        g_free (hay);
        if (match)
            out = g_list_prepend (out, row);
    }
    g_list_free (kids);
    return g_list_reverse (out);
}

/** Scroll the debug list so @row is visible, nudging the viewport the
 *  minimum amount needed.  @row's allocation is relative to the list
 *  box, which is the scrolled window's scrollable child, so its y maps
 *  straight onto the vertical adjustment. */
static void
debug_scroll_row_into_view (
        PnWindow  *self,
        GtkWidget *row)
{
    GtkWidget     *anc;
    GtkAdjustment *vadj;
    GtkAllocation  alloc;

    anc = gtk_widget_get_ancestor (self->debug_list, GTK_TYPE_SCROLLED_WINDOW);
    if (anc == NULL)
        return;
    vadj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (anc));
    if (vadj == NULL)
        return;

    gtk_widget_get_allocation (row, &alloc);
    gtk_adjustment_clamp_page (vadj, alloc.y, alloc.y + alloc.height);
}

/** Step the focused search match by @dir (+1 next, -1 previous),
 *  wrapping at the ends, then select and scroll the matching row into
 *  view.  Does nothing when there are no matches. */
static void
debug_search_step (
        PnWindow *self,
        gint      dir)
{
    GList     *matches = debug_matching_rows (self);
    gint       n       = g_list_length (matches);
    GtkWidget *row;

    if (n == 0)
    {
        self->debug_match_index = -1;
        g_list_free (matches);
        return;
    }

    if (self->debug_match_index < 0)
        self->debug_match_index = (dir > 0) ? 0 : n - 1;
    else
        self->debug_match_index =
                ((self->debug_match_index + dir) % n + n) % n;

    row = GTK_WIDGET (g_list_nth_data (matches, self->debug_match_index));
    if (row != NULL)
    {
        debug_clear_current_match (self);
        gtk_style_context_add_class (
                gtk_widget_get_style_context (row), "debug-current-match");
        debug_scroll_row_into_view (self, row);
    }
    g_list_free (matches);
}

static void
on_debug_search_next (
        GtkWidget *widget,
        gpointer   user_data)
{
    (void) widget;
    debug_search_step (PN_WINDOW (user_data), +1);
}

static void
on_debug_search_prev (
        GtkWidget *widget,
        gpointer   user_data)
{
    (void) widget;
    debug_search_step (PN_WINDOW (user_data), -1);
}

/** Append @text to the debug list as one row.  The text is parsed
 *  as JSON when possible — that gives us the structured envelope row
 *  with a clickable `from` shortcut.  Anything that doesn't parse as
 *  a JSON object falls back to a plain label so the pane still
 *  works for `text` / `oneliner` Debug Print formats and for log
 *  replay scenarios. */
static void
debug_pane_append (
        PnWindow    *self,
        const gchar *text)
{
    GtkWidget   *row_widget = NULL;
    JsonParser  *parser     = NULL;
    JsonNode    *root;

    if (self->debug_list == NULL)
        return;

    if (text == NULL)
        text = "";

    parser = json_parser_new ();
    if (json_parser_load_from_data (parser, text, -1, NULL))
    {
        root = json_parser_get_root (parser);
        if (root != NULL && JSON_NODE_HOLDS_OBJECT (root))
        {
            JsonObject *obj = json_node_get_object (root);
            row_widget = build_envelope_row (self, obj);
        }
    }
    g_object_unref (parser);

    if (row_widget == NULL)
    {
        /* Non-JSON fallback: surface the raw payload as a single
         * label so plain-text formats and log-replay paste still
         * land in the pane somewhere readable. */
        GtkWidget *lbl = pn_dbg_label_new ();
        pn_dbg_label_add_seg (lbl, text, NULL, FALSE, FALSE, FALSE, TRUE);
        pn_dbg_label_render (lbl, NULL);
        row_widget = lbl;
    }

    /* Stash the raw text on the row so the search filter can match
     * against the whole message and highlight only the matching rows. */
    g_object_set_data_full (G_OBJECT (row_widget), "pn-search-text",
                            g_strdup (text), g_free);

    gtk_widget_show_all (row_widget);
    gtk_list_box_insert (GTK_LIST_BOX (self->debug_list), row_widget, -1);

    /* Trim the oldest entries once the cap is exceeded so a noisy
     * flow doesn't grow the pane unboundedly. */
    {
        GList *children = gtk_container_get_children (
                GTK_CONTAINER (self->debug_list));
        guint n = g_list_length (children);

        while (n > PN_DEBUG_PANE_MAX_ROWS && children != NULL)
        {
            gtk_widget_destroy (GTK_WIDGET (children->data));
            children = g_list_delete_link (children, children);
            n--;
        }
        g_list_free (children);
    }

    /* Auto-scroll the parent scrolled window to keep the freshly-
     * appended row visible.  GtkListBox doesn't have a native scroll-
     * to-row helper, so we nudge the vertical adjustment to its
     * upper bound and let the size machinery clamp. */
    {
        GtkWidget *anc = gtk_widget_get_ancestor (
                self->debug_list, GTK_TYPE_SCROLLED_WINDOW);
        if (anc != NULL)
        {
            GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment (
                    GTK_SCROLLED_WINDOW (anc));
            if (vadj != NULL)
                gtk_adjustment_set_value (
                        vadj,
                        gtk_adjustment_get_upper (vadj) -
                        gtk_adjustment_get_page_size (vadj));
        }
    }
}

static void
on_worksheet_debug_message (
        PnWorksheet *worksheet,
        const gchar *text,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) worksheet;

    debug_pane_append (self, text);
}

/** Toggle Cut/Copy sensitivity to match the worksheet's selection
 *  state.  Paste stays always-on; clicking it with an empty/foreign
 *  clipboard simply no-ops. */
static void
on_worksheet_selection_changed (
        PnWorksheet *worksheet,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    gboolean  has  = pn_worksheet_has_selection (worksheet);

    gtk_widget_set_sensitive (self->menu_cut,  has);
    gtk_widget_set_sensitive (self->menu_copy, has);
    gtk_widget_set_sensitive (self->btn_cut,   has);
    gtk_widget_set_sensitive (self->btn_copy,  has);
}

/* on_worksheet_modified_changed has moved up to the flow level (see
 * on_flow_modified_changed below) — the modified flag is per-file,
 * not per-sheet, so listening on the flow once is enough. */

/* ------------------------------------------------------------------ */
/*  Status bar helper                                                  */
/* ------------------------------------------------------------------ */

static void
status_set (
        PnWindow    *self,
        const gchar *text)
{
    if (!self->statusbar)
        return;

    gtk_statusbar_pop  (GTK_STATUSBAR (self->statusbar), 0);
    gtk_statusbar_push (GTK_STATUSBAR (self->statusbar), 0, text);
}

/* ------------------------------------------------------------------ */
/*  Action handlers                                                    */
/* ------------------------------------------------------------------ */

static void
action_new (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) item;

    if (!confirm_discard_changes (self))
        return;

    /* Clearing the flow cascades: every PnWorksheet observes the
     * resulting node-removed events, the flow's sheet-removed signal
     * pulls every non-default tab out of the notebook, and the
     * sheet-list resets to a single "Worksheet" sheet. */
    pn_flow_clear (self->flow);
    g_clear_pointer (&self->current_path, g_free);

    /* The panel editor tab is not a flow sheet, so the clear above does
     * not touch it — drop it explicitly so "New" yields a truly empty
     * document. */
    if (self->panel_editor_page != NULL)
    {
        gint idx = gtk_notebook_page_num (GTK_NOTEBOOK (self->notebook),
                                          self->panel_editor_page);
        if (idx >= 0)
            gtk_notebook_remove_page (GTK_NOTEBOOK (self->notebook), idx);
        self->panel_editor_page = NULL;
    }

    update_window_title (self);
    status_set (self, "New worksheet");
}

static void
action_open (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    GtkWidget *dialog;
    GtkFileFilter *filter;
    (void) item;

    if (!confirm_discard_changes (self))
        return;

    dialog = gtk_file_chooser_dialog_new (
            "Open Worksheet",
            GTK_WINDOW (self),
            GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Open",   GTK_RESPONSE_ACCEPT,
            NULL);

    filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, "Pipnode worksheets (*.json)");
    gtk_file_filter_add_pattern (filter, "*.json");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
        gchar *path;
        GError *error = NULL;

        path = gtk_file_chooser_get_filename (
                GTK_FILE_CHOOSER (dialog));

        if (!pn_flow_load_from_file (self->flow, path, &error))
        {
            GtkWidget *err = gtk_message_dialog_new (
                    GTK_WINDOW (self),
                    GTK_DIALOG_MODAL,
                    GTK_MESSAGE_ERROR,
                    GTK_BUTTONS_CLOSE,
                    "Could not open worksheet:\n%s",
                    error->message);
            gtk_dialog_run (GTK_DIALOG (err));
            gtk_widget_destroy (err);
            g_error_free (error);
        }
        else
        {
            g_free (self->current_path);
            self->current_path = path;
            path = NULL;

            update_window_title (self);
            status_set (self, "Opened worksheet");
        }

        g_free (path);
    }

    gtk_widget_destroy (dialog);
}

static gboolean
save_to_path (
        PnWindow    *self,
        const gchar *path)
{
    GError *error = NULL;

    if (!pn_flow_save_to_file (self->flow, path, &error))
    {
        GtkWidget *err = gtk_message_dialog_new (
                GTK_WINDOW (self),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE,
                "Could not save worksheet:\n%s",
                error->message);
        gtk_dialog_run (GTK_DIALOG (err));
        gtk_widget_destroy (err);
        g_error_free (error);
        return FALSE;
    }

    status_set (self, "Saved worksheet");
    return TRUE;
}

/* Run the Save As chooser, persist the worksheet to the chosen path and
 * remember it.  Returns %TRUE only when the worksheet was actually
 * written; a cancelled chooser or a failed write yields %FALSE so
 * callers (such as the close-confirmation flow) can keep the window
 * open. */
static gboolean
save_as_dialog (PnWindow *self)
{
    GtkWidget *dialog;
    gboolean   saved = FALSE;

    dialog = gtk_file_chooser_dialog_new (
            "Save Worksheet As",
            GTK_WINDOW (self),
            GTK_FILE_CHOOSER_ACTION_SAVE,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Save",   GTK_RESPONSE_ACCEPT,
            NULL);

    gtk_file_chooser_set_do_overwrite_confirmation (
            GTK_FILE_CHOOSER (dialog), TRUE);
    gtk_file_chooser_set_current_name (
            GTK_FILE_CHOOSER (dialog), "untitled.json");

    if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
    {
        gchar *path = gtk_file_chooser_get_filename (
                GTK_FILE_CHOOSER (dialog));

        if (save_to_path (self, path))
        {
            g_free (self->current_path);
            self->current_path = path;
            path = NULL;

            update_window_title (self);
            saved = TRUE;
        }

        g_free (path);
    }

    gtk_widget_destroy (dialog);
    return saved;
}

/* Persist the worksheet to its current path, falling back to Save As
 * when it has never been saved.  Returns %TRUE when the file ends up
 * written. */
static gboolean
save_current (PnWindow *self)
{
    if (self->current_path)
        return save_to_path (self, self->current_path);

    return save_as_dialog (self);
}

void
pn_window_autosave (PnWindow *self)
{
    g_return_if_fail (PN_IS_WINDOW (self));

    if (self->current_path != NULL && pn_flow_is_modified (self->flow))
        save_to_path (self, self->current_path);
}

/* Guard any operation that would throw away the current worksheet —
 * closing the window, File -> New, File -> Open.  When there are unsaved
 * changes, ask the user to save, discard or cancel.  Returns %TRUE when
 * the caller may proceed (nothing to save, the user discarded, or the
 * save succeeded) and %FALSE when the operation must be aborted (the
 * user cancelled, or a requested save was cancelled or failed). */
static gboolean
confirm_discard_changes (PnWindow *self)
{
    GtkWidget *dialog;
    gint       response;

    if (!pn_flow_is_modified (self->flow))
        return TRUE;

    dialog = gtk_message_dialog_new (
            GTK_WINDOW (self),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_NONE,
            "Save changes to the worksheet?");
    gtk_message_dialog_format_secondary_text (
            GTK_MESSAGE_DIALOG (dialog),
            "Your changes will be lost if you don't save them.");
    gtk_dialog_add_buttons (
            GTK_DIALOG (dialog),
            "_Discard Changes", GTK_RESPONSE_REJECT,
            "_Cancel",          GTK_RESPONSE_CANCEL,
            "_Save",            GTK_RESPONSE_ACCEPT,
            NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (dialog),
                                     GTK_RESPONSE_ACCEPT);

    response = gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);

    switch (response)
    {
    case GTK_RESPONSE_ACCEPT:
        /* Proceed only if the write actually happened: the Save As
         * chooser may have been cancelled, or the write may have
         * failed. */
        return save_current (self);

    case GTK_RESPONSE_REJECT:
        return TRUE;    /* discard: proceed */

    default:
        return FALSE;   /* cancel or dismissed: abort */
    }
}

static void
action_save_as (
        GtkMenuItem *item,
        gpointer     user_data)
{
    (void) item;

    (void) save_as_dialog (PN_WINDOW (user_data));
}

static void
action_save (
        GtkMenuItem *item,
        gpointer     user_data)
{
    (void) item;

    (void) save_current (PN_WINDOW (user_data));
}

static void
action_quit (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) item;

    /* Routes through "delete-event", so the unsaved-changes guard in
     * on_window_delete_event applies to File -> Quit too. */
    gtk_window_close (GTK_WINDOW (self));
}

/* Edit actions that are not yet implemented (Undo/Redo, View/Zoom)
 * just echo their label to the status bar so the binding shows
 * something. */

static void
action_edit_stub (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);

    status_set (self, gtk_menu_item_get_label (item));
}

static void
action_cut (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    PnWorksheet *ws   = pn_window_active_worksheet (self);
    (void) item;

    if (ws == NULL)
        return;
    pn_worksheet_cut_selection (ws);
    status_set (self, "Cut");
}

static void
action_copy (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    PnWorksheet *ws   = pn_window_active_worksheet (self);
    (void) item;

    if (ws == NULL)
        return;
    pn_worksheet_copy_selection (ws);
    status_set (self, "Copied");
}

static void
action_paste (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    PnWorksheet *ws   = pn_window_active_worksheet (self);
    (void) item;

    if (ws == NULL)
        return;
    pn_worksheet_paste_from_clipboard (ws);
}

/** Open (or raise) the application-wide Preferences dialog.  The
 *  dialog is non-modal so the user can keep editing the worksheet
 *  while it is open; every change in the dialog applies live across
 *  all open windows via the #PnPreferences singleton. */
static void
action_preferences (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) item;

    pn_preferences_dialog_present (GTK_WINDOW (self));
}

/** Open (or raise) the Document Settings dialog for this window's open
 *  file.  Unlike Preferences it is bound to this window's #PnFlow, so the
 *  globals it edits are saved into the file rather than the user's config. */
static void
action_document_settings (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) item;

    pn_document_settings_dialog_present (GTK_WINDOW (self), self->flow);
}

/** Locate the application icon PNG so the About dialog can show it
 *  whether running from the build tree or an install.  The icon ships
 *  as $(datadir)/icons/hicolor/256x256/apps/org.pipas.pipnode.png (see
 *  data/Makefile.am); PKGDATADIR is $(datadir)/pipnode, so its parent
 *  is $(datadir).  Returns a freshly-allocated path the caller frees,
 *  or %NULL.  We load from file rather than by themed icon name because
 *  the icon is only on the icon-theme search path after `make install`
 *  (or inside the Flatpak), not when running straight from the tree. */
static gchar *
resolve_logo_path (void)
{
    gchar *path;

#ifdef SRCDIR
    path = g_build_filename (SRCDIR, "data", "icons", "hicolor",
                             "256x256", "apps", "org.pipas.pipnode.png",
                             NULL);
    if (g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
    g_free (path);
#endif

#ifdef PKGDATADIR
    {
        gchar *datadir = g_path_get_dirname (PKGDATADIR);
        path = g_build_filename (datadir, "icons", "hicolor", "256x256",
                                 "apps", "org.pipas.pipnode.png", NULL);
        g_free (datadir);
        if (g_file_test (path, G_FILE_TEST_EXISTS))
            return path;
        g_free (path);
    }
#endif

    return NULL;
}

/** Append a centred #GtkLabel carrying Pango markup to @box.  When
 *  @markup contains <a href="…"> spans GtkLabel renders them as
 *  hyperlinks and its built-in activate-link handler opens them in the
 *  browser, so no extra wiring is needed. */
static void
about_add_markup (GtkBox *box, const gchar *markup)
{
    GtkWidget *label = gtk_label_new (NULL);

    gtk_label_set_markup (GTK_LABEL (label), markup);
    gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 50);
    gtk_box_pack_start (box, label, FALSE, FALSE, 0);
}

static void
action_about (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow  *self = PN_WINDOW (user_data);
    GtkWidget *dialog;
    GtkWidget *box;
    gchar     *logo_path;
    gchar     *markup;
    (void) item;

    /*  A hand-built dialog rather than GtkAboutDialog: we want the
     *  GitHub/Sponsor/Discussions pointers as real paragraphs with
     *  clickable links (not buried in a Credits sub-page), a custom
     *  licence note describing the plugin exception, and no dead
     *  "License" button. */
    dialog = gtk_dialog_new_with_buttons (
            "About Pipnode",
            GTK_WINDOW (self),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Close", GTK_RESPONSE_CLOSE,
            NULL);
    gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top (box, 18);
    gtk_widget_set_margin_bottom (box, 12);
    gtk_widget_set_margin_start (box, 28);
    gtk_widget_set_margin_end (box, 28);
    gtk_container_add (
            GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
            box);

    /*  Application icon, scaled down from the 256px hicolor asset. */
    logo_path = resolve_logo_path ();
    if (logo_path != NULL)
    {
        GdkPixbuf *pix = gdk_pixbuf_new_from_file_at_size (logo_path,
                                                           128, 128, NULL);
        if (pix != NULL)
        {
            GtkWidget *image = gtk_image_new_from_pixbuf (pix);
            gtk_widget_set_margin_bottom (image, 6);
            gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);
            g_object_unref (pix);
        }
        g_free (logo_path);
    }

    /*  Name + version. */
    markup = g_markup_printf_escaped (
            "<span size='xx-large' weight='bold'>Pipnode</span>\n"
            "<span size='small' alpha='75%%'>%s</span>",
            PACKAGE_VERSION);
    about_add_markup (GTK_BOX (box), markup);
    g_free (markup);

    about_add_markup (GTK_BOX (box),
            "Pipnode is an interactive environment for building dataflows "
            "visually: drop nodes onto a worksheet, wire them together, and "
            "watch your data come alive in real time. It is flexible, highly "
            "customizable and thoroughly modular — a first-class plugin "
            "system lets the node catalogue grow to fit whatever you are "
            "building, without ever touching the core.");

    /*  Project pointers — readable paragraphs, each with an inline link. */
    about_add_markup (GTK_BOX (box),
            "You can get the full source code, browse the latest releases "
            "and report issues on the project's "
            "<a href=\"https://github.com/laszlopere/pipnode\">"
            "GitHub page</a>.");
    about_add_markup (GTK_BOX (box),
            "Pipnode is developed in the open and kept alive by the people "
            "who use it. If you find it useful, please consider "
            "<a href=\"https://github.com/sponsors/laszlopere\">"
            "sponsoring it on GitHub</a> — even the smallest one-off "
            "contribution is highly appreciated and helps keep the project "
            "going.");
    about_add_markup (GTK_BOX (box),
            "Have an idea or a feature wish? Share it on the "
            "<a href=\"https://github.com/laszlopere/pipnode/discussions\">"
            "discussion forum</a> — feedback from people who use Pipnode "
            "shapes where it goes next.");

    /*  Licence note: GPLv3 plus the plugin exception (open-core). */
    about_add_markup (GTK_BOX (box),
            "<span size='small'>This is a free, open-source program, "
            "released under the "
            "<a href=\"https://github.com/laszlopere/pipnode/blob/master/LICENSE\">"
            "GNU General Public License v3</a>. As an open-core project it "
            "also grants an "
            "<a href=\"https://github.com/laszlopere/pipnode/blob/master/"
            "LICENSE.PLUGIN-EXCEPTION\">exception permitting proprietary "
            "plugins</a>, so you are free to build and ship closed-source "
            "nodes on top of it.</span>");

    about_add_markup (GTK_BOX (box),
            "<span size='small' alpha='65%'>"
            "Copyright © 2024-2026 Laszlo Pere</span>");

    g_signal_connect (dialog, "response",
                      G_CALLBACK (gtk_widget_destroy), NULL);

    gtk_widget_show_all (dialog);
}

/** %TRUE when running inside a Flatpak sandbox.  Flatpak drops a
 *  /.flatpak-info file into every sandbox and exports $FLATPAK_ID, so
 *  either signal identifies the community-edition bundle. */
static gboolean
running_under_flatpak (void)
{
    return g_file_test ("/.flatpak-info", G_FILE_TEST_EXISTS)
        || g_getenv ("FLATPAK_ID") != NULL;
}

/** Pango markup describing how this binary was built.  The text differs
 *  per build channel: the Flatpak community edition identifies itself as
 *  the bundled open-source collection, while a local build reports the
 *  short git commit it was compiled from.  Caller frees. */
static gchar *
build_info_markup (void)
{
    if (running_under_flatpak ())
        return g_strdup (
            "<b>Pipnode Flatpak community edition.</b>\n"
            "The community edition is the open-source Pipnode bundle: the "
            "GPL core together with all of the open-source plugins, packaged "
            "as a single sandboxed Flatpak that runs on any distribution.");

#ifdef GIT_VERSION
    return g_markup_printf_escaped (
            "<b>Custom build</b> (git %s).\n"
            "Compiled locally from source.",
            GIT_VERSION);
#else
    return g_strdup (
            "<b>Custom build.</b>\n"
            "Compiled locally from source.");
#endif
}

/** Append a left-aligned, wrapped #GtkLabel of Pango @markup to @box —
 *  the paragraph style used by the Release Notes dialog (links inside
 *  the markup stay clickable, as in the About dialog). */
static void
notes_add_markup (GtkBox *box, const gchar *markup)
{
    GtkWidget *label = gtk_label_new (NULL);

    gtk_label_set_markup (GTK_LABEL (label), markup);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_LEFT);
    gtk_label_set_line_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 58);
    gtk_box_pack_start (box, label, FALSE, FALSE, 0);
}

static void
action_release_notes (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow  *self = PN_WINDOW (user_data);
    GtkWidget *dialog;
    GtkWidget *scrolled;
    GtkWidget *box;
    gchar     *markup;
    (void) item;

    dialog = gtk_dialog_new_with_buttons (
            "Release Notes",
            GTK_WINDOW (self),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Close", GTK_RESPONSE_CLOSE,
            NULL);
    gtk_window_set_default_size (GTK_WINDOW (dialog), 540, 580);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (
            GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
            scrolled, TRUE, TRUE, 0);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_top (box, 16);
    gtk_widget_set_margin_bottom (box, 16);
    gtk_widget_set_margin_start (box, 22);
    gtk_widget_set_margin_end (box, 22);
    /* GtkScrolledWindow wraps a non-scrollable child in a viewport. */
    gtk_container_add (GTK_CONTAINER (scrolled), box);

    /*  Version heading. */
    markup = g_markup_printf_escaped (
            "<span size='x-large' weight='bold'>Pipnode %s</span>\n"
            "<span alpha='70%%'>First public release — 24 May 2026</span>",
            PACKAGE_VERSION);
    notes_add_markup (GTK_BOX (box), markup);
    g_free (markup);

    /*  Build channel — differs between Flatpak and a local build. */
    markup = build_info_markup ();
    notes_add_markup (GTK_BOX (box), markup);
    g_free (markup);

    notes_add_markup (GTK_BOX (box),
            "A desktop visual flow editor for Linux. Drop nodes onto a "
            "worksheet, wire them together, and build dataflows — network "
            "probes, MQTT, sensors, gauges, image processing, local LLM "
            "calls and more — without writing glue code.");

    notes_add_markup (GTK_BOX (box),
            "<b>Early but stable.</b>\n"
            "This release is tested and usable for real work: it does not "
            "crash in normal use, and the GUI is complete and unobtrusive — "
            "no dead buttons or placeholder panes. Together, the bundled "
            "nodes and the editor are enough to build and run genuinely "
            "useful flows day to day.");

    notes_add_markup (GTK_BOX (box),
            "<b>Not yet mature.</b>\n"
            "The public surface is not frozen. The set of nodes, their names "
            "and behaviour, the messages passed between them, and the plugin "
            "ABI may all change in future 0.x releases — possibly without a "
            "migration path. For now, treat saved worksheets and third-party "
            "plugins as tied to this version.");

    notes_add_markup (GTK_BOX (box),
            "<b>What's inside</b>\n"
            "• JSON worksheets with live message flow and a debug view\n"
            "• A broad node catalogue: flow/logic, transforms, gauges and "
            "meters, HTTP/MQTT/WebSocket, sensors, image processing, shell, "
            "weather and more\n"
            "• A headless runner (<tt>pipnode-run</tt>) for servers\n"
            "• A first-class plugin system (open-core: GPLv3 + plugin "
            "exception)");

    notes_add_markup (GTK_BOX (box),
            "The full changelog is in "
            "<a href=\"https://github.com/laszlopere/pipnode/blob/master/"
            "CHANGELOG.md\">CHANGELOG.md</a>.");

    g_signal_connect (dialog, "response",
                      G_CALLBACK (gtk_widget_destroy), NULL);

    gtk_widget_show_all (dialog);
}

/** Open a #PnHelpBrowser parented to the main window.
 *
 *  When @anchor is non-%NULL it is interpreted as a node type name
 *  (e.g. "PnRtc") and the per-class help file `<anchor>.html` is
 *  loaded directly — that file lives next to help-index.html in the
 *  flat $pkgdatadir/help/ directory and is a complete standalone
 *  page about the type, so no fragment-id scroll is needed.  When
 *  the per-class file is absent (e.g. a plugin shipped a node type
 *  without a help file), the browser falls back to opening the host
 *  index.  When @anchor is %NULL — i.e. the Help → Contents menu
 *  item rather than the palette right-click — the index opens
 *  unconditionally. */
static void
open_help_contents (
        PnWindow    *self,
        const gchar *anchor)
{
    PnHelpBrowser *browser;
    gchar         *path = NULL;

    if (anchor != NULL && *anchor != '\0')
    {
        gchar *page = g_strconcat (anchor, ".html", NULL);
        path = resolve_help_path (page);
        g_free (page);
    }

    if (path == NULL)
        path = resolve_help_path ("help-index.html");

    if (!path)
    {
        status_set (self, "Help page not found");
        return;
    }

    /* The per-class page is its own file; the index is its own file
     * too — neither needs a fragment-id scroll, so always pass NULL
     * for the anchor argument here. */
    browser = pn_help_browser_new (GTK_WINDOW (self), path, NULL);
    /* gtk_window_present alone does not recurse into child widgets,
     * so the embedded WebKitWebView is created but never mapped and
     * the window comes up showing nothing but its background.  Force
     * the whole subtree visible first, the same way pipevolution
     * does, then bring the window forward. */
    gtk_widget_show_all (GTK_WIDGET (browser));
    gtk_window_present (GTK_WINDOW (browser));

    g_free (path);
}

static void
action_help_contents (
        GtkMenuItem *item,
        gpointer     user_data)
{
    (void) item;

    open_help_contents (PN_WINDOW (user_data), NULL);
}

static void
on_palette_help_requested (
        GtkWidget   *palette,
        const gchar *anchor,
        gpointer     user_data)
{
    (void) palette;

    open_help_contents (PN_WINDOW (user_data), anchor);
}

/* ------------------------------------------------------------------ */
/*  Locate the bundled help pages                                      */
/*                                                                     */
/*  All HTML lives in a single flat directory at install time          */
/*  ($pkgdatadir/help/) so the host's help-index.html and every        */
/*  per-node <ClassName>.html (whether it ships from libpipnode or a   */
/*  plugin) can reference the shared help.css with a sibling href.     */
/*  A per-node file like PnRtc.html sits next to help-index.html and   */
/*  next to PnPing.html (which shipped from the network plugin's       */
/*  Makefile.am) — the resolver below knows about every directory     */
/*  these files might live in across the dev / install / plugin axes:  */
/*                                                                     */
/*    1. $PIPNODE_HELP_PATH      colon-separated dev override          */
/*    2. $XDG_DATA_HOME/pipnode/help/   per-user installs              */
/*    3. $SRCDIR/data/help/            in-tree, host's own files       */
/*    4. $SRCDIR/plugins/<x>/help/    in-tree, bundled plugins         */
/*    5. $PKGDATADIR/help/             system install (host + plugins) */
/* ------------------------------------------------------------------ */

/** Try @dir/@filename; on success returns a freshly-allocated absolute
 *  path the caller must g_free, or %NULL if the file is absent. */
static gchar *
help_try (const gchar *dir, const gchar *filename)
{
    gchar *path;

    if (dir == NULL || *dir == '\0')
        return NULL;

    path = g_build_filename (dir, filename, NULL);
    if (g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
    g_free (path);
    return NULL;
}

/** Walk every immediate subdirectory of @plugins_root, treat each
 *  child's "help/" entry as a candidate help dir, and return the path
 *  of the first one that contains @filename.  Mirrors the build-tree
 *  fallback the plugin loader does for .so files: the bundled
 *  plugins/<x>/help/<class>.html is reachable from `make` without a
 *  prior `make install` step. */
static gchar *
help_try_in_subdirs (const gchar *plugins_root, const gchar *filename)
{
    GDir        *root;
    const gchar *child;
    gchar       *found = NULL;

    if (plugins_root == NULL)
        return NULL;

    root = g_dir_open (plugins_root, 0, NULL);
    if (root == NULL)
        return NULL;

    while ((child = g_dir_read_name (root)) != NULL)
    {
        gchar *help_dir = g_build_filename (plugins_root, child,
                                            "help", NULL);
        gchar *hit      = help_try (help_dir, filename);
        g_free (help_dir);
        if (hit != NULL)
        {
            found = hit;
            break;
        }
    }

    g_dir_close (root);
    return found;
}

static gchar *
resolve_help_path (const gchar *filename)
{
    const gchar *env;
    gchar       *path;

    g_return_val_if_fail (filename != NULL && *filename != '\0', NULL);

    /* (1) $PIPNODE_HELP_PATH — colon-separated, front-most wins.
     * Mirrors the plugin loader's $PIPNODE_PLUGIN_PATH so an
     * out-of-tree plugin developer can point both env vars at the
     * same uninstalled tree. */
    env = g_getenv ("PIPNODE_HELP_PATH");
    if (env != NULL && *env != '\0')
    {
        gchar **parts = g_strsplit (env, G_SEARCHPATH_SEPARATOR_S, -1);
        guint   i;

        for (i = 0; parts[i] != NULL; i++)
        {
            path = help_try (parts[i], filename);
            if (path != NULL)
            {
                g_strfreev (parts);
                return path;
            }
        }
        g_strfreev (parts);
    }

    /* (2) Per-user data dir.  Same XDG fallback the plugin loader
     * uses for $XDG_DATA_HOME/pipnode/plugins. */
    {
        gchar *user_dir = g_build_filename (g_get_user_data_dir (),
                                            "pipnode", "help", NULL);
        path = help_try (user_dir, filename);
        g_free (user_dir);
        if (path != NULL)
            return path;
    }

    /* (3) In-tree host pages. */
#ifdef SRCDIR
    {
        gchar *src_dir = g_build_filename (SRCDIR, "data", "help", NULL);
        path = help_try (src_dir, filename);
        g_free (src_dir);
        if (path != NULL)
            return path;
    }

    /* (4) In-tree bundled plugins (every plugins/<x>/help/). */
    {
        gchar *plugins_root = g_build_filename (SRCDIR, "plugins", NULL);
        path = help_try_in_subdirs (plugins_root, filename);
        g_free (plugins_root);
        if (path != NULL)
            return path;
    }
#endif

    /* (5) System install — flat $pkgdatadir/help/. */
#ifdef PKGDATADIR
    {
        gchar *sys_dir = g_build_filename (PKGDATADIR, "help", NULL);
        path = help_try (sys_dir, filename);
        g_free (sys_dir);
        if (path != NULL)
            return path;
    }
#endif

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Bundled example worksheets                                         */
/*                                                                     */
/*  The samples install as $pkgdatadir/examples/<category>/<name>.json */
/*  (see examples/Makefile.am) and live at $SRCDIR/examples/ in the    */
/*  tree, so the same search order resolve_help_path() uses applies:   */
/*                                                                     */
/*    1. $PIPNODE_EXAMPLES_PATH   colon-separated dev override         */
/*    2. $XDG_DATA_HOME/pipnode/examples/   per-user installs          */
/*    3. $SRCDIR/examples/               in-tree, run from `make`       */
/*    4. $PKGDATADIR/examples/           system install                */
/* ------------------------------------------------------------------ */

/** Return @dir if it names an existing directory (freshly allocated,
 *  caller frees), or %NULL otherwise. */
static gchar *
examples_try (const gchar *dir)
{
    if (dir == NULL || *dir == '\0')
        return NULL;
    if (g_file_test (dir, G_FILE_TEST_IS_DIR))
        return g_strdup (dir);
    return NULL;
}

/** Locate the directory that holds the per-category example sub-dirs,
 *  trying the dev override, the per-user dir, the in-tree copy and the
 *  system install in turn.  Returns %NULL when none exist. */
static gchar *
resolve_examples_dir (void)
{
    const gchar *env;
    gchar       *path;
    gchar       *cand;

    env = g_getenv ("PIPNODE_EXAMPLES_PATH");
    if (env != NULL && *env != '\0')
    {
        gchar **parts = g_strsplit (env, G_SEARCHPATH_SEPARATOR_S, -1);
        guint   i;

        for (i = 0; parts[i] != NULL; i++)
        {
            path = examples_try (parts[i]);
            if (path != NULL)
            {
                g_strfreev (parts);
                return path;
            }
        }
        g_strfreev (parts);
    }

    cand = g_build_filename (g_get_user_data_dir (), "pipnode",
                             "examples", NULL);
    path = examples_try (cand);
    g_free (cand);
    if (path != NULL)
        return path;

#ifdef SRCDIR
    cand = g_build_filename (SRCDIR, "examples", NULL);
    path = examples_try (cand);
    g_free (cand);
    if (path != NULL)
        return path;
#endif

#ifdef PKGDATADIR
    cand = g_build_filename (PKGDATADIR, "examples", NULL);
    path = examples_try (cand);
    g_free (cand);
    if (path != NULL)
        return path;
#endif

    return NULL;
}

/** Turn a kebab/snake slug into a menu-friendly label: "home-automation"
 *  -> "Home Automation", "bloom-glow" -> "Bloom Glow".  Caller frees. */
static gchar *
prettify_slug (const gchar *raw)
{
    gchar   *s = g_strdup (raw);
    gchar   *p;
    gboolean at_word_start = TRUE;

    for (p = s; *p != '\0'; p++)
    {
        if (*p == '-' || *p == '_')
        {
            *p = ' ';
            at_word_start = TRUE;
        }
        else
        {
            if (at_word_start && g_ascii_isalpha (*p))
                *p = g_ascii_toupper (*p);
            at_word_start = FALSE;
        }
    }
    return s;
}

/** g_ptr_array_sort comparator for an array of owned strings. */
static gint
slug_cmp (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const gchar * const *) a,
                      *(const gchar * const *) b);
}

/** Open the example whose path is stashed on the activated menu item.
 *  The worksheet is loaded but current_path is left cleared so a
 *  subsequent Save prompts for a destination instead of trying to
 *  overwrite the read-only installed sample. */
static void
action_open_example (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnWindow    *self  = PN_WINDOW (user_data);
    const gchar *path  = g_object_get_data (G_OBJECT (item), "example-path");
    GError      *error = NULL;

    if (path == NULL)
        return;

    if (!confirm_discard_changes (self))
        return;

    if (!pn_flow_load_from_file (self->flow, path, &error))
    {
        GtkWidget *err = gtk_message_dialog_new (
                GTK_WINDOW (self),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE,
                "Could not open example:\n%s",
                error->message);
        gtk_dialog_run (GTK_DIALOG (err));
        gtk_widget_destroy (err);
        g_error_free (error);
        return;
    }

    g_clear_pointer (&self->current_path, g_free);
    update_window_title (self);
    status_set (self, "Opened example");
}

/* ------------------------------------------------------------------ */
/*  Menu bar                                                           */
/* ------------------------------------------------------------------ */

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static GtkWidget *
create_image_menu_item (
        const gchar     *icon_name,
        const gchar     *label,
        GtkAccelGroup   *accel_group,
        guint            accel_key,
        GdkModifierType  accel_mods,
        GCallback        callback,
        gpointer         user_data)
{
    GtkWidget *item;
    GtkWidget *image;

    item  = gtk_image_menu_item_new_with_mnemonic (label);
    image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_MENU);
    gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
    gtk_image_menu_item_set_always_show_image (
            GTK_IMAGE_MENU_ITEM (item), TRUE);

    if (accel_group && accel_key)
        gtk_widget_add_accelerator (item, "activate", accel_group,
                                    accel_key, accel_mods,
                                    GTK_ACCEL_VISIBLE);

    g_signal_connect (item, "activate", callback, user_data);

    return item;
}

G_GNUC_END_IGNORE_DEPRECATIONS

/** Build the "Open Example" item: a submenu with one sub-menu per
 *  category directory, each listing its example worksheets.  Scanning
 *  happens once, at menu-construction time.  When no examples are found
 *  (uninstalled and run outside the tree) the item is returned
 *  insensitive so the File menu still has a stable shape. */
static GtkWidget *
create_examples_menu_item (PnWindow *self)
{
    GtkWidget   *item;
    GtkWidget   *submenu;
    gchar       *root;
    GDir        *dir;
    const gchar *entry;
    GPtrArray   *cats;
    guint        i;
    gboolean     any = FALSE;

    /* An image menu item (deprecated, but the rest of the File menu
     * uses the same widget): "accessories-dictionary" is a book that
     * reads as a library of ready-made worksheets and stays distinct
     * from the open-folder icon "Open…" carries.  (Picked over app
     * icons like gnome-books, which GTK's icon-theme lookup can't
     * resolve and would render as the broken-image placeholder.) */
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    item = gtk_image_menu_item_new_with_mnemonic ("Open E_xample");
    gtk_image_menu_item_set_image (
            GTK_IMAGE_MENU_ITEM (item),
            gtk_image_new_from_icon_name ("accessories-dictionary",
                                          GTK_ICON_SIZE_MENU));
    gtk_image_menu_item_set_always_show_image (
            GTK_IMAGE_MENU_ITEM (item), TRUE);
    G_GNUC_END_IGNORE_DEPRECATIONS

    root = resolve_examples_dir ();
    if (root == NULL)
    {
        gtk_widget_set_sensitive (item, FALSE);
        return item;
    }

    dir = g_dir_open (root, 0, NULL);
    if (dir == NULL)
    {
        g_free (root);
        gtk_widget_set_sensitive (item, FALSE);
        return item;
    }

    /* Collect category sub-directories and sort them, so the menu order
     * is stable regardless of the order g_dir_read_name happens to
     * return entries in. */
    cats = g_ptr_array_new_with_free_func (g_free);
    while ((entry = g_dir_read_name (dir)) != NULL)
    {
        gchar *child = g_build_filename (root, entry, NULL);
        if (g_file_test (child, G_FILE_TEST_IS_DIR))
            g_ptr_array_add (cats, g_strdup (entry));
        g_free (child);
    }
    g_dir_close (dir);
    g_ptr_array_sort (cats, slug_cmp);

    submenu = gtk_menu_new ();

    for (i = 0; i < cats->len; i++)
    {
        const gchar *catname = g_ptr_array_index (cats, i);
        gchar       *catpath = g_build_filename (root, catname, NULL);
        GDir        *cdir    = g_dir_open (catpath, 0, NULL);
        GPtrArray   *files;
        const gchar *fn;
        guint        j;

        if (cdir == NULL)
        {
            g_free (catpath);
            continue;
        }

        files = g_ptr_array_new_with_free_func (g_free);
        while ((fn = g_dir_read_name (cdir)) != NULL)
            if (g_str_has_suffix (fn, ".json"))
                g_ptr_array_add (files, g_strdup (fn));
        g_dir_close (cdir);
        g_ptr_array_sort (files, slug_cmp);

        if (files->len > 0)
        {
            gchar     *catlabel = prettify_slug (catname);
            GtkWidget *cat_item = gtk_menu_item_new_with_label (catlabel);
            GtkWidget *cat_menu = gtk_menu_new ();

            g_free (catlabel);

            for (j = 0; j < files->len; j++)
            {
                const gchar *file  = g_ptr_array_index (files, j);
                gchar       *stem  = g_strdup (file);
                gchar       *dot   = g_strrstr (stem, ".json");
                gchar       *label;
                gchar       *full  = g_build_filename (catpath, file, NULL);
                GtkWidget   *leaf;

                if (dot != NULL)
                    *dot = '\0';            /* drop the .json extension */
                label = prettify_slug (stem);
                leaf  = gtk_menu_item_new_with_label (label);

                /* The leaf owns its absolute path; action_open_example
                 * reads it back on activate. */
                g_object_set_data_full (G_OBJECT (leaf), "example-path",
                                        full, g_free);
                g_signal_connect (leaf, "activate",
                                  G_CALLBACK (action_open_example), self);
                gtk_menu_shell_append (GTK_MENU_SHELL (cat_menu), leaf);

                g_free (stem);
                g_free (label);
            }

            gtk_menu_item_set_submenu (GTK_MENU_ITEM (cat_item), cat_menu);
            gtk_menu_shell_append (GTK_MENU_SHELL (submenu), cat_item);
            any = TRUE;
        }

        g_ptr_array_free (files, TRUE);
        g_free (catpath);
    }

    g_ptr_array_free (cats, TRUE);
    g_free (root);

    if (!any)
    {
        gtk_widget_destroy (submenu);
        gtk_widget_set_sensitive (item, FALSE);
        return item;
    }

    gtk_menu_item_set_submenu (GTK_MENU_ITEM (item), submenu);
    return item;
}

static GtkWidget *
create_menubar (PnWindow *self)
{
    GtkWidget *menubar;
    GtkWidget *menu;
    GtkWidget *menu_item;
    GtkWidget *separator;
    GtkAccelGroup *accel_group;

    accel_group = gtk_accel_group_new ();
    gtk_window_add_accel_group (GTK_WINDOW (self), accel_group);

    menubar = gtk_menu_bar_new ();

    /* ---- File menu ---- */
    menu = gtk_menu_new ();

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("document-new", "_New", accel_group,
                                GDK_KEY_n, GDK_CONTROL_MASK,
                                G_CALLBACK (action_new), self));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("document-open", "_Open…", accel_group,
                                GDK_KEY_o, GDK_CONTROL_MASK,
                                G_CALLBACK (action_open), self));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
                           create_examples_menu_item (self));
    self->menu_save = create_image_menu_item (
            "document-save", "_Save", accel_group,
            GDK_KEY_s, GDK_CONTROL_MASK,
            G_CALLBACK (action_save), self);
    /* A freshly-created worksheet matches the (empty) on-disk state,
     * so Save starts insensitive and only ungates once the worksheet
     * emits "modified-changed" with pn_worksheet_is_modified()
     * returning TRUE. */
    gtk_widget_set_sensitive (self->menu_save, FALSE);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), self->menu_save);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("document-save-as", "Save _As…", accel_group,
                                GDK_KEY_s, GDK_CONTROL_MASK | GDK_SHIFT_MASK,
                                G_CALLBACK (action_save_as), self));

    separator = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), separator);

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("document-properties", "Document _Settings…",
                                accel_group, 0, 0,
                                G_CALLBACK (action_document_settings), self));

    separator = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), separator);

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("application-exit", "_Quit", accel_group,
                                GDK_KEY_q, GDK_CONTROL_MASK,
                                G_CALLBACK (action_quit), self));

    menu_item = gtk_menu_item_new_with_mnemonic ("_File");
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (menu_item), menu);
    gtk_menu_shell_append (GTK_MENU_SHELL (menubar), menu_item);

    /* ---- Edit menu ---- */
    menu = gtk_menu_new ();

    self->menu_undo = create_image_menu_item (
            "edit-undo", "_Undo", accel_group,
            GDK_KEY_z, GDK_CONTROL_MASK,
            G_CALLBACK (action_edit_stub), self);
    gtk_widget_set_sensitive (self->menu_undo, FALSE);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), self->menu_undo);

    self->menu_redo = create_image_menu_item (
            "edit-redo", "_Redo", accel_group,
            GDK_KEY_z, GDK_CONTROL_MASK | GDK_SHIFT_MASK,
            G_CALLBACK (action_edit_stub), self);
    gtk_widget_set_sensitive (self->menu_redo, FALSE);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), self->menu_redo);

    separator = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), separator);

    self->menu_cut = create_image_menu_item (
            "edit-cut", "Cu_t", accel_group,
            GDK_KEY_x, GDK_CONTROL_MASK,
            G_CALLBACK (action_cut), self);
    gtk_widget_set_sensitive (self->menu_cut, FALSE);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), self->menu_cut);

    self->menu_copy = create_image_menu_item (
            "edit-copy", "_Copy", accel_group,
            GDK_KEY_c, GDK_CONTROL_MASK,
            G_CALLBACK (action_copy), self);
    gtk_widget_set_sensitive (self->menu_copy, FALSE);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), self->menu_copy);

    self->menu_paste = create_image_menu_item (
            "edit-paste", "_Paste", accel_group,
            GDK_KEY_v, GDK_CONTROL_MASK,
            G_CALLBACK (action_paste), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), self->menu_paste);

    separator = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), separator);

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("preferences-system", "Pr_eferences…",
                                accel_group,
                                0, 0,
                                G_CALLBACK (action_preferences), self));

    menu_item = gtk_menu_item_new_with_mnemonic ("_Edit");
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (menu_item), menu);
    gtk_menu_shell_append (GTK_MENU_SHELL (menubar), menu_item);

    /* ---- View menu ---- */
    menu = gtk_menu_new ();

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("zoom-in", "Zoom _In", accel_group,
                                GDK_KEY_plus, GDK_CONTROL_MASK,
                                G_CALLBACK (action_edit_stub), self));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("zoom-out", "Zoom _Out", accel_group,
                                GDK_KEY_minus, GDK_CONTROL_MASK,
                                G_CALLBACK (action_edit_stub), self));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("zoom-original", "_Reset Zoom", accel_group,
                                GDK_KEY_0, GDK_CONTROL_MASK,
                                G_CALLBACK (action_edit_stub), self));

    separator = gtk_separator_menu_item_new ();
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), separator);

    self->menu_debug_view = gtk_check_menu_item_new_with_mnemonic (
            "_Debug View");
    gtk_widget_add_accelerator (self->menu_debug_view, "activate", accel_group,
                                GDK_KEY_d, GDK_CONTROL_MASK | GDK_SHIFT_MASK,
                                GTK_ACCEL_VISIBLE);
    g_signal_connect (self->menu_debug_view, "toggled",
                      G_CALLBACK (action_toggle_debug_view), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), self->menu_debug_view);

    menu_item = gtk_menu_item_new_with_mnemonic ("_View");
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (menu_item), menu);
    gtk_menu_shell_append (GTK_MENU_SHELL (menubar), menu_item);

    /* ---- Help menu ---- */
    menu = gtk_menu_new ();

    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("help-contents", "_Contents", accel_group,
                                GDK_KEY_F1, 0,
                                G_CALLBACK (action_help_contents), self));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("text-x-generic", "Release _Notes", NULL,
                                0, 0,
                                G_CALLBACK (action_release_notes), self));
    gtk_menu_shell_append (GTK_MENU_SHELL (menu),
        create_image_menu_item ("help-about", "_About", NULL,
                                0, 0,
                                G_CALLBACK (action_about), self));

    menu_item = gtk_menu_item_new_with_mnemonic ("_Help");
    gtk_menu_item_set_submenu (GTK_MENU_ITEM (menu_item), menu);
    gtk_menu_shell_append (GTK_MENU_SHELL (menubar), menu_item);

    g_object_unref (accel_group);

    return menubar;
}

/* ------------------------------------------------------------------ */
/*  Toolbar                                                            */
/* ------------------------------------------------------------------ */

static GtkWidget *
create_toolbar_button (
        const gchar *icon_name,
        const gchar *label,
        GCallback    callback,
        gpointer     user_data)
{
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *image;
    GtkWidget *lbl;

    image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
    lbl   = gtk_label_new (label);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start (GTK_BOX (box), image, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), lbl,   FALSE, FALSE, 0);

    button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (button), box);
    gtk_widget_set_tooltip_text (button, label);
    gtk_button_set_relief (GTK_BUTTON (button), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (button, FALSE);

    g_signal_connect (button, "clicked", callback, user_data);

    return button;
}

static void
on_tb_new (GtkButton *button, gpointer user_data)
{
    (void) button;
    action_new (NULL, user_data);
}

static void
on_tb_open (GtkButton *button, gpointer user_data)
{
    (void) button;
    action_open (NULL, user_data);
}

static void
on_tb_save (GtkButton *button, gpointer user_data)
{
    (void) button;
    action_save (NULL, user_data);
}

static void
on_tb_stub (GtkButton *button, gpointer user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    const gchar *tip = gtk_widget_get_tooltip_text (GTK_WIDGET (button));
    if (tip)
        status_set (self, tip);
}

static void
on_tb_cut (GtkButton *button, gpointer user_data)
{
    (void) button;
    action_cut (NULL, user_data);
}

static void
on_tb_copy (GtkButton *button, gpointer user_data)
{
    (void) button;
    action_copy (NULL, user_data);
}

static void
on_tb_paste (GtkButton *button, gpointer user_data)
{
    (void) button;
    action_paste (NULL, user_data);
}

static GtkWidget *
create_toolbar (PnWindow *self)
{
    GtkWidget *toolbar;
    GtkWidget *separator;

    toolbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class (
            gtk_widget_get_style_context (toolbar), "toolbar");

    gtk_box_pack_start (GTK_BOX (toolbar),
        create_toolbar_button ("document-new", "New",
                               G_CALLBACK (on_tb_new), self),
        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (toolbar),
        create_toolbar_button ("document-open", "Open",
                               G_CALLBACK (on_tb_open), self),
        FALSE, FALSE, 0);
    self->btn_save = create_toolbar_button (
            "document-save", "Save",
            G_CALLBACK (on_tb_save), self);
    /* Mirrors the menu's Save sensitivity — both gates flip together
     * via the worksheet's "modified-changed" signal once the canvas
     * is wired up. */
    gtk_widget_set_sensitive (self->btn_save, FALSE);
    gtk_box_pack_start (GTK_BOX (toolbar), self->btn_save,
                        FALSE, FALSE, 0);

    separator = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start (GTK_BOX (toolbar), separator, FALSE, FALSE, 4);

    self->btn_undo = create_toolbar_button (
            "edit-undo", "Undo",
            G_CALLBACK (on_tb_stub), self);
    gtk_widget_set_sensitive (self->btn_undo, FALSE);
    gtk_box_pack_start (GTK_BOX (toolbar), self->btn_undo,
                        FALSE, FALSE, 0);

    self->btn_redo = create_toolbar_button (
            "edit-redo", "Redo",
            G_CALLBACK (on_tb_stub), self);
    gtk_widget_set_sensitive (self->btn_redo, FALSE);
    gtk_box_pack_start (GTK_BOX (toolbar), self->btn_redo,
                        FALSE, FALSE, 0);

    separator = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start (GTK_BOX (toolbar), separator, FALSE, FALSE, 4);

    self->btn_cut = create_toolbar_button (
            "edit-cut", "Cut",
            G_CALLBACK (on_tb_cut), self);
    gtk_widget_set_sensitive (self->btn_cut, FALSE);
    gtk_box_pack_start (GTK_BOX (toolbar), self->btn_cut,
                        FALSE, FALSE, 0);

    self->btn_copy = create_toolbar_button (
            "edit-copy", "Copy",
            G_CALLBACK (on_tb_copy), self);
    gtk_widget_set_sensitive (self->btn_copy, FALSE);
    gtk_box_pack_start (GTK_BOX (toolbar), self->btn_copy,
                        FALSE, FALSE, 0);

    self->btn_paste = create_toolbar_button (
            "edit-paste", "Paste",
            G_CALLBACK (on_tb_paste), self);
    gtk_box_pack_start (GTK_BOX (toolbar), self->btn_paste,
                        FALSE, FALSE, 0);

    return toolbar;
}

/* ------------------------------------------------------------------ */
/*  Debug pane                                                         */
/* ------------------------------------------------------------------ */

/** Build the right-hand debug pane: a scrolled #GtkListBox of
 *  expander rows, one per message.  The pane is hidden by default;
 *  the View → Debug View menu item toggles it. */
static void
on_debug_clear_clicked (
        GtkButton *button,
        gpointer   user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    GList    *children;
    GList    *l;
    (void) button;

    if (self->debug_list == NULL)
        return;

    children = gtk_container_get_children (GTK_CONTAINER (self->debug_list));
    for (l = children; l != NULL; l = l->next)
        gtk_widget_destroy (GTK_WIDGET (l->data));
    g_list_free (children);
}

/** Close the debug pane via the View → Debug View check item, so the
 *  menu checkmark, saved width, and "open" preference all stay in sync
 *  with action_toggle_debug_view rather than hiding the pane directly. */
static void
on_debug_close_clicked (
        GtkButton *button,
        gpointer   user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) button;

    if (self->menu_debug_view != NULL)
        gtk_check_menu_item_set_active (
                GTK_CHECK_MENU_ITEM (self->menu_debug_view), FALSE);
}

static GtkWidget *
create_debug_pane (PnWindow *self)
{
    GtkWidget *vbox;
    GtkWidget *clear_row;
    GtkWidget *search_row;
    GtkWidget *clear;
    GtkWidget *close_btn;
    GtkWidget *search;
    GtkWidget *prev_btn;
    GtkWidget *next_btn;
    GtkWidget *scrolled;

    self->debug_match_index = -1;

    self->debug_list = gtk_list_box_new ();
    /* No list-box selection: clicking a row (e.g. to expand a foldable)
     * must not highlight it.  The Next/Previous buttons mark the focused
     * match with the "debug-current-match" style class instead. */
    gtk_list_box_set_selection_mode (
            GTK_LIST_BOX (self->debug_list), GTK_SELECTION_NONE);
    gtk_list_box_set_filter_func (
            GTK_LIST_BOX (self->debug_list),
            debug_row_filter, self, NULL);

    /* Style for the row holding the focused search match.  Tinted with
     * the key colour (#3465a4) so it reads as "current" without hiding
     * the white-on-dark-blue match highlights drawn inside it. */
    {
        static GtkCssProvider *prov = NULL;

        if (prov == NULL)
        {
            prov = gtk_css_provider_new ();
            gtk_css_provider_load_from_data (
                    prov,
                    "row.debug-current-match { "
                    "background-color: rgba(52,101,164,0.30); }",
                    -1, NULL);
            gtk_style_context_add_provider_for_screen (
                    gdk_screen_get_default (),
                    GTK_STYLE_PROVIDER (prov),
                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
    }

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (
            GTK_SCROLLED_WINDOW (scrolled),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (scrolled), self->debug_list);

    /* Top row above the search: a Close button at the far right that
     * dismisses the whole pane, plus a Clear button that drops the
     * accumulated output without restarting the flow. */
    clear_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

    /* Packed end-first so it sits in the top-right corner of the pane. */
    close_btn = gtk_button_new_from_icon_name ("window-close-symbolic",
                                               GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (close_btn, "Close debug view");
    g_signal_connect (close_btn, "clicked",
                      G_CALLBACK (on_debug_close_clicked), self);
    gtk_box_pack_end (GTK_BOX (clear_row), close_btn, FALSE, FALSE, 0);

    clear = gtk_button_new_from_icon_name ("user-trash",
                                           GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (clear, "Clear all debug messages");
    g_signal_connect (clear, "clicked",
                      G_CALLBACK (on_debug_clear_clicked), self);
    gtk_box_pack_end (GTK_BOX (clear_row), clear, FALSE, FALSE, 0);

    /* Search row: a quick-search entry that filters and highlights the
     * rows as the user types (mirroring the palette's search box),
     * followed by Previous/Next buttons that step through the matches. */
    search_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);

    search = gtk_entry_new ();
    self->debug_search_entry = search;
    gtk_entry_set_icon_from_icon_name (GTK_ENTRY (search),
                                       GTK_ENTRY_ICON_PRIMARY,
                                       "edit-find-symbolic");
    gtk_entry_set_icon_activatable (GTK_ENTRY (search),
                                    GTK_ENTRY_ICON_PRIMARY, FALSE);
    gtk_entry_set_placeholder_text (GTK_ENTRY (search), "Search");
    g_signal_connect (search, "changed",
                      G_CALLBACK (on_debug_search_changed), self);
    /* Enter in the search box jumps to the next match, like a find bar. */
    g_signal_connect (search, "activate",
                      G_CALLBACK (on_debug_search_next), self);
    gtk_box_pack_start (GTK_BOX (search_row), search, TRUE, TRUE, 0);

    prev_btn = gtk_button_new_from_icon_name ("go-up",
                                              GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (prev_btn, "Previous match");
    g_signal_connect (prev_btn, "clicked",
                      G_CALLBACK (on_debug_search_prev), self);
    gtk_box_pack_start (GTK_BOX (search_row), prev_btn, FALSE, FALSE, 0);

    next_btn = gtk_button_new_from_icon_name ("go-down",
                                              GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (next_btn, "Next match");
    g_signal_connect (next_btn, "clicked",
                      G_CALLBACK (on_debug_search_next), self);
    gtk_box_pack_start (GTK_BOX (search_row), next_btn, FALSE, FALSE, 0);

    /* Match the palette's framing (gtk_box spacing 4 + border width 8)
     * so the search entry sits inset from the pane edges by the same
     * margin the palette gives its own search box. */
    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width (GTK_CONTAINER (vbox), 8);
    gtk_box_pack_start (GTK_BOX (vbox), clear_row,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (vbox), search_row, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (vbox), scrolled,   TRUE,  TRUE,  0);

    gtk_widget_set_size_request (vbox, 280, -1);

    return vbox;
}

/* Minimum on-screen width pack2 (the debug pane) must end up with
 * after a toggle-on, regardless of any saved position.  280 px
 * matches the gtk_widget_set_size_request() floor on the pane vbox
 * so a clamped position never makes the contents narrower than
 * their natural width — anything narrower forces #GtkPaned to
 * allocate pack2 with the requested 280 px at an off-screen X
 * coordinate (INT_MIN), the exact failure mode this clamp exists
 * to prevent. */
#define DEBUG_PANE_MIN_VISIBLE_WIDTH 280

/** Idle callback that places the editor #GtkPaned divider after a
 *  toggle-on of the debug pane.  Setting the position from inside
 *  the toggle handler races with GtkPaned's own size-allocate cycle
 *  that runs when pack2 transitions from hidden to visible — that
 *  cycle drifts the divider toward the right edge ("pack1 absorbs
 *  all") and clamps whatever we just set.  Deferring to the next
 *  idle lets the paned finish settling and our position then sticks.
 *
 *  We need this on EVERY toggle-on, not just the first, because
 *  while pack2 is hidden GtkPaned widens pack1 to fill the whole
 *  paned alloc — so if the user widens the main window between two
 *  toggle-on events, the second toggle-on would otherwise find the
 *  divider parked at the right edge of the new (wider) paned and
 *  pack2 would be allocated 280 px wide at an off-screen
 *  X coordinate, invisible to the user.
 *
 *  When the user has previously customised the divider (e.g. by
 *  dragging it to a 50/50 split), we restore that exact position
 *  rather than imposing the default 25 % slice, but always clamp it
 *  so pack2 ends up at least DEBUG_PANE_MIN_VISIBLE_WIDTH wide on
 *  screen — covers the case where the saved position was set on a
 *  wider window and the user has since narrowed it. */
static gboolean
restore_debug_pane_position (gpointer user_data)
{
    PnWindow      *self = PN_WINDOW (user_data);
    GtkAllocation  alloc;
    gint           pos;
    gint           max_pos;

    /* Give up if the pane vanished or was toggled back off before we
     * ran; clear the restore guard so normal width tracking resumes. */
    if (self->debug_paned == NULL || self->debug_pane == NULL
            || !gtk_widget_get_visible (self->debug_pane))
    {
        self->debug_pane_restoring = FALSE;
        return G_SOURCE_REMOVE;
    }

    /* At startup this idle can fire before the window is mapped, when
     * the paned still has no allocation.  Retry on the next iteration
     * rather than giving up, so the saved width is honoured even on
     * the first frame. */
    gtk_widget_get_allocation (self->debug_paned, &alloc);
    if (alloc.width <= 0)
        return G_SOURCE_CONTINUE;

    /* Place the divider so the pane ends up debug_pane_width px wide,
     * falling back to a ~25 % slice when no width has been recorded. */
    if (self->debug_pane_width > 0)
        pos = alloc.width - self->debug_pane_width;
    else
        pos = alloc.width - alloc.width / 4;

    max_pos = alloc.width - DEBUG_PANE_MIN_VISIBLE_WIDTH;
    if (pos > max_pos) pos = max_pos;
    if (pos < 0)       pos = 0;

    gtk_paned_set_position (GTK_PANED (self->debug_paned), pos);

    /* Saved width applied — resume recording user divider drags. */
    self->debug_pane_restoring = FALSE;
    return G_SOURCE_REMOVE;
}

/** Persist the debug pane width whenever the user drags the divider.
 *  Only meaningful while the pane is visible — when it is hidden the
 *  paned drifts the divider on its own, which we must not record. */
static void
on_debug_paned_position_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnWindow      *self = PN_WINDOW (user_data);
    GtkAllocation  alloc;
    gint           pos;
    gint           width;
    (void) object;
    (void) pspec;

    if (self->debug_pane_restoring
            || self->debug_pane == NULL
            || !gtk_widget_get_visible (self->debug_pane))
        return;

    gtk_widget_get_allocation (self->debug_paned, &alloc);
    if (alloc.width <= 0)
        return;

    pos   = gtk_paned_get_position (GTK_PANED (self->debug_paned));
    width = alloc.width - pos;
    if (width <= 0)
        return;

    self->debug_pane_width = width;
    pn_preferences_set_debug_view_width (pn_preferences_get_default (),
                                         width);
}

/** Persist the main window size on every resize so the next launch
 *  reopens at the same dimensions.  Skips the maximized state — saving
 *  the maximized size would make a later un-maximize restore to that
 *  oversized geometry.  Returns %FALSE to let the event propagate. */
static gboolean
on_window_configure (
        GtkWidget         *widget,
        GdkEventConfigure *event,
        gpointer           user_data)
{
    PnWindow *self = PN_WINDOW (widget);
    gint      w = 0;
    gint      h = 0;
    (void) event;
    (void) user_data;

    if (!gtk_widget_get_realized (widget)
            || gtk_window_is_maximized (GTK_WINDOW (self)))
        return GDK_EVENT_PROPAGATE;

    gtk_window_get_size (GTK_WINDOW (self), &w, &h);
    if (w > 0 && h > 0)
        pn_preferences_set_window_size (pn_preferences_get_default (),
                                        w, h);

    return GDK_EVENT_PROPAGATE;
}

static void
action_toggle_debug_view (
        GtkCheckMenuItem *item,
        gpointer          user_data)
{
    PnWindow *self    = PN_WINDOW (user_data);
    gboolean  active  = gtk_check_menu_item_get_active (item);

    if (self->debug_pane == NULL)
        return;

    /* Remember the open/closed choice across restarts. */
    pn_preferences_set_debug_view_open (pn_preferences_get_default (),
                                        active);

    if (active)
    {
        /* Guard before showing: the pane's first allocation hands out
         * a transient natural width whose notify::position must not be
         * mistaken for a user-chosen size. */
        self->debug_pane_restoring = TRUE;
        gtk_widget_show (self->debug_pane);
        if (self->debug_paned != NULL)
            g_idle_add (restore_debug_pane_position, self);
    }
    else
    {
        /* Capture the pane width the user is currently looking at so
         * the next toggle-on restores the same size.  Do this before
         * hiding pack2 — once it is hidden the paned starts drifting
         * its divider toward the right edge as the window resizes and
         * gtk_paned_get_position() would no longer report what the
         * user saw. */
        if (self->debug_paned != NULL
                && gtk_widget_get_visible (self->debug_pane))
        {
            GtkAllocation alloc;
            gint          pos;

            gtk_widget_get_allocation (self->debug_paned, &alloc);
            pos = gtk_paned_get_position (GTK_PANED (self->debug_paned));
            if (alloc.width > 0 && alloc.width - pos > 0)
            {
                self->debug_pane_width = alloc.width - pos;
                pn_preferences_set_debug_view_width (
                        pn_preferences_get_default (),
                        self->debug_pane_width);
            }
        }
        gtk_widget_hide (self->debug_pane);
    }
}

/* ------------------------------------------------------------------ */
/*  Sheet tab strip                                                    */
/* ------------------------------------------------------------------ */

/** Find the notebook page (the per-sheet container box) whose
 *  embedded #PnWorksheet renders @sheet_name.  Returns %NULL when no
 *  page matches — happens for the brief window between
 *  pn_flow_remove_sheet emitting "sheet-removed" and the host
 *  finishing the matching gtk_notebook_remove_page below. */
static GtkWidget *
pn_window_find_sheet_page (
        PnWindow    *self,
        const gchar *sheet_name)
{
    const gint n = gtk_notebook_get_n_pages (GTK_NOTEBOOK (self->notebook));
    gint       i;

    if (sheet_name == NULL)
        return NULL;

    for (i = 0; i < n; i++)
    {
        GtkWidget *page = gtk_notebook_get_nth_page (
                GTK_NOTEBOOK (self->notebook), i);
        PnWorksheet *ws;

        if (page == NULL)
            continue;

        ws = g_object_get_data (G_OBJECT (page), "pn-worksheet");
        if (ws == NULL)
            continue;
        if (g_strcmp0 (pn_worksheet_get_sheet_name (ws), sheet_name) == 0)
            return page;
    }
    return NULL;
}

/** Return the worksheet on the currently-selected tab, or %NULL when
 *  the notebook is empty (transient state during the cascade of a
 *  multi-sheet remove).  Borrowed pointer. */
static PnWorksheet *
pn_window_active_worksheet (PnWindow *self)
{
    gint       page_num;
    GtkWidget *page;

    if (self == NULL || self->notebook == NULL)
        return NULL;

    page_num = gtk_notebook_get_current_page (GTK_NOTEBOOK (self->notebook));
    if (page_num < 0)
        return NULL;

    page = gtk_notebook_get_nth_page (GTK_NOTEBOOK (self->notebook), page_num);
    if (page == NULL)
        return NULL;

    return g_object_get_data (G_OBJECT (page), "pn-worksheet");
}

/** Build a tab containing palette + separator + scrolled + worksheet,
 *  attach it to the notebook with a clickable label, and return the
 *  page widget.  The caller may immediately switch to it via
 *  gtk_notebook_set_current_page() when constructed in response to a
 *  user action; at startup the notebook just defaults to the first
 *  page. */
static GtkWidget *
pn_window_append_sheet_tab (
        PnWindow    *self,
        const gchar *sheet_name)
{
    GtkWidget *tab_box;
    GtkWidget *palette;
    GtkWidget *separator;
    GtkWidget *scrolled;
    GtkWidget *worksheet;
    GtkWidget *label;
    GtkWidget *event_box;
    gint       index;

    tab_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);

    palette = pn_palette_new ();
    g_signal_connect (palette, "help-requested",
                      G_CALLBACK (on_palette_help_requested), self);
    gtk_box_pack_start (GTK_BOX (tab_box), palette, FALSE, FALSE, 0);

    separator = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start (GTK_BOX (tab_box), separator, FALSE, FALSE, 0);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (
            GTK_SCROLLED_WINDOW (scrolled),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (tab_box), scrolled, TRUE, TRUE, 0);

    worksheet = pn_worksheet_new_for_flow (self->flow, sheet_name);
    gtk_container_add (GTK_CONTAINER (scrolled), worksheet);

    /* Stash a borrowed pointer to the worksheet on the page widget so
     * pn_window_find_sheet_page / active_worksheet can look it up
     * without re-walking the container hierarchy on every event. */
    g_object_set_data (G_OBJECT (tab_box), "pn-worksheet", worksheet);

    /* Tab label wrapped in an event box so right-click can pop the
     * sheet context menu (rename / delete / reorder). */
    label = gtk_label_new (sheet_name);
    event_box = gtk_event_box_new ();
    gtk_event_box_set_visible_window (GTK_EVENT_BOX (event_box), FALSE);
    gtk_container_add (GTK_CONTAINER (event_box), label);
    gtk_widget_show_all (event_box);
    g_object_set_data (G_OBJECT (event_box), "pn-tab-label",  label);
    g_object_set_data (G_OBJECT (event_box), "pn-window",     self);
    g_object_set_data (G_OBJECT (event_box), "pn-sheet-page", tab_box);
    gtk_widget_add_events (event_box, GDK_BUTTON_PRESS_MASK);
    g_signal_connect (event_box, "button-press-event",
                      G_CALLBACK (on_tab_label_button_press), self);

    index = gtk_notebook_append_page (GTK_NOTEBOOK (self->notebook),
                                      tab_box, event_box);
    gtk_notebook_set_tab_reorderable (GTK_NOTEBOOK (self->notebook),
                                      tab_box, TRUE);

    /* Wire the per-page worksheet's selection-changed up to the
     * window so Cut/Copy can grey out exactly when the active page's
     * selection is empty.  modified-changed sits on the flow itself
     * (it is per-file, not per-sheet) so the window only listens on
     * the flow once. */
    g_signal_connect (worksheet, "selection-changed",
                      G_CALLBACK (on_worksheet_selection_changed), self);
    g_signal_connect (worksheet, "status-message",
                      G_CALLBACK (on_worksheet_status_message), self);
    g_signal_connect (worksheet, "debug-message",
                      G_CALLBACK (on_worksheet_debug_message), self);
    /* A node's right-click "Help" entry routes through the same
     * forwarder as the palette's: both carry a #GType name anchor and
     * want the per-node help page opened. */
    g_signal_connect (worksheet, "help-requested",
                      G_CALLBACK (on_palette_help_requested), self);

    gtk_widget_show_all (tab_box);
    (void) index;
    return tab_box;
}

static void
on_notebook_switch_page (
        GtkNotebook *notebook,
        GtkWidget   *page,
        guint        page_num,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    PnWorksheet *ws;
    (void) notebook; (void) page_num;

    if (page == NULL)
        return;

    ws = g_object_get_data (G_OBJECT (page), "pn-worksheet");
    self->worksheet = (GtkWidget *) ws;

    /* Refresh Cut/Copy sensitivity to track the active sheet's
     * selection (each sheet has its own selection).  A non-worksheet
     * page (the panel editor tab) has nothing to cut or copy, so grey
     * both out while it is shown. */
    if (ws != NULL)
    {
        on_worksheet_selection_changed (ws, self);

        /* Tell the flow which sheet is now active so the next save
         * persists the change. */
        pn_flow_set_active_sheet (self->flow,
                                  pn_worksheet_get_sheet_name (ws));
    }
    else
    {
        gtk_widget_set_sensitive (self->menu_cut,  FALSE);
        gtk_widget_set_sensitive (self->menu_copy, FALSE);
        gtk_widget_set_sensitive (self->btn_cut,   FALSE);
        gtk_widget_set_sensitive (self->btn_copy,  FALSE);
    }
}

/** Modal one-line text prompt shared by New Sheet and Rename Sheet.
 *  Returns %NULL when the user cancelled or entered an empty name;
 *  otherwise a freshly-allocated string the caller frees. */
static gchar *
prompt_for_sheet_name (
        PnWindow    *self,
        const gchar *title,
        const gchar *prompt,
        const gchar *initial)
{
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *label;
    GtkWidget *entry;
    gint       response;
    gchar     *out = NULL;

    dialog = gtk_dialog_new_with_buttons (
            title,
            GTK_WINDOW (self),
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_OK",     GTK_RESPONSE_ACCEPT,
            NULL);
    gtk_dialog_set_default_response (GTK_DIALOG (dialog), GTK_RESPONSE_ACCEPT);

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_box_set_spacing (GTK_BOX (content), 8);
    gtk_container_set_border_width (GTK_CONTAINER (content), 12);

    label = gtk_label_new (prompt);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (content), label, FALSE, FALSE, 0);

    entry = gtk_entry_new ();
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    if (initial != NULL)
        gtk_entry_set_text (GTK_ENTRY (entry), initial);
    gtk_box_pack_start (GTK_BOX (content), entry, FALSE, FALSE, 0);
    gtk_widget_grab_focus (entry);

    gtk_widget_show_all (dialog);
    response = gtk_dialog_run (GTK_DIALOG (dialog));

    if (response == GTK_RESPONSE_ACCEPT)
    {
        const gchar *text = gtk_entry_get_text (GTK_ENTRY (entry));
        if (text != NULL && *text != '\0')
            out = g_strdup (text);
    }

    gtk_widget_destroy (dialog);
    return out;
}

static void
on_new_sheet_clicked (GtkButton *button, gpointer user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    gchar    *name;
    gchar    *placeholder = NULL;
    guint     n = 0;
    guint     suffix;
    (void) button;

    /* Pre-fill the entry with the next "Sheet N" so the common
     * "click + Enter" gesture creates a fresh sheet without typing. */
    (void) pn_flow_get_sheets (self->flow, &n);
    for (suffix = n + 1; ; suffix++)
    {
        gchar       *candidate = g_strdup_printf ("Sheet %u", suffix);
        const guint  m = n;
        guint        i;
        gboolean     hit = FALSE;
        const gchar *const *names = pn_flow_get_sheets (self->flow, NULL);
        for (i = 0; i < m; i++)
            if (g_strcmp0 (names[i], candidate) == 0)
            { hit = TRUE; break; }
        if (!hit)
        {
            placeholder = candidate;
            break;
        }
        g_free (candidate);
    }

    name = prompt_for_sheet_name (self,
                                  "Add Sheet",
                                  "Name the new sheet:",
                                  placeholder);
    g_free (placeholder);
    if (name == NULL)
        return;

    if (!pn_flow_add_sheet (self->flow, name))
    {
        GtkWidget *err = gtk_message_dialog_new (
                GTK_WINDOW (self),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE,
                "A sheet named \"%s\" already exists.", name);
        gtk_dialog_run (GTK_DIALOG (err));
        gtk_widget_destroy (err);
    }
    else
    {
        /* Switch to the newly-added tab so the user can start
         * working in it immediately. */
        GtkWidget *page = pn_window_find_sheet_page (self, name);
        if (page != NULL)
            gtk_notebook_set_current_page (
                    GTK_NOTEBOOK (self->notebook),
                    gtk_notebook_page_num (
                            GTK_NOTEBOOK (self->notebook), page));
    }
    g_free (name);
}

/* The trailing "+" tab button opens a small menu rather than acting
 * directly, so the user can add either a node-flow sheet or the single
 * panel-applet GUI layout editor.  The menu is rebuilt on every click,
 * which lets the panel entry reflect the live one-instance state without
 * the window having to hold onto a menu-item handle. */
static void
on_add_tab_button_clicked (GtkButton *button, gpointer user_data)
{
    PnWindow  *self = PN_WINDOW (user_data);
    GtkWidget *menu;
    GtkWidget *sheet_item;
    GtkWidget *panel_item;

    menu = gtk_menu_new ();

    sheet_item = gtk_menu_item_new_with_mnemonic ("Add _Sheet");
    g_signal_connect (sheet_item, "activate",
                      G_CALLBACK (on_new_sheet_clicked), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), sheet_item);

    panel_item = gtk_menu_item_new_with_mnemonic ("Add _Panel Applet Layout");
    /* Only one panel editor at a time: grey the entry out while a tab
     * already exists.  The close button re-enables it by clearing
     * self->panel_editor_page. */
    gtk_widget_set_sensitive (panel_item, self->panel_editor_page == NULL);
    g_signal_connect (panel_item, "activate",
                      G_CALLBACK (on_add_panel_editor_activate), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), panel_item);

    gtk_widget_show_all (menu);
    gtk_menu_popup_at_widget (GTK_MENU (menu),
                              GTK_WIDGET (button),
                              GDK_GRAVITY_SOUTH_WEST,
                              GDK_GRAVITY_NORTH_WEST,
                              NULL);
}

static void
on_add_panel_editor_activate (GtkMenuItem *item, gpointer user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) item;

    pn_window_add_panel_editor_tab (self);
}

/* Close handler for the panel editor tab's "✕" button.  Pulls the page
 * out of the notebook and clears the one-instance guard so the "+" menu
 * offers the entry again. */
static void
on_panel_editor_close_clicked (GtkButton *button, gpointer user_data)
{
    PnWindow  *self = PN_WINDOW (user_data);
    GtkWidget *page = g_object_get_data (G_OBJECT (button), "pn-panel-page");
    gint       idx;

    if (page == NULL)
        return;

    idx = gtk_notebook_page_num (GTK_NOTEBOOK (self->notebook), page);
    if (idx >= 0)
        gtk_notebook_remove_page (GTK_NOTEBOOK (self->notebook), idx);

    if (page == self->panel_editor_page)
        self->panel_editor_page = NULL;

    status_set (self, "Closed panel applet layout");
}

/* Append (or, if one somehow already exists, just reveal) the single
 * panel-applet GUI layout editor tab.  The page is a scrolled window
 * wrapping a #PnPanelEditor; its tab label carries a close button. */
static void
pn_window_add_panel_editor_tab (PnWindow *self)
{
    GtkWidget *scrolled;
    GtkWidget *editor;
    GtkWidget *label_box;
    GtkWidget *label;
    GtkWidget *close_btn;
    gint       index;

    if (self->panel_editor_page != NULL)
    {
        gtk_notebook_set_current_page (
                GTK_NOTEBOOK (self->notebook),
                gtk_notebook_page_num (GTK_NOTEBOOK (self->notebook),
                                       self->panel_editor_page));
        return;
    }

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (
            GTK_SCROLLED_WINDOW (scrolled),
            GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);

    editor = pn_panel_editor_new (self->flow);
    gtk_container_add (GTK_CONTAINER (scrolled), editor);

    /* Tab label: a title plus a close button, so the user can dismiss
     * the editor and create a new one later (unlike the worksheet tabs,
     * which are removed through the sheet context menu). */
    label_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    label = gtk_label_new ("Panel Applet");
    gtk_box_pack_start (GTK_BOX (label_box), label, FALSE, FALSE, 0);

    close_btn = gtk_button_new_from_icon_name ("window-close-symbolic",
                                               GTK_ICON_SIZE_MENU);
    gtk_button_set_relief (GTK_BUTTON (close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_focus_on_click (close_btn, FALSE);
    gtk_widget_set_tooltip_text (close_btn, "Close the panel applet layout");
    g_object_set_data (G_OBJECT (close_btn), "pn-panel-page", scrolled);
    g_signal_connect (close_btn, "clicked",
                      G_CALLBACK (on_panel_editor_close_clicked), self);
    gtk_box_pack_start (GTK_BOX (label_box), close_btn, FALSE, FALSE, 0);
    gtk_widget_show_all (label_box);

    index = gtk_notebook_append_page (GTK_NOTEBOOK (self->notebook),
                                      scrolled, label_box);
    gtk_notebook_set_tab_reorderable (GTK_NOTEBOOK (self->notebook),
                                      scrolled, TRUE);

    self->panel_editor_page = scrolled;

    gtk_widget_show_all (scrolled);
    gtk_notebook_set_current_page (GTK_NOTEBOOK (self->notebook), index);
    status_set (self, "Added panel applet layout");
}

static void
sheet_action_rename (GtkMenuItem *item, gpointer user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    const gchar *from;
    gchar       *to;

    from = g_object_get_data (G_OBJECT (item), "pn-sheet-name");
    if (from == NULL)
        return;

    to = prompt_for_sheet_name (self, "Rename Sheet", "New name:", from);
    if (to == NULL)
        return;

    if (!pn_flow_rename_sheet (self->flow, from, to))
    {
        GtkWidget *err = gtk_message_dialog_new (
                GTK_WINDOW (self),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE,
                "Could not rename sheet \"%s\" to \"%s\".",
                from, to);
        gtk_dialog_run (GTK_DIALOG (err));
        gtk_widget_destroy (err);
    }
    g_free (to);
}

static void
sheet_action_delete (GtkMenuItem *item, gpointer user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    const gchar *name;
    GtkWidget   *confirm;
    gint         response;
    guint        n = 0;

    name = g_object_get_data (G_OBJECT (item), "pn-sheet-name");
    if (name == NULL)
        return;

    (void) pn_flow_get_sheets (self->flow, &n);
    if (n <= 1)
    {
        GtkWidget *err = gtk_message_dialog_new (
                GTK_WINDOW (self),
                GTK_DIALOG_MODAL,
                GTK_MESSAGE_ERROR,
                GTK_BUTTONS_CLOSE,
                "A worksheet always carries at least one sheet.");
        gtk_dialog_run (GTK_DIALOG (err));
        gtk_widget_destroy (err);
        return;
    }

    confirm = gtk_message_dialog_new (
            GTK_WINDOW (self),
            GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_OK_CANCEL,
            "Delete sheet \"%s\" and every node it contains?",
            name);
    response = gtk_dialog_run (GTK_DIALOG (confirm));
    gtk_widget_destroy (confirm);

    if (response == GTK_RESPONSE_OK)
        (void) pn_flow_remove_sheet (self->flow, name);
}

/** Move the sheet @name one slot left or right in the tab order. */
static void
sheet_action_move (
        PnWindow    *self,
        const gchar *name,
        gint         delta)
{
    guint                n = 0;
    guint                i;
    gint                 cur = -1;
    gint                 dst;
    const gchar *const  *sheets = pn_flow_get_sheets (self->flow, &n);

    for (i = 0; i < n; i++)
        if (g_strcmp0 (sheets[i], name) == 0)
        { cur = (gint) i; break; }
    if (cur < 0)
        return;

    dst = cur + delta;
    if (dst < 0)            dst = 0;
    if (dst >= (gint) n)    dst = (gint) n - 1;
    if (dst == cur)
        return;

    pn_flow_reorder_sheet (self->flow, name, (guint) dst);
}

static void
sheet_action_move_left (GtkMenuItem *item, gpointer user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    const gchar *name = g_object_get_data (G_OBJECT (item), "pn-sheet-name");
    if (name != NULL)
        sheet_action_move (self, name, -1);
}

static void
sheet_action_move_right (GtkMenuItem *item, gpointer user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    const gchar *name = g_object_get_data (G_OBJECT (item), "pn-sheet-name");
    if (name != NULL)
        sheet_action_move (self, name, +1);
}

static gboolean
on_tab_label_button_press (
        GtkWidget      *event_box,
        GdkEventButton *event,
        gpointer        user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    GtkWidget   *page;
    PnWorksheet *ws;
    GtkWidget   *menu;
    GtkWidget   *rename_item;
    GtkWidget   *delete_item;
    GtkWidget   *left_item;
    GtkWidget   *right_item;
    const gchar *sheet_name;

    if (event->type != GDK_BUTTON_PRESS || event->button != 3)
        return GDK_EVENT_PROPAGATE;

    page = g_object_get_data (G_OBJECT (event_box), "pn-sheet-page");
    if (page == NULL)
        return GDK_EVENT_PROPAGATE;

    ws = g_object_get_data (G_OBJECT (page), "pn-worksheet");
    if (ws == NULL)
        return GDK_EVENT_PROPAGATE;

    sheet_name = pn_worksheet_get_sheet_name (ws);

    menu        = gtk_menu_new ();
    rename_item = gtk_menu_item_new_with_label ("Rename…");
    delete_item = gtk_menu_item_new_with_label ("Delete");
    left_item   = gtk_menu_item_new_with_label ("Move Left");
    right_item  = gtk_menu_item_new_with_label ("Move Right");

    /* Stash the sheet name on each menu item so the action handlers
     * know which sheet was right-clicked even after the menu loses
     * track of its attach widget. */
    g_object_set_data_full (G_OBJECT (rename_item), "pn-sheet-name",
                            g_strdup (sheet_name), g_free);
    g_object_set_data_full (G_OBJECT (delete_item), "pn-sheet-name",
                            g_strdup (sheet_name), g_free);
    g_object_set_data_full (G_OBJECT (left_item),   "pn-sheet-name",
                            g_strdup (sheet_name), g_free);
    g_object_set_data_full (G_OBJECT (right_item),  "pn-sheet-name",
                            g_strdup (sheet_name), g_free);

    g_signal_connect (rename_item, "activate",
                      G_CALLBACK (sheet_action_rename), self);
    g_signal_connect (delete_item, "activate",
                      G_CALLBACK (sheet_action_delete), self);
    g_signal_connect (left_item, "activate",
                      G_CALLBACK (sheet_action_move_left), self);
    g_signal_connect (right_item, "activate",
                      G_CALLBACK (sheet_action_move_right), self);

    gtk_menu_shell_append (GTK_MENU_SHELL (menu), rename_item);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), left_item);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), right_item);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), gtk_separator_menu_item_new ());
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), delete_item);
    gtk_widget_show_all (menu);

    gtk_menu_popup_at_pointer (GTK_MENU (menu), (GdkEvent *) event);
    return GDK_EVENT_STOP;
}

static void
on_flow_sheet_added (
        PnFlow      *flow,
        const gchar *name,
        gpointer     user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) flow;

    if (pn_window_find_sheet_page (self, name) != NULL)
        return;     /* already mapped (defensive) */

    pn_window_append_sheet_tab (self, name);
}

static void
on_flow_sheet_removed (
        PnFlow      *flow,
        const gchar *name,
        gpointer     user_data)
{
    PnWindow  *self = PN_WINDOW (user_data);
    GtkWidget *page = pn_window_find_sheet_page (self, name);
    gint       page_num;
    (void) flow;

    if (page == NULL)
        return;

    page_num = gtk_notebook_page_num (GTK_NOTEBOOK (self->notebook), page);
    if (page_num >= 0)
        gtk_notebook_remove_page (GTK_NOTEBOOK (self->notebook), page_num);
}

static void
on_flow_sheet_renamed (
        PnFlow      *flow,
        const gchar *from,
        const gchar *to,
        gpointer     user_data)
{
    PnWindow    *self = PN_WINDOW (user_data);
    GtkWidget   *page;
    GtkWidget   *event_box;
    GtkWidget   *label;
    PnWorksheet *ws;
    (void) flow;

    /* The flow already updated its sheets[] entry and every affected
     * node's worksheet tag before emitting; the per-page worksheet
     * widgets, on the other hand, still carry the old @from name in
     * their internal filter.  Find the page by @from (which is what
     * the widget still reports) and repoint the widget at @to in
     * lockstep with the label.  Without the repoint the widget keeps
     * filtering on @from and the canvas appears to lose every node. */
    page = pn_window_find_sheet_page (self, from);
    if (page == NULL)
    {
        /* Defensive: maybe the widget has already been re-pointed by
         * a previous emission (idempotency), in which case @to
         * resolves it. */
        page = pn_window_find_sheet_page (self, to);
    }
    if (page == NULL)
        return;

    ws = g_object_get_data (G_OBJECT (page), "pn-worksheet");
    if (ws != NULL)
        pn_worksheet_set_sheet_name (ws, to);

    event_box = gtk_notebook_get_tab_label (GTK_NOTEBOOK (self->notebook), page);
    if (event_box == NULL)
        return;
    label = g_object_get_data (G_OBJECT (event_box), "pn-tab-label");
    if (label != NULL)
        gtk_label_set_text (GTK_LABEL (label), to);
}

static void
on_flow_sheets_reordered (
        PnFlow   *flow,
        gpointer  user_data)
{
    PnWindow            *self = PN_WINDOW (user_data);
    guint                n = 0;
    guint                i;
    const gchar *const  *sheets;
    (void) flow;

    sheets = pn_flow_get_sheets (self->flow, &n);
    for (i = 0; i < n; i++)
    {
        GtkWidget *page = pn_window_find_sheet_page (self, sheets[i]);
        if (page == NULL)
            continue;
        gtk_notebook_reorder_child (GTK_NOTEBOOK (self->notebook),
                                    page, (gint) i);
    }
}

static void
on_flow_active_sheet_changed (
        PnFlow      *flow,
        const gchar *name,
        gpointer     user_data)
{
    PnWindow  *self = PN_WINDOW (user_data);
    GtkWidget *page = pn_window_find_sheet_page (self, name);
    gint       page_num;
    (void) flow;

    if (page == NULL)
        return;

    page_num = gtk_notebook_page_num (GTK_NOTEBOOK (self->notebook), page);
    if (page_num < 0)
        return;

    if (gtk_notebook_get_current_page (GTK_NOTEBOOK (self->notebook)) != page_num)
        gtk_notebook_set_current_page (GTK_NOTEBOOK (self->notebook), page_num);
}

/** Mirror the flow's "has unsaved changes" flag onto the Save toolbar
 *  button and File → Save menu item, plus a leading asterisk in the
 *  window title.  Save As is left always-on so the user can still
 *  write to a different filename even when the in-memory file
 *  matches the source on disk. */
/** Recompute the window title from `self->current_path` and the
 *  current modified state of the flow.  Shape:
 *
 *      Pipnode                       — no file loaded, unmodified
 *      *Pipnode                      — no file loaded, modified
 *      Pipnode - worksheet.json      — file loaded, unmodified
 *      *Pipnode - worksheet.json     — file loaded, modified
 *
 *  Only the basename of `current_path` is shown; the directory parts
 *  are deliberately stripped so the titlebar stays readable even when
 *  the worksheet lives several levels deep in a project tree. */
static void
update_window_title (PnWindow *self)
{
    gboolean  modified = pn_flow_is_modified (self->flow);
    gchar    *base     = NULL;
    gchar    *title;

    if (self->current_path != NULL)
        base = g_path_get_basename (self->current_path);

    if (base != NULL && *base != '\0')
        title = g_strdup_printf ("%sPipnode - %s",
                                 modified ? "*" : "", base);
    else
        title = g_strdup_printf ("%sPipnode", modified ? "*" : "");

    gtk_window_set_title (GTK_WINDOW (self), title);

    g_free (title);
    g_free (base);
}

static void
on_flow_modified_changed (
        PnFlow   *flow,
        gboolean  modified,
        gpointer  user_data)
{
    PnWindow *self = PN_WINDOW (user_data);
    (void) flow;

    gtk_widget_set_sensitive (self->menu_save, modified);
    gtk_widget_set_sensitive (self->btn_save,  modified);

    update_window_title (self);
}

/* Guard against losing unsaved work.  Connected to "delete-event",
 * which fires for both the window-manager close button and
 * gtk_window_close() (File -> Quit, Ctrl+Q).  Returning %TRUE vetoes
 * the close; %FALSE lets it proceed to destroy the window. */
static gboolean
on_window_delete_event (
        GtkWidget *widget,
        GdkEvent  *event,
        gpointer   user_data)
{
    PnWindow *self = PN_WINDOW (widget);
    (void) event;
    (void) user_data;

    /* A panel-backed window is a view onto a flow that must keep running
     * after the user closes it.  Autosave silently (the worksheet always
     * has a path, and a panel applet should survive an engine restart
     * with the user's edits) then hide instead of destroying, and veto
     * the close so the window and its flow live on. */
    if (self->panel_backed)
    {
        pn_window_autosave (self);
        gtk_widget_hide (GTK_WIDGET (self));
        return TRUE;
    }

    /* "delete-event" semantics are the inverse of the helper: returning
     * %TRUE vetoes the close, %FALSE lets it proceed.  Veto exactly when
     * the user did not agree to discard or save the changes. */
    return !confirm_discard_changes (self);
}

/* ------------------------------------------------------------------ */
/*  Window construction                                                */
/* ------------------------------------------------------------------ */

static void
pn_window_constructed (GObject *object)
{
    PnWindow      *self  = PN_WINDOW (object);
    PnPreferences *prefs = pn_preferences_get_default ();
    GtkWidget *vbox;
    GtkWidget *new_sheet_btn;
    gint       win_w = 1024;
    gint       win_h = 720;

    G_OBJECT_CLASS (pn_window_parent_class)->constructed (object);

    /* Suppress debug-pane width bookkeeping for the whole of
     * construction.  gtk_widget_show_all() below briefly maps the pane
     * before we hide it, and the paned is already allocated by then, so
     * the resulting notify::position would otherwise record the pane's
     * transient natural width over the saved one.  The guard is lifted
     * at the end of this function (by the restore idle when the pane is
     * reopened, or directly when it stays closed). */
    self->debug_pane_restoring = TRUE;

    gtk_window_set_title (GTK_WINDOW (self), "Pipnode");

    /* Restore the saved window size, and remember the recorded debug
     * pane width so the first toggle-on reopens it at that size. */
    pn_preferences_get_window_size (prefs, &win_w, &win_h);
    gtk_window_set_default_size (GTK_WINDOW (self), win_w, win_h);
    self->debug_pane_width = pn_preferences_get_debug_view_width (prefs);

    g_signal_connect (self, "configure-event",
                      G_CALLBACK (on_window_configure), NULL);
    g_signal_connect (self, "delete-event",
                      G_CALLBACK (on_window_delete_event), NULL);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (self), vbox);

    self->menubar = create_menubar (self);
    gtk_box_pack_start (GTK_BOX (vbox), self->menubar, FALSE, FALSE, 0);

    self->toolbar = create_toolbar (self);
    gtk_box_pack_start (GTK_BOX (vbox), self->toolbar, FALSE, FALSE, 0);

    /* Editor row: a horizontal pane that carries the document
     * notebook on the left and the (optionally-visible) debug pane
     * on the right.  The notebook hosts one tab per spreadsheet-style
     * sheet, all backed by a single shared #PnFlow. */
    self->debug_paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start (GTK_BOX (vbox), self->debug_paned, TRUE, TRUE, 0);
    g_signal_connect (self->debug_paned, "notify::position",
                      G_CALLBACK (on_debug_paned_position_notify), self);

    self->flow     = pn_flow_new ();
    self->notebook = gtk_notebook_new ();
    gtk_notebook_set_scrollable (GTK_NOTEBOOK (self->notebook), TRUE);
    gtk_paned_pack1 (GTK_PANED (self->debug_paned),
                     self->notebook, TRUE, FALSE);

    /* Trailing "+" button for adding a sheet, sitting at the right
     * edge of the tab strip exactly the way LibreOffice Calc and the
     * GNOME Builder editor place theirs. */
    new_sheet_btn = gtk_button_new_from_icon_name (
            "list-add-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text (new_sheet_btn,
                                 "Add a sheet or panel applet layout");
    gtk_button_set_relief       (GTK_BUTTON (new_sheet_btn), GTK_RELIEF_NONE);
    g_signal_connect (new_sheet_btn, "clicked",
                      G_CALLBACK (on_add_tab_button_clicked), self);
    gtk_widget_show (new_sheet_btn);
    gtk_notebook_set_action_widget (GTK_NOTEBOOK (self->notebook),
                                    new_sheet_btn, GTK_PACK_END);

    g_signal_connect (self->notebook, "switch-page",
                      G_CALLBACK (on_notebook_switch_page), self);

    /* Build one tab per sheet currently in the flow.  A fresh flow
     * carries exactly one sheet named "Worksheet"; on file load the
     * sheet-added signal handler grows the tab strip to match. */
    {
        guint                 i;
        guint                 n      = 0;
        const gchar *const   *sheets = pn_flow_get_sheets (self->flow, &n);
        for (i = 0; i < n; i++)
            pn_window_append_sheet_tab (self, sheets[i]);
    }
    self->worksheet = (GtkWidget *) pn_window_active_worksheet (self);

    /* Connect the per-flow signal that drive the tab strip and Save UI. */
    g_signal_connect (self->flow, "sheet-added",
                      G_CALLBACK (on_flow_sheet_added), self);
    g_signal_connect (self->flow, "sheet-removed",
                      G_CALLBACK (on_flow_sheet_removed), self);
    g_signal_connect (self->flow, "sheet-renamed",
                      G_CALLBACK (on_flow_sheet_renamed), self);
    g_signal_connect (self->flow, "sheets-reordered",
                      G_CALLBACK (on_flow_sheets_reordered), self);
    g_signal_connect (self->flow, "active-sheet-changed",
                      G_CALLBACK (on_flow_active_sheet_changed), self);
    g_signal_connect (self->flow, "modified-changed",
                      G_CALLBACK (on_flow_modified_changed), self);

    self->debug_pane = create_debug_pane (self);
    gtk_paned_pack2 (GTK_PANED (self->debug_paned),
                     self->debug_pane, FALSE, TRUE);

    self->statusbar = gtk_statusbar_new ();
    gtk_box_pack_start (GTK_BOX (vbox), self->statusbar, FALSE, FALSE, 0);
    gtk_statusbar_push (GTK_STATUSBAR (self->statusbar), 0, "Ready");

    gtk_widget_show_all (GTK_WIDGET (self));

    /* Panel-backed windows are built but never shown until the engine
     * present()s them for an "Edit…" request.  show_all above marked the
     * child widgets visible (so a later present shows the full editor);
     * hiding the toplevel again before we yield to the main loop means
     * the window is never actually mapped — the flow still runs. */
    if (self->panel_backed)
        gtk_widget_hide (GTK_WIDGET (self));

    /* The debug pane is opt-in via View → Debug View; start hidden. */
    if (self->debug_pane != NULL)
        gtk_widget_hide (self->debug_pane);

    /* Reopen the debug pane if it was open on the last run.  Driving
     * the menu item (rather than showing the pane directly) keeps its
     * checkmark in sync and routes through action_toggle_debug_view,
     * which also schedules the width-restoring idle. */
    if (self->menu_debug_view != NULL
            && pn_preferences_get_debug_view_open (prefs))
    {
        gtk_check_menu_item_set_active (
                GTK_CHECK_MENU_ITEM (self->menu_debug_view), TRUE);
    }
    else
    {
        /* Pane stays closed: no restore idle will run, so lift the
         * construction-time guard here or user divider drags would
         * never be recorded. */
        self->debug_pane_restoring = FALSE;
    }
}

static void
pn_window_finalize (GObject *object)
{
    PnWindow *self = PN_WINDOW (object);

    g_free (self->current_path);
    g_free (self->debug_search_casefold);
    g_clear_object (&self->flow);

    G_OBJECT_CLASS (pn_window_parent_class)->finalize (object);
}

enum {
    PROP_0,
    PROP_PANEL_BACKED,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void
pn_window_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnWindow *self = PN_WINDOW (object);

    switch (prop_id)
    {
    case PROP_PANEL_BACKED:
        g_value_set_boolean (value, self->panel_backed);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_window_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnWindow *self = PN_WINDOW (object);

    switch (prop_id)
    {
    case PROP_PANEL_BACKED:
        /* Construct-only: read by pn_window_constructed to decide
         * whether to keep the toplevel hidden. */
        self->panel_backed = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_window_class_init (PnWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->constructed  = pn_window_constructed;
    object_class->finalize     = pn_window_finalize;
    object_class->get_property = pn_window_get_property;
    object_class->set_property = pn_window_set_property;

    props[PROP_PANEL_BACKED] = g_param_spec_boolean (
            "panel-backed", "Panel-backed",
            "Whether the window backs an XFCE panel applet: built hidden, "
            "autosaves and hides on close instead of prompting and "
            "destroying, so its flow keeps running for the panel.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
            G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_window_init (PnWindow *self)
{
    /* <= 0 marks "no recorded pane width yet" so the first toggle-on
     * falls through to the 25 %-slice default.  pn_window_constructed
     * seeds this from saved preferences once they are available. */
    self->debug_pane_width = -1;
    self->debug_pane_restoring = FALSE;
}

PnWindow *
pn_window_new (PnApplication *app)
{
    return g_object_new (PN_TYPE_WINDOW,
                         "application", app,
                         NULL);
}

PnWindow *
pn_window_new_panel (PnApplication *app)
{
    return g_object_new (PN_TYPE_WINDOW,
                         "application",   app,
                         "panel-backed",  TRUE,
                         NULL);
}

PnWorksheet *
pn_window_get_worksheet (PnWindow *self)
{
    g_return_val_if_fail (PN_IS_WINDOW (self), NULL);

    return pn_window_active_worksheet (self);
}

/* ------------------------------------------------------------------ */
/*  Test surface                                                       */
/*                                                                     */
/*  Thin wrappers that the DBus interface in pn-application.c calls    */
/*  on behalf of functional tests.  Each function operates on the      */
/*  same widgets and code paths the real UI uses — no parallel state. */
/* ------------------------------------------------------------------ */

void
pn_window_inject_debug_message (
        PnWindow    *self,
        const gchar *text)
{
    g_return_if_fail (PN_IS_WINDOW (self));
    debug_pane_append (self, text != NULL ? text : "");
}

/** Walk a row's widget tree and return its from-button, or %NULL.
 *  The from-button is the only #GtkButton in a structured-envelope row
 *  (it lives inside the message expander's body); a depth-first search
 *  finds it regardless of how the row is nested.  Rows that fell back
 *  to the plain-label path have no button. */
static GtkWidget *
debug_row_find_from_button (GtkWidget *widget)
{
    GList     *kids;
    GList     *l;
    GtkWidget *found = NULL;

    if (GTK_IS_BUTTON (widget))
        return widget;
    if (!GTK_IS_CONTAINER (widget))
        return NULL;

    kids = gtk_container_get_children (GTK_CONTAINER (widget));
    for (l = kids; l != NULL && found == NULL; l = l->next)
        found = debug_row_find_from_button (GTK_WIDGET (l->data));
    g_list_free (kids);
    return found;
}

/** Pull the row at @index out of the list box; returns %NULL when
 *  the index is out of bounds. */
static GtkWidget *
debug_row_at (PnWindow *self, guint index)
{
    GtkListBoxRow *row;

    if (self->debug_list == NULL)
        return NULL;

    row = gtk_list_box_get_row_at_index (
            GTK_LIST_BOX (self->debug_list), (gint) index);
    if (row == NULL)
        return NULL;

    return gtk_bin_get_child (GTK_BIN (row));
}

guint
pn_window_get_debug_row_count (PnWindow *self)
{
    GList *children;
    guint  n;

    g_return_val_if_fail (PN_IS_WINDOW (self), 0);

    if (self->debug_list == NULL)
        return 0;

    children = gtk_container_get_children (GTK_CONTAINER (self->debug_list));
    n = g_list_length (children);
    g_list_free (children);
    return n;
}

gchar *
pn_window_get_debug_row_from_id (
        PnWindow *self,
        guint     index)
{
    GtkWidget   *child;
    GtkWidget   *btn;
    const gchar *uuid;

    g_return_val_if_fail (PN_IS_WINDOW (self), NULL);

    child = debug_row_at (self, index);
    if (child == NULL)
        return NULL;

    btn = debug_row_find_from_button (child);
    if (btn == NULL)
        return NULL;

    uuid = g_object_get_data (G_OBJECT (btn), "pn-from-id");
    return uuid != NULL ? g_strdup (uuid) : NULL;
}

gboolean
pn_window_click_debug_from_button (
        PnWindow *self,
        guint     index)
{
    GtkWidget *child;
    GtkWidget *btn;

    g_return_val_if_fail (PN_IS_WINDOW (self), FALSE);

    child = debug_row_at (self, index);
    if (child == NULL)
        return FALSE;

    btn = debug_row_find_from_button (child);
    if (btn == NULL || !gtk_widget_get_sensitive (btn))
        return FALSE;

    /* Fire the same "clicked" signal a real mouse click would, so
     * the GtkButton's full activation path runs (the on_debug_from_
     * clicked handler installed at row build time receives it). */
    gtk_button_clicked (GTK_BUTTON (btn));
    return TRUE;
}

void
pn_window_get_debug_pane_allocation (
        PnWindow *self,
        gint     *width,
        gint     *height)
{
    GtkAllocation alloc = { 0, 0, 0, 0 };

    g_return_if_fail (PN_IS_WINDOW (self));

    if (self->debug_pane != NULL
            && gtk_widget_get_visible (self->debug_pane)
            && gtk_widget_get_mapped  (self->debug_pane))
        gtk_widget_get_allocation (self->debug_pane, &alloc);

    if (width)  *width  = alloc.width;
    if (height) *height = alloc.height;
}

/* ------------------------------------------------------------------ */
/*  Test surface: node settings dialog                                 */
/*                                                                     */
/*  Lets a functional test pop up a node's #PnNodeDialog, read its     */
/*  notebook tab structure, and drive the per-property editor widgets  */
/*  the same way a user would — the load-bearing regression net for    */
/*  the headless core / GTK split, where the dialog-building vfuncs     */
/*  (build_class_tab[s]) move into the GUI tier.                        */
/* ------------------------------------------------------------------ */

/** Depth-first search for the first descendant of @widget whose
 *  runtime type is (or derives from) @type. */
static GtkWidget *
find_descendant_by_type (GtkWidget *widget, GType type)
{
    GList     *kids, *l;
    GtkWidget *found = NULL;

    if (widget == NULL)
        return NULL;
    if (G_TYPE_CHECK_INSTANCE_TYPE (widget, type))
        return widget;
    if (!GTK_IS_CONTAINER (widget))
        return NULL;

    kids = gtk_container_get_children (GTK_CONTAINER (widget));
    for (l = kids; l != NULL && found == NULL; l = l->next)
        found = find_descendant_by_type (GTK_WIDGET (l->data), type);
    g_list_free (kids);
    return found;
}

/** Depth-first search for the first descendant of @widget whose
 *  #GtkWidget name equals @name.  Unnamed widgets report their class
 *  name from gtk_widget_get_name(), which never collides with the
 *  "pn-prop-*" names the dialog stamps onto its editors. */
static GtkWidget *
find_descendant_by_name (GtkWidget *widget, const gchar *name)
{
    GList     *kids, *l;
    GtkWidget *found = NULL;

    if (widget == NULL)
        return NULL;
    if (g_strcmp0 (gtk_widget_get_name (widget), name) == 0)
        return widget;
    if (!GTK_IS_CONTAINER (widget))
        return NULL;

    kids = gtk_container_get_children (GTK_CONTAINER (widget));
    for (l = kids; l != NULL && found == NULL; l = l->next)
        found = find_descendant_by_name (GTK_WIDGET (l->data), name);
    g_list_free (kids);
    return found;
}

/** Locate the editor widget the dialog built for property @prop. */
static GtkWidget *
dialog_editor_for (PnWindow *self, const gchar *prop)
{
    GtkWidget *editor;
    gchar     *wname;

    if (self->test_node_dialog == NULL || prop == NULL)
        return NULL;

    wname  = g_strconcat ("pn-prop-", prop, NULL);
    editor = find_descendant_by_name (self->test_node_dialog, wname);
    g_free (wname);
    return editor;
}

gboolean
pn_window_open_node_dialog (PnWindow *self, guint index)
{
    PnWorksheet *worksheet;
    PnNodeStore *nodes;
    PnNode      *node;
    GtkWidget   *dialog;

    g_return_val_if_fail (PN_IS_WINDOW (self), FALSE);

    /* Only one test dialog at a time: drop any previous one first. */
    pn_window_close_node_dialog (self);

    worksheet = pn_window_get_worksheet (self);
    if (worksheet == NULL)
        return FALSE;

    nodes = pn_worksheet_get_nodes (worksheet);
    node  = pn_node_store_get_node (nodes, index);
    if (node == NULL)
        return FALSE;

    /* Same construction the worksheet's context menu uses — non-modal,
     * destroyed on response — so we exercise the real dialog code. */
    dialog = pn_node_dialog_new (GTK_WINDOW (self), node);
    g_signal_connect_swapped (dialog, "response",
                              G_CALLBACK (gtk_widget_destroy), dialog);
    gtk_widget_show_all (dialog);
    /* Raise it to the front so the slow/visible test mode is actually
     * watchable even when other windows are stacked above the editor. */
    gtk_window_present (GTK_WINDOW (dialog));

    self->test_node_dialog = dialog;
    g_object_add_weak_pointer (G_OBJECT (dialog),
                               (gpointer *) &self->test_node_dialog);
    return TRUE;
}

gboolean
pn_window_close_node_dialog (PnWindow *self)
{
    g_return_val_if_fail (PN_IS_WINDOW (self), FALSE);

    if (self->test_node_dialog == NULL)
        return FALSE;

    /* The weak pointer registered in open_node_dialog nulls the field. */
    gtk_widget_destroy (self->test_node_dialog);
    return TRUE;
}

gchar **
pn_window_get_dialog_page_titles (PnWindow *self)
{
    GtkWidget *notebook;
    GPtrArray *titles;
    gint       n, i;

    g_return_val_if_fail (PN_IS_WINDOW (self), NULL);

    if (self->test_node_dialog == NULL)
        return NULL;

    notebook = find_descendant_by_type (self->test_node_dialog,
                                        GTK_TYPE_NOTEBOOK);
    if (notebook == NULL)
        return NULL;

    titles = g_ptr_array_new ();
    n = gtk_notebook_get_n_pages (GTK_NOTEBOOK (notebook));
    for (i = 0; i < n; i++)
    {
        GtkWidget   *page = gtk_notebook_get_nth_page (
                GTK_NOTEBOOK (notebook), i);
        const gchar *text = gtk_notebook_get_tab_label_text (
                GTK_NOTEBOOK (notebook), page);
        g_ptr_array_add (titles, g_strdup (text != NULL ? text : ""));
    }
    g_ptr_array_add (titles, NULL);  /* NULL-terminate for strv */
    return (gchar **) g_ptr_array_free (titles, FALSE);
}

gboolean
pn_window_select_dialog_page (PnWindow *self, guint index)
{
    GtkWidget *notebook;

    g_return_val_if_fail (PN_IS_WINDOW (self), FALSE);

    if (self->test_node_dialog == NULL)
        return FALSE;

    notebook = find_descendant_by_type (self->test_node_dialog,
                                        GTK_TYPE_NOTEBOOK);
    if (notebook == NULL)
        return FALSE;

    if ((gint) index >= gtk_notebook_get_n_pages (GTK_NOTEBOOK (notebook)))
        return FALSE;

    gtk_notebook_set_current_page (GTK_NOTEBOOK (notebook), (gint) index);
    return TRUE;
}

gchar *
pn_window_get_dialog_editor_text (PnWindow *self, const gchar *prop)
{
    GtkWidget *editor;

    g_return_val_if_fail (PN_IS_WINDOW (self), NULL);

    editor = dialog_editor_for (self, prop);
    if (editor == NULL)
        return NULL;

    /* #GtkSpinButton is a #GtkEntry, so the entry branch covers both
     * string entries and numeric spinners (reporting the spinner's
     * formatted text). */
    if (GTK_IS_ENTRY (editor))
        return g_strdup (gtk_entry_get_text (GTK_ENTRY (editor)));

    return NULL;
}

gboolean
pn_window_get_dialog_editor_sensitive (PnWindow    *self,
                                       const gchar *prop,
                                       gboolean    *found)
{
    GtkWidget *editor;

    if (found != NULL)
        *found = FALSE;
    g_return_val_if_fail (PN_IS_WINDOW (self), FALSE);

    editor = dialog_editor_for (self, prop);
    if (editor == NULL)
        return FALSE;

    if (found != NULL)
        *found = TRUE;
    return gtk_widget_get_sensitive (editor);
}

gboolean
pn_window_set_dialog_editor_text (PnWindow    *self,
                                  const gchar *prop,
                                  const gchar *text)
{
    GtkWidget *editor;

    g_return_val_if_fail (PN_IS_WINDOW (self), FALSE);

    editor = dialog_editor_for (self, prop);
    if (editor == NULL)
        return FALSE;

    /* Driving the widget the way a user would makes the bidirectional
     * g_object_bind_property write straight through to the node. */
    if (GTK_IS_SPIN_BUTTON (editor))
    {
        gtk_spin_button_set_value (
                GTK_SPIN_BUTTON (editor),
                g_ascii_strtod (text != NULL ? text : "0", NULL));
        return TRUE;
    }
    if (GTK_IS_ENTRY (editor))
    {
        gtk_entry_set_text (GTK_ENTRY (editor), text != NULL ? text : "");
        return TRUE;
    }
    return FALSE;
}

gboolean
pn_window_load_file (
        PnWindow     *self,
        const gchar  *path,
        GError      **error)
{
    gchar *resolved;

    g_return_val_if_fail (PN_IS_WINDOW (self), FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    if (g_path_is_absolute (path))
        resolved = g_strdup (path);
    else
    {
        gchar *cwd = g_get_current_dir ();
        resolved = g_build_filename (cwd, path, NULL);
        g_free (cwd);
    }

    if (!pn_flow_load_from_file (self->flow, resolved, error))
    {
        g_free (resolved);
        return FALSE;
    }

    g_free (self->current_path);
    self->current_path = resolved;

    update_window_title (self);
    status_set (self, "Opened worksheet");
    return TRUE;
}
