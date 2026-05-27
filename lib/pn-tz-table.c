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

#include "pn-tz-table.h"

typedef struct
{
    const gchar *label;
    gint         off_min;   /* minutes east of UTC */
} TzEntry;

static const TzEntry tz_table[] =
{
    { "HST — Hawaii Standard Time",                -600 },
    { "AKST — Alaska Standard Time",               -540 },
    { "PST — Pacific Standard Time",               -480 },
    { "PDT — Pacific Daylight Time",               -420 },
    { "MST — Mountain Standard Time",              -420 },
    { "MDT — Mountain Daylight Time",              -360 },
    { "CST — Central Standard Time",               -360 },
    { "CDT — Central Daylight Time",               -300 },
    { "EST — Eastern Standard Time",               -300 },
    { "EDT — Eastern Daylight Time",               -240 },
    { "AST — Atlantic Standard Time",              -240 },
    { "BRT — Brasília Time",                        -180 },
    { "UTC — Coordinated Universal Time",             0 },
    { "GMT — Greenwich Mean Time",                    0 },
    { "WET — Western European Time",                  0 },
    { "WEST — Western European Summer Time",         60 },
    { "CET — Central European Time",                 60 },
    { "CEST — Central European Summer Time",        120 },
    { "EET — Eastern European Time",                120 },
    { "EEST — Eastern European Summer Time",        180 },
    { "MSK — Moscow Standard Time",                 180 },
    { "GST — Gulf Standard Time",                   240 },
    { "IST — India Standard Time",                  330 },
    { "CST — China Standard Time",                  480 },
    { "JST — Japan Standard Time",                  540 },
    { "AEST — Australian Eastern Standard Time",    600 },
    { "AEDT — Australian Eastern Daylight Time",    660 },
    { "NZST — New Zealand Standard Time",           720 },
};

/* NULL-terminated mirror of tz_table's labels, lazily built on first call.
 * pn_settings_schema_choices() takes a const gchar * const * so we have to
 * hand it a real array (not a stride into TzEntry). */
static const gchar **
build_choices (void)
{
    static const gchar **choices = NULL;
    static gsize         once    = 0;

    if (g_once_init_enter (&once))
    {
        const gchar **a = g_new0 (const gchar *, G_N_ELEMENTS (tz_table) + 1);
        guint         i;

        for (i = 0; i < G_N_ELEMENTS (tz_table); i++)
            a[i] = tz_table[i].label;
        a[G_N_ELEMENTS (tz_table)] = NULL;

        choices = a;
        g_once_init_leave (&once, 1);
    }

    return choices;
}

const gchar * const *
pn_tz_table_choices (void)
{
    return (const gchar * const *) build_choices ();
}

/* The bare label table with PN_TZ_NOT_SET prepended.  Lazily built once,
 * static thereafter — both DigitalClock and Deadline hand the pointer
 * straight to pn_settings_schema_choices(). */
static const gchar **
build_choices_with_not_set (void)
{
    static const gchar **choices = NULL;
    static gsize         once    = 0;

    if (g_once_init_enter (&once))
    {
        const gchar **a = g_new0 (const gchar *, G_N_ELEMENTS (tz_table) + 2);
        guint         i;

        a[0] = PN_TZ_NOT_SET;
        for (i = 0; i < G_N_ELEMENTS (tz_table); i++)
            a[i + 1] = tz_table[i].label;
        a[G_N_ELEMENTS (tz_table) + 1] = NULL;

        choices = a;
        g_once_init_leave (&once, 1);
    }

    return choices;
}

const gchar * const *
pn_tz_table_choices_with_not_set (void)
{
    return (const gchar * const *) build_choices_with_not_set ();
}

gint
pn_tz_table_offset_minutes (const gchar *label)
{
    guint i;

    if (label == NULL || *label == '\0')
        return 0;

    for (i = 0; i < G_N_ELEMENTS (tz_table); i++)
        if (g_strcmp0 (label, tz_table[i].label) == 0)
            return tz_table[i].off_min;

    return 0;
}

const gchar *
pn_tz_table_lookup_by_abbreviation (const gchar *abbr)
{
    const gchar *match = NULL;
    gsize        n;
    guint        i;
    gboolean     ambiguous = FALSE;

    if (abbr == NULL || *abbr == '\0')
        return NULL;

    /* Each label starts "<ABBR> — <full name>"; match the prefix up to the
     * space.  Reject ambiguous abbreviations (the two "CST" rows) — the
     * caller cannot know which the Clock meant, so we fall back to GMT
     * rather than guess wrong by half a planet. */
    n = strlen (abbr);
    for (i = 0; i < G_N_ELEMENTS (tz_table); i++)
    {
        const gchar *lab = tz_table[i].label;

        if (strncmp (lab, abbr, n) != 0)
            continue;
        if (lab[n] != ' ')
            continue;   /* prefix-only hit ("CS" against "CST — …") */

        if (match != NULL)
        {
            ambiguous = TRUE;
            break;
        }
        match = lab;
    }

    return ambiguous ? NULL : match;
}
