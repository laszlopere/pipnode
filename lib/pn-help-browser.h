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

G_END_DECLS

#endif /* PN_HELP_BROWSER_H */
