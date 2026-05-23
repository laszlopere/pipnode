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

#include "pn-palette.h"
#include "pn-node-factory.h"

#include <string.h>

#define PN_PALETTE_DRAG_TARGET "application/x-pn-node-type"

/* ------------------------------------------------------------------ */
/*  Tree model columns                                                 */
/*                                                                     */
/*  COL_ICON       — single-glyph icon string for leaves (empty for   */
/*                   group rows).                                      */
/*  COL_MARKUP     — display label.  Group rows pre-format with bold  */
/*                   Pango markup; leaves carry the escaped class name*/
/*                   so DnD-supplied markup never reaches the renderer*/
/*                   unsanitised.                                      */
/*  COL_TYPE_NAME  — the registered #GType name for leaves.  Empty   */
/*                   on group rows; the empty string also gates DnD  */
/*                   so groups cannot be dragged.                     */
/* ------------------------------------------------------------------ */

enum
{
    COL_ICON,
    COL_MARKUP,
    COL_TYPE_NAME,
    N_COLS,
};

struct _PnPalette
{
    GtkBox parent_instance;

    GtkTreeStore       *store;
    GtkTreeModelFilter *filter;
    GtkTreeView        *tree;
    GtkEntry           *search_entry;

    /* Case-folded current search needle.  %NULL when the search entry
     * is empty; in that case the filter is a pass-through and every
     * row is shown.  Refreshed on every "changed" of the search
     * entry; freed in finalize. */
    gchar              *search_casefold;

    /* Lazily built popup menu offering a single "Help" entry.  Owned
     * by the palette via gtk_menu_attach_to_widget(); we never
     * gtk_widget_destroy() it explicitly. */
    GtkWidget          *popup_menu;

    /* Anchor (HTML fragment id) of the node row whose row was right-
     * clicked just before the popup opened.  %NULL when the popup was
     * opened on the header or on empty space, which makes Help open
     * the index page from the top.  Replaced by every popup
     * invocation; freed in finalize. */
    gchar              *pending_anchor;

    /* #GType of the row currently being dragged out of the palette, or
     * %G_TYPE_INVALID when no drag is in flight.  Set in drag-begin and
     * cleared in drag-end so a same-app drop site can read the dragged
     * type during drag-motion (see pn_palette_get_drag_node_type). */
    GType               drag_type;
};

enum
{
    SIGNAL_HELP_REQUESTED,
    N_SIGNALS,
};

static guint palette_signals[N_SIGNALS];

G_DEFINE_TYPE (PnPalette, pn_palette, GTK_TYPE_BOX)

static GtkTargetEntry palette_targets[] = {
    { (gchar *) PN_PALETTE_DRAG_TARGET, GTK_TARGET_SAME_APP, 0 },
};

/* ------------------------------------------------------------------ */
/*  Drag source                                                        */
/* ------------------------------------------------------------------ */

/** Provide the dragged node type's name as the drag payload.  Reads
 *  the COL_TYPE_NAME of the currently-selected row; group rows store
 *  an empty string so the payload is empty and the worksheet's
 *  drop handler treats it as a no-op. */
static void
on_tree_drag_data_get (
        GtkWidget        *widget,
        GdkDragContext   *context,
        GtkSelectionData *data,
        guint             info,
        guint             time,
        gpointer          user_data)
{
    GtkTreeView      *tree = GTK_TREE_VIEW (widget);
    GtkTreeSelection *sel;
    GtkTreeModel     *model;
    GtkTreeIter       iter;
    gchar            *type_name = NULL;

    (void) context;
    (void) info;
    (void) time;
    (void) user_data;

    sel = gtk_tree_view_get_selection (tree);
    if (!gtk_tree_selection_get_selected (sel, &model, &iter))
        return;

    gtk_tree_model_get (model, &iter, COL_TYPE_NAME, &type_name, -1);

    if (type_name != NULL && *type_name != '\0')
        gtk_selection_data_set (data,
                                gtk_selection_data_get_target (data),
                                8,
                                (const guchar *) type_name,
                                (gint) strlen (type_name));

    g_free (type_name);
}

/** Reject drags that originate on a group row (no #GType name).  We
 *  cannot make GtkTreeStore declare those rows non-draggable without
 *  subclassing it, so we cancel the drag at the very start instead. */
static void
on_tree_drag_begin (
        GtkWidget      *widget,
        GdkDragContext *context,
        gpointer        user_data)
{
    GtkTreeView      *tree = GTK_TREE_VIEW (widget);
    PnPalette        *self = PN_PALETTE (user_data);
    GtkTreeSelection *sel;
    GtkTreeModel     *model;
    GtkTreeIter       iter;
    gchar            *type_name = NULL;

    self->drag_type = G_TYPE_INVALID;

    sel = gtk_tree_view_get_selection (tree);
    if (!gtk_tree_selection_get_selected (sel, &model, &iter))
    {
        gtk_drag_cancel (context);
        return;
    }

    gtk_tree_model_get (model, &iter, COL_TYPE_NAME, &type_name, -1);

    if (type_name == NULL || *type_name == '\0')
        gtk_drag_cancel (context);
    else
        /* Stash the dragged type so a same-app drop site can read it
         * during drag-motion without a selection round-trip. */
        self->drag_type = g_type_from_name (type_name);

    g_free (type_name);
}

/** Suppress GtkTreeView's default drag icon — a snapshot of the dragged
 *  row — so only the worksheet's red wireframe follows the cursor.  The
 *  tree view sets that icon in its own "drag-begin" class handler, so
 *  this runs connected with g_signal_connect_after() to override it with
 *  a 1x1 transparent surface (no built-in "hide the icon" call exists). */
static void
on_tree_drag_begin_hide_icon (
        GtkWidget      *widget,
        GdkDragContext *context,
        gpointer        user_data)
{
    cairo_surface_t *blank;

    (void) widget;
    (void) user_data;

    blank = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, 1, 1);
    gtk_drag_set_icon_surface (context, blank);
    cairo_surface_destroy (blank);
}

/** Clear the stashed drag type once the drag ends (dropped, cancelled,
 *  or failed) so a later drag-motion peek cannot read a stale type. */
static void
on_tree_drag_end (
        GtkWidget      *widget,
        GdkDragContext *context,
        gpointer        user_data)
{
    PnPalette *self = PN_PALETTE (user_data);

    (void) widget;
    (void) context;

    self->drag_type = G_TYPE_INVALID;
}

/* ------------------------------------------------------------------ */
/*  Model population                                                   */
/* ------------------------------------------------------------------ */

/** Append a group row labelled @title under @parent (NULL for a
 *  top-level group).  The returned iter is suitable as the parent for
 *  subsequent subgroup or leaf rows, so categories may nest to any
 *  depth via slash-separated paths (see populate_palette()). */
static void
append_group (
        GtkTreeStore *store,
        const gchar  *title,
        GtkTreeIter  *parent,
        GtkTreeIter  *out_iter)
{
    gchar *markup = g_markup_printf_escaped ("<b>%s</b>", title);

    gtk_tree_store_append (store, out_iter, parent);
    gtk_tree_store_set (store, out_iter,
                        COL_ICON,      "",
                        COL_MARKUP,    markup,
                        COL_TYPE_NAME, "",
                        -1);

    g_free (markup);
}

/** Append a leaf row under @parent representing @node_type.  The
 *  visual identity (icon glyph, class label) is read from the node
 *  factory's metadata for the type, which in turn pulls from the
 *  per-type values pinned on the #PnNodeClass struct — no transient
 *  instance is needed. */
static void
append_node (
        GtkTreeStore  *store,
        GtkTreeIter   *parent,
        PnNodeFactory *factory,
        GType          node_type)
{
    GtkTreeIter  iter;
    const gchar *icon;
    const gchar *class_name;
    const gchar *type_name;
    gchar       *escaped;

    icon       = pn_node_factory_get_palette_icon (factory, node_type);
    class_name = pn_node_factory_get_class_name   (factory, node_type);
    type_name  = g_type_name (node_type);

    if (class_name == NULL || *class_name == '\0')
        class_name = type_name;

    escaped = g_markup_escape_text (class_name, -1);

    gtk_tree_store_append (store, &iter, parent);
    gtk_tree_store_set (store, &iter,
                        COL_ICON,      icon ? icon : "",
                        COL_MARKUP,    escaped,
                        COL_TYPE_NAME, type_name,
                        -1);

    g_free (escaped);
}

/* ------------------------------------------------------------------ */
/*  Search filter                                                      */
/*                                                                     */
/*  The tree view is backed by a #GtkTreeModelFilter wrapping the      */
/*  store.  When the user types in the top-of-palette search entry we  */
/*  refresh @self->search_casefold (case-folded once so the per-row    */
/*  comparison is a flat g_strstr_len), refilter, and expand the tree  */
/*  so every surviving match is immediately on-screen.  Group rows     */
/*  survive the filter when at least one of their descendants does, so */
/*  the categorisation stays intact during a search.                   */
/* ------------------------------------------------------------------ */

/** TRUE if any leaf at or below @iter has a label matching @needle.
 *  Recurses so an arbitrarily-deep subgroup chain keeps its ancestors
 *  visible during a search. */
static gboolean
row_matches_recursive (
        GtkTreeModel *model,
        GtkTreeIter  *iter,
        const gchar  *needle)
{
    GtkTreeIter child;

    if (gtk_tree_model_iter_children (model, &child, iter))
    {
        do
        {
            if (row_matches_recursive (model, &child, needle))
                return TRUE;
        }
        while (gtk_tree_model_iter_next (model, &child));
        return FALSE; /* group rows never match by their own label */
    }
    else
    {
        gchar    *label = NULL;
        gboolean  hit   = FALSE;

        gtk_tree_model_get (model, iter, COL_MARKUP, &label, -1);
        if (label != NULL)
        {
            gchar *fold = g_utf8_casefold (label, -1);
            if (fold != NULL && g_strstr_len (fold, -1, needle) != NULL)
                hit = TRUE;
            g_free (fold);
        }
        g_free (label);
        return hit;
    }
}

static gboolean
filter_visible_func (
        GtkTreeModel *model,
        GtkTreeIter  *iter,
        gpointer      user_data)
{
    PnPalette *self = PN_PALETTE (user_data);
    gchar     *type_name = NULL;
    gchar     *label     = NULL;
    gboolean   visible   = FALSE;
    gboolean   is_group;

    if (self->search_casefold == NULL)
        return TRUE;

    gtk_tree_model_get (model, iter,
                        COL_TYPE_NAME, &type_name,
                        COL_MARKUP,    &label,
                        -1);

    is_group = (type_name == NULL || *type_name == '\0');

    if (is_group)
    {
        /* A group (top-level or nested subgroup) is visible iff any
         * leaf anywhere beneath it matches.  Recurse so a match in a
         * deep subgroup keeps the whole ancestor chain on-screen. */
        visible = row_matches_recursive (model, iter,
                                         self->search_casefold);
    }
    else if (label != NULL)
    {
        gchar *fold = g_utf8_casefold (label, -1);
        if (fold != NULL
            && g_strstr_len (fold, -1, self->search_casefold) != NULL)
            visible = TRUE;
        g_free (fold);
    }

    g_free (type_name);
    g_free (label);
    return visible;
}

/** Refresh the cached case-folded needle and re-evaluate the filter.
 *  Expanding the tree on every keystroke keeps surviving matches on-
 *  screen; collapsing back to the user's previous expansion state when
 *  the search clears is not worth the bookkeeping — the palette is
 *  small enough that "everything expanded" is the natural default. */
static void
on_search_entry_changed (
        GtkEditable *editable,
        gpointer     user_data)
{
    PnPalette   *self = PN_PALETTE (user_data);
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (editable));

    g_clear_pointer (&self->search_casefold, g_free);
    if (text != NULL && *text != '\0')
        self->search_casefold = g_utf8_casefold (text, -1);

    if (self->filter != NULL)
        gtk_tree_model_filter_refilter (self->filter);

    gtk_tree_view_expand_all (self->tree);
}

/* ------------------------------------------------------------------ */
/*  Popup menu                                                         */
/*                                                                     */
/*  Right-clicking the palette (header or empty space below the tree)  */
/*  pops up a context menu with a single "Help" entry.  Activating it  */
/*  emits #PnPalette::help-requested; the host window connects to that */
/*  signal and opens its #PnHelpBrowser.  We deliberately keep the     */
/*  palette ignorant of help-page paths so it can be reused outside    */
/*  the main window.                                                   */
/* ------------------------------------------------------------------ */

G_GNUC_BEGIN_IGNORE_DEPRECATIONS

static void
on_help_menu_item_activate (
        GtkMenuItem *item,
        gpointer     user_data)
{
    PnPalette *self = PN_PALETTE (user_data);

    (void) item;

    g_signal_emit (self, palette_signals[SIGNAL_HELP_REQUESTED], 0,
                   self->pending_anchor);
}

/** Build the popup menu on first use.  Subsequent right-clicks reuse
 *  the same widget.  Attaching it to the palette ties the menu's
 *  lifetime to ours so we do not have to free it manually. */
static GtkWidget *
ensure_popup_menu (PnPalette *self)
{
    GtkWidget *menu;
    GtkWidget *item;
    GtkWidget *image;

    if (self->popup_menu != NULL)
        return self->popup_menu;

    menu = gtk_menu_new ();

    item  = gtk_image_menu_item_new_with_mnemonic ("_Help");
    image = gtk_image_new_from_icon_name ("help-contents",
                                          GTK_ICON_SIZE_MENU);
    gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
    gtk_image_menu_item_set_always_show_image (
            GTK_IMAGE_MENU_ITEM (item), TRUE);

    g_signal_connect (item, "activate",
                      G_CALLBACK (on_help_menu_item_activate), self);

    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
    gtk_widget_show_all (menu);

    gtk_menu_attach_to_widget (GTK_MENU (menu), GTK_WIDGET (self), NULL);
    self->popup_menu = menu;

    return menu;
}

G_GNUC_END_IGNORE_DEPRECATIONS

/** Determine the anchor that the about-to-open popup should target.
 *  When the secondary-click landed on a tree-view leaf, we transfer
 *  the row's #GType name into @self->pending_anchor (HTML headings
 *  in help-index.html are tagged with the same identifiers).  Group
 *  rows and clicks outside any row reset the anchor so Help opens at
 *  the top.  Also moves the cursor to the clicked row so it is clear
 *  which entry the popup applies to. */
static void
update_pending_anchor_from_event (
        PnPalette      *self,
        GtkWidget      *widget,
        GdkEventButton *event)
{
    GtkTreeView  *tree;
    GtkTreePath  *path = NULL;
    GtkTreeModel *model;
    GtkTreeIter   iter;
    gchar        *type_name = NULL;

    g_clear_pointer (&self->pending_anchor, g_free);

    if (!GTK_IS_TREE_VIEW (widget))
        return;

    tree = GTK_TREE_VIEW (widget);

    if (!gtk_tree_view_get_path_at_pos (tree,
                                        (gint) event->x,
                                        (gint) event->y,
                                        &path, NULL, NULL, NULL))
        return;

    model = gtk_tree_view_get_model (tree);
    if (gtk_tree_model_get_iter (model, &iter, path))
    {
        gtk_tree_model_get (model, &iter,
                            COL_TYPE_NAME, &type_name, -1);

        /* Group rows store an empty type name; treat them like an
         * empty-area click so the user lands at the top. */
        if (type_name != NULL && *type_name != '\0')
        {
            self->pending_anchor = type_name;  /* take ownership */
            type_name = NULL;
        }
    }

    gtk_tree_view_set_cursor (tree, path, NULL, FALSE);

    g_free (type_name);
    gtk_tree_path_free (path);
}

/** Pop the menu up at the event location, if the event is a secondary
 *  button press.  Returns %TRUE when the event was consumed so the
 *  caller can use the result as a button-press-event handler. */
static gboolean
on_palette_button_press (
        GtkWidget      *widget,
        GdkEventButton *event,
        gpointer        user_data)
{
    PnPalette *self = PN_PALETTE (user_data);

    if (gdk_event_triggers_context_menu ((GdkEvent *) event)
        && event->type == GDK_BUTTON_PRESS)
    {
        update_pending_anchor_from_event (self, widget, event);

        gtk_menu_popup_at_pointer (GTK_MENU (ensure_popup_menu (self)),
                                   (GdkEvent *) event);
        return GDK_EVENT_STOP;
    }

    return GDK_EVENT_PROPAGATE;
}

/** Keyboard equivalent: the Menu key (or Shift+F10) invokes
 *  GtkWidget::popup-menu.  Pop our menu up at the widget instead of
 *  the cursor so it lands on the palette regardless of mouse focus.
 *  When the focus is in the tree view, target the currently selected
 *  leaf so Help still jumps to the right section. */
static gboolean
on_palette_popup_menu (
        GtkWidget *widget,
        gpointer   user_data)
{
    PnPalette *self = PN_PALETTE (user_data);

    g_clear_pointer (&self->pending_anchor, g_free);

    if (GTK_IS_TREE_VIEW (widget))
    {
        GtkTreeSelection *sel;
        GtkTreeModel     *model;
        GtkTreeIter       iter;
        gchar            *type_name = NULL;

        sel = gtk_tree_view_get_selection (GTK_TREE_VIEW (widget));
        if (gtk_tree_selection_get_selected (sel, &model, &iter))
        {
            gtk_tree_model_get (model, &iter,
                                COL_TYPE_NAME, &type_name, -1);
            if (type_name != NULL && *type_name != '\0')
            {
                self->pending_anchor = type_name;
                type_name = NULL;
            }
        }
        g_free (type_name);
    }

    gtk_menu_popup_at_widget (GTK_MENU (ensure_popup_menu (self)),
                              widget,
                              GDK_GRAVITY_SOUTH_WEST,
                              GDK_GRAVITY_NORTH_WEST,
                              NULL);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Construction                                                       */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_tree_view (PnPalette *self)
{
    GtkWidget         *tree;
    GtkTreeViewColumn *column;
    GtkCellRenderer   *icon_cell;
    GtkCellRenderer   *label_cell;

    self->store = gtk_tree_store_new (N_COLS,
                                      G_TYPE_STRING,   /* COL_ICON      */
                                      G_TYPE_STRING,   /* COL_MARKUP    */
                                      G_TYPE_STRING);  /* COL_TYPE_NAME */

    /* Wrap the store in a filter so the search entry can hide rows
     * without disturbing the canonical model.  When the search needle
     * is empty the visible func is a pass-through, so this is also a
     * zero-cost layer in the default state. */
    self->filter = GTK_TREE_MODEL_FILTER (
            gtk_tree_model_filter_new (GTK_TREE_MODEL (self->store), NULL));
    gtk_tree_model_filter_set_visible_func (self->filter,
                                            filter_visible_func,
                                            self, NULL);

    tree = gtk_tree_view_new_with_model (GTK_TREE_MODEL (self->filter));
    self->tree = GTK_TREE_VIEW (tree);
    /* Release the construction refs: the filter still holds the
     * store, and the tree view still holds the filter. */
    g_object_unref (self->store);
    g_object_unref (self->filter);

    gtk_tree_view_set_headers_visible (self->tree, FALSE);
    gtk_tree_view_set_show_expanders  (self->tree, TRUE);
    gtk_tree_view_set_enable_tree_lines (self->tree, FALSE);

    /* Single column packing two text renderers: glyph then markup. */
    column     = gtk_tree_view_column_new ();
    icon_cell  = gtk_cell_renderer_text_new ();
    label_cell = gtk_cell_renderer_text_new ();

    g_object_set (icon_cell,
                  "scale", 1.4,
                  "xpad",  2,
                  NULL);

    gtk_tree_view_column_pack_start    (column, icon_cell, FALSE);
    gtk_tree_view_column_add_attribute (column, icon_cell,
                                        "text", COL_ICON);

    gtk_tree_view_column_pack_start    (column, label_cell, TRUE);
    gtk_tree_view_column_add_attribute (column, label_cell,
                                        "markup", COL_MARKUP);

    gtk_tree_view_append_column (self->tree, column);

    /* Drag source: payload is the COL_TYPE_NAME of the dragged row. */
    gtk_tree_view_enable_model_drag_source (self->tree,
                                            GDK_BUTTON1_MASK,
                                            palette_targets,
                                            G_N_ELEMENTS (palette_targets),
                                            GDK_ACTION_COPY);

    g_signal_connect (tree, "drag-data-get",
                      G_CALLBACK (on_tree_drag_data_get), NULL);
    g_signal_connect (tree, "drag-begin",
                      G_CALLBACK (on_tree_drag_begin),    self);
    /* Runs after the tree view's own drag-begin so it can replace the
     * default row-snapshot drag icon with a blank one. */
    g_signal_connect_after (tree, "drag-begin",
                      G_CALLBACK (on_tree_drag_begin_hide_icon), NULL);
    g_signal_connect (tree, "drag-end",
                      G_CALLBACK (on_tree_drag_end),      self);

    return tree;
}

/** Populate @self by walking the node factory.  The factory holds
 *  the canonical list of registered node types (built-ins now and
 *  out-of-tree plugins later) along with each type's metadata; the
 *  palette no longer hard-codes the type list.  Categories are
 *  emitted in the order they first appear in the factory and types
 *  appear within each group in registration order, so adding a new
 *  type to the factory is the only change needed for it to show up
 *  here.  Types whose class did not pin a category are placed in a
 *  fallback "Other" group so a half-configured plugin still shows
 *  its types instead of silently disappearing. */
static void
populate_palette (PnPalette *self)
{
    PnNodeFactory *factory = pn_node_factory_get_default ();
    GHashTable    *groups; /* category(string) -> GtkTreeIter* (heap) */
    guint          n;
    guint          i;

    groups = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    g_free, g_free);

    n = pn_node_factory_get_n_types (factory);
    for (i = 0; i < n; i++)
    {
        GType        t        = pn_node_factory_get_type_at  (factory, i);
        const gchar *category = pn_node_factory_get_category (factory, t);
        GtkTreeIter *group_iter = NULL;
        gchar      **segments;
        gchar       *prefix     = NULL;
        guint        s;

        if (category == NULL || *category == '\0')
            category = "Other";

        /* The category is a slash-separated path ("cat1/cat2/…").  Walk
         * the segments, creating a group row for each path prefix not
         * seen before and reusing the cached row otherwise, so siblings
         * share a parent and the leaf lands under the deepest segment.
         * A category with no slash yields a single segment and renders
         * flat exactly as a plain top-level group. */
        segments = g_strsplit (category, "/", -1);
        for (s = 0; segments[s] != NULL; s++)
        {
            GtkTreeIter *next_iter;
            gchar       *new_prefix;

            /* Skip empty segments from stray/leading/trailing slashes. */
            if (*segments[s] == '\0')
                continue;

            new_prefix = (prefix == NULL)
                       ? g_strdup (segments[s])
                       : g_strconcat (prefix, "/", segments[s], NULL);
            g_free (prefix);
            prefix = new_prefix;

            next_iter = g_hash_table_lookup (groups, prefix);
            if (next_iter == NULL)
            {
                next_iter = g_new0 (GtkTreeIter, 1);
                append_group (self->store, segments[s],
                              group_iter, next_iter);
                g_hash_table_insert (groups, g_strdup (prefix), next_iter);
            }

            group_iter = next_iter;
        }
        g_free (prefix);
        g_strfreev (segments);

        /* All segments were empty (e.g. category was just "/"): fall
         * back to the "Other" group so the type still appears. */
        if (group_iter == NULL)
        {
            group_iter = g_hash_table_lookup (groups, "Other");
            if (group_iter == NULL)
            {
                group_iter = g_new0 (GtkTreeIter, 1);
                append_group (self->store, "Other", NULL, group_iter);
                g_hash_table_insert (groups, g_strdup ("Other"), group_iter);
            }
        }

        append_node (self->store, group_iter, factory, t);
    }

    g_hash_table_destroy (groups);

    gtk_tree_view_expand_all (self->tree);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_palette_finalize (GObject *object)
{
    PnPalette *self = PN_PALETTE (object);

    g_free (self->pending_anchor);
    g_free (self->search_casefold);

    G_OBJECT_CLASS (pn_palette_parent_class)->finalize (object);
}

static void
pn_palette_class_init (PnPaletteClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = pn_palette_finalize;

    /**
     * PnPalette::help-requested:
     * @self: the palette
     * @anchor: (nullable): #GType name of the node row whose context
     *          the popup was opened on, or %NULL when the popup was
     *          invoked from a non-row area.  Receivers can use it as
     *          an HTML fragment id when opening the help page.
     *
     * Emitted when the user activates the "Help" entry of the
     * palette's context menu.  The host window is expected to open
     * the application's help browser; the palette deliberately does
     * not know any help-page paths.
     */
    palette_signals[SIGNAL_HELP_REQUESTED] = g_signal_new (
            "help-requested",
            PN_TYPE_PALETTE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL, NULL,
            G_TYPE_NONE, 1,
            G_TYPE_STRING);
}

static void
pn_palette_init (PnPalette *self)
{
    GtkWidget *header_event_box;
    GtkWidget *header;
    GtkWidget *scrolled;
    GtkWidget *tree;

    gtk_orientable_set_orientation (GTK_ORIENTABLE (self),
                                    GTK_ORIENTATION_VERTICAL);
    gtk_box_set_spacing (GTK_BOX (self), 4);
    gtk_container_set_border_width (GTK_CONTAINER (self), 8);
    gtk_widget_set_size_request (GTK_WIDGET (self), 180, -1);

    /* Wrap the header label in a GtkEventBox so right-clicks on the
     * header open our popup menu.  Plain GtkLabel has no event
     * window of its own. */
    header_event_box = gtk_event_box_new ();
    gtk_event_box_set_visible_window (
            GTK_EVENT_BOX (header_event_box), FALSE);
    gtk_box_pack_start (GTK_BOX (self), header_event_box, FALSE, FALSE, 0);

    header = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (header), "<b>Palette</b>");
    gtk_label_set_xalign (GTK_LABEL (header), 0.0);
    gtk_container_add (GTK_CONTAINER (header_event_box), header);

    g_signal_connect (header_event_box, "button-press-event",
                      G_CALLBACK (on_palette_button_press), self);

    /* Quick-search entry pinned above the tree: as the user types we
     * casefold the needle once and refilter the tree, hiding leaves
     * whose label does not contain the substring and the now-empty
     * groups along with them.  The primary "search" icon is purely
     * decorative — the entry has no commit semantics, the filter
     * tracks every keystroke. */
    {
        GtkWidget *entry = gtk_entry_new ();

        self->search_entry = GTK_ENTRY (entry);
        gtk_entry_set_icon_from_icon_name (self->search_entry,
                                           GTK_ENTRY_ICON_PRIMARY,
                                           "edit-find-symbolic");
        gtk_entry_set_icon_activatable (self->search_entry,
                                        GTK_ENTRY_ICON_PRIMARY, FALSE);
        gtk_entry_set_placeholder_text (self->search_entry, "Search");
        gtk_box_pack_start (GTK_BOX (self), entry, FALSE, FALSE, 0);

        g_signal_connect (entry, "changed",
                          G_CALLBACK (on_search_entry_changed), self);
    }

    /* Scrolled wrapper so a long type list does not stretch the
     * sidebar.  The tree itself decides its natural width. */
    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (self), scrolled, TRUE, TRUE, 0);

    tree = build_tree_view (self);
    gtk_container_add (GTK_CONTAINER (scrolled), tree);

    /* Right-clicking anywhere on the tree view (empty space or a
     * leaf) also opens the popup.  The handler runs before the tree
     * view's own selection logic, which is fine — secondary clicks
     * don't have meaningful default behaviour here. */
    g_signal_connect (tree, "button-press-event",
                      G_CALLBACK (on_palette_button_press), self);

    /* Keyboard-driven popup (Menu / Shift+F10) on the palette as a
     * whole and on the tree view that actually owns focus. */
    g_signal_connect (self, "popup-menu",
                      G_CALLBACK (on_palette_popup_menu), self);
    g_signal_connect (tree, "popup-menu",
                      G_CALLBACK (on_palette_popup_menu), self);

    populate_palette (self);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_palette_new (void)
{
    return g_object_new (PN_TYPE_PALETTE, NULL);
}

const gchar *
pn_palette_drag_target_name (void)
{
    return PN_PALETTE_DRAG_TARGET;
}

GType
pn_palette_get_drag_node_type (PnPalette *self)
{
    g_return_val_if_fail (PN_IS_PALETTE (self), G_TYPE_INVALID);

    return self->drag_type;
}
