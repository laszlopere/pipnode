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

#ifndef PN_PATH_H
#define PN_PATH_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pn_path_expand:
 * @path: (nullable): a file path as the user typed it in a setting
 *
 * Expands a leading "~" to the user's home directory, the way a shell
 * would: "~" alone and "~/rest" become the home directory and
 * "<home>/rest".  Anything else — including "~user/…", a "~" later in
 * the path, or a relative path — is returned unchanged.  pipnode starts
 * from desktop launchers with no shell in between, so every setting that
 * names a local file should pass its value through this before use,
 * while keeping the unexpanded form for saving and display.
 *
 * Returns: (transfer full): a newly allocated path; "" for %NULL.
 */
gchar *pn_path_expand (const gchar *path);

G_END_DECLS

#endif /* PN_PATH_H */
