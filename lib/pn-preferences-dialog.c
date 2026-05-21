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

#include "pn-preferences-dialog.h"
#include "pn-preferences.h"
#include "pn-node-factory.h"

/* Process-wide dialog instance.  The dialog is non-modal and
 * application-scoped — every "Edit → Preferences" click maps to the
 * same window, raised back to the top.  A weak pointer here resets
 * the slot to NULL when the user closes the dialog, so the next
 * present() call constructs a fresh one. */
static PnPreferencesDialog *current_dialog = NULL;

struct _PnPreferencesDialog
{
    GtkWindow parent_instance;

    /* Left: category tree.  Two-level so we already have a place to
     * add second-level entries (e.g. "Worksheet Editor → Snapping")
     * without a UI refactor — every leaf is mapped to a #GtkStack
     * page name via the PAGE_ID column. */
    GtkTreeStore *category_store;
    GtkWidget    *category_view;

    /* Right: stack of setting pages.  Selecting a row in the tree
     * flips the stack to the matching child name. */
    GtkStack     *stack;

    /* Per-page widgets we keep references to so the live-binding code
     * can push fresh values in (when the preference changes from
     * outside the dialog, e.g. another future surface) and read them
     * out without an indexed lookup. */
    GtkWidget    *grid_snap_check;
    GtkWidget    *grid_type_combo;
    GtkWidget    *grid_color_button;

    /* Re-entrancy guard: TRUE while a notify:: handler is pushing a
     * fresh value into a widget.  Without this the widget's own
     * "toggled" / "changed" / "color-set" signal would fire and write
     * the same value straight back to the preference, which itself
     * fires the notify:: hook for any peer subscriber -- one user
     * click would otherwise tour every observer twice. */
    gboolean      updating;

    /* Connections back into the preferences singleton.  Kept here so
     * dispose can disconnect cleanly; otherwise a delete-event that
     * tears the dialog down before the singleton would leave handlers
     * pointing at freed memory. */
    gulong        notify_snap_id;
    gulong        notify_type_id;
    gulong        notify_color_id;
};

G_DEFINE_TYPE (PnPreferencesDialog, pn_preferences_dialog, GTK_TYPE_WINDOW)

enum {
    COL_LABEL,
    COL_PAGE_ID,
    N_COLS,
};

#define PAGE_ID_GRID        "grid"
#define PAGE_ID_WORKSHEET   "worksheet"   /* group node: no per-page settings */
#define PAGE_ID_PLUGINS     "plugins"

/* ------------------------------------------------------------------ */
/*  Live binding between dialog widgets and the preferences singleton  */
/* ------------------------------------------------------------------ */

static void
sync_widgets_from_prefs (PnPreferencesDialog *self)
{
    PnPreferences *prefs = pn_preferences_get_default ();
    GdkRGBA        color;

    self->updating = TRUE;

    gtk_toggle_button_set_active (
            GTK_TOGGLE_BUTTON (self->grid_snap_check),
            pn_preferences_get_snap_to_grid (prefs));

    gtk_combo_box_set_active (
            GTK_COMBO_BOX (self->grid_type_combo),
            (int) pn_preferences_get_grid_type (prefs));

    pn_preferences_get_grid_color (prefs, &color);
    gtk_color_chooser_set_rgba (
            GTK_COLOR_CHOOSER (self->grid_color_button), &color);

    self->updating = FALSE;
}

static void
on_pref_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    PnPreferencesDialog *self = PN_PREFERENCES_DIALOG (user_data);
    (void) object; (void) pspec;
    sync_widgets_from_prefs (self);
}

/* ------------------------------------------------------------------ */
/*  Widget → preferences handlers                                      */
/* ------------------------------------------------------------------ */

static void
on_snap_toggled (GtkToggleButton *button, gpointer user_data)
{
    PnPreferencesDialog *self = PN_PREFERENCES_DIALOG (user_data);
    if (self->updating)
        return;
    pn_preferences_set_snap_to_grid (
            pn_preferences_get_default (),
            gtk_toggle_button_get_active (button));
}

static void
on_grid_type_changed (GtkComboBox *combo, gpointer user_data)
{
    PnPreferencesDialog *self = PN_PREFERENCES_DIALOG (user_data);
    int active;

    if (self->updating)
        return;
    active = gtk_combo_box_get_active (combo);
    if (active < 0)
        return;
    pn_preferences_set_grid_type (
            pn_preferences_get_default (),
            (PnGridType) active);
}

static void
on_grid_color_set (GtkColorButton *button, gpointer user_data)
{
    PnPreferencesDialog *self = PN_PREFERENCES_DIALOG (user_data);
    GdkRGBA              color;

    if (self->updating)
        return;
    gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER (button), &color);
    pn_preferences_set_grid_color (pn_preferences_get_default (), &color);
}

/* ------------------------------------------------------------------ */
/*  Category tree                                                      */
/* ------------------------------------------------------------------ */

static void
on_category_selection_changed (
        GtkTreeSelection *selection,
        gpointer          user_data)
{
    PnPreferencesDialog *self = PN_PREFERENCES_DIALOG (user_data);
    GtkTreeIter          iter;
    GtkTreeModel        *model;
    gchar               *page_id = NULL;

    if (!gtk_tree_selection_get_selected (selection, &model, &iter))
        return;
    gtk_tree_model_get (model, &iter, COL_PAGE_ID, &page_id, -1);

    if (page_id != NULL)
        gtk_stack_set_visible_child_name (self->stack, page_id);

    g_free (page_id);
}

/* Build the left-side category tree.  Two-level so the "Worksheet
 * Editor" parent is itself selectable (it maps to a placeholder page
 * with a short blurb) and so subsequent additions slot in next to
 * "Grid" without touching this layout. */
static void
build_category_tree (PnPreferencesDialog *self)
{
    GtkTreeIter        worksheet_iter, grid_iter, plugins_iter;
    GtkCellRenderer   *renderer;
    GtkTreeViewColumn *column;
    GtkTreeSelection  *selection;
    GtkTreePath       *path;

    self->category_store = gtk_tree_store_new (N_COLS,
                                               G_TYPE_STRING,
                                               G_TYPE_STRING);

    gtk_tree_store_append (self->category_store, &worksheet_iter, NULL);
    gtk_tree_store_set (self->category_store, &worksheet_iter,
                        COL_LABEL,   "Worksheet Editor",
                        COL_PAGE_ID, PAGE_ID_WORKSHEET,
                        -1);

    gtk_tree_store_append (self->category_store, &grid_iter, &worksheet_iter);
    gtk_tree_store_set (self->category_store, &grid_iter,
                        COL_LABEL,   "Grid",
                        COL_PAGE_ID, PAGE_ID_GRID,
                        -1);

    /* Plugins: top-level entry so it sits next to "Worksheet Editor"
     * rather than nested underneath -- plugin enable/disable is not a
     * worksheet-editor concern, it controls which node types exist at
     * all. */
    gtk_tree_store_append (self->category_store, &plugins_iter, NULL);
    gtk_tree_store_set (self->category_store, &plugins_iter,
                        COL_LABEL,   "Plugins",
                        COL_PAGE_ID, PAGE_ID_PLUGINS,
                        -1);
    (void) plugins_iter;

    self->category_view = gtk_tree_view_new_with_model (
            GTK_TREE_MODEL (self->category_store));
    g_object_unref (self->category_store);
    gtk_tree_view_set_headers_visible (
            GTK_TREE_VIEW (self->category_view), FALSE);
    gtk_tree_view_expand_all (GTK_TREE_VIEW (self->category_view));

    renderer = gtk_cell_renderer_text_new ();
    column   = gtk_tree_view_column_new_with_attributes (
                       NULL, renderer, "text", COL_LABEL, NULL);
    gtk_tree_view_append_column (GTK_TREE_VIEW (self->category_view),
                                 column);

    selection = gtk_tree_view_get_selection (
            GTK_TREE_VIEW (self->category_view));
    gtk_tree_selection_set_mode (selection, GTK_SELECTION_BROWSE);
    g_signal_connect (selection, "changed",
                      G_CALLBACK (on_category_selection_changed), self);

    /* Select the Grid leaf at startup -- it is the only page with real
     * settings today, so jumping straight to it saves the user a click. */
    path = gtk_tree_model_get_path (GTK_TREE_MODEL (self->category_store),
                                    &grid_iter);
    gtk_tree_selection_select_path (selection, path);
    gtk_tree_path_free (path);
}

/* ------------------------------------------------------------------ */
/*  Page builders                                                      */
/* ------------------------------------------------------------------ */

/* Group-node placeholder.  The "Worksheet Editor" parent in the
 * category tree is itself selectable; clicking it should show a tidy
 * "pick a subcategory" hint instead of leaving the right pane blank. */
static GtkWidget *
build_placeholder_page (const gchar *title, const gchar *blurb)
{
    GtkWidget *box;
    GtkWidget *heading;
    GtkWidget *body;
    PangoAttrList *attrs;

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    g_object_set (box,
                  "margin-start", 16, "margin-end", 16,
                  "margin-top",   16, "margin-bottom", 16,
                  NULL);

    heading = gtk_label_new (title);
    attrs   = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    pango_attr_list_insert (attrs, pango_attr_scale_new  (PANGO_SCALE_LARGE));
    gtk_label_set_attributes (GTK_LABEL (heading), attrs);
    pango_attr_list_unref (attrs);
    gtk_widget_set_halign (heading, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (box), heading, FALSE, FALSE, 0);

    body = gtk_label_new (blurb);
    gtk_label_set_line_wrap (GTK_LABEL (body), TRUE);
    gtk_label_set_xalign (GTK_LABEL (body), 0.0);
    gtk_box_pack_start (GTK_BOX (box), body, FALSE, FALSE, 0);

    return box;
}

/* Helper: attach a "Label: <editor>" row at the next free row of @grid.
 * Keeps the page builders readable. */
static void
attach_labelled_row (
        GtkGrid     *grid,
        gint         row,
        const gchar *label_text,
        GtkWidget   *editor)
{
    GtkWidget *label;

    label = gtk_label_new (label_text);
    gtk_widget_set_halign (label, GTK_ALIGN_END);
    gtk_widget_set_valign (label, GTK_ALIGN_CENTER);

    gtk_widget_set_halign (editor, GTK_ALIGN_START);
    gtk_widget_set_valign (editor, GTK_ALIGN_CENTER);

    gtk_grid_attach (grid, label,  0, row, 1, 1);
    gtk_grid_attach (grid, editor, 1, row, 1, 1);
}

static GtkWidget *
build_grid_page (PnPreferencesDialog *self)
{
    GtkWidget *box;
    GtkWidget *heading;
    GtkWidget *grid;
    PangoAttrList *attrs;

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    g_object_set (box,
                  "margin-start", 16, "margin-end", 16,
                  "margin-top",   16, "margin-bottom", 16,
                  NULL);

    heading = gtk_label_new ("Grid");
    attrs   = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    pango_attr_list_insert (attrs, pango_attr_scale_new  (PANGO_SCALE_LARGE));
    gtk_label_set_attributes (GTK_LABEL (heading), attrs);
    pango_attr_list_unref (attrs);
    gtk_widget_set_halign (heading, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (box), heading, FALSE, FALSE, 0);

    grid = gtk_grid_new ();
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);

    /* Snap-to-grid. */
    self->grid_snap_check = gtk_check_button_new_with_label (
            "Snap node positions to the grid");
    g_signal_connect (self->grid_snap_check, "toggled",
                      G_CALLBACK (on_snap_toggled), self);
    attach_labelled_row (GTK_GRID (grid), 0,
                         "Snap to grid:", self->grid_snap_check);

    /* Grid type.  Order of model entries matches the PnGridType enum so
     * the combo's active-index doubles as the enum value. */
    self->grid_type_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (self->grid_type_combo),
                               NULL, "Lines");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (self->grid_type_combo),
                               NULL, "Dots");
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (self->grid_type_combo),
                               NULL, "None");
    g_signal_connect (self->grid_type_combo, "changed",
                      G_CALLBACK (on_grid_type_changed), self);
    attach_labelled_row (GTK_GRID (grid), 1,
                         "Grid type:", self->grid_type_combo);

    /* Grid colour. */
    self->grid_color_button = gtk_color_button_new ();
    gtk_color_chooser_set_use_alpha (
            GTK_COLOR_CHOOSER (self->grid_color_button), TRUE);
    gtk_color_button_set_title (
            GTK_COLOR_BUTTON (self->grid_color_button), "Grid Color");
    g_signal_connect (self->grid_color_button, "color-set",
                      G_CALLBACK (on_grid_color_set), self);
    attach_labelled_row (GTK_GRID (grid), 2,
                         "Grid color:", self->grid_color_button);

    return box;
}

/* ------------------------------------------------------------------ */
/*  Plugins page                                                       */
/* ------------------------------------------------------------------ */

/* Each row in the plugins list owns a strdup'd basename so the toggle
 * callback can look the plugin up in the preferences set without
 * walking the model.  Attached via g_object_set_data_full so the
 * GtkCheckButton's destruction frees it. */
#define PLUGIN_BASENAME_KEY "pipnode-plugin-basename"

static void
on_plugin_toggled (GtkToggleButton *button, gpointer user_data)
{
    const gchar *basename;
    gboolean     active;

    (void) user_data;

    basename = g_object_get_data (G_OBJECT (button), PLUGIN_BASENAME_KEY);
    if (basename == NULL)
        return;

    /* Active (checked) = plugin loaded; unchecked = disabled. */
    active = gtk_toggle_button_get_active (button);
    pn_preferences_set_plugin_disabled (pn_preferences_get_default (),
                                        basename,
                                        !active);
}

static GtkWidget *
build_plugins_page (PnPreferencesDialog *self)
{
    GtkWidget     *box;
    GtkWidget     *heading;
    GtkWidget     *blurb;
    GtkWidget     *list_box;
    GtkWidget     *scroll;
    PangoAttrList *attrs;
    PnPreferences *prefs;
    PnNodeFactory *factory;
    GList         *descriptors;
    GList         *l;

    (void) self;

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    g_object_set (box,
                  "margin-start", 16, "margin-end", 16,
                  "margin-top",   16, "margin-bottom", 16,
                  NULL);

    heading = gtk_label_new ("Plugins");
    attrs   = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    pango_attr_list_insert (attrs, pango_attr_scale_new  (PANGO_SCALE_LARGE));
    gtk_label_set_attributes (GTK_LABEL (heading), attrs);
    pango_attr_list_unref (attrs);
    gtk_widget_set_halign (heading, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (box), heading, FALSE, FALSE, 0);

    blurb = gtk_label_new (
            "Tick a plugin to load it; untick to ignore.  All "
            "plugins are loaded by default.  Changes take effect "
            "the next time pipnode starts -- plugin types cannot "
            "be unregistered from a running process.");
    gtk_label_set_line_wrap (GTK_LABEL (blurb), TRUE);
    gtk_label_set_xalign    (GTK_LABEL (blurb), 0.0);
    /* Cap the natural width so a long blurb does not force the
     * whole Preferences dialog wider when this page is selected. */
    gtk_label_set_max_width_chars (GTK_LABEL (blurb), 50);
    gtk_label_set_width_chars     (GTK_LABEL (blurb), 50);
    gtk_box_pack_start (GTK_BOX (box), blurb, FALSE, FALSE, 0);

    list_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);

    factory     = pn_node_factory_get_default ();
    prefs       = pn_preferences_get_default ();
    descriptors = pn_node_factory_list_plugins (factory);

    if (descriptors == NULL)
    {
        GtkWidget *empty = gtk_label_new (
                "No plugins discovered in the search path.");
        gtk_label_set_xalign (GTK_LABEL (empty), 0.0);
        gtk_widget_set_sensitive (empty, FALSE);
        gtk_box_pack_start (GTK_BOX (list_box), empty, FALSE, FALSE, 0);
    }

    for (l = descriptors; l != NULL; l = l->next)
    {
        PnPluginDescriptor *desc      = l->data;
        gboolean            disabled  =
                pn_preferences_is_plugin_disabled (prefs, desc->basename);
        GtkWidget          *row;
        GtkWidget          *check;
        GtkWidget          *meta;
        gchar              *primary;
        gchar              *secondary;

        row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);

        /* Primary label: plugin display name (or basename when the
         * .so is on disk but currently disabled, so we have no
         * PnPluginInfo to read from). */
        if (desc->loaded && desc->name != NULL)
        {
            const gchar *version = (desc->version != NULL)
                                       ? desc->version : "";
            primary = g_strdup_printf ("%s %s", desc->name, version);
        }
        else
        {
            primary = g_strdup (desc->basename);
        }

        check = gtk_check_button_new_with_label (primary);
        g_free (primary);

        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (check), !disabled);
        g_object_set_data_full (G_OBJECT (check),
                                PLUGIN_BASENAME_KEY,
                                g_strdup (desc->basename),
                                g_free);
        g_signal_connect (check, "toggled",
                          G_CALLBACK (on_plugin_toggled), NULL);
        gtk_box_pack_start (GTK_BOX (row), check, FALSE, FALSE, 0);

        /* Secondary line: basename + optional description.  Helps the
         * user disambiguate two plugins with similar names. */
        if (desc->loaded && desc->description != NULL
            && *desc->description != '\0')
            secondary = g_strdup_printf ("(%s) — %s",
                                         desc->basename,
                                         desc->description);
        else if (desc->loaded)
            secondary = g_strdup_printf ("(%s)", desc->basename);
        else
            secondary = g_strdup ("(disabled — not loaded)");

        meta = gtk_label_new (secondary);
        g_free (secondary);
        gtk_label_set_xalign (GTK_LABEL (meta), 0.0);
        /* Ellipsize at the end so a long description does not push
         * the page (and therefore the whole dialog) wider than the
         * other pages in the stack. */
        gtk_label_set_ellipsize    (GTK_LABEL (meta), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars (GTK_LABEL (meta), 40);
        gtk_widget_set_sensitive (meta, FALSE);
        gtk_box_pack_start (GTK_BOX (row), meta, TRUE, TRUE, 0);

        gtk_box_pack_start (GTK_BOX (list_box), row, FALSE, FALSE, 0);
    }

    g_list_free_full (descriptors,
                      (GDestroyNotify) pn_plugin_descriptor_free);

    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (scroll), list_box);
    gtk_box_pack_start (GTK_BOX (box), scroll, TRUE, TRUE, 0);

    return box;
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_preferences_dialog_dispose (GObject *object)
{
    PnPreferencesDialog *self = PN_PREFERENCES_DIALOG (object);
    PnPreferences       *prefs = pn_preferences_get_default ();

    if (self->notify_snap_id != 0)
    {
        g_signal_handler_disconnect (prefs, self->notify_snap_id);
        self->notify_snap_id = 0;
    }
    if (self->notify_type_id != 0)
    {
        g_signal_handler_disconnect (prefs, self->notify_type_id);
        self->notify_type_id = 0;
    }
    if (self->notify_color_id != 0)
    {
        g_signal_handler_disconnect (prefs, self->notify_color_id);
        self->notify_color_id = 0;
    }

    G_OBJECT_CLASS (pn_preferences_dialog_parent_class)->dispose (object);
}

static void
pn_preferences_dialog_class_init (PnPreferencesDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->dispose = pn_preferences_dialog_dispose;
}

static void
pn_preferences_dialog_init (PnPreferencesDialog *self)
{
    GtkWidget     *outer;
    GtkWidget     *paned;
    GtkWidget     *left_scroll;
    GtkWidget     *right_scroll;
    GtkWidget     *button_box;
    GtkWidget     *close_button;
    GtkWidget     *worksheet_page;
    GtkWidget     *grid_page;
    GtkWidget     *plugins_page;
    PnPreferences *prefs;

    gtk_window_set_title (GTK_WINDOW (self), "Preferences");
    gtk_window_set_default_size (GTK_WINDOW (self), 640, 420);
    gtk_window_set_modal (GTK_WINDOW (self), FALSE);
    gtk_window_set_destroy_with_parent (GTK_WINDOW (self), FALSE);
    gtk_window_set_position (GTK_WINDOW (self), GTK_WIN_POS_CENTER_ON_PARENT);

    outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (self), outer);

    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position (GTK_PANED (paned), 180);
    gtk_paned_set_wide_handle (GTK_PANED (paned), TRUE);
    gtk_box_pack_start (GTK_BOX (outer), paned, TRUE, TRUE, 0);

    /* Left: category tree, in a scroller in case future additions
     * push it past the dialog height. */
    build_category_tree (self);
    left_scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (left_scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (left_scroll), self->category_view);
    gtk_paned_pack1 (GTK_PANED (paned), left_scroll, FALSE, FALSE);

    /* Right: page stack. */
    self->stack = GTK_STACK (gtk_stack_new ());
    gtk_stack_set_transition_type (self->stack,
                                   GTK_STACK_TRANSITION_TYPE_NONE);
    /* By default the stack requests the size of its largest child;
     * that lets a single oversized page (a long plugin description
     * with a wide row) drag the whole dialog wider than every other
     * page actually needs.  Ask the stack to size to the visible
     * child instead so each page is sized in isolation. */
    gtk_stack_set_hhomogeneous (self->stack, FALSE);
    gtk_stack_set_vhomogeneous (self->stack, FALSE);

    worksheet_page = build_placeholder_page (
            "Worksheet Editor",
            "Select a subcategory on the left to edit "
            "worksheet-editor settings.");
    gtk_stack_add_named (self->stack, worksheet_page, PAGE_ID_WORKSHEET);

    grid_page = build_grid_page (self);
    gtk_stack_add_named (self->stack, grid_page, PAGE_ID_GRID);

    plugins_page = build_plugins_page (self);
    gtk_stack_add_named (self->stack, plugins_page, PAGE_ID_PLUGINS);

    right_scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (right_scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (right_scroll),
                       GTK_WIDGET (self->stack));
    gtk_paned_pack2 (GTK_PANED (paned), right_scroll, TRUE, FALSE);

    /* Footer with a Close button.  Non-modal: edits are live, so there
     * is no Apply / OK -- "Close" simply hides the dialog. */
    button_box = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout (GTK_BUTTON_BOX (button_box),
                               GTK_BUTTONBOX_END);
    g_object_set (button_box,
                  "margin-start", 8, "margin-end", 8,
                  "margin-top",   6, "margin-bottom", 6,
                  NULL);
    gtk_box_pack_start (GTK_BOX (outer), button_box, FALSE, FALSE, 0);

    close_button = gtk_button_new_with_mnemonic ("_Close");
    g_signal_connect_swapped (close_button, "clicked",
                              G_CALLBACK (gtk_widget_destroy), self);
    gtk_container_add (GTK_CONTAINER (button_box), close_button);

    /* Pull the current preferences into every widget, then wire the
     * notify::* handlers so a live change elsewhere refreshes the
     * dialog without bouncing through us. */
    prefs = pn_preferences_get_default ();
    sync_widgets_from_prefs (self);

    self->notify_snap_id = g_signal_connect (
            prefs, "notify::snap-to-grid",
            G_CALLBACK (on_pref_notify), self);
    self->notify_type_id = g_signal_connect (
            prefs, "notify::grid-type",
            G_CALLBACK (on_pref_notify), self);
    self->notify_color_id = g_signal_connect (
            prefs, "notify::grid-color",
            G_CALLBACK (on_pref_notify), self);
}

/* ------------------------------------------------------------------ */
/*  Public surface                                                     */
/* ------------------------------------------------------------------ */

static void
on_dialog_destroyed (gpointer data, GObject *where_the_object_was)
{
    (void) data;
    if (current_dialog == (PnPreferencesDialog *) where_the_object_was)
        current_dialog = NULL;
}

void
pn_preferences_dialog_present (GtkWindow *parent)
{
    if (current_dialog == NULL)
    {
        current_dialog = g_object_new (PN_TYPE_PREFERENCES_DIALOG, NULL);
        g_object_weak_ref (G_OBJECT (current_dialog),
                           on_dialog_destroyed, NULL);
    }

    if (parent != NULL)
        gtk_window_set_transient_for (GTK_WINDOW (current_dialog), parent);

    gtk_widget_show_all (GTK_WIDGET (current_dialog));
    gtk_window_present  (GTK_WINDOW (current_dialog));
}
