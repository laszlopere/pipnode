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

#include "pn-image-message.h"

/* Final type: the carried pixbuf is the only state added on top of the
 * #PnMessage envelope + data bag, so it lives directly in the instance
 * struct rather than in a separate private. */
struct _PnImageMessage
{
    PnMessage  parent_instance;

    /* The decoded image, ref-shared across clones.  May be %NULL when
     * the message was constructed without one. */
    GdkPixbuf *pixbuf;
};

G_DEFINE_TYPE (PnImageMessage, pn_image_message, PN_TYPE_MESSAGE)

/* ------------------------------------------------------------------ */
/*  Clone hook                                                         */
/*                                                                     */
/*  Invoked by pn_message_clone() after it has deep-copied the         */
/*  envelope and data bag into a freshly-constructed instance of the   */
/*  same GType.  We ref-share the pixbuf so a fan-out to N downstream  */
/*  branches costs N references, not N pixel copies — the reason the   */
/*  image travels by pointer in the first place.                       */
/* ------------------------------------------------------------------ */

static void
pn_image_message_copy_private (
        PnMessage *dst,
        PnMessage *src)
{
    PnImageMessage *d = PN_IMAGE_MESSAGE (dst);
    PnImageMessage *s = PN_IMAGE_MESSAGE (src);

    g_clear_object (&d->pixbuf);
    if (s->pixbuf != NULL)
        d->pixbuf = g_object_ref (s->pixbuf);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_image_message_dispose (GObject *object)
{
    PnImageMessage *self = PN_IMAGE_MESSAGE (object);

    g_clear_object (&self->pixbuf);

    G_OBJECT_CLASS (pn_image_message_parent_class)->dispose (object);
}

static void
pn_image_message_class_init (PnImageMessageClass *klass)
{
    GObjectClass   *object_class  = G_OBJECT_CLASS (klass);
    PnMessageClass *message_class = PN_MESSAGE_CLASS (klass);

    object_class->dispose       = pn_image_message_dispose;
    message_class->copy_private = pn_image_message_copy_private;
}

static void
pn_image_message_init (PnImageMessage *self)
{
    self->pixbuf = NULL;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnImageMessage *
pn_image_message_new (
        PnNode      *source,
        const gchar *topic,
        GdkPixbuf   *pixbuf)
{
    PnImageMessage *self;
    gchar          *resolved = NULL;

    /* Mirror pn_message_new()'s topic defaulting: an unset topic on a
     * real source node resolves to that node's configured template so
     * an image message stamps the same envelope a plain message would. */
    if ((topic == NULL || *topic == '\0') && source != NULL)
    {
        resolved = pn_node_resolve_topic (source);
        topic    = resolved;
    }

    self = g_object_new (PN_TYPE_IMAGE_MESSAGE,
                         "source", source,
                         "topic",  topic,
                         NULL);

    g_free (resolved);

    if (pixbuf != NULL)
        self->pixbuf = g_object_ref (pixbuf);

    return self;
}

GdkPixbuf *
pn_image_message_get_pixbuf (PnImageMessage *self)
{
    g_return_val_if_fail (PN_IS_IMAGE_MESSAGE (self), NULL);
    return self->pixbuf;
}
