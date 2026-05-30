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

#include "pn-table.h"
#include "pn-json-path.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Match PnGraph so a row of mixed sinks aligns visually:  280 px    */
/*  wide, 40 px standard header, 4 px gap, 173 px body.               */
/* ------------------------------------------------------------------ */

#define PN_TABLE_WIDTH         280.0
#define PN_TABLE_HEADER_HEIGHT  40.0
#define PN_TABLE_GAP             4.0
#define PN_TABLE_BODY_HEIGHT   173.0
#define PN_TABLE_TOTAL_HEIGHT  (PN_TABLE_HEADER_HEIGHT + \
                                PN_TABLE_GAP +           \
                                PN_TABLE_BODY_HEIGHT)

/* Hard ceiling on the configurable row limit.  A table is a UI
 * surface, not a database — past a few hundred rows the user can't
 * meaningfully read the data even when zoomed, and the per-row heap
 * footprint (one PnTableRow plus n_cols stringified gchar*) starts to
 * matter for high-arrival-rate feeds. */
#define PN_TABLE_LIMIT_MAX  1000

/* Match PnGraph's 10 Hz repaint cap so a high-frequency feed cannot
 * push the worksheet to redraw at the display refresh rate. */
#define PN_TABLE_MIN_REPAINT_INTERVAL_US  (G_TIME_SPAN_MILLISECOND * 100)

/* Layout constants that tune how the body draws.  Fixed at the
 * canonical size; the painter scales them gently when the table is
 * lifted into the zoom overlay so the larger rectangle gets used for
 * extra rows rather than gigantic text. */
#define PN_TABLE_INSET           6.0
#define PN_TABLE_HEADER_BAND_PX 22.0
#define PN_TABLE_ROW_HEIGHT_PX  18.0
#define PN_TABLE_FONT_PX        12.0

/* PnTableColumn and PnTableRow are published in pn-table.h: they are
 * plain data (no GTK) and the gui-tier painter has to read the same
 * parsed columns + row buffer receive() fills, so the structs cross the
 * tier boundary through the public header. */

struct _PnTable
{
    PnNode parent_instance;

    /* User-facing properties. */
    gchar   *columns;
    guint    limit;
    PnColor  background_color;
    PnColor  header_background_color;
    PnColor  grid_color;
    PnColor  text_color;
    PnColor  header_text_color;
    gboolean alternate_row_background;

    /* Parsed-spec cache, derived from #columns. */
    PnTableColumn *cols;
    guint          n_cols;

    /* Row buffer.  Newest at head, oldest at tail. */
    GQueue *rows;

    /* Scroll state (rows skipped from the head when painting).
     * Honoured only while the table is the zoomed overlay; reset to 0
     * the next time the on-canvas view receives a message so the user
     * doesn't return from a zoom session to find their on-canvas
     * table mysteriously offset. */
    int scroll_offset;

    /* Repaint throttle, identical to the one in PnGraph. */
    gint64  last_repaint_us;
    guint   pending_repaint_id;
};

/* ------------------------------------------------------------------ */
/*  Properties                                                         */
/* ------------------------------------------------------------------ */

enum
{
    PROP_0,
    PROP_COLUMNS,
    PROP_LIMIT,
    PROP_BACKGROUND_COLOR,
    PROP_HEADER_BACKGROUND_COLOR,
    PROP_GRID_COLOR,
    PROP_TEXT_COLOR,
    PROP_HEADER_TEXT_COLOR,
    PROP_ALTERNATE_ROW_BACKGROUND,
    N_PROPS
};

static GParamSpec *props[N_PROPS];

G_DEFINE_TYPE (PnTable, pn_table, PN_TYPE_NODE)

/* Forward declarations. */
static void  pn_table_reparse_columns (PnTable *self);
static void  pn_table_clear_rows      (PnTable *self);
static void  pn_table_trim_to_limit   (PnTable *self);
static void  schedule_repaint         (PnTable *self);

/* ------------------------------------------------------------------ */
/*  Column-spec parsing                                                */
/* ------------------------------------------------------------------ */

static gchar *
trim_dup (const gchar *start, const gchar *end)
{
    while (start < end && g_ascii_isspace (*start))
        start++;
    while (end > start && g_ascii_isspace (*(end - 1)))
        end--;
    return g_strndup (start, (gsize) (end - start));
}

/** Parse "Title:path,Title:path,..." into the #cols/#n_cols cache.
 *  An entry without a ':' is treated as path-only, with the path
 *  doubling as its title (so "data/value" works as a one-column
 *  shorthand).  Empty entries (consecutive commas, leading/trailing
 *  commas) are skipped so cosmetic typos don't fabricate blank
 *  columns.  Whitespace around titles and paths is stripped. */
static void
pn_table_reparse_columns (PnTable *self)
{
    GArray      *acc;
    const gchar *p;
    const gchar *spec;

    /* Drop the previous cache. */
    if (self->cols != NULL)
    {
        guint i;
        for (i = 0; i < self->n_cols; i++)
        {
            g_free (self->cols[i].title);
            g_free (self->cols[i].path);
        }
        g_free (self->cols);
        self->cols   = NULL;
        self->n_cols = 0;
    }

    spec = (self->columns != NULL) ? self->columns : "";
    acc  = g_array_new (FALSE, FALSE, sizeof (PnTableColumn));

    p = spec;
    while (*p != '\0')
    {
        const gchar *comma = strchr (p, ',');
        const gchar *end   = (comma != NULL) ? comma : (p + strlen (p));
        const gchar *colon = memchr (p, ':', (gsize) (end - p));
        PnTableColumn col  = { NULL, NULL };
        gchar        *title;
        gchar        *path;

        if (colon != NULL)
        {
            title = trim_dup (p, colon);
            path  = trim_dup (colon + 1, end);
        }
        else
        {
            /* No ':' — use the path as its own title. */
            path  = trim_dup (p, end);
            title = g_strdup (path);
        }

        /* Skip entries with an empty path; an empty title is fine
         * (renders as a blank header cell — sometimes desirable for
         * single-column "raw value" tables). */
        if (path == NULL || *path == '\0')
        {
            g_free (title);
            g_free (path);
        }
        else
        {
            col.title = title;
            col.path  = path;
            g_array_append_val (acc, col);
        }

        p = (comma != NULL) ? (comma + 1) : end;
    }

    self->n_cols = acc->len;
    self->cols   = (PnTableColumn *) g_array_free (acc, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Row buffer management                                              */
/* ------------------------------------------------------------------ */

static void
pn_table_row_free (gpointer ptr)
{
    PnTableRow *row = ptr;
    guint       i;

    if (row == NULL)
        return;

    for (i = 0; i < row->n_values; i++)
        g_free (row->values[i]);
    g_free (row->values);
    g_free (row);
}

static void
pn_table_clear_rows (PnTable *self)
{
    g_queue_clear_full (self->rows, pn_table_row_free);
}

static void
pn_table_trim_to_limit (PnTable *self)
{
    while (g_queue_get_length (self->rows) > self->limit)
    {
        PnTableRow *row = g_queue_pop_tail (self->rows);
        pn_table_row_free (row);
    }
}

/* ------------------------------------------------------------------ */
/*  Cell stringifier                                                   */
/* ------------------------------------------------------------------ */

/** Convert a borrowed JsonNode into a freshly-allocated display
 *  string.  Compact placeholders for nested structures so a row that
 *  accidentally points at a sub-object doesn't blow out the column
 *  width with a multi-line dump.  Caller owns the returned string. */
static gchar *
json_node_to_display_string (JsonNode *node)
{
    if (node == NULL)
        return g_strdup ("\xe2\x80\x94");  /* em dash */

    switch (json_node_get_node_type (node))
    {
    case JSON_NODE_NULL:
        return g_strdup ("null");

    case JSON_NODE_OBJECT:
        return g_strdup ("{\xe2\x80\xa6}");  /* {…} */

    case JSON_NODE_ARRAY:
        return g_strdup ("[\xe2\x80\xa6]");  /* […] */

    case JSON_NODE_VALUE:
    {
        GType vt = json_node_get_value_type (node);

        if (vt == G_TYPE_STRING)
            return g_strdup (json_node_get_string (node));

        if (vt == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT,
                                    json_node_get_int (node));

        if (vt == G_TYPE_DOUBLE)
        {
            gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_formatd (buf, sizeof buf, "%g",
                             json_node_get_double (node));
            return g_strdup (buf);
        }

        if (vt == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (node) ? "true" : "false");

        return g_strdup ("?");
    }

    default:
        return g_strdup ("?");
    }
}

/* ------------------------------------------------------------------ */
/*  Repaint throttle                                                   */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnTable *self = PN_TABLE (user_data);

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
schedule_repaint (PnTable *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_TABLE_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_TABLE_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_table_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnTable    *self = PN_TABLE (node);
    JsonObject *root;
    PnTableRow *row;
    guint       i;

    if (self->n_cols == 0)
        return;

    row              = g_new0 (PnTableRow, 1);
    row->received_us = g_get_monotonic_time ();
    row->n_values    = self->n_cols;
    row->values      = g_new0 (gchar *, self->n_cols);

    root = pn_json_lookup_root_for_message (message);
    for (i = 0; i < self->n_cols; i++)
    {
        JsonNode *vn = pn_json_resolve_path (root, self->cols[i].path);
        row->values[i] = json_node_to_display_string (vn);
    }
    json_object_unref (root);

    g_queue_push_head (self->rows, row);
    pn_table_trim_to_limit (self);

    schedule_repaint (self);
}

/* ------------------------------------------------------------------ */
/*  Scroll                                                             */
/* ------------------------------------------------------------------ */

static void
pn_table_scroll (PnNode *node, double dy)
{
    PnTable *self = PN_TABLE (node);
    int      step = (int) lround (dy * 3.0);

    if (step == 0)
        step = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
    if (step == 0)
        return;

    self->scroll_offset += step;
    /* Clamping to the live extents happens in the painter (it has the
     * dimensions to compute the maximum), so this just nudges and
     * lets the next paint pin the value. */
    if (self->scroll_offset < 0)
        self->scroll_offset = 0;

    pn_node_request_repaint (PN_NODE (self));
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The gui-tier cairo painter (pn-table-gui.c) reads the scalar        */
/*  drawing config through a snapshot and the parsed columns + row      */
/*  buffer through borrowed-pointer accessors.  Scroll clamping crosses */
/*  back the other way: the painter knows the live extents, so it asks  */
/*  the core to pin the stored offset before reading it.                */
/* ------------------------------------------------------------------ */

void
pn_table_get_paint_state (PnTable *self, PnTablePaintState *out)
{
    g_return_if_fail (PN_IS_TABLE (self));
    g_return_if_fail (out != NULL);

    out->background_color         = self->background_color;
    out->header_background_color  = self->header_background_color;
    out->grid_color               = self->grid_color;
    out->text_color               = self->text_color;
    out->header_text_color        = self->header_text_color;
    out->alternate_row_background = self->alternate_row_background;
}

const PnTableColumn *
pn_table_peek_columns (PnTable *self, guint *n_out)
{
    g_return_val_if_fail (PN_IS_TABLE (self), NULL);
    if (n_out != NULL)
        *n_out = self->n_cols;
    return self->cols;
}

GQueue *
pn_table_peek_rows (PnTable *self)
{
    g_return_val_if_fail (PN_IS_TABLE (self), NULL);
    return self->rows;
}

int
pn_table_get_scroll_offset (PnTable *self)
{
    g_return_val_if_fail (PN_IS_TABLE (self), 0);
    return self->scroll_offset;
}

void
pn_table_clamp_scroll_offset (PnTable *self, int max_offset)
{
    g_return_if_fail (PN_IS_TABLE (self));
    if (max_offset < 0) max_offset = 0;
    if (self->scroll_offset > max_offset) self->scroll_offset = max_offset;
    if (self->scroll_offset < 0)          self->scroll_offset = 0;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/* ------------------------------------------------------------------ */

static void
pn_table_get_size (
        PnNode *self,
        double *out_width,
        double *out_height)
{
    (void) self;
    if (out_width  != NULL) *out_width  = PN_TABLE_WIDTH;
    if (out_height != NULL) *out_height = PN_TABLE_TOTAL_HEIGHT;
}

static double
pn_table_get_header_height (PnNode *self)
{
    (void) self;
    return PN_TABLE_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_table_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTable *self = PN_TABLE (object);

    switch (prop_id)
    {
    case PROP_COLUMNS:
        g_value_set_string (value, self->columns);
        break;
    case PROP_LIMIT:
        g_value_set_uint (value, self->limit);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_HEADER_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->header_background_color);
        break;
    case PROP_GRID_COLOR:
        g_value_set_boxed (value, &self->grid_color);
        break;
    case PROP_TEXT_COLOR:
        g_value_set_boxed (value, &self->text_color);
        break;
    case PROP_HEADER_TEXT_COLOR:
        g_value_set_boxed (value, &self->header_text_color);
        break;
    case PROP_ALTERNATE_ROW_BACKGROUND:
        g_value_set_boolean (value, self->alternate_row_background);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_table_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTable *self = PN_TABLE (object);

    switch (prop_id)
    {
    case PROP_COLUMNS:
    {
        const gchar *s = g_value_get_string (value);
        g_free (self->columns);
        self->columns = g_strdup (s != NULL ? s : "");
        pn_table_reparse_columns (self);
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_LIMIT:
    {
        guint v = g_value_get_uint (value);
        if (v < 1) v = 1;
        if (v > PN_TABLE_LIMIT_MAX) v = PN_TABLE_LIMIT_MAX;
        self->limit = v;
        pn_table_trim_to_limit (self);
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_BACKGROUND_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_HEADER_BACKGROUND_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->header_background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_GRID_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->grid_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_TEXT_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->text_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_HEADER_TEXT_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->header_text_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_ALTERNATE_ROW_BACKGROUND:
        self->alternate_row_background = g_value_get_boolean (value);
        pn_node_request_repaint (PN_NODE (self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_table_finalize (GObject *object)
{
    PnTable *self = PN_TABLE (object);
    guint    i;

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }

    pn_table_clear_rows (self);
    g_queue_free (self->rows);
    self->rows = NULL;

    for (i = 0; i < self->n_cols; i++)
    {
        g_free (self->cols[i].title);
        g_free (self->cols[i].path);
    }
    g_free (self->cols);
    self->cols   = NULL;
    self->n_cols = 0;

    g_clear_pointer (&self->columns, g_free);

    G_OBJECT_CLASS (pn_table_parent_class)->finalize (object);
}

static void
pn_table_class_init (PnTableClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_table_get_property;
    object_class->set_property = pn_table_set_property;
    object_class->finalize     = pn_table_finalize;

    node_class->receive           = pn_table_receive;
    node_class->get_size          = pn_table_get_size;
    node_class->get_header_height = pn_table_get_header_height;
    node_class->scroll            = pn_table_scroll;
    /* The cairo table painter (paint_plot) is installed onto this class
     * by the gui tier — pn_table_gui_install() in pn-table-gui.c — so the
     * headless core carries no GTK/cairo.  The scroll vfunc stays here:
     * it only nudges an int. */

    node_class->class_name        = "Table";
    node_class->icon              = "\xef\x83\x8e";  /* fa-table U+F0CE */
    node_class->color             = (PnColor){ 0.27, 0.71, 0.85, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = FALSE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row       (schema, "topic", PN_EDITOR_AUTO);
        pn_settings_schema_row_flags (schema, "topic", PN_ROW_FLAG_HIDDEN);
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_COLUMNS] = g_param_spec_string (
            "columns", "Columns",
            "Comma-separated list of \"Title:path\" entries; each "
            "entry becomes one column.  Path is the same "
            "\"/\"-separated JSON pointer used elsewhere (e.g. "
            "\"data/value\").  An entry without a ':' is treated "
            "as path-only with the path doubling as the title.",
            "topic:topic,id:id",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LIMIT] = g_param_spec_uint (
            "limit", "Limit",
            "Maximum number of rows kept in the buffer.  Older rows "
            "are dropped from the bottom as new ones arrive.",
            1, PN_TABLE_LIMIT_MAX, 200,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the table rectangle behind the rows",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEADER_BACKGROUND_COLOR] = g_param_spec_boxed (
            "header-background-color", "Header background colour",
            "Fill colour of the header band (and, at 30%% alpha, the "
            "alternating-row stripe when that option is enabled)",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_GRID_COLOR] = g_param_spec_boxed (
            "grid-color", "Grid colour",
            "Colour of the frame, the column separators, and the "
            "header underline",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the body cell text",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HEADER_TEXT_COLOR] = g_param_spec_boxed (
            "header-text-color", "Header text colour",
            "Colour of the bold column titles in the header band",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ALTERNATE_ROW_BACKGROUND] = g_param_spec_boolean (
            "alternate-row-background", "Alternate row background",
            "Tint every second body row with the header background "
            "colour at 30%% alpha to make dense feeds easier to read",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_table_init (PnTable *self)
{
    PnNode  *node      = PN_NODE (self);
    PnColor  cyan      = { 0.27, 0.71, 0.85, 1.0 };

    self->columns = g_strdup ("topic:topic,id:id");
    self->limit   = 200;

    self->background_color        = (PnColor) { 1.0, 1.0, 1.0, 1.0 };
    self->header_background_color = (PnColor) { 0.92, 0.92, 0.92, 1.0 };
    self->grid_color              = (PnColor) { 0.55, 0.55, 0.55, 1.0 };
    self->text_color              = (PnColor) { 0.10, 0.10, 0.10, 1.0 };
    self->header_text_color       = (PnColor) { 0.10, 0.10, 0.10, 1.0 };
    self->alternate_row_background = TRUE;

    self->cols    = NULL;
    self->n_cols  = 0;
    self->rows    = g_queue_new ();
    self->scroll_offset      = 0;
    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    pn_table_reparse_columns (self);

    pn_node_set_class_name (node, "Table");
    pn_node_set_icon       (node, "\xef\x83\x8e");  /* fa-table U+F0CE */
    pn_node_set_color (node, &cyan);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTable *
pn_table_new (void)
{
    return g_object_new (PN_TYPE_TABLE, NULL);
}

guint
pn_table_get_row_count (PnTable *self)
{
    g_return_val_if_fail (PN_IS_TABLE (self), 0);
    return g_queue_get_length (self->rows);
}
