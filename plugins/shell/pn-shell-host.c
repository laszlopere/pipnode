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

#include "pn-shell-host.h"

gboolean
pn_shell_host_is_local (const gchar *host)
{
    if (host == NULL || *host == '\0')
        return TRUE;
    if (g_strcmp0 (host, "localhost") == 0)
        return TRUE;
    if (g_strcmp0 (host, "127.0.0.1") == 0)
        return TRUE;
    if (g_strcmp0 (host, "::1") == 0)
        return TRUE;
    return FALSE;
}

gchar **
pn_shell_wrap_argv (const gchar        *host,
                    const gchar *const *base_argv)
{
    GPtrArray *out;
    guint      i;

    if (base_argv == NULL || base_argv[0] == NULL)
    {
        gchar **empty = g_new0 (gchar *, 1);
        return empty;
    }

    if (pn_shell_host_is_local (host))
        return g_strdupv ((gchar **) base_argv);

    out = g_ptr_array_new ();
    g_ptr_array_add (out, g_strdup ("ssh"));
    g_ptr_array_add (out, g_strdup ("-o"));
    g_ptr_array_add (out, g_strdup ("BatchMode=yes"));
    g_ptr_array_add (out, g_strdup ("-o"));
    g_ptr_array_add (out, g_strdup ("ConnectTimeout=5"));
    g_ptr_array_add (out, g_strdup (host));

    for (i = 0; base_argv[i] != NULL; i++)
        g_ptr_array_add (out, g_strdup (base_argv[i]));

    g_ptr_array_add (out, NULL);
    return (gchar **) g_ptr_array_free (out, FALSE);
}
