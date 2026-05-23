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

#ifndef PN_FILE_VIEWER_H
#define PN_FILE_VIEWER_H

#include "pn-node.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnFileViewer                                                        */
/*                                                                     */
/*  Sink node that displays whatever arrives on its input the same way */
/*  the #PnFileDrop source paints a dropped file: it paints a view     */
/*  rectangle below its header (the paint_plot extension mechanism the */
/*  Graph node uses for its plot) and shows the picture inside it.     */
/*                                                                     */
/*  When the incoming message is a #PnImageMessage the node takes a    */
/*  reference on the carried #GdkPixbuf and shows it scaled-to-fit     */
/*  (aspect preserved) inside the view rectangle — exactly the         */
/*  rendering #PnFileDrop produces for the same pixbuf, so wiring a     */
/*  FileDrop's output to a FileViewer's input shows the very same      */
/*  image twice, both straight from the message.  Any other message    */
/*  clears the preview and falls back to a filename / "Nothing to      */
/*  show" hint, reading the basename from the message's data bag       */
/*  (the `filename` member FileDrop populates) when present.           */
/* ------------------------------------------------------------------ */

#define PN_TYPE_FILE_VIEWER (pn_file_viewer_get_type ())

G_DECLARE_FINAL_TYPE (PnFileViewer, pn_file_viewer, PN, FILE_VIEWER, PnNode)

PnFileViewer *pn_file_viewer_new (void);

/* ------------------------------------------------------------------ */
/*  GUI paint-state seam (GTK-free)                                    */
/*                                                                     */
/*  The cairo preview painter lives in the gui tier                    */
/*  (pn-file-viewer-gui.c) and reads the node's preview state through  */
/*  this one snapshot rather than reaching into the private instance   */
/*  struct.  The pixbuf and filename are borrowed (owned by the node), */
/*  valid only for the duration of the paint call.  gdk-pixbuf is an   */
/*  allowed core image-data dep, so the pixbuf rides the snapshot as a */
/*  plain pointer with no GTK involvement.                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    GdkPixbuf   *pixbuf;        /* borrowed; %NULL when no image preview */
    const gchar *last_filename; /* borrowed; %NULL before the first message */
    PnColor      area_color;
    PnColor      border_color;
} PnFileViewerPaintState;

void          pn_file_viewer_get_paint_state (PnFileViewer           *self,
                                              PnFileViewerPaintState *out);

G_END_DECLS

#endif /* PN_FILE_VIEWER_H */
