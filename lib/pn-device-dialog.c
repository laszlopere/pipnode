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
/*  PnDeviceDialog -- the reusable device-dialog frame.                */
/*                                                                     */
/*  Extracted from pn-mesh-dialog.c's build_dialog / build_status_bar  */
/*  / show_loading / hide_loading / busy_inc / busy_dec, with no        */
/*  behaviour change.  The struct is attached to the GtkDialog via      */
/*  qdata so it lives exactly as long as the dialog.                    */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-device-dialog.h"
#include "pn-device-provider.h"

#define PN_DEVICE_DIALOG_CTX_QDATA "pn-device-dialog-ctx"
#define PN_DEVICE_DIALOG_ROW_QDATA "pn-device-dialog-row"

/* Default HPaned divider for the built-in device-list pane. */
#define PN_DEVICE_DIALOG_DIVIDER 250

struct _PnDeviceDialog
{
    GtkWidget   *dialog;
    GtkWidget   *content;          /* the dialog's content vbox */
    GtkWidget   *paned;            /* NULL until a sidebar is set */
    GtkWidget   *body;             /* the overlay (notebook + spinner) */
    GtkNotebook *notebook;
    GtkWidget   *loading_overlay;  /* spinner+label box; visibility toggled */
    GtkSpinner  *loading_spinner;
    GtkLabel    *status_label;

    /* Reference-counted busy state.  The overlay is shown on the 0->1
     * edge and hidden on the 1->0 edge so concurrent waiters keep the
     * spinner up until the last one settles. */
    gint         busy_count;

    /* Built-in device-list pane (PN_DEVICE_DIALOG_WITH_DEVICE_LIST).
     * NULL across the board when the flag was not passed. */
    GtkWidget   *list_root;        /* the sidebar widget */
    GtkStack    *list_stack;       /* pre-scan / empty-result / list */
    GtkListBox  *list;
    GtkWidget   *prescan_page;
    GtkWidget   *empty_page;
    GtkWidget   *reload_menu;      /* right-click Reload, built lazily */

    /* id of the most recently selected row, so a rescan can re-select
     * the same device (refreshing the right pane) instead of going
     * stale; NULL until something is selected. */
    gchar       *selected_id;
    gboolean     auto_scan;            /* scan once on open */
    gboolean     auto_select_pending;  /* arm first-row auto-select */
    gboolean     initial_scan_scheduled;

    PnDeviceScanFunc      scan_cb;
    gpointer              scan_ud;
    PnDeviceSelectedFunc  selected_cb;
    gpointer              selected_ud;

    /* Empty-state hint strings (owned); NULL falls back to the
     * built-in defaults in build_empty_page(). */
    gchar       *prescan_icon, *prescan_primary, *prescan_secondary;
    gchar       *empty_icon,   *empty_primary,   *empty_secondary;

    /* D-Bus introspection seam (see the header).  provider_id is the
     * registry key, captured at construction; the callbacks let the host
     * watch changes and a plugin expose rich per-device detail. */
    gchar                     *provider_id;
    PnDeviceDialogChangedFunc  changed_cb;
    gpointer                   changed_ud;
    PnDeviceDialogDetailFunc   detail_cb;
    gpointer                   detail_ud;
};

/* ------------------------------------------------------------------ */
/*  Open-dialog registry + change notification                          */
/* ------------------------------------------------------------------ */

/* provider id -> the open #PnDeviceDialog* (borrowed; owned by its
 * GtkDialog).  Keyed by a strdup of the id; at most one dialog per
 * provider (a second present raises the first), so the most recently
 * opened wins across windows.  Lazily created, never torn down. */
static GHashTable                 *open_dialogs   = NULL;
static PnDeviceDialogObserverFunc  observer_cb    = NULL;
static gpointer                    observer_ud    = NULL;

/* Notify the host that the device set / status / selection changed. */
static void
emit_changed (PnDeviceDialog *self)
{
    if (self->changed_cb != NULL)
        self->changed_cb (self, self->changed_ud);
}

/* ------------------------------------------------------------------ */
/*  Status bar                                                          */
/* ------------------------------------------------------------------ */

void
pn_device_dialog_set_status (PnDeviceDialog *self, const gchar *text)
{
    g_return_if_fail (self != NULL);
    gtk_label_set_text (self->status_label, text != NULL ? text : "");
    emit_changed (self);
}

void
pn_device_dialog_set_statusf (PnDeviceDialog *self, const gchar *fmt, ...)
{
    va_list  ap;
    gchar   *text;

    g_return_if_fail (self != NULL);

    va_start (ap, fmt);
    text = g_strdup_vprintf (fmt, ap);
    va_end (ap);

    pn_device_dialog_set_status (self, text);
    g_free (text);
}

/* ------------------------------------------------------------------ */
/*  Busy overlay                                                        */
/* ------------------------------------------------------------------ */

/* Show the big spinner over the notebook and lock the notebook so the
 * user cannot switch tabs (or read a half-populated form) while a
 * device round-trip is in flight. */
static void
show_loading (PnDeviceDialog *self)
{
    gtk_widget_set_sensitive (GTK_WIDGET (self->notebook), FALSE);
    gtk_widget_show (self->loading_overlay);
    gtk_spinner_start (self->loading_spinner);
}

static void
hide_loading (PnDeviceDialog *self)
{
    gtk_spinner_stop (self->loading_spinner);
    gtk_widget_hide (self->loading_overlay);
    gtk_widget_set_sensitive (GTK_WIDGET (self->notebook), TRUE);
}

void
pn_device_dialog_push_busy (PnDeviceDialog *self)
{
    g_return_if_fail (self != NULL);
    if (self->busy_count++ == 0)
        show_loading (self);
}

void
pn_device_dialog_pop_busy (PnDeviceDialog *self)
{
    g_return_if_fail (self != NULL);
    if (self->busy_count == 0)
        return;
    if (--self->busy_count == 0)
        hide_loading (self);
}

gboolean
pn_device_dialog_is_busy (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, FALSE);
    return self->busy_count > 0;
}

/* ------------------------------------------------------------------ */
/*  Notebook                                                            */
/* ------------------------------------------------------------------ */

GtkNotebook *
pn_device_dialog_get_notebook (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->notebook;
}

void
pn_device_dialog_append_page (PnDeviceDialog *self,
                              GtkWidget      *child,
                              const gchar    *tab_label)
{
    g_return_if_fail (self != NULL);
    g_return_if_fail (GTK_IS_WIDGET (child));
    gtk_notebook_append_page (self->notebook, child,
                              gtk_label_new (tab_label));
}

gint
pn_device_dialog_get_current_page (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, -1);
    return gtk_notebook_get_current_page (self->notebook);
}

void
pn_device_dialog_set_current_page (PnDeviceDialog *self, gint index)
{
    g_return_if_fail (self != NULL);
    gtk_notebook_set_current_page (self->notebook, index);
}

void
pn_device_dialog_set_pages_sensitive (PnDeviceDialog *self,
                                      gboolean        sensitive)
{
    g_return_if_fail (self != NULL);
    gtk_widget_set_sensitive (GTK_WIDGET (self->notebook), sensitive);
}

/* ------------------------------------------------------------------ */
/*  Sidebar                                                             */
/* ------------------------------------------------------------------ */

void
pn_device_dialog_set_sidebar (PnDeviceDialog *self,
                              GtkWidget      *sidebar,
                              gint            position)
{
    g_return_if_fail (self != NULL);

    if (sidebar == NULL)
    {
        /* Drop an existing sidebar: move the body back out of the paned
         * and into the content box where the paned sat. */
        if (self->paned != NULL)
        {
            g_object_ref (self->body);
            gtk_container_remove (GTK_CONTAINER (self->paned), self->body);
            gtk_widget_destroy (self->paned);
            self->paned = NULL;
            gtk_box_pack_start (GTK_BOX (self->content), self->body,
                                TRUE, TRUE, 0);
            gtk_box_reorder_child (GTK_BOX (self->content), self->body, 0);
            g_object_unref (self->body);
            gtk_widget_show (self->body);
        }
        return;
    }

    if (self->paned == NULL)
    {
        /* First sidebar: lift the body out of the content box, wrap it
         * and the sidebar in an HPaned, and put that back where the
         * body was (above the separator + status bar). */
        self->paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
        gtk_paned_set_wide_handle (GTK_PANED (self->paned), TRUE);

        g_object_ref (self->body);
        gtk_container_remove (GTK_CONTAINER (self->content), self->body);

        gtk_paned_pack1 (GTK_PANED (self->paned), sidebar, FALSE, FALSE);
        gtk_paned_pack2 (GTK_PANED (self->paned), self->body, TRUE, FALSE);
        g_object_unref (self->body);

        gtk_box_pack_start (GTK_BOX (self->content), self->paned,
                            TRUE, TRUE, 0);
        gtk_box_reorder_child (GTK_BOX (self->content), self->paned, 0);
    }
    else
    {
        /* Replace the existing sidebar in place. */
        GtkWidget *old = gtk_paned_get_child1 (GTK_PANED (self->paned));
        if (old != NULL)
            gtk_container_remove (GTK_CONTAINER (self->paned), old);
        gtk_paned_pack1 (GTK_PANED (self->paned), sidebar, FALSE, FALSE);
    }

    gtk_paned_set_position (GTK_PANED (self->paned), position);
    gtk_widget_show_all (self->paned);
}

/* ------------------------------------------------------------------ */
/*  Device-list pane: row descriptors                                   */
/* ------------------------------------------------------------------ */

PnDeviceRow *
pn_device_row_new (const gchar *id,
                   const gchar *primary,
                   const gchar *secondary,
                   const gchar *disabled_reason)
{
    PnDeviceRow *row = g_slice_new0 (PnDeviceRow);
    row->id              = g_strdup (id);
    row->primary         = g_strdup (primary);
    row->secondary       = g_strdup (secondary);
    row->disabled_reason = g_strdup (disabled_reason);
    return row;
}

PnDeviceRow *
pn_device_row_copy (const PnDeviceRow *row)
{
    if (row == NULL)
        return NULL;
    return pn_device_row_new (row->id, row->primary,
                              row->secondary, row->disabled_reason);
}

void
pn_device_row_free (PnDeviceRow *row)
{
    if (row == NULL)
        return;
    g_free (row->id);
    g_free (row->primary);
    g_free (row->secondary);
    g_free (row->disabled_reason);
    g_slice_free (PnDeviceRow, row);
}

GPtrArray *
pn_device_row_array_new (void)
{
    return g_ptr_array_new_with_free_func (
            (GDestroyNotify) pn_device_row_free);
}

/* ------------------------------------------------------------------ */
/*  Device-list pane: callbacks                                         */
/* ------------------------------------------------------------------ */

void
pn_device_dialog_set_scan_callback (PnDeviceDialog   *self,
                                    PnDeviceScanFunc  callback,
                                    gpointer          user_data)
{
    g_return_if_fail (self != NULL);
    self->scan_cb = callback;
    self->scan_ud = user_data;
}

void
pn_device_dialog_set_selected_callback (PnDeviceDialog       *self,
                                        PnDeviceSelectedFunc  callback,
                                        gpointer              user_data)
{
    g_return_if_fail (self != NULL);
    self->selected_cb = callback;
    self->selected_ud = user_data;
}

/* ------------------------------------------------------------------ */
/*  Device-list pane: rows + selection                                  */
/* ------------------------------------------------------------------ */

/* One device row.  Two-line layout (bold primary, dim secondary); a row
 * with a disabled_reason gets a third wrapped line and is rendered
 * insensitive + tooltipped so it cannot be clicked, keyboard-selected
 * or auto-selected.  The PnDeviceRow is copied onto the widget as
 * qdata. */
static GtkWidget *
list_row_new (const PnDeviceRow *desc)
{
    GtkWidget *row = gtk_list_box_row_new ();
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *primary;
    GtkWidget *secondary;

    gtk_widget_set_margin_start  (box, 8);
    gtk_widget_set_margin_end    (box, 8);
    gtk_widget_set_margin_top    (box, 6);
    gtk_widget_set_margin_bottom (box, 6);

    primary = gtk_label_new (desc->primary);
    gtk_label_set_xalign (GTK_LABEL (primary), 0.0);
    {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs,
                                pango_attr_weight_new (PANGO_WEIGHT_BOLD));
        gtk_label_set_attributes (GTK_LABEL (primary), attrs);
        pango_attr_list_unref (attrs);
    }
    gtk_box_pack_start (GTK_BOX (box), primary, FALSE, FALSE, 0);

    secondary = gtk_label_new (desc->secondary);
    gtk_label_set_xalign (GTK_LABEL (secondary), 0.0);
    {
        GtkStyleContext *sc = gtk_widget_get_style_context (secondary);
        gtk_style_context_add_class (sc, "dim-label");
    }
    gtk_box_pack_start (GTK_BOX (box), secondary, FALSE, FALSE, 0);

    if (desc->disabled_reason != NULL)
    {
        GtkWidget *reason = gtk_label_new (desc->disabled_reason);
        gtk_label_set_xalign (GTK_LABEL (reason), 0.0);
        gtk_label_set_line_wrap (GTK_LABEL (reason), TRUE);
        gtk_box_pack_start (GTK_BOX (box), reason, FALSE, FALSE, 0);

        gtk_widget_set_sensitive (row, FALSE);
        gtk_widget_set_tooltip_text (row, desc->disabled_reason);
    }

    gtk_container_add (GTK_CONTAINER (row), box);

    g_object_set_data_full (G_OBJECT (row), PN_DEVICE_DIALOG_ROW_QDATA,
                            pn_device_row_copy (desc),
                            (GDestroyNotify) pn_device_row_free);
    gtk_widget_show_all (row);
    return row;
}

static PnDeviceRow *
row_desc (GtkListBoxRow *row)
{
    return row != NULL
        ? g_object_get_data (G_OBJECT (row), PN_DEVICE_DIALOG_ROW_QDATA)
        : NULL;
}

static void
clear_list (GtkListBox *list)
{
    GList *rows = gtk_container_get_children (GTK_CONTAINER (list));
    GList *l;
    for (l = rows; l != NULL; l = l->next)
        gtk_widget_destroy (GTK_WIDGET (l->data));
    g_list_free (rows);
}

/* Find the row whose descriptor id equals @id.  When @connectable_only
 * is TRUE, a disabled match is skipped (returns NULL for it). */
static GtkListBoxRow *
find_row_by_id (PnDeviceDialog *self, const gchar *id,
                gboolean connectable_only)
{
    GList         *rows = gtk_container_get_children (
            GTK_CONTAINER (self->list));
    GtkListBoxRow *out  = NULL;
    GList         *l;

    for (l = rows; l != NULL && out == NULL; l = l->next)
    {
        PnDeviceRow *d = row_desc (GTK_LIST_BOX_ROW (l->data));
        if (d == NULL || g_strcmp0 (d->id, id) != 0)
            continue;
        if (connectable_only && d->disabled_reason != NULL)
            continue;
        out = GTK_LIST_BOX_ROW (l->data);
    }
    g_list_free (rows);
    return out;
}

/* First row that is actually connectable (no disabled_reason).
 * Programmatic selection bypasses widget sensitivity, so the
 * auto-select path must skip disabled rows explicitly. */
static GtkListBoxRow *
first_connectable_row (PnDeviceDialog *self)
{
    GList         *rows = gtk_container_get_children (
            GTK_CONTAINER (self->list));
    GtkListBoxRow *out  = NULL;
    GList         *l;

    for (l = rows; l != NULL && out == NULL; l = l->next)
    {
        PnDeviceRow *d = row_desc (GTK_LIST_BOX_ROW (l->data));
        if (d != NULL && d->disabled_reason == NULL)
            out = GTK_LIST_BOX_ROW (l->data);
    }
    g_list_free (rows);
    return out;
}

/* Select @row and, if it is connectable, record its id and fire the
 * selected callback.  Disabled rows are never fired (their port may be
 * held by another process). */
static void
select_and_fire (PnDeviceDialog *self, GtkListBoxRow *row)
{
    PnDeviceRow *d = row_desc (row);

    gtk_list_box_select_row (self->list, row);
    if (d != NULL && d->disabled_reason == NULL)
    {
        g_free (self->selected_id);
        self->selected_id = g_strdup (d->id);
        if (self->selected_cb != NULL)
            self->selected_cb (d, self->selected_ud);
        emit_changed (self);
    }
}

static void
show_list_state (PnDeviceDialog *self, const gchar *name)
{
    gtk_stack_set_visible_child_name (self->list_stack, name);
}

/* ------------------------------------------------------------------ */
/*  Device-list pane: scan flow                                         */
/* ------------------------------------------------------------------ */

static void
trigger_scan (PnDeviceDialog *self)
{
    if (self->scan_cb != NULL)
        self->scan_cb (self->scan_ud);
}

/* One-shot idle: fire the initial scan once the caller has had a chance
 * to wire its callbacks (deferred past construction). */
static gboolean
idle_initial_scan (gpointer user_data)
{
    PnDeviceDialog *self = user_data;
    trigger_scan (self);
    return G_SOURCE_REMOVE;
}

static void
on_reload_activate (GtkMenuItem *item, gpointer user_data)
{
    (void) item;
    trigger_scan (user_data);
}

static GtkWidget *
build_reload_menu (PnDeviceDialog *self)
{
    GtkWidget *menu  = gtk_menu_new ();
    GtkWidget *item  = gtk_menu_item_new ();
    GtkWidget *row   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *icon  = gtk_image_new_from_icon_name ("view-refresh",
                                                     GTK_ICON_SIZE_MENU);
    GtkWidget *label = gtk_label_new ("Reload");

    gtk_label_set_xalign (GTK_LABEL (label), 0.0);
    gtk_box_pack_start (GTK_BOX (row), icon,  FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (row), label, TRUE,  TRUE,  0);
    gtk_container_add  (GTK_CONTAINER (item), row);

    g_signal_connect (item, "activate",
                      G_CALLBACK (on_reload_activate), self);
    gtk_widget_show_all (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

    /* Attach to the root so the menu dies alongside it. */
    gtk_menu_attach_to_widget (GTK_MENU (menu), self->list_root, NULL);
    return menu;
}

static gboolean
on_list_button_press (GtkWidget      *widget,
                      GdkEventButton *event,
                      gpointer        user_data)
{
    PnDeviceDialog *self = user_data;

    (void) widget;
    if (event->type != GDK_BUTTON_PRESS ||
        event->button != GDK_BUTTON_SECONDARY)
        return GDK_EVENT_PROPAGATE;

    if (self->reload_menu == NULL)
        self->reload_menu = build_reload_menu (self);

    gtk_menu_popup_at_pointer (GTK_MENU (self->reload_menu),
                               (const GdkEvent *) event);
    return GDK_EVENT_STOP;
}

static void
on_list_row_activated (GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    PnDeviceDialog *self = user_data;
    PnDeviceRow    *d    = row_desc (row);

    (void) box;
    /* An in-use row is insensitive so GTK should not fire this for it,
     * but guard anyway -- never hand a disabled device to the host. */
    if (row != NULL && d != NULL && d->disabled_reason == NULL)
        select_and_fire (self, row);
}

/* ------------------------------------------------------------------ */
/*  Device-list pane: public list operations                            */
/* ------------------------------------------------------------------ */

void
pn_device_dialog_set_device_rows (PnDeviceDialog *self, GPtrArray *rows)
{
    GtkListBoxRow *target = NULL;
    guint          i;

    g_return_if_fail (self != NULL);
    g_return_if_fail (self->list != NULL);

    clear_list (self->list);
    if (rows != NULL)
        for (i = 0; i < rows->len; i++)
            gtk_container_add (GTK_CONTAINER (self->list),
                               list_row_new (g_ptr_array_index (rows, i)));

    show_list_state (self,
                     (rows == NULL || rows->len == 0)
                     ? "empty-result" : "list");

    /* Selection policy:
     *   1. The previously-selected device, when still present and
     *      connectable -- so the right pane refreshes after a rescan.
     *   2. Otherwise the first connectable row, but only on the initial
     *      auto-scan or when something was selected before (prefer a
     *      fresh selection over a now-orphaned right pane). */
    if (self->selected_id != NULL)
        target = find_row_by_id (self, self->selected_id, TRUE);

    if (target == NULL &&
        (self->auto_select_pending || self->selected_id != NULL))
        target = first_connectable_row (self);

    self->auto_select_pending = FALSE;

    if (target != NULL)
        select_and_fire (self, target);

    emit_changed (self);
}

void
pn_device_dialog_add_device_row (PnDeviceDialog *self, const PnDeviceRow *row)
{
    g_return_if_fail (self != NULL);
    g_return_if_fail (self->list != NULL);
    g_return_if_fail (row != NULL);

    gtk_container_add (GTK_CONTAINER (self->list), list_row_new (row));
    show_list_state (self, "list");
    emit_changed (self);
}

void
pn_device_dialog_update_device_row (PnDeviceDialog *self, const PnDeviceRow *row)
{
    GtkListBoxRow *existing;

    g_return_if_fail (self != NULL);
    g_return_if_fail (self->list != NULL);
    g_return_if_fail (row != NULL);

    /* Match on id regardless of sensitivity, then swap the row widget
     * in place so a presence change (e.g. became in-use) repaints. */
    existing = find_row_by_id (self, row->id, FALSE);
    if (existing == NULL)
        return;
    {
        gint       index = gtk_list_box_row_get_index (existing);
        gboolean   was_selected =
                gtk_list_box_get_selected_row (self->list) == existing;
        GtkWidget *fresh = list_row_new (row);

        gtk_widget_destroy (GTK_WIDGET (existing));
        gtk_list_box_insert (self->list, fresh, index);
        if (was_selected && row->disabled_reason == NULL)
            gtk_list_box_select_row (self->list, GTK_LIST_BOX_ROW (fresh));
    }
    emit_changed (self);
}

void
pn_device_dialog_remove_device_row (PnDeviceDialog *self, const gchar *id)
{
    GtkListBoxRow *existing;

    g_return_if_fail (self != NULL);
    g_return_if_fail (self->list != NULL);

    existing = find_row_by_id (self, id, FALSE);
    if (existing != NULL)
        gtk_widget_destroy (GTK_WIDGET (existing));

    if (gtk_container_get_children (GTK_CONTAINER (self->list)) == NULL)
        show_list_state (self, "empty-result");
    emit_changed (self);
}

void
pn_device_dialog_reselect_device (PnDeviceDialog *self, const gchar *id)
{
    GtkListBoxRow *row;

    g_return_if_fail (self != NULL);
    if (self->list == NULL)
        return;

    row = find_row_by_id (self, id, TRUE);
    if (row != NULL)
        select_and_fire (self, row);
}

void
pn_device_dialog_set_auto_scan (PnDeviceDialog *self, gboolean enabled)
{
    g_return_if_fail (self != NULL);

    self->auto_scan = enabled;
    if (enabled && self->list != NULL && !self->initial_scan_scheduled)
    {
        self->initial_scan_scheduled = TRUE;
        self->auto_select_pending    = TRUE;
        g_idle_add (idle_initial_scan, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Device-list pane: construction                                      */
/* ------------------------------------------------------------------ */

/* One empty-state page: a dim icon over a primary line and an optional
 * dim secondary line, centred. */
static GtkWidget *
build_empty_page (const gchar *icon_name, const gchar *primary,
                  const gchar *secondary)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *icon;
    GtkWidget *primary_label;

    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);

    icon = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_opacity (icon, 0.4);
    gtk_box_pack_start (GTK_BOX (box), icon, FALSE, FALSE, 0);

    primary_label = gtk_label_new (primary);
    gtk_label_set_justify (GTK_LABEL (primary_label), GTK_JUSTIFY_CENTER);
    gtk_box_pack_start (GTK_BOX (box), primary_label, FALSE, FALSE, 0);

    if (secondary != NULL)
    {
        GtkWidget       *secondary_label = gtk_label_new (secondary);
        GtkStyleContext *sc;
        gtk_label_set_justify (GTK_LABEL (secondary_label),
                               GTK_JUSTIFY_CENTER);
        sc = gtk_widget_get_style_context (secondary_label);
        gtk_style_context_add_class (sc, "dim-label");
        gtk_box_pack_start (GTK_BOX (box), secondary_label, FALSE, FALSE, 0);
    }

    gtk_widget_show_all (box);
    return box;
}

/* (Re)build the two empty-state pages from the current hint strings,
 * falling back to the built-in defaults for any NULL. */
static void
rebuild_empty_pages (PnDeviceDialog *self)
{
    if (self->prescan_page != NULL)
        gtk_widget_destroy (self->prescan_page);
    if (self->empty_page != NULL)
        gtk_widget_destroy (self->empty_page);

    self->prescan_page = build_empty_page (
            self->prescan_icon      ? self->prescan_icon      : "network-wireless-disabled",
            self->prescan_primary   ? self->prescan_primary   : "Looking for devices…",
            self->prescan_secondary ? self->prescan_secondary : "Right-click to reload.");
    gtk_stack_add_named (self->list_stack, self->prescan_page, "pre-scan");

    self->empty_page = build_empty_page (
            self->empty_icon      ? self->empty_icon      : "dialog-question",
            self->empty_primary   ? self->empty_primary   : "No devices found.",
            self->empty_secondary ? self->empty_secondary : "Plug one in and right-click → Reload.");
    gtk_stack_add_named (self->list_stack, self->empty_page, "empty-result");
}

void
pn_device_dialog_set_list_hints (PnDeviceDialog *self,
                                 const gchar    *prescan_icon,
                                 const gchar    *prescan_primary,
                                 const gchar    *prescan_secondary,
                                 const gchar    *empty_icon,
                                 const gchar    *empty_primary,
                                 const gchar    *empty_secondary)
{
    g_return_if_fail (self != NULL);
    if (self->list_stack == NULL)
        return;

    g_free (self->prescan_icon);      self->prescan_icon      = g_strdup (prescan_icon);
    g_free (self->prescan_primary);   self->prescan_primary   = g_strdup (prescan_primary);
    g_free (self->prescan_secondary); self->prescan_secondary = g_strdup (prescan_secondary);
    g_free (self->empty_icon);        self->empty_icon        = g_strdup (empty_icon);
    g_free (self->empty_primary);     self->empty_primary     = g_strdup (empty_primary);
    g_free (self->empty_secondary);   self->empty_secondary   = g_strdup (empty_secondary);

    rebuild_empty_pages (self);
    /* Stay on the pre-scan page if that is what is currently showing. */
    show_list_state (self, "pre-scan");
}

/* Build the device-list sidebar: a stack of {pre-scan, empty-result,
 * list} wrapped in an event box for right-click Reload. */
static GtkWidget *
build_list_pane (PnDeviceDialog *self)
{
    GtkWidget *root;
    GtkWidget *event_box;
    GtkWidget *stack;
    GtkWidget *list_window;
    GtkWidget *list;

    root = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start  (root, 6);
    gtk_widget_set_margin_end    (root, 6);
    gtk_widget_set_margin_top    (root, 6);
    gtk_widget_set_margin_bottom (root, 6);
    self->list_root = root;

    stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (stack),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_vexpand (stack, TRUE);
    self->list_stack = GTK_STACK (stack);

    list_window = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (list_window),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    list = gtk_list_box_new ();
    gtk_list_box_set_selection_mode (GTK_LIST_BOX (list),
                                     GTK_SELECTION_SINGLE);
    gtk_container_add (GTK_CONTAINER (list_window), list);
    self->list = GTK_LIST_BOX (list);

    /* Empty-state pages first (added by rebuild), then the list page. */
    rebuild_empty_pages (self);
    gtk_stack_add_named (GTK_STACK (stack), list_window, "list");
    gtk_stack_set_visible_child_name (GTK_STACK (stack), "pre-scan");

    /* Event box so right-click works over the non-windowed empty
     * pages too. */
    event_box = gtk_event_box_new ();
    gtk_event_box_set_above_child (GTK_EVENT_BOX (event_box), FALSE);
    gtk_container_add (GTK_CONTAINER (event_box), stack);
    gtk_box_pack_start (GTK_BOX (root), event_box, TRUE, TRUE, 0);

    g_signal_connect (list, "row-activated",
                      G_CALLBACK (on_list_row_activated), self);
    g_signal_connect (event_box, "button-press-event",
                      G_CALLBACK (on_list_button_press), self);
    g_signal_connect (list, "button-press-event",
                      G_CALLBACK (on_list_button_press), self);
    return root;
}

/* ------------------------------------------------------------------ */
/*  Construction                                                        */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_status_bar (PnDeviceDialog *self)
{
    GtkWidget *bar = gtk_label_new (NULL);

    gtk_label_set_xalign (GTK_LABEL (bar), 0.0);
    gtk_widget_set_margin_start  (bar, 6);
    gtk_widget_set_margin_end    (bar, 6);
    gtk_widget_set_margin_top    (bar, 2);
    gtk_widget_set_margin_bottom (bar, 4);
    /* Long error strings get an ellipsis so they do not stretch the
     * dialog.  The label is selectable so the user can copy any
     * error text out for support. */
    gtk_label_set_ellipsize  (GTK_LABEL (bar), PANGO_ELLIPSIZE_END);
    gtk_label_set_selectable (GTK_LABEL (bar), TRUE);

    self->status_label = GTK_LABEL (bar);
    return bar;
}

/* The notebook wrapped in a GtkOverlay so a big spinner can float on
 * top while the dialog waits on the device (handshake or any write's
 * verify-cycle).  The notebook stays the bottom (interactive) layer;
 * the spinner+label box is the top (passive) overlay child, set
 * no_show_all so gtk_widget_show_all does NOT reveal it -- the busy
 * counter flips its visibility. */
static GtkWidget *
build_body (PnDeviceDialog *self)
{
    GtkWidget *overlay;
    GtkWidget *notebook;
    GtkWidget *box;
    GtkWidget *spinner;
    GtkWidget *label;

    notebook = gtk_notebook_new ();
    gtk_notebook_set_scrollable (GTK_NOTEBOOK (notebook), TRUE);
    self->notebook = GTK_NOTEBOOK (notebook);

    overlay = gtk_overlay_new ();
    gtk_container_add (GTK_CONTAINER (overlay), notebook);

    box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    /* Centred horizontally + vertically so the spinner lands in the
     * middle of the body regardless of dialog size. */
    gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all (box, TRUE);

    spinner = gtk_spinner_new ();
    /* GtkSpinner has no intrinsic size beyond the theme's
     * "spinner-size" -- bump it to something a user notices across the
     * whole body. */
    gtk_widget_set_size_request (spinner, 64, 64);
    gtk_widget_show (spinner);
    gtk_box_pack_start (GTK_BOX (box), spinner, FALSE, FALSE, 0);

    label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (label),
            "<b><span size=\"large\">Talking to device…</span></b>");
    gtk_widget_show (label);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), box);

    self->loading_overlay = box;
    self->loading_spinner = GTK_SPINNER (spinner);
    self->body            = overlay;
    return overlay;
}

static void
device_dialog_free (gpointer data)
{
    PnDeviceDialog *self = data;

    /* Leave the open-dialog registry and tell the host, but only if we
     * are still the entry under our id -- a newer dialog for the same
     * provider may have replaced us. */
    if (self->provider_id != NULL)
    {
        if (open_dialogs != NULL &&
            g_hash_table_lookup (open_dialogs, self->provider_id) == self)
            g_hash_table_remove (open_dialogs, self->provider_id);
        if (observer_cb != NULL)
            observer_cb (self->provider_id, self, FALSE, observer_ud);
    }

    g_free (self->provider_id);
    g_free (self->selected_id);
    g_free (self->prescan_icon);
    g_free (self->prescan_primary);
    g_free (self->prescan_secondary);
    g_free (self->empty_icon);
    g_free (self->empty_primary);
    g_free (self->empty_secondary);
    g_slice_free (PnDeviceDialog, self);
}

PnDeviceDialog *
pn_device_dialog_new (GtkWindow           *parent,
                      const gchar         *title,
                      PnDeviceDialogFlags  flags)
{
    PnDeviceDialog *self;
    GtkWidget      *dialog;
    GtkWidget      *content;

    g_return_val_if_fail (parent == NULL || GTK_IS_WINDOW (parent), NULL);

    self = g_slice_new0 (PnDeviceDialog);

    /* Modeless so the user keeps access to whatever is behind the
     * dialog (useful when comparing the device against the app). */
    dialog = gtk_dialog_new_with_buttons (
            title, parent, GTK_DIALOG_DESTROY_WITH_PARENT,
            "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_modal (GTK_WINDOW (dialog), FALSE);
    g_signal_connect (dialog, "response",
                      G_CALLBACK (gtk_widget_destroy), NULL);
    self->dialog = dialog;

    content = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_box_set_spacing (GTK_BOX (content), 0);
    self->content = content;

    /* Body (notebook + spinner overlay) on top, a separator, then the
     * status bar.  A sidebar -- if set later -- lifts the body into the
     * left/right panes of a GtkPaned in place. */
    gtk_box_pack_start (GTK_BOX (content), build_body (self), TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (content),
                        gtk_separator_new (GTK_ORIENTATION_HORIZONTAL),
                        FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (content), build_status_bar (self),
                        FALSE, FALSE, 0);

    /* Build the device-list pane and install it as the left sidebar. */
    if (flags & PN_DEVICE_DIALOG_WITH_DEVICE_LIST)
        pn_device_dialog_set_sidebar (self, build_list_pane (self),
                                      PN_DEVICE_DIALOG_DIVIDER);

    /* Own the shell from the dialog so it is freed on destroy. */
    g_object_set_data_full (G_OBJECT (dialog), PN_DEVICE_DIALOG_CTX_QDATA,
                            self, device_dialog_free);

    /* If we are being built inside a provider present (the menu or the
     * D-Bus Present method), adopt that id and join the open-dialog
     * registry so the Devices interface can address us.  device_dialog_free
     * leaves the registry again. */
    {
        const gchar *id = pn_device_provider_current_id ();
        if (id != NULL)
        {
            self->provider_id = g_strdup (id);
            if (open_dialogs == NULL)
                open_dialogs = g_hash_table_new_full (
                        g_str_hash, g_str_equal, g_free, NULL);
            g_hash_table_insert (open_dialogs, g_strdup (id), self);
            if (observer_cb != NULL)
                observer_cb (id, self, TRUE, observer_ud);
        }
    }
    return self;
}

GtkWidget *
pn_device_dialog_get_dialog (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->dialog;
}

/* ------------------------------------------------------------------ */
/*  D-Bus introspection seam                                            */
/* ------------------------------------------------------------------ */

const gchar *
pn_device_dialog_get_provider_id (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->provider_id;
}

PnDeviceDialog *
pn_device_dialog_lookup_open (const gchar *id)
{
    if (open_dialogs == NULL || id == NULL)
        return NULL;
    return g_hash_table_lookup (open_dialogs, id);
}

GPtrArray *
pn_device_dialog_get_rows (PnDeviceDialog *self)
{
    GPtrArray *out;
    GList     *rows, *l;

    g_return_val_if_fail (self != NULL, NULL);

    out = pn_device_row_array_new ();
    if (self->list == NULL)
        return out;

    rows = gtk_container_get_children (GTK_CONTAINER (self->list));
    for (l = rows; l != NULL; l = l->next)
    {
        PnDeviceRow *d = row_desc (GTK_LIST_BOX_ROW (l->data));
        if (d != NULL)
            g_ptr_array_add (out, pn_device_row_copy (d));
    }
    g_list_free (rows);
    return out;
}

const gchar *
pn_device_dialog_get_status (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return gtk_label_get_text (self->status_label);
}

const gchar *
pn_device_dialog_get_selected_id (PnDeviceDialog *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->selected_id;
}

void
pn_device_dialog_scan (PnDeviceDialog *self)
{
    g_return_if_fail (self != NULL);
    if (self->scan_cb != NULL)
        self->scan_cb (self->scan_ud);
}

void
pn_device_dialog_set_changed_callback (PnDeviceDialog            *self,
                                       PnDeviceDialogChangedFunc  callback,
                                       gpointer                   user_data)
{
    g_return_if_fail (self != NULL);
    self->changed_cb = callback;
    self->changed_ud = user_data;
}

void
pn_device_dialog_set_detail_callback (PnDeviceDialog           *self,
                                      PnDeviceDialogDetailFunc  callback,
                                      gpointer                  user_data)
{
    g_return_if_fail (self != NULL);
    self->detail_cb = callback;
    self->detail_ud = user_data;
}

gchar *
pn_device_dialog_get_device_detail (PnDeviceDialog *self,
                                    const gchar    *device_id)
{
    g_return_val_if_fail (self != NULL, NULL);
    if (self->detail_cb == NULL || device_id == NULL)
        return NULL;
    return self->detail_cb (device_id, self->detail_ud);
}

void
pn_device_dialog_set_observer (PnDeviceDialogObserverFunc callback,
                               gpointer                   user_data)
{
    observer_cb = callback;
    observer_ud = user_data;
}
