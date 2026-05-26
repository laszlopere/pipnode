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

#ifndef PN_HELP_BROWSER_H
#define PN_HELP_BROWSER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PN_TYPE_HELP_BROWSER (pn_help_browser_get_type ())

G_DECLARE_FINAL_TYPE (PnHelpBrowser,
                      pn_help_browser,
                      PN, HELP_BROWSER,
                      GtkWindow)

/**
 * pn_help_browser_new:
 * @parent: (nullable): transient parent window
 * @page_path: absolute filesystem path to an HTML file
 * @anchor: (nullable): optional fragment id to scroll to within the
 *          page (without the leading '#').  %NULL or empty opens the
 *          page at the top.
 *
 * Creates a new help browser window and loads the given page.
 *
 * Returns: (transfer full): the new help browser
 */
PnHelpBrowser *
pn_help_browser_new (
        GtkWindow   *parent,
        const gchar *page_path,
        const gchar *anchor);

/**
 * pn_help_browser_load_page:
 * @self: the help browser
 * @page_path: absolute filesystem path to an HTML file
 * @anchor: (nullable): optional fragment id to scroll to within the
 *          page.
 *
 * Navigates the browser to a different page.
 */
void
pn_help_browser_load_page (
        PnHelpBrowser *self,
        const gchar   *page_path,
        const gchar   *anchor);

/**
 * pn_help_browser_resolve_page:
 * @filename: a help-page filename (e.g. "PnRtc.html", "MqttBroker.html");
 *            no directory components.
 *
 * Locates @filename across the host's help-page search path, in order:
 *   1. each directory in <code>$PIPNODE_HELP_PATH</code>
 *   2. <code>$XDG_DATA_HOME/pipnode/help/</code>
 *   3. the in-tree <code>$SRCDIR/data/help/</code> (build tree only)
 *   4. every <code>$SRCDIR/plugins/&lt;x&gt;/help/</code> (build tree only)
 *   5. the system <code>$pkgdatadir/help/</code>
 *
 * Returns: (transfer full) (nullable): the absolute path to the first
 *   match, or %NULL when no candidate exists.  Caller frees with
 *   g_free().
 */
gchar *
pn_help_browser_resolve_page (const gchar *filename);

/**
 * pn_help_browser_open_page:
 * @parent: (nullable): a transient parent
 * @filename: help-page filename, as accepted by
 *            pn_help_browser_resolve_page()
 * @anchor: (nullable): optional fragment id within the page
 *
 * Convenience wrapper that resolves @filename and, if found, opens a
 * fresh #PnHelpBrowser anchored on @parent.  Logs a warning and is a
 * no-op when no candidate path exists.
 *
 * Returns: (transfer none) (nullable): the live help browser, or
 *   %NULL on failure.
 */
PnHelpBrowser *
pn_help_browser_open_page (
        GtkWindow   *parent,
        const gchar *filename,
        const gchar *anchor);

G_END_DECLS

#endif /* PN_HELP_BROWSER_H */
