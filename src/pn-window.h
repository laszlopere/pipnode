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

#ifndef PN_WINDOW_H
#define PN_WINDOW_H

#include <gtk/gtk.h>
#include "pn-application.h"
#include "pn-worksheet.h"

G_BEGIN_DECLS

#define PN_TYPE_WINDOW (pn_window_get_type ())

G_DECLARE_FINAL_TYPE (PnWindow,
                      pn_window,
                      PN, WINDOW,
                      GtkApplicationWindow)

PnWindow    *pn_window_new          (PnApplication *app);

/**
 * pn_window_new_panel:
 * @app: the application that owns the window
 *
 * Creates a window in "panel-backed" mode for the background engine
 * (pipnode-editor --gapplication-service).  Unlike pn_window_new() the
 * window is built but kept hidden — its flow runs off the main loop the
 * whole time, but nothing appears on screen until the engine
 * gtk_window_present()s it for an "Edit…" request.  Closing such a
 * window autosaves it and hides it again rather than prompting and
 * destroying, so the running worksheet survives the editor being closed.
 */
PnWindow    *pn_window_new_panel    (PnApplication *app);

PnWorksheet *pn_window_get_worksheet (PnWindow *self);

/* The window's document flow — its single node store spans every sheet.
 * Unlike pn_window_get_worksheet (the *active* tab, NULL when the panel
 * editor tab is active), this is always available, so the engine reads the
 * nodes here rather than depending on which tab the user is looking at. */
PnFlow      *pn_window_get_flow      (PnWindow *self);

gboolean     pn_window_load_file     (PnWindow      *self,
                                      const gchar   *path,
                                      GError       **error);

/**
 * pn_window_new_document:
 * @self: the window
 *
 * Discards the current document — clears the flow and forgets the
 * current file path — without the usual unsaved-changes prompt or any
 * dialog.  The dialog-free counterpart to the File → New action, used
 * by the D-Bus Editor.New automation method (TODO #40.10).
 */
void         pn_window_new_document  (PnWindow *self);

/**
 * pn_window_save_to:
 * @self: the window
 * @path: filesystem path to write to
 * @error: (out) (optional): location for a #GError
 *
 * Saves the document to @path and adopts it as the current path (so a
 * subsequent pn_window_save() writes back to it), without any file
 * chooser or error dialog.  The dialog-free counterpart to File →
 * Save As, backing the D-Bus Editor.SaveAs method.  Returns %TRUE on
 * success.
 */
gboolean     pn_window_save_to       (PnWindow      *self,
                                      const gchar   *path,
                                      GError       **error);

/**
 * pn_window_save_current:
 * @self: the window
 * @error: (out) (optional): location for a #GError
 *
 * Saves the document to its current path.  Fails (without prompting for
 * a path) when the document has never been saved — the caller is
 * expected to use pn_window_save_to() with an explicit path in that
 * case.  Backs the D-Bus Editor.Save method.  Returns %TRUE on success.
 */
gboolean     pn_window_save_current  (PnWindow      *self,
                                      GError       **error);

/**
 * pn_window_get_current_path:
 * @self: the window
 *
 * Returns: (transfer none) (nullable): the path the document was last
 *   loaded from or saved to, or %NULL when it has never been written.
 */
const gchar *pn_window_get_current_path (PnWindow *self);

/**
 * pn_window_autosave:
 * @self: the window
 *
 * Silently persists the worksheet to its current file when it has
 * unsaved changes and a known path (no dialogs).  Used by the panel
 * engine to save a worksheet before dropping it, mirroring the
 * autosave a panel-backed window does on close.
 */
void         pn_window_autosave      (PnWindow *self);

/* ------------------------------------------------------------------ */
/*  Test surface                                                       */
/*                                                                     */
/*  The functions below exist to give the DBus test interface in       */
/*  pn-application.c programmatic access to the debug pane — they      */
/*  let an external functional test inject a synthesised message,     */
/*  enumerate the pane's rows, and trigger the row's `from`-button     */
/*  the same way the user would by clicking it.  Production code       */
/*  outside the DBus layer has no business calling these.              */
/* ------------------------------------------------------------------ */

/**
 * pn_window_inject_debug_message:
 * @self: the window
 * @text: payload to insert into the debug pane
 *
 * Appends @text into the debug pane as if a #PnDebug node targeting
 * the debug view had emitted it.  When @text parses as a JSON
 * envelope the row gains the structured layout (clickable
 * `from`-button, expander, pretty-printed body); otherwise it falls
 * back to the raw-text label.
 */
void         pn_window_inject_debug_message (PnWindow    *self,
                                             const gchar *text);

/**
 * pn_window_get_debug_row_count:
 * @self: the window
 *
 * Returns: the number of rows currently in the debug pane.
 */
guint        pn_window_get_debug_row_count  (PnWindow *self);

/**
 * pn_window_get_debug_row_from_id:
 * @self:  the window
 * @index: zero-based row index
 *
 * Returns: (transfer full) (nullable): the `from_id` UUID stashed on
 *   the row's `from`-button, or %NULL when the row has no clickable
 *   from-button (text-fallback row, or producer-emitted empty
 *   from_id).  The caller owns the string.
 */
gchar       *pn_window_get_debug_row_from_id (PnWindow *self,
                                              guint     index);

/**
 * pn_window_click_debug_from_button:
 * @self:  the window
 * @index: zero-based row index
 *
 * Synthesises a click on the `from`-button at @index — the same
 * code path a real mouse click would take, including the worksheet
 * focus pulse if the row carries a non-empty `from_id`.  Returns
 * %FALSE when @index is out of range or the row has no from-button.
 */
gboolean     pn_window_click_debug_from_button (PnWindow *self,
                                                guint     index);

/**
 * pn_window_get_debug_pane_allocation:
 * @self:   the window
 * @width:  (out): on-screen width allocated to the debug pane, in
 *   widget pixels; zero when the pane is hidden or unrealised
 * @height: (out): on-screen height allocated to the debug pane, in
 *   widget pixels; zero when the pane is hidden or unrealised
 *
 * Reports the live #GtkWidget allocation of the right-hand debug
 * pane.  A regression test uses this to verify that toggling the
 * pane open after the window has been widened actually gives the
 * pane a non-zero on-screen extent (the AT-SPI bridge can lag a
 * window resize, so reading the allocation directly is more
 * reliable than going through Atspi.CoordType.SCREEN).
 */
void         pn_window_get_debug_pane_allocation (PnWindow *self,
                                                  gint     *width,
                                                  gint     *height);

/**
 * pn_window_open_node_dialog:
 * @self:  the window
 * @index: zero-based index of the node in the active worksheet
 *
 * Pops up the #PnNodeDialog for the node at @index — the same
 * non-modal dialog the worksheet's context menu opens — and tracks it
 * so the other dialog test calls can introspect and drive it.  Any
 * previously-opened test dialog is closed first.  Returns %FALSE when
 * there is no active worksheet or @index is out of range.
 */
gboolean     pn_window_open_node_dialog (PnWindow *self, guint index);

/**
 * pn_window_close_node_dialog:
 * @self: the window
 *
 * Destroys the test dialog opened by pn_window_open_node_dialog().
 * Returns %FALSE when no test dialog is open.
 */
gboolean     pn_window_close_node_dialog (PnWindow *self);

/**
 * pn_window_get_dialog_page_titles:
 * @self: the window
 *
 * Returns: (transfer full) (nullable): a %NULL-terminated array of the
 *   open dialog's notebook tab titles, in order, or %NULL when no test
 *   dialog is open.  Free with g_strfreev().
 */
gchar      **pn_window_get_dialog_page_titles (PnWindow *self);

/**
 * pn_window_select_dialog_page:
 * @self:  the window
 * @index: zero-based notebook page index
 *
 * Switches the open test dialog's notebook to page @index so the
 * corresponding tab becomes visible — used by the slow/visible test
 * mode to walk the tabs and to surface the page an editor lives on
 * before driving it.  Returns %FALSE when no test dialog is open or
 * @index is out of range.
 */
gboolean     pn_window_select_dialog_page (PnWindow *self, guint index);

/**
 * pn_window_get_dialog_editor_text:
 * @self: the window
 * @prop: the GObject property name whose editor to read
 *
 * Returns: (transfer full) (nullable): the text shown in the editor
 *   widget the dialog built for @prop (entries and numeric spinners),
 *   or %NULL when no test dialog is open, the editor is not found, or
 *   the editor is not text-shaped.  Free with g_free().
 */
gchar       *pn_window_get_dialog_editor_text (PnWindow    *self,
                                               const gchar *prop);

/**
 * pn_window_get_dialog_editor_sensitive:
 * @self:  the window
 * @prop:  the GObject property name whose editor to inspect
 * @found: (out) (optional): set %TRUE when an editor for @prop exists
 *
 * Returns: whether the editor widget the dialog built for @prop is
 *   sensitive (greyed-out test, e.g. PnLed's `hold-ms` row tracking
 *   `mode`).  Returns %FALSE with @found %FALSE when no test dialog is
 *   open or the editor is not found — check @found to disambiguate.
 */
gboolean     pn_window_get_dialog_editor_sensitive (PnWindow    *self,
                                                    const gchar *prop,
                                                    gboolean    *found);

/**
 * pn_window_set_dialog_editor_text:
 * @self: the window
 * @prop: the GObject property name whose editor to drive
 * @text: the value to type into the editor
 *
 * Drives the editor widget for @prop as a user would, letting the
 * dialog's bidirectional binding write the value through to the node.
 * Spinners parse @text as a number.  Returns %FALSE when no test
 * dialog is open, the editor is not found, or it is not text-shaped.
 */
gboolean     pn_window_set_dialog_editor_text (PnWindow    *self,
                                               const gchar *prop,
                                               const gchar *text);

/**
 * pn_window_note_automation_activity:
 * @self: the window
 *
 * Reveal the "automation active" badge in the statusbar and (re)arm its
 * auto-hide timer.  Called by the D-Bus automation dispatch (TODO #40.16)
 * for every non-read call, so the user can see when an external program is
 * editing the worksheet over the org.pipas.pipnode automation interface.
 * The badge fades a few seconds after the automation traffic stops.
 */
void         pn_window_note_automation_activity (PnWindow *self);

G_END_DECLS

#endif /* PN_WINDOW_H */
