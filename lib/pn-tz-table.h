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

#ifndef PN_TZ_TABLE_H
#define PN_TZ_TABLE_H

#include <glib.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Shared fixed-offset timezone table                                  */
/*                                                                     */
/*  A small lookup keyed by combo label (abbreviation + spelled-out     */
/*  name, never a city).  DST variants are listed as their own offsets  */
/*  (CET vs CEST, PST vs PDT, ...) so there is no daylight-saving       */
/*  guessing.  Carrying the full name keeps ambiguous abbreviations     */
/*  distinct — "CST — Central Standard Time" (-360) and "CST — China    */
/*  Standard Time" (+480) are separate rows.                            */
/*                                                                     */
/*  Used by #PnDeadline (target wall-clock zone) and #PnDigitalClock    */
/*  (display offset for a Unix-epoch reading); both pull the choices    */
/*  and the offset lookup from here so the lists stay in sync.          */
/* ------------------------------------------------------------------ */

/**
 * pn_tz_table_choices:
 *
 * NULL-terminated array of combo labels suitable for
 * pn_settings_schema_choices().  Pointer to a static table; do not free.
 */
const gchar * const *pn_tz_table_choices (void);

/**
 * pn_tz_table_offset_minutes:
 * @label: a combo label from pn_tz_table_choices(), or %NULL / empty
 *
 * Minutes east of UTC for @label.  An empty, %NULL or unknown label
 * returns 0 (GMT / UTC+0) — the same fallback both nodes rely on.
 */
gint                 pn_tz_table_offset_minutes (const gchar *label);

/**
 * pn_tz_table_lookup_by_abbreviation:
 * @abbr: a bare abbreviation (e.g. "CEST") — the form the Clock node
 *        emits as the message's "timezone" member
 *
 * Resolve a bare abbreviation to the full label as it appears in
 * pn_tz_table_choices() (e.g. "CEST — Central European Summer Time").
 * Returns %NULL when the abbreviation is unknown or ambiguous (matches
 * more than one entry, like the two "CST" rows).  The returned pointer
 * is static; do not free.
 */
const gchar         *pn_tz_table_lookup_by_abbreviation (const gchar *abbr);

G_END_DECLS

#endif /* PN_TZ_TABLE_H */
