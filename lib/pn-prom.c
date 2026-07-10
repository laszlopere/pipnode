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

#include "pn-prom.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <math.h>
#include <string.h>

/* The word an address that was never programmed reads back as — an
 * unburnt cell, 0x00. */
#define PN_PROM_UNPROGRAMMED  0.0

#define PN_PROM_DEFAULT_CONTENTS  "0x0000 0xff\n0x0001 0x1a\n"

struct _PnProm
{
    PnNode parent_instance;

    /* The memory image as the user typed it, kept verbatim so the
     * dialog round-trips comments and layout untouched. */
    gchar *contents;

    /* The same image compiled for lookup: address (gint64, boxed into
     * the key pointer via GINT_TO_POINTER-style int64 keys) -> word
     * (gdouble, heap-allocated and owned by the table). */
    GHashTable *cells;
};

G_DEFINE_TYPE (PnProm, pn_prom, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_CONTENTS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Parsing                                                            */
/* ------------------------------------------------------------------ */

/** Parse one column of the image.  A leading "0x"/"0X" (optionally
 *  signed) is read as hexadecimal, anything else as a decimal number —
 *  so both `0xff` and `255`, and for the word column `1.5`, work.  A
 *  bare leading zero is NOT octal: "010" is ten, not eight.  Returns
 *  %FALSE unless the whole token was consumed. */
static gboolean
parse_number (const gchar *tok, gdouble *out)
{
    const gchar *p    = tok;
    gboolean     neg  = FALSE;
    gchar       *end  = NULL;

    if (*p == '+' || *p == '-')
    {
        neg = (*p == '-');
        p++;
    }

    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        guint64 v;

        if (!g_ascii_string_to_unsigned (p + 2, 16, 0, G_MAXINT64, &v, NULL))
            return FALSE;

        *out = neg ? -(gdouble) v : (gdouble) v;
        return TRUE;
    }

    *out = g_ascii_strtod (tok, &end);
    return end != NULL && *end == '\0' && end != tok && isfinite (*out);
}

/** Round an address the way a real PROM decodes its address lines: to
 *  the nearest whole cell, halves away from zero. */
static gint64
address_of (gdouble value)
{
    return (gint64) llround (value);
}

static void
insert_cell (GHashTable *cells, gint64 address, gdouble word)
{
    gint64  *key  = g_new (gint64, 1);
    gdouble *cell = g_new (gdouble, 1);

    *key  = address;
    *cell = word;
    g_hash_table_insert (cells, key, cell);
}

/** Recompile @contents into the lookup table.  Blank lines and
 *  comments ('#' to end of line) are ignored; every other line must be
 *  an "<address> <word>" pair.  A later line for the same address wins,
 *  as if the cell had been reprogrammed.  Any malformed line puts the
 *  node into the generic error state (red + ❗ on the worksheet) while
 *  still keeping the lines that did parse. */
static void
prom_recompile (PnProm *self)
{
    gchar    **lines;
    gboolean   bad = FALSE;
    guint      i;

    g_hash_table_remove_all (self->cells);

    lines = g_strsplit (self->contents != NULL ? self->contents : "", "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        gchar   *comment = strchr (lines[i], '#');
        gchar  **cols;
        gdouble  address, word;

        if (comment != NULL)
            *comment = '\0';

        g_strstrip (lines[i]);
        if (*lines[i] == '\0')
            continue;

        cols = g_strsplit_set (lines[i], " \t", -1);
        {
            const gchar *tok[2] = { NULL, NULL };
            guint        n = 0, c;

            /* g_strsplit_set() yields an empty token for every extra
             * separator in a run, so "0x00    0xff" arrives as two
             * words and three blanks: keep the words, in order. */
            for (c = 0; cols[c] != NULL; c++)
                if (*cols[c] != '\0')
                {
                    if (n < G_N_ELEMENTS (tok))
                        tok[n] = cols[c];
                    n++;   /* counts past 2 as well, so a third column
                            * makes the line malformed */
                }

            if (n != 2 ||
                !parse_number (tok[0], &address) ||
                !parse_number (tok[1], &word))
                bad = TRUE;
            else
                insert_cell (self->cells, address_of (address), word);
        }
        g_strfreev (cols);
    }

    g_strfreev (lines);
    pn_node_set_has_error (PN_NODE (self), bad);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** Pull the address out of @message under "value".  Numeric members
 *  only — a message with no numeric data.value drives no address line,
 *  so the node has nothing to read out. */
static gboolean
read_address (PnMessage *message, gint64 *out)
{
    JsonNode *node = pn_message_get_member (message, "value");
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt != G_TYPE_DOUBLE && vt != G_TYPE_INT64)
        return FALSE;

    *out = address_of (json_node_get_double (node));
    return TRUE;
}

static void
pn_prom_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnProm   *self = PN_PROM (node);
    gint64    address;
    gdouble  *cell;
    gdouble   word;

    if (!read_address (message, &address))
        return;

    cell = g_hash_table_lookup (self->cells, &address);
    word = cell != NULL ? *cell : PN_PROM_UNPROGRAMMED;

    /* The decoded address stays on the message so a downstream Debug or
     * Format can show which cell was read. */
    pn_message_set_double (message, "address", (gdouble) address);
    pn_message_set_double (message, "value",   word);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_prom_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnProm *self = PN_PROM (object);

    switch (prop_id)
    {
    case PROP_CONTENTS:
        g_value_set_string (value, self->contents);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_prom_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnProm *self = PN_PROM (object);

    switch (prop_id)
    {
    case PROP_CONTENTS:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->contents, s) != 0)
            {
                g_free (self->contents);
                self->contents = g_strdup (s != NULL ? s : "");
                prom_recompile (self);
                g_object_notify_by_pspec (object, props[PROP_CONTENTS]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_prom_finalize (GObject *object)
{
    PnProm *self = PN_PROM (object);

    g_clear_pointer (&self->contents, g_free);
    g_clear_pointer (&self->cells,    g_hash_table_unref);

    G_OBJECT_CLASS (pn_prom_parent_class)->finalize (object);
}

static void
pn_prom_class_init (PnPromClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_prom_get_property;
    object_class->set_property = pn_prom_set_property;
    object_class->finalize     = pn_prom_finalize;
    node_class->receive        = pn_prom_receive;

    node_class->class_name     = "PROM";
    node_class->icon           = "\xef\x8b\x9b";  /* fa-microchip U+F2DB */
    node_class->color          = (PnColor){ 0.35, 0.62, 0.58, 1.0 };
    node_class->category       = "Filters/Reshape";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_CONTENTS] = g_param_spec_string (
            "contents", "Contents",
            "The memory image: one \"<address> <word>\" pair per line, "
            "e.g. \"0x0000 0xff\". Either column may be hexadecimal "
            "(0x…) or decimal; blank lines and #-comments are ignored. "
            "The incoming data.value is rounded to the nearest whole "
            "address and the word stored there replaces data.value; "
            "addresses absent from the image read back as 0.0.",
            PN_PROM_DEFAULT_CONTENTS,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_multiline (props[PROP_CONTENTS]);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* The node is nothing but its memory image, so give the dialog a
     * single "Contents" tab whose multiline editor fills the page —
     * no redundant "Contents :" row label. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();

        pn_settings_schema_tab (schema, "Contents");
        pn_settings_schema_row (schema, "contents", PN_EDITOR_MULTILINE);
        pn_settings_schema_row_flags (schema, "contents",
                                      PN_ROW_FLAG_FULL_WIDTH);

        pn_node_class_set_settings_schema (PN_NODE_CLASS (klass), schema);
    }
}

static void
pn_prom_init (PnProm *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.35, 0.62, 0.58, 1.0 };

    self->cells = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                         g_free, g_free);

    /* Mirror the property default so a freshly dropped node already has
     * the two example cells burnt in. */
    self->contents = g_strdup (PN_PROM_DEFAULT_CONTENTS);
    prom_recompile (self);

    pn_node_set_class_name (node, "PROM");
    pn_node_set_icon       (node, "\xef\x8b\x9b");  /* fa-microchip U+F2DB */
    pn_node_set_color      (node, &teal);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnProm *
pn_prom_new (void)
{
    return g_object_new (PN_TYPE_PROM, NULL);
}
