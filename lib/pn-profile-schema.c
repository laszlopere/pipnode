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

#include "pn-profile-schema.h"

/* ------------------------------------------------------------------ */
/*  Internal data model                                                */
/*                                                                     */
/*  Plain owned structs in an array — the GTK-free data half of the    */
/*  credentials feature, read through the indexed accessors below.     */
/*  Mirrors pn-settings-schema.c deliberately so the two read alike.   */
/* ------------------------------------------------------------------ */

typedef struct
{
    gchar              *name;        /* machine name (owned)            */
    gchar              *label;       /* human label (owned)            */
    PnProfileFieldKind  kind;
    gchar              *def;         /* default value, or NULL          */
    gchar             **choices;     /* NULL-terminated, owned, or NULL */
} PnSchemaField;

struct _PnProfileSchema
{
    gint       ref_count;
    gchar     *type_id;
    gchar     *display_name;
    gchar     *help_page;            /* HTML filename, owned, or NULL  */
    GPtrArray *fields;               /* of PnSchemaField*               */
};

static void
schema_field_free (PnSchemaField *f)
{
    g_free (f->name);
    g_free (f->label);
    g_free (f->def);
    g_strfreev (f->choices);
    g_free (f);
}

static PnSchemaField *
schema_find (PnProfileSchema *self, const gchar *name)
{
    guint i;

    for (i = 0; i < self->fields->len; i++)
    {
        PnSchemaField *f = g_ptr_array_index (self->fields, i);
        if (g_strcmp0 (f->name, name) == 0)
            return f;
    }
    return NULL;
}

static PnSchemaField *
schema_at (PnProfileSchema *self, guint index)
{
    if (self == NULL || index >= self->fields->len)
        return NULL;
    return g_ptr_array_index (self->fields, index);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle + boxed registration                                     */
/* ------------------------------------------------------------------ */

PnProfileSchema *
pn_profile_schema_new (const gchar *type_id,
                       const gchar *display_name)
{
    PnProfileSchema *self;

    g_return_val_if_fail (type_id != NULL, NULL);

    self = g_new0 (PnProfileSchema, 1);
    self->ref_count    = 1;
    self->type_id      = g_strdup (type_id);
    self->display_name = g_strdup (display_name != NULL ? display_name
                                                        : type_id);
    self->fields       = g_ptr_array_new_with_free_func (
                                 (GDestroyNotify) schema_field_free);
    return self;
}

PnProfileSchema *
pn_profile_schema_ref (PnProfileSchema *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    self->ref_count += 1;
    return self;
}

void
pn_profile_schema_unref (PnProfileSchema *self)
{
    g_return_if_fail (self != NULL);

    self->ref_count -= 1;
    if (self->ref_count > 0)
        return;

    g_free (self->type_id);
    g_free (self->display_name);
    g_free (self->help_page);
    g_ptr_array_free (self->fields, TRUE);
    g_free (self);
}

G_DEFINE_BOXED_TYPE (PnProfileSchema, pn_profile_schema,
                     pn_profile_schema_ref, pn_profile_schema_unref)

void
pn_profile_schema_set_help_page (PnProfileSchema *self,
                                 const gchar     *help_page)
{
    g_return_if_fail (self != NULL);

    g_free (self->help_page);
    self->help_page = (help_page != NULL && *help_page != '\0')
                      ? g_strdup (help_page) : NULL;
}

const gchar *
pn_profile_schema_get_help_page (PnProfileSchema *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->help_page;
}

/* ------------------------------------------------------------------ */
/*  Builder                                                            */
/* ------------------------------------------------------------------ */

void
pn_profile_schema_field (PnProfileSchema   *self,
                         const gchar       *name,
                         const gchar       *label,
                         PnProfileFieldKind kind)
{
    PnSchemaField *f;

    g_return_if_fail (self != NULL);
    g_return_if_fail (name != NULL);

    if (schema_find (self, name) != NULL)
    {
        g_warning ("pn_profile_schema_field: duplicate field '%s' on '%s'",
                   name, self->type_id);
        return;
    }

    f         = g_new0 (PnSchemaField, 1);
    f->name   = g_strdup (name);
    f->label  = g_strdup (label != NULL ? label : name);
    f->kind   = kind;
    g_ptr_array_add (self->fields, f);
}

void
pn_profile_schema_field_default (PnProfileSchema *self,
                                 const gchar     *name,
                                 const gchar     *default_value)
{
    PnSchemaField *f;

    g_return_if_fail (self != NULL);
    g_return_if_fail (name != NULL);

    f = schema_find (self, name);
    if (f == NULL)
    {
        g_warning ("pn_profile_schema_field_default: no field '%s'", name);
        return;
    }

    g_free (f->def);
    f->def = g_strdup (default_value);
}

void
pn_profile_schema_field_choices (PnProfileSchema    *self,
                                 const gchar        *name,
                                 const gchar *const *choices)
{
    PnSchemaField *f;

    g_return_if_fail (self != NULL);
    g_return_if_fail (name != NULL);

    f = schema_find (self, name);
    if (f == NULL)
    {
        g_warning ("pn_profile_schema_field_choices: no field '%s'", name);
        return;
    }

    g_strfreev (f->choices);
    f->choices = g_strdupv ((gchar **) choices);
    if (f->kind == PN_FIELD_STRING)
        f->kind = PN_FIELD_ENUM;
}

/* ------------------------------------------------------------------ */
/*  Read-back accessors                                                */
/* ------------------------------------------------------------------ */

const gchar *
pn_profile_schema_get_type_id (PnProfileSchema *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->type_id;
}

const gchar *
pn_profile_schema_get_display_name (PnProfileSchema *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->display_name;
}

guint
pn_profile_schema_get_n_fields (PnProfileSchema *self)
{
    g_return_val_if_fail (self != NULL, 0);
    return self->fields->len;
}

const gchar *
pn_profile_schema_field_name (PnProfileSchema *self, guint index)
{
    PnSchemaField *f = schema_at (self, index);
    return f != NULL ? f->name : NULL;
}

const gchar *
pn_profile_schema_field_get_label (PnProfileSchema *self, guint index)
{
    PnSchemaField *f = schema_at (self, index);
    return f != NULL ? f->label : NULL;
}

PnProfileFieldKind
pn_profile_schema_field_get_kind (PnProfileSchema *self, guint index)
{
    PnSchemaField *f = schema_at (self, index);
    return f != NULL ? f->kind : PN_FIELD_STRING;
}

const gchar *
pn_profile_schema_field_get_default (PnProfileSchema *self, guint index)
{
    PnSchemaField *f = schema_at (self, index);
    return f != NULL ? f->def : NULL;
}

const gchar *const *
pn_profile_schema_field_get_choices (PnProfileSchema *self, guint index)
{
    PnSchemaField *f = schema_at (self, index);
    return f != NULL ? (const gchar *const *) f->choices : NULL;
}

gint
pn_profile_schema_find_field (PnProfileSchema *self, const gchar *name)
{
    guint i;

    g_return_val_if_fail (self != NULL, -1);
    g_return_val_if_fail (name != NULL, -1);

    for (i = 0; i < self->fields->len; i++)
    {
        PnSchemaField *f = g_ptr_array_index (self->fields, i);
        if (g_strcmp0 (f->name, name) == 0)
            return (gint) i;
    }
    return -1;
}
