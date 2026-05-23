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

#ifndef PN_FILEDROP_H
#define PN_FILEDROP_H

#include "pn-node.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnFileDrop                                                          */
/*                                                                     */
/*  Source node that paints a plain drop-area rectangle below its      */
/*  header (the same paint_plot extension mechanism the Graph node     */
/*  uses to draw its plot) and accepts files dropped onto it from the  */
/*  desktop / a file manager.                                          */
/*                                                                     */
/*  Every dropped file emits one message:                              */
/*    - For an image the node loads it into a #GdkPixbuf, shows it      */
/*      scaled-to-fit (aspect preserved, never larger than the area)   */
/*      inside the drop rectangle, and emits a #PnImageMessage that    */
/*      carries the pixbuf by pointer.  Only metadata (filename, mime  */
/*      type, pixel dimensions, byte size) goes into the JSON data     */
/*      bag; the pixels travel out of band.                            */
/*    - For any other file it emits a plain #PnMessage whose data bag  */
/*      carries the same filename / mime type / size metadata.         */
/* ------------------------------------------------------------------ */

#define PN_TYPE_FILEDROP (pn_filedrop_get_type ())

G_DECLARE_FINAL_TYPE (PnFileDrop, pn_filedrop, PN, FILEDROP, PnNode)

PnFileDrop *pn_filedrop_new (void);

/**
 * pn_filedrop_drop_file:
 * @self: the node
 * @path: absolute filesystem path of the dropped file
 *
 * Loads @path, updates the on-canvas preview (an image is shown
 * scaled-to-fit; anything else clears any previous preview), and emits
 * the corresponding message on the node's output.  Called by the
 * worksheet's desktop drag-and-drop handler once it has resolved the
 * dropped URI to a local path and routed it to the node under the
 * cursor; safe to call directly from tests.
 */
void        pn_filedrop_drop_file (PnFileDrop  *self,
                                   const gchar *path);

/* ------------------------------------------------------------------ */
/*  GUI paint-state seam (GTK-free)                                    */
/*                                                                     */
/*  The cairo drop-area painter lives in the gui tier                  */
/*  (pn-filedrop-gui.c) and reads the node's preview state through     */
/*  this one snapshot rather than reaching into the private instance   */
/*  struct.  The pixbuf and filename are borrowed (owned by the node), */
/*  valid only for the duration of the paint call.  gdk-pixbuf is an   */
/*  allowed core image-data dep, so the pixbuf rides the snapshot as a */
/*  plain pointer with no GTK involvement.                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    GdkPixbuf   *pixbuf;        /* borrowed; %NULL when no image preview */
    const gchar *last_filename; /* borrowed; %NULL before the first drop */
    PnColor      area_color;
    PnColor      border_color;
} PnFileDropPaintState;

void        pn_filedrop_get_paint_state (PnFileDrop           *self,
                                         PnFileDropPaintState *out);

G_END_DECLS

#endif /* PN_FILEDROP_H */
