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

#ifndef PN_VECTOR_H
#define PN_VECTOR_H

#include <glib-object.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnVector                                                           */
/*                                                                     */
/*  A reference-counted, IMMUTABLE 1-D array of doubles backed by one  */
/*  contiguous allocation — the payload behind a "$pnvector" marker in */
/*  a message's data bag (see TODO #43).  A JsonNode cannot hold a     */
/*  GObject, so big numeric arrays live out-of-band as a #PnVector and */
/*  are referenced from the JSON tree by an integer handle; the buffer */
/*  is shared across message fan-out with a single g_object_ref rather */
/*  than copied (the same trick PnImageMessage uses for a GdkPixbuf).  */
/*                                                                     */
/*  8 bytes per element, contiguous and naturally aligned, so it is    */
/*  cheap to memcpy and friendly to vectorised numeric code.  Because  */
/*  it is shared, it is treated as IMMUTABLE: a node that transforms a */
/*  vector allocates a NEW #PnVector rather than mutating a shared one. */
/* ------------------------------------------------------------------ */

#define PN_TYPE_VECTOR (pn_vector_get_type ())

G_DECLARE_FINAL_TYPE (PnVector, pn_vector, PN, VECTOR, GObject)

/**
 * pn_vector_new_take:
 * @data: (transfer full) (array length=len): a buffer of @len doubles
 *        allocated with the GLib allocator (g_malloc/g_new); may be %NULL
 *        iff @len is 0.
 * @len:  number of elements in @data.
 *
 * Creates a #PnVector that ADOPTS @data — no copy is made and the buffer
 * is released with g_free() when the last reference drops.  The buffer
 * must not be touched by the caller afterwards.
 *
 * Returns: (transfer full): a new #PnVector.
 */
PnVector *pn_vector_new_take (gdouble *data, gsize len);

/**
 * pn_vector_new_copy:
 * @data: (array length=len) (nullable): @len doubles to copy.
 * @len:  number of elements to copy from @data.
 *
 * Creates a #PnVector holding a private copy of @data.
 *
 * Returns: (transfer full): a new #PnVector.
 */
PnVector *pn_vector_new_copy (const gdouble *data, gsize len);

/**
 * pn_vector_get_data:
 * @self: a #PnVector.
 *
 * Returns: (array length=(pn_vector_get_len)) (transfer none): a borrowed,
 *          read-only pointer to the contiguous buffer; valid while a
 *          reference to @self is held.  %NULL iff the length is 0.
 */
const gdouble *pn_vector_get_data (PnVector *self);

/**
 * pn_vector_get_len:
 * @self: a #PnVector.
 *
 * Returns: the number of elements.
 */
gsize pn_vector_get_len (PnVector *self);

G_END_DECLS

#endif /* PN_VECTOR_H */
