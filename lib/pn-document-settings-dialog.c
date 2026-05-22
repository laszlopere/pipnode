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

#include "pn-document-settings-dialog.h"

/* The dialog edits one #PnFlow's document globals.  Unlike the process-
 * wide preferences dialog it is bound to a specific open file, so the
 * single-instance bookkeeping is per-flow: the live dialog is stashed on
 * the flow under this key and raised again instead of duplicated. */
#define DIALOG_DATA_KEY "pn-document-settings-dialog"

/* Per-row editor state, attached to each row container. */
#define ROW_CTX_KEY     "pn-doc-settings-row-ctx"

struct _PnDocumentSettingsDialog
{
    GtkWindow parent_instance;

    /* The document being edited.  Held with a reference for the dialog's
     * lifetime; the window that owns the flow keeps it alive anyway, but
     * the ref makes the dependency explicit and crash-proof. */
    PnFlow       *flow;

    /* Left category tree -> right page stack, mirroring the preferences
     * dialog so future document-setting pages slot in beside "Globals". */
    GtkTreeStore *category_store;
    GtkWidget    *category_view;
    GtkStack     *stack;

    /* The vertical box holding one editable row per global; rebuilt
     * wholesale on add/delete. */
    GtkWidget    *globals_list;

    /* TRUE while rows are being (re)built or programmatically refilled, so
     * the editors' own change signals do not write straight back. */
    gboolean      rebuilding;
};

G_DEFINE_TYPE (PnDocumentSettingsDialog, pn_document_settings_dialog,
               GTK_TYPE_WINDOW)

enum {
    COL_LABEL,
    COL_PAGE_ID,
    N_COLS,
};

#define PAGE_ID_GLOBALS "globals"

/* ------------------------------------------------------------------ */
/*  Per-row state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    PnDocumentSettingsDialog *self;
    gchar     *name;          /* current key in the flow's globals table */
    GtkWidget *name_entry;
    GtkWidget *type_combo;
    GtkWidget *value_slot;    /* container holding @value_editor          */
    GtkWidget *value_editor;  /* checkbox (bool) or entry (int/dbl/str)   */
    GType      type;          /* current value GType                       */
} RowCtx;

static void
row_ctx_free (gpointer data)
{
    RowCtx *ctx = data;
    g_free (ctx->name);
    g_free (ctx);
}

/* ------------------------------------------------------------------ */
/*  Type <-> combo-index mapping                                       */
/* ------------------------------------------------------------------ */

static GType
gtype_for_combo_index (gint idx)
{
    switch (idx)
    {
    case 0:  return G_TYPE_BOOLEAN;
    case 1:  return G_TYPE_INT64;
    case 2:  return G_TYPE_DOUBLE;
    case 3:  return G_TYPE_STRING;
    default: return G_TYPE_STRING;
    }
}

static gint
combo_index_for_gtype (GType type)
{
    if (type == G_TYPE_BOOLEAN) return 0;
    if (type == G_TYPE_INT64)   return 1;
    if (type == G_TYPE_DOUBLE)  return 2;
    return 3; /* string */
}

/* Forward declarations for the web of row callbacks. */
static GtkWidget *make_value_editor   (RowCtx *ctx, const GValue *value);
static void       commit_entry_value  (RowCtx *ctx);
static void       revert_value_editor (RowCtx *ctx);
static GtkWidget *make_global_row     (PnDocumentSettingsDialog *self,
                                       const gchar *name,
                                       const GValue *value);
static void       rebuild_globals_rows (PnDocumentSettingsDialog *self);

/* ------------------------------------------------------------------ */
/*  Value editors                                                      */
/* ------------------------------------------------------------------ */

static void
on_value_toggled (GtkToggleButton *button, gpointer user_data)
{
    RowCtx *ctx = user_data;
    GValue  v   = G_VALUE_INIT;

    if (ctx->self->rebuilding)
        return;

    g_value_init (&v, G_TYPE_BOOLEAN);
    g_value_set_boolean (&v, gtk_toggle_button_get_active (button));
    pn_flow_set_global (ctx->self->flow, ctx->name, &v);
    g_value_unset (&v);
}

static void
on_value_entry_activate (GtkEntry *entry, gpointer user_data)
{
    (void) entry;
    commit_entry_value (user_data);
}

static gboolean
on_value_entry_focus_out (GtkWidget *widget, GdkEvent *event,
                          gpointer user_data)
{
    (void) widget; (void) event;
    commit_entry_value (user_data);
    return FALSE;
}

/** Re-fill the entry from the value currently stored in the flow, used to
 *  reject input that does not parse. */
static void
revert_value_editor (RowCtx *ctx)
{
    GValue v = G_VALUE_INIT;

    if (!pn_flow_get_global (ctx->self->flow, ctx->name, &v))
        return;

    ctx->self->rebuilding = TRUE;
    if (ctx->type == G_TYPE_INT64 && G_VALUE_HOLDS_INT64 (&v))
    {
        gchar *s = g_strdup_printf ("%" G_GINT64_FORMAT,
                                    g_value_get_int64 (&v));
        gtk_entry_set_text (GTK_ENTRY (ctx->value_editor), s);
        g_free (s);
    }
    else if (ctx->type == G_TYPE_DOUBLE && G_VALUE_HOLDS_DOUBLE (&v))
    {
        gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (buf, sizeof buf, g_value_get_double (&v));
        gtk_entry_set_text (GTK_ENTRY (ctx->value_editor), buf);
    }
    ctx->self->rebuilding = FALSE;
    g_value_unset (&v);
}

static void
commit_entry_value (RowCtx *ctx)
{
    const gchar *text;
    GValue       v = G_VALUE_INIT;

    if (ctx->self->rebuilding)
        return;

    text = gtk_entry_get_text (GTK_ENTRY (ctx->value_editor));

    if (ctx->type == G_TYPE_INT64)
    {
        gchar  *end = NULL;
        gint64  n   = g_ascii_strtoll (text, &end, 10);
        if (end == text || end == NULL || *end != '\0')
        {
            revert_value_editor (ctx);
            return;
        }
        g_value_init (&v, G_TYPE_INT64);
        g_value_set_int64 (&v, n);
    }
    else if (ctx->type == G_TYPE_DOUBLE)
    {
        gchar   *end = NULL;
        gdouble  d   = g_ascii_strtod (text, &end);
        if (end == text || end == NULL || *end != '\0')
        {
            revert_value_editor (ctx);
            return;
        }
        g_value_init (&v, G_TYPE_DOUBLE);
        g_value_set_double (&v, d);
    }
    else /* G_TYPE_STRING */
    {
        g_value_init (&v, G_TYPE_STRING);
        g_value_set_string (&v, text);
    }

    pn_flow_set_global (ctx->self->flow, ctx->name, &v);
    g_value_unset (&v);
}

/** Build the value editor matching @ctx->type, primed from @value. */
static GtkWidget *
make_value_editor (RowCtx *ctx, const GValue *value)
{
    GtkWidget *w;

    if (ctx->type == G_TYPE_BOOLEAN)
    {
        w = gtk_check_button_new ();
        gtk_toggle_button_set_active (
                GTK_TOGGLE_BUTTON (w),
                value != NULL && G_VALUE_HOLDS_BOOLEAN (value)
                    && g_value_get_boolean (value));
        g_signal_connect (w, "toggled",
                          G_CALLBACK (on_value_toggled), ctx);
        gtk_widget_set_halign (w, GTK_ALIGN_START);
        return w;
    }

    /* Integer / double / string all use an entry. */
    w = gtk_entry_new ();
    if (ctx->type == G_TYPE_INT64
        && value != NULL && G_VALUE_HOLDS_INT64 (value))
    {
        gchar *s = g_strdup_printf ("%" G_GINT64_FORMAT,
                                    g_value_get_int64 (value));
        gtk_entry_set_text (GTK_ENTRY (w), s);
        g_free (s);
    }
    else if (ctx->type == G_TYPE_DOUBLE
             && value != NULL && G_VALUE_HOLDS_DOUBLE (value))
    {
        gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (buf, sizeof buf, g_value_get_double (value));
        gtk_entry_set_text (GTK_ENTRY (w), buf);
    }
    else if (ctx->type == G_TYPE_STRING
             && value != NULL && G_VALUE_HOLDS_STRING (value)
             && g_value_get_string (value) != NULL)
    {
        gtk_entry_set_text (GTK_ENTRY (w), g_value_get_string (value));
    }

    gtk_widget_set_hexpand (w, TRUE);
    gtk_widget_set_halign  (w, GTK_ALIGN_FILL);
    g_signal_connect (w, "activate",
                      G_CALLBACK (on_value_entry_activate), ctx);
    g_signal_connect (w, "focus-out-event",
                      G_CALLBACK (on_value_entry_focus_out), ctx);
    return w;
}

/* ------------------------------------------------------------------ */
/*  Type change                                                        */
/* ------------------------------------------------------------------ */

static void
on_type_changed (GtkComboBox *combo, gpointer user_data)
{
    RowCtx *ctx = user_data;
    GType   new_type;
    GValue  v = G_VALUE_INIT;

    if (ctx->self->rebuilding)
        return;

    new_type = gtype_for_combo_index (gtk_combo_box_get_active (combo));
    if (new_type == ctx->type)
        return;
    ctx->type = new_type;

    /* Reset to the new type's natural default (g_value_init zeroes
     * boolean/int/double; strings need an explicit ""). */
    g_value_init (&v, new_type);
    if (new_type == G_TYPE_STRING)
        g_value_set_string (&v, "");

    /* Swap the value editor in place. */
    ctx->self->rebuilding = TRUE;
    if (ctx->value_editor != NULL)
        gtk_widget_destroy (ctx->value_editor);
    ctx->value_editor = make_value_editor (ctx, &v);
    gtk_box_pack_start (GTK_BOX (ctx->value_slot), ctx->value_editor,
                        TRUE, TRUE, 0);
    gtk_widget_show_all (ctx->value_slot);
    ctx->self->rebuilding = FALSE;

    pn_flow_set_global (ctx->self->flow, ctx->name, &v);
    g_value_unset (&v);
}

/* ------------------------------------------------------------------ */
/*  Rename                                                             */
/* ------------------------------------------------------------------ */

static void
commit_rename (RowCtx *ctx)
{
    const gchar *new_name;
    GValue       v = G_VALUE_INIT;

    if (ctx->self->rebuilding)
        return;

    new_name = gtk_entry_get_text (GTK_ENTRY (ctx->name_entry));
    if (g_strcmp0 (new_name, ctx->name) == 0)
        return;

    /* Empty or already-taken names are rejected: snap the entry back. */
    if (new_name == NULL || *new_name == '\0')
    {
        ctx->self->rebuilding = TRUE;
        gtk_entry_set_text (GTK_ENTRY (ctx->name_entry), ctx->name);
        ctx->self->rebuilding = FALSE;
        return;
    }

    {
        GValue probe = G_VALUE_INIT;
        if (pn_flow_get_global (ctx->self->flow, new_name, &probe))
        {
            g_value_unset (&probe);
            ctx->self->rebuilding = TRUE;
            gtk_entry_set_text (GTK_ENTRY (ctx->name_entry), ctx->name);
            ctx->self->rebuilding = FALSE;
            return;
        }
    }

    /* Move the value from the old key to the new one. */
    if (pn_flow_get_global (ctx->self->flow, ctx->name, &v))
    {
        pn_flow_remove_global (ctx->self->flow, ctx->name);
        pn_flow_set_global (ctx->self->flow, new_name, &v);
        g_value_unset (&v);
    }
    g_free (ctx->name);
    ctx->name = g_strdup (new_name);
}

static void
on_name_activate (GtkEntry *entry, gpointer user_data)
{
    (void) entry;
    commit_rename (user_data);
}

static gboolean
on_name_focus_out (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void) widget; (void) event;
    commit_rename (user_data);
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Delete / Add                                                       */
/* ------------------------------------------------------------------ */

static void
on_delete_clicked (GtkButton *button, gpointer user_data)
{
    RowCtx                   *ctx  = user_data;
    PnDocumentSettingsDialog *self = ctx->self;

    (void) button;
    if (self->rebuilding)
        return;

    /* rebuild frees @ctx (it lives on the row being destroyed), so grab
     * everything off it first. */
    pn_flow_remove_global (self->flow, ctx->name);
    rebuild_globals_rows (self);
}

/** Move keyboard focus to the name entry of the row whose key is @name. */
static void
focus_row_named (PnDocumentSettingsDialog *self, const gchar *name)
{
    GList *rows = gtk_container_get_children (GTK_CONTAINER (self->globals_list));
    GList *l;

    for (l = rows; l != NULL; l = l->next)
    {
        RowCtx *ctx = g_object_get_data (G_OBJECT (l->data), ROW_CTX_KEY);
        if (ctx != NULL && g_strcmp0 (ctx->name, name) == 0)
        {
            gtk_widget_grab_focus (ctx->name_entry);
            break;
        }
    }
    g_list_free (rows);
}

static void
on_add_clicked (GtkButton *button, gpointer user_data)
{
    PnDocumentSettingsDialog *self = user_data;
    gchar                    *name = NULL;
    guint                     n;
    GValue                    v    = G_VALUE_INIT;

    (void) button;
    if (self->rebuilding)
        return;

    /* Lowest free "variable_N". */
    for (n = 1; ; n++)
    {
        GValue probe = G_VALUE_INIT;
        g_free (name);
        name = g_strdup_printf ("variable_%u", n);
        if (!pn_flow_get_global (self->flow, name, &probe))
            break;
        g_value_unset (&probe);
    }

    g_value_init (&v, G_TYPE_STRING);
    g_value_set_string (&v, "");
    pn_flow_set_global (self->flow, name, &v);
    g_value_unset (&v);

    rebuild_globals_rows (self);
    focus_row_named (self, name);
    g_free (name);
}

/* ------------------------------------------------------------------ */
/*  Row + list construction                                            */
/* ------------------------------------------------------------------ */

static GtkWidget *
make_global_row (PnDocumentSettingsDialog *self,
                 const gchar              *name,
                 const GValue             *value)
{
    RowCtx    *ctx = g_new0 (RowCtx, 1);
    GtkWidget *row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *del;

    ctx->self = self;
    ctx->name = g_strdup (name);
    ctx->type = G_VALUE_TYPE (value);

    /* Name. */
    ctx->name_entry = gtk_entry_new ();
    gtk_entry_set_text (GTK_ENTRY (ctx->name_entry), name);
    gtk_entry_set_width_chars (GTK_ENTRY (ctx->name_entry), 14);
    g_signal_connect (ctx->name_entry, "activate",
                      G_CALLBACK (on_name_activate), ctx);
    g_signal_connect (ctx->name_entry, "focus-out-event",
                      G_CALLBACK (on_name_focus_out), ctx);
    gtk_box_pack_start (GTK_BOX (row), ctx->name_entry, FALSE, FALSE, 0);

    /* Type. */
    ctx->type_combo = gtk_combo_box_text_new ();
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (ctx->type_combo),
                                    "Boolean");
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (ctx->type_combo),
                                    "Integer");
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (ctx->type_combo),
                                    "Double");
    gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (ctx->type_combo),
                                    "String");
    gtk_combo_box_set_active (GTK_COMBO_BOX (ctx->type_combo),
                              combo_index_for_gtype (ctx->type));
    g_signal_connect (ctx->type_combo, "changed",
                      G_CALLBACK (on_type_changed), ctx);
    gtk_box_pack_start (GTK_BOX (row), ctx->type_combo, FALSE, FALSE, 0);

    /* Value (in a slot so the editor can be swapped on type change). */
    ctx->value_slot = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand (ctx->value_slot, TRUE);
    ctx->value_editor = make_value_editor (ctx, value);
    gtk_box_pack_start (GTK_BOX (ctx->value_slot), ctx->value_editor,
                        TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (row), ctx->value_slot, TRUE, TRUE, 0);

    /* Delete. */
    del = gtk_button_new_from_icon_name ("edit-delete-symbolic",
                                         GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text (del, "Delete this variable");
    g_signal_connect (del, "clicked", G_CALLBACK (on_delete_clicked), ctx);
    gtk_box_pack_start (GTK_BOX (row), del, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (row), ROW_CTX_KEY, ctx, row_ctx_free);
    return row;
}

static void
rebuild_globals_rows (PnDocumentSettingsDialog *self)
{
    GList *children, *l, *names;

    self->rebuilding = TRUE;

    children = gtk_container_get_children (GTK_CONTAINER (self->globals_list));
    for (l = children; l != NULL; l = l->next)
        gtk_widget_destroy (GTK_WIDGET (l->data));
    g_list_free (children);

    names = (self->flow != NULL) ? pn_flow_list_globals (self->flow) : NULL;

    if (names == NULL)
    {
        GtkWidget *empty = gtk_label_new ("No variables defined.");
        gtk_label_set_xalign (GTK_LABEL (empty), 0.0);
        gtk_widget_set_sensitive (empty, FALSE);
        gtk_box_pack_start (GTK_BOX (self->globals_list), empty,
                            FALSE, FALSE, 0);
    }
    else
    {
        for (l = names; l != NULL; l = l->next)
        {
            const gchar *name = l->data;
            GValue       v    = G_VALUE_INIT;

            if (pn_flow_get_global (self->flow, name, &v))
            {
                GtkWidget *row = make_global_row (self, name, &v);
                gtk_box_pack_start (GTK_BOX (self->globals_list), row,
                                    FALSE, FALSE, 0);
                g_value_unset (&v);
            }
        }
        g_list_free_full (names, g_free);
    }

    gtk_widget_show_all (self->globals_list);
    self->rebuilding = FALSE;
}

/* ------------------------------------------------------------------ */
/*  Pages + category tree                                              */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_globals_page (PnDocumentSettingsDialog *self)
{
    GtkWidget     *box;
    GtkWidget     *heading;
    GtkWidget     *blurb;
    GtkWidget     *scroll;
    GtkWidget     *add_btn;
    GtkWidget     *add_box;
    PangoAttrList *attrs;

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    g_object_set (box,
                  "margin-start", 16, "margin-end", 16,
                  "margin-top",   16, "margin-bottom", 16,
                  NULL);

    heading = gtk_label_new ("Globals");
    attrs   = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    pango_attr_list_insert (attrs, pango_attr_scale_new  (PANGO_SCALE_LARGE));
    gtk_label_set_attributes (GTK_LABEL (heading), attrs);
    pango_attr_list_unref (attrs);
    gtk_widget_set_halign (heading, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (box), heading, FALSE, FALSE, 0);

    blurb = gtk_label_new (
            "These variables are saved in this file and shared by every "
            "sheet in it.");
    gtk_label_set_line_wrap (GTK_LABEL (blurb), TRUE);
    gtk_label_set_xalign    (GTK_LABEL (blurb), 0.0);
    gtk_label_set_max_width_chars (GTK_LABEL (blurb), 50);
    gtk_box_pack_start (GTK_BOX (box), blurb, FALSE, FALSE, 0);

    self->globals_list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (scroll), self->globals_list);
    gtk_box_pack_start (GTK_BOX (box), scroll, TRUE, TRUE, 0);

    add_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    add_btn = gtk_button_new_with_mnemonic ("_Add variable");
    gtk_button_set_image (GTK_BUTTON (add_btn),
                          gtk_image_new_from_icon_name (
                                  "list-add-symbolic",
                                  GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image (GTK_BUTTON (add_btn), TRUE);
    gtk_widget_set_halign (add_btn, GTK_ALIGN_START);
    g_signal_connect (add_btn, "clicked", G_CALLBACK (on_add_clicked), self);
    gtk_box_pack_start (GTK_BOX (add_box), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), add_box, FALSE, FALSE, 0);

    return box;
}

static void
on_category_selection_changed (GtkTreeSelection *selection,
                               gpointer          user_data)
{
    PnDocumentSettingsDialog *self = user_data;
    GtkTreeIter               iter;
    GtkTreeModel             *model;
    gchar                    *page_id = NULL;

    if (!gtk_tree_selection_get_selected (selection, &model, &iter))
        return;
    gtk_tree_model_get (model, &iter, COL_PAGE_ID, &page_id, -1);
    if (page_id != NULL)
        gtk_stack_set_visible_child_name (self->stack, page_id);
    g_free (page_id);
}

static void
build_category_tree (PnDocumentSettingsDialog *self)
{
    GtkTreeIter        globals_iter;
    GtkCellRenderer   *renderer;
    GtkTreeViewColumn *column;
    GtkTreeSelection  *selection;
    GtkTreePath       *path;

    self->category_store = gtk_tree_store_new (N_COLS,
                                               G_TYPE_STRING,
                                               G_TYPE_STRING);

    gtk_tree_store_append (self->category_store, &globals_iter, NULL);
    gtk_tree_store_set (self->category_store, &globals_iter,
                        COL_LABEL,   "Globals",
                        COL_PAGE_ID, PAGE_ID_GLOBALS,
                        -1);

    self->category_view = gtk_tree_view_new_with_model (
            GTK_TREE_MODEL (self->category_store));
    g_object_unref (self->category_store);
    gtk_tree_view_set_headers_visible (
            GTK_TREE_VIEW (self->category_view), FALSE);

    renderer = gtk_cell_renderer_text_new ();
    column   = gtk_tree_view_column_new_with_attributes (
                       NULL, renderer, "text", COL_LABEL, NULL);
    gtk_tree_view_append_column (GTK_TREE_VIEW (self->category_view), column);

    selection = gtk_tree_view_get_selection (
            GTK_TREE_VIEW (self->category_view));
    gtk_tree_selection_set_mode (selection, GTK_SELECTION_BROWSE);
    g_signal_connect (selection, "changed",
                      G_CALLBACK (on_category_selection_changed), self);

    path = gtk_tree_model_get_path (GTK_TREE_MODEL (self->category_store),
                                    &globals_iter);
    gtk_tree_selection_select_path (selection, path);
    gtk_tree_path_free (path);
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_document_settings_dialog_dispose (GObject *object)
{
    PnDocumentSettingsDialog *self = PN_DOCUMENT_SETTINGS_DIALOG (object);

    if (self->flow != NULL)
    {
        if (g_object_get_data (G_OBJECT (self->flow), DIALOG_DATA_KEY) == self)
            g_object_set_data (G_OBJECT (self->flow), DIALOG_DATA_KEY, NULL);
        g_clear_object (&self->flow);
    }

    G_OBJECT_CLASS (pn_document_settings_dialog_parent_class)->dispose (object);
}

static void
pn_document_settings_dialog_class_init (PnDocumentSettingsDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = pn_document_settings_dialog_dispose;
}

static void
pn_document_settings_dialog_init (PnDocumentSettingsDialog *self)
{
    GtkWidget *outer;
    GtkWidget *paned;
    GtkWidget *left_scroll;
    GtkWidget *right_scroll;
    GtkWidget *button_box;
    GtkWidget *close_button;
    GtkWidget *globals_page;

    gtk_window_set_title (GTK_WINDOW (self), "Document Settings");
    gtk_window_set_default_size (GTK_WINDOW (self), 560, 380);
    gtk_window_set_modal (GTK_WINDOW (self), FALSE);
    gtk_window_set_position (GTK_WINDOW (self), GTK_WIN_POS_CENTER_ON_PARENT);

    outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (self), outer);

    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position (GTK_PANED (paned), 150);
    gtk_paned_set_wide_handle (GTK_PANED (paned), TRUE);
    gtk_box_pack_start (GTK_BOX (outer), paned, TRUE, TRUE, 0);

    build_category_tree (self);
    left_scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (left_scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (left_scroll), self->category_view);
    gtk_paned_pack1 (GTK_PANED (paned), left_scroll, FALSE, FALSE);

    self->stack = GTK_STACK (gtk_stack_new ());
    gtk_stack_set_transition_type (self->stack,
                                   GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_hhomogeneous (self->stack, FALSE);
    gtk_stack_set_vhomogeneous (self->stack, FALSE);

    globals_page = build_globals_page (self);
    gtk_stack_add_named (self->stack, globals_page, PAGE_ID_GLOBALS);

    right_scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (right_scroll),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (right_scroll),
                       GTK_WIDGET (self->stack));
    gtk_paned_pack2 (GTK_PANED (paned), right_scroll, TRUE, FALSE);

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
}

/* ------------------------------------------------------------------ */
/*  Public surface                                                     */
/* ------------------------------------------------------------------ */

void
pn_document_settings_dialog_present (GtkWindow *parent, PnFlow *flow)
{
    PnDocumentSettingsDialog *dialog;

    g_return_if_fail (flow == NULL || PN_IS_FLOW (flow));
    if (flow == NULL)
        return;

    dialog = g_object_get_data (G_OBJECT (flow), DIALOG_DATA_KEY);
    if (dialog == NULL)
    {
        dialog = g_object_new (PN_TYPE_DOCUMENT_SETTINGS_DIALOG, NULL);
        dialog->flow = g_object_ref (flow);
        g_object_set_data (G_OBJECT (flow), DIALOG_DATA_KEY, dialog);
    }

    if (parent != NULL)
    {
        gtk_window_set_transient_for (GTK_WINDOW (dialog), parent);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);
    }

    rebuild_globals_rows (dialog);

    gtk_widget_show_all (GTK_WIDGET (dialog));
    gtk_window_present  (GTK_WINDOW (dialog));
}
