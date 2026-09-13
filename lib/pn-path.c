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

#include "pn-path.h"

gchar *
pn_path_expand (const gchar *path)
{
    if (path == NULL)
        return g_strdup ("");

    if (path[0] == '~' && (path[1] == '\0' || path[1] == '/'))
        return g_build_filename (g_get_home_dir (), path + 1, NULL);

    return g_strdup (path);
}
