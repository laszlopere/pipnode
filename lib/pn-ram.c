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

#include "pn-ram.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <math.h>
#include <string.h>

/* The word an address that was never written reads back as. */
#define PN_RAM_UNWRITTEN  0.0

struct _PnRam
{
    PnNode parent_instance;

    /* Optional seed image, kept verbatim so the dialog round-trips. */
    gchar *contents;

    /* Live memory: address (gint64) -> word (gdouble, owned). */
    GHashTable *cells;
};

G_DEFINE_TYPE (PnRam, pn_ram, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_CONTENTS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Parsing (shares PROM's image syntax)                              */
/* ------------------------------------------------------------------ */

/** Parse one column of the image: a leading "0x"/"0X" is hexadecimal,
 *  anything else decimal; the whole token must be consumed. */
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

/** Decode an address line the way a real memory does: nearest whole
 *  cell, halves away from zero. */
static gint64
address_of (gdouble value)
{
    return (gint64) llround (value);
}

static void
store_cell (GHashTable *cells, gint64 address, gdouble word)
{
    gint64  *key  = g_new (gint64, 1);
    gdouble *cell = g_new (gdouble, 1);

    *key  = address;
    *cell = word;
    g_hash_table_insert (cells, key, cell);
}

/** Reset the memory to the seed image.  Blank lines and '#'-comments are
 *  ignored; every other line must be an "<address> <word>" pair.  A
 *  malformed line puts the node into the generic error state while the
 *  lines that did parse still load. */
static void
ram_load_contents (PnRam *self)
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

            for (c = 0; cols[c] != NULL; c++)
                if (*cols[c] != '\0')
                {
                    if (n < G_N_ELEMENTS (tok))
                        tok[n] = cols[c];
                    n++;
                }

            if (n != 2 ||
                !parse_number (tok[0], &address) ||
                !parse_number (tok[1], &word))
                bad = TRUE;
            else
                store_cell (self->cells, address_of (address), word);
        }
        g_strfreev (cols);
    }

    g_strfreev (lines);
    pn_node_set_has_error (PN_NODE (self), bad);
}

/* ------------------------------------------------------------------ */
/*  Message helpers                                                    */
/* ------------------------------------------------------------------ */

/** Numeric member @key -> @out (int64/double only). */
static gboolean
read_number (PnMessage *message, const gchar *key, gdouble *out)
{
    JsonNode *node = pn_message_get_member (message, key);
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt != G_TYPE_DOUBLE && vt != G_TYPE_INT64)
        return FALSE;

    *out = json_node_get_double (node);
    return TRUE;
}

/** Is the write strobe asserted?  A boolean data.write, or a numeric one
 *  past the 0.5 midpoint, both count. */
static gboolean
write_asserted (PnMessage *message)
{
    JsonNode *node = pn_message_get_member (message, "write");
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt == G_TYPE_BOOLEAN)
        return json_node_get_boolean (node);
    if (vt == G_TYPE_DOUBLE || vt == G_TYPE_INT64)
        return json_node_get_double (node) > 0.5;
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_ram_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnRam    *self = PN_RAM (node);
    gdouble   value;
    gint64    address;
    gdouble  *cell;
    gdouble   word;

    /* No address line driven — nothing to read or write. */
    if (!read_number (message, "value", &value))
        return;

    address = address_of (value);

    if (write_asserted (message))
    {
        /* Store data.word (missing/non-numeric stores 0); silent, so a
         * write-back cannot re-trigger the graph. */
        gdouble w = 0.0;
        read_number (message, "word", &w);
        store_cell (self->cells, address, w);
        return;
    }

    /* Read: the stored word replaces data.value, the decoded cell is
     * echoed on data.address — exactly the shape PROM emits. */
    cell = g_hash_table_lookup (self->cells, &address);
    word = cell != NULL ? *cell : PN_RAM_UNWRITTEN;

    pn_message_set_double (message, "address", (gdouble) address);
    pn_message_set_double (message, "value",   word);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_ram_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnRam *self = PN_RAM (object);

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
pn_ram_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnRam *self = PN_RAM (object);

    switch (prop_id)
    {
    case PROP_CONTENTS:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->contents, s) != 0)
            {
                g_free (self->contents);
                self->contents = g_strdup (s != NULL ? s : "");
                ram_load_contents (self);
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
pn_ram_finalize (GObject *object)
{
    PnRam *self = PN_RAM (object);

    g_clear_pointer (&self->contents, g_free);
    g_clear_pointer (&self->cells,    g_hash_table_unref);

    G_OBJECT_CLASS (pn_ram_parent_class)->finalize (object);
}

static void
pn_ram_class_init (PnRamClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_ram_get_property;
    object_class->set_property = pn_ram_set_property;
    object_class->finalize     = pn_ram_finalize;
    node_class->receive        = pn_ram_receive;

    node_class->class_name     = "RAM";
    node_class->icon           = "\xef\x87\x80";  /* fa-database U+F1C0 */
    node_class->color          = (PnColor){ 0.42, 0.40, 0.68, 1.0 };
    node_class->category       = "CPU";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_CONTENTS] = g_param_spec_string (
            "contents", "Contents",
            "Optional seed image, one \"<address> <word>\" pair per line "
            "(hex 0x… or decimal), same syntax as PROM. The memory is "
            "addressed by data.value: with data.write true the word on "
            "data.word is stored (no message is emitted); otherwise the "
            "stored word is read back onto data.value (unwritten cells "
            "read 0.0), with the cell echoed on data.address.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_multiline (props[PROP_CONTENTS]);

    g_object_class_install_properties (object_class, N_PROPS, props);

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
pn_ram_init (PnRam *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  indigo = { 0.42, 0.40, 0.68, 1.0 };

    self->cells = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                         g_free, g_free);
    self->contents = g_strdup ("");
    ram_load_contents (self);

    pn_node_set_class_name (node, "RAM");
    pn_node_set_icon       (node, "\xef\x87\x80");  /* fa-database U+F1C0 */
    pn_node_set_color      (node, &indigo);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnRam *
pn_ram_new (void)
{
    return g_object_new (PN_TYPE_RAM, NULL);
}
