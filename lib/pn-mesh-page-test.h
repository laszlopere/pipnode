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

#ifndef PN_MESH_PAGE_TEST_H
#define PN_MESH_PAGE_TEST_H

#include <gtk/gtk.h>

#include "pn-mesh-connection.h"
#include "pn-mesh-page-busy.h"

G_BEGIN_DECLS

/* Test page (Phase 7).
 *
 * The "did this thing actually plug in and talk to the mesh?" view.
 * Top row: channel picker + message entry + Send button.  Below:
 * live "Incoming traffic" log -- every TEXT_MESSAGE_APP packet the
 * device hears, plus the user's own sends echoed back so the log
 * reads as a single transcript.
 *
 * The page does not own a timer; the dialog calls
 * pn_mesh_page_test_drain_event() once per PnMeshTextEvent it took
 * off the connection's queue.  That keeps the "pump serial only when
 * no write is in flight" gate in the dialog where the busy_count
 * lives, and lets this page be a pure renderer plus a Send action. */
GtkWidget *pn_mesh_page_test_new (void);

void       pn_mesh_page_test_set_state (GtkWidget         *page,
                                        const PnMeshState *state,
                                        PnMeshConnection  *connection);

/* Append one event to the log view (taking ownership of the event --
 * the page frees it).  Called by the dialog for every event drained
 * from the connection's queue.  No-op if @page is NULL. */
void       pn_mesh_page_test_drain_event (GtkWidget       *page,
                                          PnMeshTextEvent *event);

void       pn_mesh_page_test_set_busy_callback (
        GtkWidget          *page,
        PnMeshPageBusyFunc  callback,
        gpointer            user_data);

G_END_DECLS

#endif /* PN_MESH_PAGE_TEST_H */
