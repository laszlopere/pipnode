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

#include <string.h>

#include "pn-sci-fi-clips.h"

/* ------------------------------------------------------------------ */
/*  Storage layout                                                     */
/* ------------------------------------------------------------------ */

gchar *
pn_sci_fi_cache_dir (void)
{
    return g_build_filename (g_get_user_data_dir (),
                             "pipnode",
                             "sound-effects",
                             "startrek",
                             NULL);
}

gchar *
pn_sci_fi_resolve_path (const gchar *clip)
{
    gchar *dir;
    gchar *path;

    if (clip == NULL || *clip == '\0')
        return NULL;
    if (clip[0] == '/' || strchr (clip, '\\') != NULL)
        return NULL;
    if (strstr (clip, "..") != NULL)
        return NULL;

    dir  = pn_sci_fi_cache_dir ();
    path = g_build_filename (dir, clip, NULL);
    g_free (dir);
    return path;
}

/* ------------------------------------------------------------------ */
/*  Playback                                                           */
/* ------------------------------------------------------------------ */

gchar *
pn_sci_fi_find_player (void)
{
    static const gchar *candidates[] = {
        "mpv", "ffplay", "gst-play-1.0", "mpg123", "paplay", NULL
    };

    for (gsize i = 0; candidates[i] != NULL; i++)
    {
        gchar *path = g_find_program_in_path (candidates[i]);
        if (path != NULL)
            return path;
    }
    return NULL;
}

gchar **
pn_sci_fi_build_argv (const gchar *player, const gchar *file)
{
    const gchar *basename = strrchr (player, '/');
    GPtrArray   *argv     = g_ptr_array_new ();

    basename = (basename != NULL) ? basename + 1 : player;

    g_ptr_array_add (argv, g_strdup (player));

    if (g_strcmp0 (basename, "mpv") == 0)
    {
        g_ptr_array_add (argv, g_strdup ("--no-video"));
        g_ptr_array_add (argv, g_strdup ("--really-quiet"));
        g_ptr_array_add (argv, g_strdup ("--no-config"));
    }
    else if (g_strcmp0 (basename, "ffplay") == 0)
    {
        g_ptr_array_add (argv, g_strdup ("-nodisp"));
        g_ptr_array_add (argv, g_strdup ("-autoexit"));
        g_ptr_array_add (argv, g_strdup ("-loglevel"));
        g_ptr_array_add (argv, g_strdup ("quiet"));
    }
    else if (g_strcmp0 (basename, "mpg123") == 0)
    {
        g_ptr_array_add (argv, g_strdup ("-q"));
    }

    g_ptr_array_add (argv, g_strdup (file));
    g_ptr_array_add (argv, NULL);

    return (gchar **) g_ptr_array_free (argv, FALSE);
}
