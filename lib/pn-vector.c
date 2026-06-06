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

#include "pn-vector.h"

struct _PnVector
{
    GObject  parent_instance;

    gdouble *data;   /* owned; freed with g_free, %NULL iff len == 0 */
    gsize    len;
};

G_DEFINE_FINAL_TYPE (PnVector, pn_vector, G_TYPE_OBJECT)

static void
pn_vector_finalize (GObject *object)
{
    PnVector *self = PN_VECTOR (object);

    g_clear_pointer (&self->data, g_free);

    G_OBJECT_CLASS (pn_vector_parent_class)->finalize (object);
}

static void
pn_vector_class_init (PnVectorClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = pn_vector_finalize;
}

static void
pn_vector_init (PnVector *self)
{
    self->data = NULL;
    self->len  = 0;
}

PnVector *
pn_vector_new_take (gdouble *data, gsize len)
{
    PnVector *self;

    g_return_val_if_fail (len == 0 || data != NULL, NULL);

    self = g_object_new (PN_TYPE_VECTOR, NULL);

    /* Normalise the empty vector to a NULL buffer regardless of what the
     * caller handed us, so get_data() is consistently NULL-iff-empty. */
    if (len == 0)
    {
        g_free (data);
        return self;
    }

    self->data = data;
    self->len  = len;
    return self;
}

PnVector *
pn_vector_new_copy (const gdouble *data, gsize len)
{
    gdouble *copy = NULL;

    if (len > 0)
    {
        g_return_val_if_fail (data != NULL, NULL);
        copy = g_memdup2 (data, len * sizeof (gdouble));
    }

    return pn_vector_new_take (copy, len);
}

const gdouble *
pn_vector_get_data (PnVector *self)
{
    g_return_val_if_fail (PN_IS_VECTOR (self), NULL);

    return self->data;
}

gsize
pn_vector_get_len (PnVector *self)
{
    g_return_val_if_fail (PN_IS_VECTOR (self), 0);

    return self->len;
}

gchar *
pn_vector_to_sample_string (PnVector *self, gsize max_show)
{
    gsize    show, i;
    GString *s;

    g_return_val_if_fail (PN_IS_VECTOR (self), NULL);

    if (max_show == 0)
        max_show = 8;

    show = MIN (self->len, max_show);
    s    = g_string_new ("[");
    for (i = 0; i < show; i++)
        g_string_append_printf (s, "%s%g", i ? ", " : "", self->data[i]);
    if (self->len > show)
        g_string_append (s, ", \xE2\x80\xA6");   /* … */
    g_string_append_printf (s, "] (%" G_GSIZE_FORMAT " values)", self->len);
    return g_string_free (s, FALSE);
}
