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

#ifndef PN_HELP_TEXT_H
#define PN_HELP_TEXT_H

#include <glib.h>

G_BEGIN_DECLS

/**
 * pn_help_text_from_html:
 * @html: the contents of a bundled help page
 * @for_search: %TRUE to normalise the result for substring matching
 *
 * Reduces one of the bundled help pages to plain text.  This is not a
 * renderer — it strips everything between `<` and `>` and keeps the
 * rest — but the pages are hand-written, carry no `<script>` or
 * `<style>` blocks, and never rely on markup for meaning, so the
 * result reads correctly.
 *
 * With @for_search %FALSE the surviving text is returned verbatim,
 * newlines and HTML entities included: that is what the help browser's
 * no-WebKit fallback shows in its #GtkTextView.
 *
 * With @for_search %TRUE each `&entity;` becomes a single space and
 * every run of whitespace collapses to one space, so a needle typed as
 * plain prose still matches text the page wrote as
 * `Wheel&nbsp;up` or broke across a source line.  The result is not
 * case-folded; callers that need that do it once and cache it.
 *
 * Returns: (transfer full): the extracted text; never %NULL.
 */
gchar *pn_help_text_from_html (const gchar *html,
                               gboolean     for_search);

G_END_DECLS

#endif /* PN_HELP_TEXT_H */
