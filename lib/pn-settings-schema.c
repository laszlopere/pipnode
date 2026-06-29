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

#include "pn-settings-schema.h"

/* ------------------------------------------------------------------ */
/*  Internal data model                                                */
/*                                                                     */
/*  Plain owned structs in arrays — no GObject machinery beyond the    */
/*  boxed registration, and (deliberately) no GTK: the schema is the   */
/*  data half of the Phase-7 split.  The GUI renderer reads it through */
/*  the indexed accessors at the bottom of this file.                  */
/* ------------------------------------------------------------------ */

typedef struct
{
    gchar        *prop;        /* property name (owned)                  */
    PnEditorKind  kind;
    PnRowFlags    flags;
    gchar       **choices;     /* NULL-terminated, owned, or NULL        */
    gchar        *when_prop;   /* controller property, or NULL           */
    gchar        *when_value;  /* equality target, or NULL == truthy     */
    gchar        *code_lang;   /* GtkSourceView language id for a         */
                               /* PN_EDITOR_CODE row, or NULL == "json"   */
} PnSchemaRow;

typedef struct
{
    gchar     *title;          /* NULL for the implicit default tab      */
    GPtrArray *rows;           /* of PnSchemaRow*                        */
} PnSchemaTab;

struct _PnSettingsSchema
{
    gint       ref_count;
    GPtrArray *tabs;           /* of PnSchemaTab*                        */
    gboolean   has_named_tabs; /* TRUE once pn_settings_schema_tab() ran */
};

/* ------------------------------------------------------------------ */
/*  Row / tab helpers                                                  */
/* ------------------------------------------------------------------ */

static void
schema_row_free (PnSchemaRow *row)
{
    g_free (row->prop);
    g_strfreev (row->choices);
    g_free (row->when_prop);
    g_free (row->when_value);
    g_free (row->code_lang);
    g_free (row);
}

static void
schema_tab_free (PnSchemaTab *tab)
{
    g_free (tab->title);
    g_ptr_array_free (tab->rows, TRUE);
    g_free (tab);
}

/* The tab that pn_settings_schema_row() appends to: the most recently
 * opened named tab, or a lazily-created unnamed default tab when no
 * pn_settings_schema_tab() has been called yet. */
static PnSchemaTab *
schema_current_tab (PnSettingsSchema *self)
{
    PnSchemaTab *tab;

    if (self->tabs->len == 0)
    {
        tab        = g_new0 (PnSchemaTab, 1);
        tab->title = NULL;
        tab->rows  = g_ptr_array_new_with_free_func (
                             (GDestroyNotify) schema_row_free);
        g_ptr_array_add (self->tabs, tab);
    }

    return g_ptr_array_index (self->tabs, self->tabs->len - 1);
}

/* Find the last-added row for @prop across every tab.  The builder
 * mutators (choices / flags / conditions) target a property by name so
 * they read the same way the row() call that introduced it does. */
static PnSchemaRow *
schema_find_row (PnSettingsSchema *self,
                 const gchar      *prop)
{
    guint t;

    for (t = self->tabs->len; t > 0; t--)
    {
        PnSchemaTab *tab = g_ptr_array_index (self->tabs, t - 1);
        guint        r;

        for (r = tab->rows->len; r > 0; r--)
        {
            PnSchemaRow *row = g_ptr_array_index (tab->rows, r - 1);
            if (g_strcmp0 (row->prop, prop) == 0)
                return row;
        }
    }

    return NULL;
}

/* Resolve (tab, row) for the read accessors; NULL on out-of-range. */
static PnSchemaRow *
schema_row_at (PnSettingsSchema *self,
               guint             tab,
               guint             row)
{
    PnSchemaTab *t;

    if (self == NULL || tab >= self->tabs->len)
        return NULL;
    t = g_ptr_array_index (self->tabs, tab);
    if (row >= t->rows->len)
        return NULL;
    return g_ptr_array_index (t->rows, row);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle + boxed registration                                     */
/* ------------------------------------------------------------------ */

PnSettingsSchema *
pn_settings_schema_new (void)
{
    PnSettingsSchema *self = g_new0 (PnSettingsSchema, 1);

    self->ref_count      = 1;
    self->tabs           = g_ptr_array_new_with_free_func (
                                   (GDestroyNotify) schema_tab_free);
    self->has_named_tabs = FALSE;

    return self;
}

PnSettingsSchema *
pn_settings_schema_ref (PnSettingsSchema *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    self->ref_count += 1;
    return self;
}

void
pn_settings_schema_unref (PnSettingsSchema *self)
{
    g_return_if_fail (self != NULL);

    self->ref_count -= 1;
    if (self->ref_count > 0)
        return;

    g_ptr_array_free (self->tabs, TRUE);
    g_free (self);
}

G_DEFINE_BOXED_TYPE (PnSettingsSchema, pn_settings_schema,
                     pn_settings_schema_ref, pn_settings_schema_unref)

/* ------------------------------------------------------------------ */
/*  Builder                                                            */
/* ------------------------------------------------------------------ */

void
pn_settings_schema_tab (PnSettingsSchema *self,
                        const gchar      *title)
{
    PnSchemaTab *tab;

    g_return_if_fail (self != NULL);
    g_return_if_fail (title != NULL);

    tab        = g_new0 (PnSchemaTab, 1);
    tab->title = g_strdup (title);
    tab->rows  = g_ptr_array_new_with_free_func (
                         (GDestroyNotify) schema_row_free);
    g_ptr_array_add (self->tabs, tab);

    self->has_named_tabs = TRUE;
}

void
pn_settings_schema_row (PnSettingsSchema *self,
                        const gchar      *prop,
                        PnEditorKind      kind)
{
    PnSchemaTab *tab;
    PnSchemaRow *row;

    g_return_if_fail (self != NULL);
    g_return_if_fail (prop != NULL);

    tab = schema_current_tab (self);

    row        = g_new0 (PnSchemaRow, 1);
    row->prop  = g_strdup (prop);
    row->kind  = kind;
    row->flags = PN_ROW_FLAG_NONE;
    g_ptr_array_add (tab->rows, row);
}

void
pn_settings_schema_choices (PnSettingsSchema   *self,
                            const gchar        *prop,
                            const gchar *const *choices)
{
    PnSchemaRow *row;

    g_return_if_fail (self != NULL);
    g_return_if_fail (prop != NULL);

    row = schema_find_row (self, prop);
    if (row == NULL)
    {
        g_warning ("pn_settings_schema_choices: no row for property '%s'",
                   prop);
        return;
    }

    g_strfreev (row->choices);
    row->choices = g_strdupv ((gchar **) choices);
    /* An explicit choice list implies a combo. */
    if (row->kind == PN_EDITOR_AUTO)
        row->kind = PN_EDITOR_COMBO;
}

void
pn_settings_schema_code_language (PnSettingsSchema *self,
                                  const gchar      *prop,
                                  const gchar      *language)
{
    PnSchemaRow *row;

    g_return_if_fail (self != NULL);
    g_return_if_fail (prop != NULL);

    row = schema_find_row (self, prop);
    if (row == NULL)
    {
        g_warning ("pn_settings_schema_code_language: no row for "
                   "property '%s'", prop);
        return;
    }

    g_free (row->code_lang);
    row->code_lang = g_strdup (language);
    /* Naming a source language implies a code editor. */
    if (row->kind == PN_EDITOR_AUTO)
        row->kind = PN_EDITOR_CODE;
}

void
pn_settings_schema_row_flags (PnSettingsSchema *self,
                              const gchar      *prop,
                              PnRowFlags        flags)
{
    PnSchemaRow *row;

    g_return_if_fail (self != NULL);
    g_return_if_fail (prop != NULL);

    row = schema_find_row (self, prop);
    if (row == NULL)
    {
        g_warning ("pn_settings_schema_row_flags: no row for property '%s'",
                   prop);
        return;
    }

    row->flags |= flags;
}

void
pn_settings_schema_enable_when_eq (PnSettingsSchema *self,
                                   const gchar      *prop,
                                   const gchar      *when_prop,
                                   const gchar      *value)
{
    PnSchemaRow *row;

    g_return_if_fail (self != NULL);
    g_return_if_fail (prop != NULL);
    g_return_if_fail (when_prop != NULL);
    g_return_if_fail (value != NULL);

    row = schema_find_row (self, prop);
    if (row == NULL)
    {
        g_warning ("pn_settings_schema_enable_when_eq: no row for '%s'",
                   prop);
        return;
    }

    g_free (row->when_prop);
    g_free (row->when_value);
    row->when_prop  = g_strdup (when_prop);
    row->when_value = g_strdup (value);
}

void
pn_settings_schema_enable_when_truthy (PnSettingsSchema *self,
                                       const gchar      *prop,
                                       const gchar      *when_prop)
{
    PnSchemaRow *row;

    g_return_if_fail (self != NULL);
    g_return_if_fail (prop != NULL);
    g_return_if_fail (when_prop != NULL);

    row = schema_find_row (self, prop);
    if (row == NULL)
    {
        g_warning ("pn_settings_schema_enable_when_truthy: no row for '%s'",
                   prop);
        return;
    }

    g_free (row->when_prop);
    g_free (row->when_value);
    row->when_prop  = g_strdup (when_prop);
    row->when_value = NULL;            /* NULL == truthy test */
}

/* ------------------------------------------------------------------ */
/*  Per-class attachment (qdata on the GType)                          */
/* ------------------------------------------------------------------ */

static GQuark
pn_node_class_schema_quark (void)
{
    /* Same single-threaded lazy cache as the param-spec hint quarks in
     * pn-node.c — a racy second init just re-resolves the same literal. */
    static GQuark q = 0;
    if (q == 0)
        q = g_quark_from_static_string ("pn-node-class-settings-schema");
    return q;
}

void
pn_node_class_set_settings_schema (PnNodeClass      *klass,
                                   PnSettingsSchema *schema)
{
    GType             type;
    PnSettingsSchema *old;

    g_return_if_fail (PN_IS_NODE_CLASS (klass));

    type = G_TYPE_FROM_CLASS (klass);

    /* g_type_set_qdata has no destroy notify and types live for the whole
     * process, so we own the stored ref for good and just unref any schema
     * a previous call left behind. */
    old = g_type_get_qdata (type, pn_node_class_schema_quark ());
    if (old == schema)
        return;
    if (old != NULL)
        pn_settings_schema_unref (old);

    g_type_set_qdata (type, pn_node_class_schema_quark (), schema);
}

PnSettingsSchema *
pn_node_class_get_settings_schema (PnNodeClass *klass)
{
    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), NULL);

    return g_type_get_qdata (G_TYPE_FROM_CLASS (klass),
                             pn_node_class_schema_quark ());
}

/* Locate @prop in @schema and return its row flags, or PN_ROW_FLAG_NONE
 * with *found left FALSE if no row names it. */
static PnRowFlags
schema_lookup_row_flags (PnSettingsSchema *schema,
                         const gchar      *prop,
                         gboolean         *found)
{
    guint n_tabs = pn_settings_schema_get_n_tabs (schema);
    guint t;

    for (t = 0; t < n_tabs; t++)
    {
        guint n_rows = pn_settings_schema_get_n_rows (schema, t);
        guint r;

        for (r = 0; r < n_rows; r++)
        {
            if (g_strcmp0 (pn_settings_schema_row_prop (schema, t, r),
                           prop) == 0)
            {
                *found = TRUE;
                return pn_settings_schema_row_get_flags (schema, t, r);
            }
        }
    }

    return PN_ROW_FLAG_NONE;
}

gboolean
pn_node_class_property_row_hidden (PnNodeClass *klass,
                                   const gchar *prop)
{
    GType t;

    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), FALSE);
    g_return_val_if_fail (prop != NULL, FALSE);

    /* Walk the type chain leaf -> PnNode.  The nearest class whose schema
     * names @prop wins, so a subclass overrides a base class's row (hiding
     * an inherited property, or re-showing one the base hid). */
    for (t = G_TYPE_FROM_CLASS (klass);
         t != G_TYPE_INVALID && g_type_is_a (t, PN_TYPE_NODE);
         t = g_type_parent (t))
    {
        gpointer          kt = g_type_class_peek (t);
        PnSettingsSchema *schema;
        gboolean          found = FALSE;
        PnRowFlags        flags;

        if (kt == NULL || !PN_IS_NODE_CLASS (kt))
            continue;

        schema = pn_node_class_get_settings_schema (PN_NODE_CLASS (kt));
        if (schema == NULL)
            continue;

        flags = schema_lookup_row_flags (schema, prop, &found);
        if (found)
            return (flags & PN_ROW_FLAG_HIDDEN) != 0;
    }

    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Read-back accessors                                                */
/* ------------------------------------------------------------------ */

gboolean
pn_settings_schema_has_tabs (PnSettingsSchema *self)
{
    g_return_val_if_fail (self != NULL, FALSE);
    return self->has_named_tabs;
}

guint
pn_settings_schema_get_n_tabs (PnSettingsSchema *self)
{
    g_return_val_if_fail (self != NULL, 0);
    return self->tabs->len;
}

const gchar *
pn_settings_schema_get_tab_title (PnSettingsSchema *self,
                                  guint             tab)
{
    g_return_val_if_fail (self != NULL, NULL);
    if (tab >= self->tabs->len)
        return NULL;
    return ((PnSchemaTab *) g_ptr_array_index (self->tabs, tab))->title;
}

guint
pn_settings_schema_get_n_rows (PnSettingsSchema *self,
                               guint             tab)
{
    g_return_val_if_fail (self != NULL, 0);
    if (tab >= self->tabs->len)
        return 0;
    return ((PnSchemaTab *) g_ptr_array_index (self->tabs, tab))->rows->len;
}

const gchar *
pn_settings_schema_row_prop (PnSettingsSchema *self,
                             guint             tab,
                             guint             row)
{
    PnSchemaRow *r = schema_row_at (self, tab, row);
    return r != NULL ? r->prop : NULL;
}

PnEditorKind
pn_settings_schema_row_kind (PnSettingsSchema *self,
                             guint             tab,
                             guint             row)
{
    PnSchemaRow *r = schema_row_at (self, tab, row);
    return r != NULL ? r->kind : PN_EDITOR_AUTO;
}

PnRowFlags
pn_settings_schema_row_get_flags (PnSettingsSchema *self,
                                  guint             tab,
                                  guint             row)
{
    PnSchemaRow *r = schema_row_at (self, tab, row);
    return r != NULL ? r->flags : PN_ROW_FLAG_NONE;
}

const gchar *const *
pn_settings_schema_row_choices (PnSettingsSchema *self,
                                guint             tab,
                                guint             row)
{
    PnSchemaRow *r = schema_row_at (self, tab, row);
    return r != NULL ? (const gchar *const *) r->choices : NULL;
}

const gchar *
pn_settings_schema_row_code_language (PnSettingsSchema *self,
                                      guint             tab,
                                      guint             row)
{
    PnSchemaRow *r = schema_row_at (self, tab, row);
    return r != NULL ? r->code_lang : NULL;
}

const gchar *
pn_settings_schema_row_when_prop (PnSettingsSchema *self,
                                  guint             tab,
                                  guint             row)
{
    PnSchemaRow *r = schema_row_at (self, tab, row);
    return r != NULL ? r->when_prop : NULL;
}

const gchar *
pn_settings_schema_row_when_value (PnSettingsSchema *self,
                                   guint             tab,
                                   guint             row)
{
    PnSchemaRow *r = schema_row_at (self, tab, row);
    return r != NULL ? r->when_value : NULL;
}
