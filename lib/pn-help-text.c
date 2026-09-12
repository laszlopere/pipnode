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

#include "pn-help-text.h"

/* ------------------------------------------------------------------ */
/*  Help-page text extraction                                          */
/*                                                                     */
/*  Two consumers share this: the help browser's no-WebKit fallback,   */
/*  which shows the text in a GtkTextView, and the palette's full-text */
/*  search, which matches a needle against it.  Keeping one extractor  */
/*  means the two can never disagree about what a page says.           */
/*                                                                     */
/*  The scan works on bytes.  Every byte it tests against is ASCII and */
/*  UTF-8 continuation bytes are all >= 0x80, so multi-byte characters */
/*  pass through untouched.                                            */
/* ------------------------------------------------------------------ */

/** Return a pointer to the ';' closing the HTML entity that starts at
 *  @amp, or %NULL when @amp is a bare ampersand.  The name is bounded
 *  so prose such as "A & B" — and an unterminated entity — is left
 *  alone rather than swallowing the rest of the line. */
static const gchar *
entity_end (const gchar *amp)
{
    const gchar *p = amp + 1;
    guint        n = 0;

    if (*p == '#')      /* numeric: &#160; and &#x2014; */
        p++;

    while (*p != '\0' && n < 10)
    {
        if (*p == ';')
            return (p == amp + 1) ? NULL : p;   /* "&;" is not an entity */
        if (!g_ascii_isalnum (*p))
            return NULL;
        p++;
        n++;
    }

    return NULL;
}

gchar *
pn_help_text_from_html (
        const gchar *html,
        gboolean     for_search)
{
    GString     *out;
    const gchar *p;
    gboolean     in_tag        = FALSE;
    gboolean     pending_space = FALSE;

    g_return_val_if_fail (html != NULL, g_strdup (""));

    out = g_string_new (NULL);

    for (p = html; *p != '\0'; p++)
    {
        if (in_tag)
        {
            if (*p == '>')
                in_tag = FALSE;
            continue;
        }

        if (*p == '<')
        {
            in_tag = TRUE;
            continue;
        }

        if (!for_search)
        {
            g_string_append_c (out, *p);
            continue;
        }

        /* An entity stands for one character we do not decode; a space
         * keeps the words on either side of it apart, which is all a
         * substring match needs. */
        if (*p == '&')
        {
            const gchar *end = entity_end (p);
            if (end != NULL)
            {
                pending_space = TRUE;
                p = end;    /* the loop's p++ steps past the ';' */
                continue;
            }
        }

        if (g_ascii_isspace (*p))
        {
            pending_space = TRUE;
            continue;
        }

        /* Emitted lazily, so leading whitespace is dropped, a run
         * collapses to one space, and nothing trails the last word. */
        if (pending_space)
        {
            if (out->len > 0)
                g_string_append_c (out, ' ');
            pending_space = FALSE;
        }

        g_string_append_c (out, *p);
    }

    return g_string_free (out, FALSE);
}
