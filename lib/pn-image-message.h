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

#ifndef PN_IMAGE_MESSAGE_H
#define PN_IMAGE_MESSAGE_H

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "pn-message.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnImageMessage                                                      */
/*                                                                     */
/*  A #PnMessage that additionally carries a decoded image as a live   */
/*  #GdkPixbuf pointer.  It exists so a file-drop of an image can hand */
/*  the pixels downstream without ever round-tripping them through the */
/*  JSON data bag: the bag holds only lightweight metadata (filename,  */
/*  mime type, pixel dimensions, byte size), exactly what a Debug      */
/*  Print or a save would otherwise try to serialise, while the heavy  */
/*  pixel buffer travels by pointer.                                   */
/*                                                                     */
/*  The pixbuf is ref-shared, never deep-copied: pn_message_clone()    */
/*  (the wire layer's per-branch fan-out copy) takes another reference */
/*  on the same #GdkPixbuf rather than duplicating the pixels, so      */
/*  delivering one drop to ten downstream nodes costs ten refs, not    */
/*  ten image copies.  Because the pointer lives in instance state and */
/*  not in the data bag, it is structurally impossible for it to leak  */
/*  into any JSON string produced from the message.                    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_IMAGE_MESSAGE (pn_image_message_get_type ())

G_DECLARE_FINAL_TYPE (PnImageMessage, pn_image_message,
                      PN, IMAGE_MESSAGE, PnMessage)

/**
 * pn_image_message_new:
 * @source: (nullable) (transfer none): node that emitted the message
 * @topic:  (nullable): short string tag; %NULL/empty resolves to the
 *          source node's configured topic, exactly as pn_message_new()
 * @pixbuf: (nullable) (transfer none): the decoded image; a reference
 *          is taken, so the caller keeps ownership of its own ref
 *
 * Constructs an image message.  The id and creation timestamp are
 * auto-generated like any #PnMessage; callers are expected to fill the
 * metadata members (filename, mimetype, width, height, size, …) in the
 * data bag with the usual typed setters after construction.
 *
 * Returns: (transfer full): a new #PnImageMessage.
 */
PnImageMessage *pn_image_message_new (PnNode      *source,
                                      const gchar *topic,
                                      GdkPixbuf   *pixbuf);

/**
 * pn_image_message_get_pixbuf:
 * @self: the image message
 *
 * Returns: (nullable) (transfer none): the carried image, or %NULL if
 * the message was constructed without one.  The reference belongs to
 * the message; ref it if you need it to outlive the message.
 */
GdkPixbuf      *pn_image_message_get_pixbuf (PnImageMessage *self);

G_END_DECLS

#endif /* PN_IMAGE_MESSAGE_H */
