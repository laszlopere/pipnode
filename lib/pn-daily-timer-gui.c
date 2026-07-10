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

/* ------------------------------------------------------------------ */
/*  PnDailyTimer — gui tier.                                           */
/*                                                                     */
/*  The settings-dialog customisation for the Daily Timer node.  The   */
/*  node's GType, the "schedule" string property, the compiled         */
/*  interval cache and the poll-and-emit-on-edge trigger live in the   */
/*  GTK-free core file pn-daily-timer.c; this file installs the        */
/*  build_class_tab vfunc onto that class at editor startup            */
/*  (pn_daily_timer_gui_install).  The editor reads and writes the     */
/*  node's "schedule" property via g_object_get / g_object_set, so it  */
/*  needs no core seam -- the same shape as the Set and Filter rule    */
/*  editors.                                                           */
/*                                                                     */
/*  The schedule is edited as a GtkSheet: exactly one row per          */
/*  interval, three columns (Day, On, Off).  Rows are added and        */
/*  removed with the buttons below the grid -- there is no trailing    */
/*  blank "type here" row, which only ever read as a phantom second    */
/*  interval and could not be removed.  GtkSheet insists on at least   */
/*  one row, so an empty schedule shows a single blank one.            */
/*                                                                     */
/*  Cells are free text, canonicalised when the cell is left ("mon" -> */
/*  "Monday", "7:5" -> "07:05"); anything that will not parse is       */
/*  tagged with the .pn-dt-invalid CSS class and left out of the       */
/*  serialised property rather than silently coerced.                  */
/*                                                                     */
/*  Two GtkSheet behaviours dictate the shape of this file:            */
/*                                                                     */
/*  1. Writing the ACTIVE cell recurses, and it segfaults.            */
/*     _gtk_sheet_set_cell_internal() pushes the text into the cell    */
/*     entry when the target is the active cell; the entry emits       */
/*     ::changed; gtk_sheet_entry_changed_handler() answers by calling */
/*     _gtk_sheet_set_cell_internal() again.  No application-side      */
/*     re-entrancy flag can break that loop, and deferring the write   */
/*     to an idle does not either -- a cell the user merely *left*     */
/*     (focus went to a button, or the dialog closed) is still the     */
/*     active cell.  Every write therefore goes through dt_write_cell, */
/*     which drops the active cell first; the entry then hides, and    */
/*     gtk_sheet_entry_changed_handler() early-returns on an invisible */
/*     entry.  Same for gtk_sheet_range_clear() and row deletion.      */
/*                                                                     */
/*  2. ::changed is NOT a commit signal.  It is emitted only from      */
/*     _gtk_sheet_set_cell_internal(), which the entry handler calls   */
/*     on every keystroke -- so it fires per character, from inside    */
/*     GtkSheet's own callback.  It is used here purely to *schedule*  */
/*     a g_idle sync; canonicalisation waits for ::deactivate, when    */
/*     the user has really left the cell and is no longer typing.      */
/*                                                                     */
/*  3. There are two ways to point at a row, and the active cell only  */
/*     knows one of them: clicking a row-title button selects the row  */
/*     without activating any cell.  "Remove interval" therefore asks  */
/*     dt_target_row(), which prefers the selection state and falls    */
/*     back to the active cell (itself unreliable -- (-1,-1) after a   */
/*     delete, or once the sheet yields focus to the button being      */
/*     clicked -- so ::activate / ::select-row are tracked too).       */
/*     With no target the button is insensitive rather than inert.     */
/*                                                                     */
/*  Day cells are deliberately text and not combo boxes: GtkSheet's    */
/*  per-column entry type must contain a GtkEditable, and handing it   */
/*  GTK_TYPE_COMBO_BOX_TEXT makes create_sheet_entry() fall back to a  */
/*  GtkDataEntry without complaint.                                    */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-daily-timer-gui.h"
#include "pn-daily-timer.h"

#include <gtk/gtk.h>
#include <gtksheet/gtksheet.h>
#include <json-glib/json-glib.h>

#include <string.h>

/* Columns of the schedule sheet. */
enum
{
    DT_COL_DAY = 0,
    DT_COL_ON,
    DT_COL_OFF,
    DT_N_COLS
};

/* Tagged onto any cell whose text will not parse. */
#define PN_DT_INVALID_CLASS "pn-dt-invalid"

/* A translucent red wash plus red text: legible against both the light
 * and the dark theme's cell background, which a flat opaque fill is not.
 * Literal colours rather than @error_color -- a theme that does not
 * define that name would fail to parse the whole provider. */
static const gchar PN_DT_CSS[] =
    "sheet .cell." PN_DT_INVALID_CLASS " {"
    "  background-color: rgba(224, 27, 36, 0.28);"
    "  color: #e01b24;"
    "}";

static const gchar *const dt_day_names[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday",
    "Friday", "Saturday", "Sunday"
};

typedef struct
{
    GObject   *target;               /* borrowed PnDailyTimer */
    GtkSheet  *sheet;
    GtkWidget *del_btn;              /* borrowed; sensitivity follows the target row */
    gulong     notify_handler;
    gboolean   updating;             /* our own writes; skip our handlers */
    guint      idle_id;              /* pending dt_idle_sync, or 0 */
    gint       canon_row;            /* cell to canonicalise, or -1 */
    gint       canon_col;
    gint       active_row;           /* last ::activate / ::select-row row, or -1 */
} PnDailyTimerBinding;

static void dt_serialise             (PnDailyTimerBinding *bind);
static void dt_rebuild_from_property (PnDailyTimerBinding *bind);

static void
dt_binding_free (gpointer data)
{
    PnDailyTimerBinding *bind = data;

    /* The dialog can be destroyed with a sync still queued; the idle
     * would then run against a freed binding and a dead sheet. */
    if (bind->idle_id != 0)
        g_source_remove (bind->idle_id);

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

/* ------------------------------------------------------------------ */
/*  Cell text helpers                                                  */
/* ------------------------------------------------------------------ */

/* gtk_sheet_cell_get_text() hands back a pointer into the cell (never a
 * copy) and NULL for an empty cell.  Normalise that to "". */
static const gchar *
dt_cell (GtkSheet *sheet, gint row, gint col)
{
    const gchar *text = gtk_sheet_cell_get_text (sheet, row, col);

    return (text != NULL) ? text : "";
}

static gboolean
dt_row_is_blank (GtkSheet *sheet, gint row)
{
    gint col;

    for (col = 0; col < DT_N_COLS; col++)
    {
        if (*dt_cell (sheet, row, col) != '\0')
            return FALSE;
    }
    return TRUE;
}

/* Step out of the cell editor.  Every write below goes through this
 * first; see dt_write_cell(). */
static void
dt_leave_active_cell (PnDailyTimerBinding *bind)
{
    /* Emits ::deactivate, which our handler ignores while updating. */
    gtk_sheet_set_active_cell (bind->sheet, -1, -1);
}

/* Which row "Remove interval" acts on.
 *
 * Clicking a row-title button selects the row without activating any
 * cell, so the active cell alone is the wrong thing to ask -- it would
 * still point at whatever was last typed in, and the wrong interval
 * would disappear.  The selection state wins; the active cell is the
 * fallback for a plain cell click.  Returns -1 when there is no target,
 * which is also what keeps the button insensitive. */
static gint
dt_target_row (PnDailyTimerBinding *bind)
{
    GtkSheet      *sheet = bind->sheet;
    GtkSheetState  state = GTK_SHEET_NORMAL;
    GtkSheetRange  range;
    gint           row = -1, col = -1;

    gtk_sheet_get_selection (sheet, &state, &range);

    if (state == GTK_SHEET_ROW_SELECTED || state == GTK_SHEET_RANGE_SELECTED)
        row = range.row0;

    if (row < 0)
        gtk_sheet_get_active_cell (sheet, &row, &col);

    if (row < 0)
        row = bind->active_row;

    if (row < 0 || (guint) row >= gtk_sheet_get_rows_count (sheet))
        return -1;

    return row;
}

/* The button used to look enabled while doing nothing whenever there
 * was no target row.  Now it says so. */
static void
dt_update_remove_sensitivity (PnDailyTimerBinding *bind)
{
    gint     row = dt_target_row (bind);
    gboolean can_remove;

    if (bind->del_btn == NULL)
        return;

    /* The trailing blank row is not an interval; nothing to remove. */
    /* The lone placeholder row of an empty schedule is not an interval. */
    can_remove = (row >= 0 && !dt_row_is_blank (bind->sheet, row));

    gtk_widget_set_sensitive (bind->del_btn, can_remove);
}

/* Leave the grid with nothing focused and nothing selected.
 *
 * Order matters: gtk_sheet_unselect_range() ends by re-activating the
 * current active cell, so the cell has to be dropped first.  With the
 * active cell already (-1,-1) that tail call is a no-op. */
static void
dt_clear_focus_and_selection (PnDailyTimerBinding *bind)
{
    dt_leave_active_cell (bind);
    gtk_sheet_unselect_range (bind->sheet);   /* also resets state to NORMAL */
    bind->active_row = -1;
}

/* The only safe way to put text into a cell.
 *
 * _gtk_sheet_set_cell_internal() pushes the new text into the cell
 * ENTRY whenever the target happens to be the active cell.  The entry
 * then emits ::changed, gtk_sheet_entry_changed_handler() answers it by
 * calling _gtk_sheet_set_cell_internal() again, and the recursion walks
 * off a cliff -- this is the reported segfault, and deferring the write
 * to an idle does not help, because a cell that was merely *left*
 * (focus moved to a button, dialog closed) is still the active cell.
 *
 * Dropping the active cell first hides the entry, and
 * gtk_sheet_entry_changed_handler() early-returns on an invisible
 * entry, so the write cannot bounce. */
static void
dt_write_cell (
        PnDailyTimerBinding *bind,
        gint                 row,
        gint                 col,
        const gchar         *text)
{
    gint active_row = -1, active_col = -1;

    gtk_sheet_get_active_cell (bind->sheet, &active_row, &active_col);

    if (active_row == row && active_col == col)
        dt_leave_active_cell (bind);

    gtk_sheet_set_cell_text (bind->sheet, row, col, text);
}

/* ------------------------------------------------------------------ */
/*  Parsing                                                            */
/* ------------------------------------------------------------------ */

/* "" / "*" / "any" / "every day" -> PN_DAILY_TIMER_EVERY_DAY (-1).
 * "1".."7", a full weekday name, or any prefix of one at least three
 * characters long (shorter is ambiguous: s, t, m...).  Case-insensitive. */
static gboolean
dt_parse_day (const gchar *text, gint *out_day)
{
    gchar   *trimmed = g_strstrip (g_strdup (text));
    gsize    len;
    gboolean ok = FALSE;
    gint     i;

    len = strlen (trimmed);

    if (len == 0 ||
        g_strcmp0 (trimmed, "*") == 0 ||
        g_ascii_strcasecmp (trimmed, "any") == 0 ||
        g_ascii_strcasecmp (trimmed, "every day") == 0 ||
        g_ascii_strcasecmp (trimmed, "everyday") == 0)
    {
        *out_day = -1;
        ok = TRUE;
        goto out;
    }

    if (len == 1 && trimmed[0] >= '1' && trimmed[0] <= '7')
    {
        *out_day = trimmed[0] - '0';
        ok = TRUE;
        goto out;
    }

    for (i = 0; i < 7; i++)
    {
        if (len >= 3 && len <= strlen (dt_day_names[i]) &&
            g_ascii_strncasecmp (dt_day_names[i], trimmed, len) == 0)
        {
            *out_day = i + 1;
            ok = TRUE;
            goto out;
        }
    }

out:
    g_free (trimmed);
    return ok;
}

/* Accepts "H:M", "HH:MM", "H", "HH" (minutes 0), "HMM" and "HHMM".
 * Rejects trailing junk and out-of-range values -- 25:00 is a mistake,
 * not something to clamp silently behind the user's back. */
static gboolean
dt_parse_time (const gchar *text, gint *out_h, gint *out_m)
{
    gchar    *trimmed = g_strstrip (g_strdup (text));
    gboolean  ok      = FALSE;
    gint      h = 0, m = 0;
    gsize     len;
    gchar    *colon;

    len = strlen (trimmed);
    if (len == 0)
        goto out;

    colon = strchr (trimmed, ':');
    if (colon != NULL)
    {
        gchar *endptr = NULL;

        *colon = '\0';
        if (*trimmed == '\0' || *(colon + 1) == '\0')
            goto out;

        h = (gint) g_ascii_strtoll (trimmed, &endptr, 10);
        if (endptr == NULL || *endptr != '\0')
            goto out;

        m = (gint) g_ascii_strtoll (colon + 1, &endptr, 10);
        if (endptr == NULL || *endptr != '\0')
            goto out;
    }
    else
    {
        gsize i;

        for (i = 0; i < len; i++)
        {
            if (!g_ascii_isdigit (trimmed[i]))
                goto out;
        }

        switch (len)
        {
        case 1:
        case 2:
            h = (gint) g_ascii_strtoll (trimmed, NULL, 10);
            m = 0;
            break;
        case 3:
            h = trimmed[0] - '0';
            m = (gint) g_ascii_strtoll (trimmed + 1, NULL, 10);
            break;
        case 4:
            m = (gint) g_ascii_strtoll (trimmed + 2, NULL, 10);
            trimmed[2] = '\0';
            h = (gint) g_ascii_strtoll (trimmed, NULL, 10);
            break;
        default:
            goto out;
        }
    }

    if (h < 0 || h > 23 || m < 0 || m > 59)
        goto out;

    *out_h = h;
    *out_m = m;
    ok = TRUE;

out:
    g_free (trimmed);
    return ok;
}

static const gchar *
dt_day_text (gint day)
{
    if (day >= 1 && day <= 7)
        return dt_day_names[day - 1];

    return "Every day";
}

/* ------------------------------------------------------------------ */
/*  Validity marking                                                   */
/* ------------------------------------------------------------------ */

static void
dt_mark_cell (GtkSheet *sheet, gint row, gint col, gboolean valid)
{
    GtkSheetRange range = { row, col, row, col };

    gtk_sheet_range_set_css_class (sheet, &range,
                                   valid ? NULL : (gchar *) PN_DT_INVALID_CLASS);
}

/* Re-check one row and paint its cells.  A wholly blank row is the
 * trailing "type here" row, not an error, so it stays unmarked. */
static void
dt_revalidate_row (PnDailyTimerBinding *bind, gint row)
{
    GtkSheet *sheet = bind->sheet;
    gint      day, h, m;

    if (dt_row_is_blank (sheet, row))
    {
        gint col;

        for (col = 0; col < DT_N_COLS; col++)
            dt_mark_cell (sheet, row, col, TRUE);
        return;
    }

    dt_mark_cell (sheet, row, DT_COL_DAY,
                  dt_parse_day (dt_cell (sheet, row, DT_COL_DAY), &day));
    dt_mark_cell (sheet, row, DT_COL_ON,
                  dt_parse_time (dt_cell (sheet, row, DT_COL_ON), &h, &m));
    dt_mark_cell (sheet, row, DT_COL_OFF,
                  dt_parse_time (dt_cell (sheet, row, DT_COL_OFF), &h, &m));
}

/* Rewrite a committed cell in canonical form.  Only ever called for a
 * cell the user has left (::deactivate) -- rewriting the cell that is
 * being typed into would fight the entry, character by character.
 * Unparseable text is left exactly as typed so it can be seen and
 * fixed. */
static void
dt_canonicalise_cell (PnDailyTimerBinding *bind, gint row, gint col)
{
    GtkSheet *sheet = bind->sheet;
    gint      day, h, m;
    gchar     buf[8];

    if (row < 0 || (guint) row >= gtk_sheet_get_rows_count (sheet))
        return;
    if (col < 0 || col >= DT_N_COLS)
        return;

    if (col == DT_COL_DAY)
    {
        /* An empty day cell in an otherwise blank row is the trailing
         * row; do not stamp "Every day" onto it. */
        if (dt_row_is_blank (sheet, row))
            return;

        if (dt_parse_day (dt_cell (sheet, row, col), &day))
            dt_write_cell (bind, row, col, dt_day_text (day));
        return;
    }

    if (*dt_cell (sheet, row, col) == '\0')
        return;

    if (dt_parse_time (dt_cell (sheet, row, col), &h, &m))
    {
        g_snprintf (buf, sizeof buf, "%02d:%02d", h, m);
        dt_write_cell (bind, row, col, buf);
    }
}

/* ------------------------------------------------------------------ */
/*  Serialise / rebuild                                                */
/* ------------------------------------------------------------------ */

static void
dt_serialise (PnDailyTimerBinding *bind)
{
    GtkSheet  *sheet = bind->sheet;
    JsonArray *arr   = json_array_new ();
    JsonNode  *root;
    gchar     *json_string;
    guint      n_rows;
    guint      row;

    if (bind->updating)
    {
        json_array_unref (arr);
        return;
    }

    n_rows = gtk_sheet_get_rows_count (sheet);

    for (row = 0; row < n_rows; row++)
    {
        JsonObject *obj;
        gint        day, on_h, on_m, off_h, off_m;

        if (dt_row_is_blank (sheet, row))
            continue;

        /* A row that does not parse is skipped rather than written as
         * garbage.  Its cells stay marked, so the omission is visible. */
        if (!dt_parse_day (dt_cell (sheet, row, DT_COL_DAY), &day) ||
            !dt_parse_time (dt_cell (sheet, row, DT_COL_ON), &on_h, &on_m) ||
            !dt_parse_time (dt_cell (sheet, row, DT_COL_OFF), &off_h, &off_m))
            continue;

        obj = json_object_new ();
        json_object_set_int_member (obj, "day",        day);
        json_object_set_int_member (obj, "on_hour",    on_h);
        json_object_set_int_member (obj, "on_minute",  on_m);
        json_object_set_int_member (obj, "off_hour",   off_h);
        json_object_set_int_member (obj, "off_minute", off_m);

        json_array_add_object_element (arr, obj);
    }

    root = json_node_new (JSON_NODE_ARRAY);
    json_node_take_array (root, arr);
    json_string = json_to_string (root, FALSE);
    json_node_unref (root);

    /* Our own write must not bounce back in through notify::schedule and
     * rebuild the grid the user is typing into. */
    bind->updating = TRUE;
    g_object_set (bind->target, "schedule", json_string, NULL);
    bind->updating = FALSE;

    g_free (json_string);
}

/* Grow or shrink the sheet to exactly n_rows.  Never lets the row count
 * reach zero -- GtkSheet keeps an active cell and dislikes an empty
 * grid, and there is always at least the trailing blank row. */
static void
dt_set_row_count (GtkSheet *sheet, guint want)
{
    guint have = gtk_sheet_get_rows_count (sheet);

    if (want < 1)
        want = 1;

    if (want > have)
        gtk_sheet_add_row (sheet, want - have);
    else if (want < have)
        gtk_sheet_delete_rows (sheet, want, have - want);
}

/* Every mutation of the sheet funnels through here, one idle tick after
 * whatever GtkSheet callback asked for it.  See the header comment: the
 * ::changed callback runs inside GtkSheet's entry handler, and mutating
 * the sheet from there re-enters that handler and crashes. */
static gboolean
dt_idle_sync (gpointer data)
{
    PnDailyTimerBinding *bind  = data;
    GtkSheet            *sheet = bind->sheet;
    guint                n_rows;
    guint                row;

    bind->idle_id = 0;

    bind->updating = TRUE;
    gtk_sheet_freeze (sheet);

    if (bind->canon_row >= 0)
        dt_canonicalise_cell (bind, bind->canon_row, bind->canon_col);
    bind->canon_row = -1;
    bind->canon_col = -1;

    n_rows = gtk_sheet_get_rows_count (sheet);
    for (row = 0; row < n_rows; row++)
        dt_revalidate_row (bind, (gint) row);

    gtk_sheet_thaw (sheet);
    bind->updating = FALSE;

    /* Typing into the placeholder row turns it into a real interval. */
    dt_update_remove_sensitivity (bind);

    dt_serialise (bind);

    return G_SOURCE_REMOVE;
}

static void
dt_queue_sync (PnDailyTimerBinding *bind)
{
    if (bind->idle_id == 0)
        bind->idle_id = g_idle_add (dt_idle_sync, bind);
}

static gint
dt_object_get_int (JsonObject *obj, const gchar *member)
{
    return json_object_has_member (obj, member)
               ? (gint) json_object_get_int_member (obj, member)
               : 0;
}

static void
dt_fill_row (PnDailyTimerBinding *bind, gint row, JsonObject *obj)
{
    gchar buf[8];
    gint  day;

    day = json_object_has_member (obj, "day")
              ? (gint) json_object_get_int_member (obj, "day")
              : -1;

    dt_write_cell (bind, row, DT_COL_DAY, dt_day_text (day));

    g_snprintf (buf, sizeof buf, "%02d:%02d",
                dt_object_get_int (obj, "on_hour"),
                dt_object_get_int (obj, "on_minute"));
    dt_write_cell (bind, row, DT_COL_ON, buf);

    g_snprintf (buf, sizeof buf, "%02d:%02d",
                dt_object_get_int (obj, "off_hour"),
                dt_object_get_int (obj, "off_minute"));
    dt_write_cell (bind, row, DT_COL_OFF, buf);
}

static void
dt_rebuild_from_property (PnDailyTimerBinding *bind)
{
    GtkSheet   *sheet       = bind->sheet;
    gchar      *json_string = NULL;
    JsonParser *parser;
    JsonArray  *arr = NULL;
    guint       n   = 0;
    guint       i;

    if (bind->updating)
        return;

    g_object_get (bind->target, "schedule", &json_string, NULL);

    parser = json_parser_new ();
    if (json_string != NULL && *json_string != '\0' &&
        json_parser_load_from_data (parser, json_string, -1, NULL))
    {
        JsonNode *root = json_parser_get_root (parser);

        if (root != NULL && JSON_NODE_HOLDS_ARRAY (root))
        {
            arr = json_node_get_array (root);
            n   = json_array_get_length (arr);
        }
    }

    bind->updating = TRUE;
    gtk_sheet_freeze (sheet);

    /* Clearing or refilling the cell under the entry would recurse; the
     * whole grid is about to be replaced, so step out of it entirely. */
    dt_leave_active_cell (bind);
    bind->active_row = -1;

    /* Exactly n interval rows.  dt_set_row_count() floors at 1, so an
     * empty schedule leaves a single blank placeholder row. */
    dt_set_row_count (sheet, n);

    {
        GtkSheetRange all = { 0, 0,
                              (gint) gtk_sheet_get_rows_count (sheet) - 1,
                              DT_N_COLS - 1 };
        gtk_sheet_range_clear (sheet, &all);
        gtk_sheet_range_set_css_class (sheet, &all, NULL);
    }

    for (i = 0; i < n; i++)
    {
        JsonNode *item = json_array_get_element (arr, i);

        if (item == NULL || !JSON_NODE_HOLDS_OBJECT (item))
            continue;

        dt_fill_row (bind, (gint) i, json_node_get_object (item));
    }

    gtk_sheet_thaw (sheet);
    bind->updating = FALSE;

    dt_update_remove_sensitivity (bind);

    g_object_unref (parser);
    g_free (json_string);
}

/* ------------------------------------------------------------------ */
/*  Signals                                                            */
/* ------------------------------------------------------------------ */

/* NOT a commit signal: fires once per keystroke, from inside GtkSheet's
 * entry handler.  Schedule the work, touch nothing. */
static void
on_dt_sheet_changed (
        GtkSheet *sheet,
        gint      row,
        gint      col,
        gpointer  user_data)
{
    PnDailyTimerBinding *bind = user_data;

    (void) sheet;

    if (bind->updating)
        return;

    /* Row-button label changes come through with col == -1. */
    if (row < 0 || col < 0 || col >= DT_N_COLS)
        return;

    dt_queue_sync (bind);
}

/* The real commit point: the user has left the cell. */
static gboolean
on_dt_sheet_deactivate (
        GtkSheet *sheet,
        gint      row,
        gint      col,
        gpointer  user_data)
{
    PnDailyTimerBinding *bind = user_data;

    (void) sheet;

    if (!bind->updating && row >= 0 && col >= 0 && col < DT_N_COLS)
    {
        bind->canon_row = row;
        bind->canon_col = col;
        dt_queue_sync (bind);
    }

    return TRUE;   /* allow the cell to be left */
}

/* gtk_sheet_get_active_cell() is unreliable once focus has moved to a
 * button, so remember the row while the sheet still has it. */
static gboolean
on_dt_sheet_activate (
        GtkSheet *sheet,
        gint      row,
        gint      col,
        gpointer  user_data)
{
    PnDailyTimerBinding *bind = user_data;

    (void) sheet;
    (void) col;

    if (row >= 0)
        bind->active_row = row;

    dt_update_remove_sensitivity (bind);

    return TRUE;   /* allow the cell to be entered */
}

/* Clicking a row-title button selects the row without activating a cell. */
static void
on_dt_sheet_select_row (GtkSheet *sheet, gint row, gpointer user_data)
{
    PnDailyTimerBinding *bind = user_data;

    (void) sheet;

    if (row >= 0)
        bind->active_row = row;

    dt_update_remove_sensitivity (bind);
}

static void
on_dt_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    dt_rebuild_from_property (user_data);
}

static void
on_dt_add_clicked (GtkButton *btn, gpointer user_data)
{
    PnDailyTimerBinding *bind  = user_data;
    GtkSheet            *sheet = bind->sheet;
    gint                 row;

    (void) btn;

    /* Append a row -- unless the sheet is showing the lone blank
     * placeholder of an empty schedule, which we fill instead. */
    row = (gint) gtk_sheet_get_rows_count (sheet) - 1;
    if (row < 0 || !dt_row_is_blank (sheet, row))
    {
        gtk_sheet_add_row (sheet, 1);
        row = (gint) gtk_sheet_get_rows_count (sheet) - 1;
    }

    /* A plausible morning heating window, so the common case is two
     * edits rather than five. */
    bind->updating = TRUE;
    gtk_sheet_freeze (sheet);
    dt_write_cell (bind, row, DT_COL_DAY, dt_day_text (-1));
    dt_write_cell (bind, row, DT_COL_ON,  "07:00");
    dt_write_cell (bind, row, DT_COL_OFF, "09:00");
    dt_revalidate_row (bind, row);
    gtk_sheet_thaw (sheet);
    bind->updating = FALSE;

    bind->active_row = row;
    gtk_sheet_set_active_cell (sheet, row, DT_COL_DAY);
    dt_update_remove_sensitivity (bind);
    dt_serialise (bind);
}

static void
on_dt_remove_clicked (GtkButton *btn, gpointer user_data)
{
    PnDailyTimerBinding *bind   = user_data;
    GtkSheet            *sheet  = bind->sheet;
    gint                 row    = dt_target_row (bind);
    guint                n_rows = gtk_sheet_get_rows_count (sheet);

    (void) btn;

    if (row < 0)
        return;

    bind->updating = TRUE;
    gtk_sheet_freeze (sheet);

    /* The row being removed may still hold the active cell -- the click
     * moved focus to the button, not out of the cell -- and clearing or
     * deleting the cell under the entry recurses.  See dt_write_cell. */
    dt_leave_active_cell (bind);

    if (n_rows <= 1)
    {
        /* GtkSheet wants at least one row: blank it instead of deleting,
         * leaving the empty-schedule placeholder. */
        GtkSheetRange only = { row, 0, row, DT_N_COLS - 1 };
        gtk_sheet_range_clear (sheet, &only);
        gtk_sheet_range_set_css_class (sheet, &only, NULL);
    }
    else
    {
        gtk_sheet_delete_rows (sheet, (guint) row, 1);
    }

    gtk_sheet_thaw (sheet);
    bind->updating = FALSE;

    /* Nothing focused, nothing selected: re-activating a cell here would
     * highlight whichever interval slid up into the deleted row's place,
     * which reads as "this one is next to go". */
    dt_clear_focus_and_selection (bind);
    dt_update_remove_sensitivity (bind);

    dt_serialise (bind);
}

/* ------------------------------------------------------------------ */
/*  PnNodeClass.build_class_tab override                               */
/* ------------------------------------------------------------------ */

static GtkWidget *
dt_build_sheet (PnDailyTimerBinding *bind)
{
    GtkWidget      *sheet = gtk_sheet_new (1, DT_N_COLS, "schedule");
    GtkSheet       *s     = GTK_SHEET (sheet);
    GtkCssProvider *prov  = gtk_css_provider_new ();
    static const struct { const gchar *title; gint width; } cols[DT_N_COLS] = {
        { "Day", 140 }, { "On", 80 }, { "Off", 80 }
    };
    gint i;

    gtk_css_provider_load_from_data (prov, PN_DT_CSS, -1, NULL);
    gtk_style_context_add_provider (gtk_widget_get_style_context (sheet),
                                    GTK_STYLE_PROVIDER (prov),
                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref (prov);

    for (i = 0; i < DT_N_COLS; i++)
    {
        gtk_sheet_set_column_title (s, i, cols[i].title);
        gtk_sheet_column_button_add_label (s, i, cols[i].title);
        gtk_sheet_set_column_width (s, i, cols[i].width);
        gtk_sheet_column_set_justification (
                s, i, (i == DT_COL_DAY) ? GTK_JUSTIFY_LEFT
                                        : GTK_JUSTIFY_RIGHT);
    }

    gtk_sheet_show_grid (s, TRUE);
    gtk_sheet_set_selection_mode (s, GTK_SELECTION_BROWSE);

    bind->sheet = s;
    return sheet;
}

static GtkWidget *
pn_daily_timer_build_class_tab (
        PnNode    *self,
        GtkWindow *parent G_GNUC_UNUSED)
{
    GObject             *target   = G_OBJECT (self);
    GtkWidget           *outer    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget           *hint;
    GtkWidget           *scrolled = gtk_scrolled_window_new (NULL, NULL);
    GtkWidget           *sheet;
    GtkWidget           *add_btn  = gtk_button_new_with_label ("Add interval");
    GtkWidget           *del_btn  = gtk_button_new_with_label ("Remove interval");
    GtkWidget           *btn_row  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    PnDailyTimerBinding *bind;

    g_object_set (outer,
                  "margin-start",  12,
                  "margin-end",    12,
                  "margin-top",    12,
                  "margin-bottom", 12,
                  NULL);

    bind = g_new0 (PnDailyTimerBinding, 1);
    bind->target     = target;
    bind->del_btn    = del_btn;
    bind->canon_row  = -1;
    bind->canon_col  = -1;
    bind->active_row = -1;

    sheet = dt_build_sheet (bind);

    /* The midnight-wrap rule is not discoverable from the grid --
     * nothing about two time columns says what 22:00-06:00 means -- so
     * it is spelled out where the user is typing it. */
    hint = gtk_label_new ("Sends value = 1 when the time enters an "
                          "interval, 0 when it leaves.\n"
                          "An off time at or before the on time runs "
                          "past midnight into the next day.\n"
                          "Leave the day blank or type \"*\" for every day.");
    gtk_label_set_xalign (GTK_LABEL (hint), 0.0);
    gtk_widget_set_sensitive (hint, FALSE);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    gtk_widget_set_size_request (scrolled, -1, 200);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);

    gtk_container_add (GTK_CONTAINER (scrolled), sheet);

    gtk_box_pack_start (GTK_BOX (outer), hint,     FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (outer), scrolled, TRUE,  TRUE,  0);

    gtk_widget_set_halign (btn_row, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (btn_row), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (btn_row), del_btn, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (outer),   btn_row, FALSE, FALSE, 0);

    bind->notify_handler = g_signal_connect (
            target, "notify::schedule",
            G_CALLBACK (on_dt_target_notify), bind);

    g_signal_connect (sheet,   "changed",
                      G_CALLBACK (on_dt_sheet_changed), bind);
    g_signal_connect (sheet,   "deactivate",
                      G_CALLBACK (on_dt_sheet_deactivate), bind);
    g_signal_connect (sheet,   "activate",
                      G_CALLBACK (on_dt_sheet_activate), bind);
    g_signal_connect (sheet,   "select-row",
                      G_CALLBACK (on_dt_sheet_select_row), bind);
    g_signal_connect (add_btn, "clicked",
                      G_CALLBACK (on_dt_add_clicked), bind);
    g_signal_connect (del_btn, "clicked",
                      G_CALLBACK (on_dt_remove_clicked), bind);

    g_object_set_data_full (G_OBJECT (outer),
                            "pn-daily-timer-binding",
                            bind, dt_binding_free);

    dt_rebuild_from_property (bind);

    /* Open with nothing focused or selected; Remove stays insensitive
     * until the user picks a row, rather than looking live and doing
     * nothing. */
    dt_clear_focus_and_selection (bind);
    dt_update_remove_sensitivity (bind);

    return outer;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_daily_timer_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_DAILY_TIMER));

    node_class->build_class_tab = pn_daily_timer_build_class_tab;

    /* The class ref is intentionally held for the process lifetime --
     * the same lifetime the factory keeps it alive for -- so the slot
     * we just wrote stays valid. */
}
