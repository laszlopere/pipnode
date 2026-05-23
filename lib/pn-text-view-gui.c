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

/* ------------------------------------------------------------------ */
/*  PnTextView — gui tier.                                              */
/*                                                                     */
/*  The cairo terminal-style text painter (background fill + monospace  */
/*  line rendering with the waiting-for-output empty state).  The       */
/*  node's GType, properties, receive() with line-splitting +           */
/*  passthrough re-emit, and the scroll bookkeeping live in the         */
/*  GTK-free core file pn-text-view.c; this file installs the paint_plot */
/*  vfunc onto that class at editor startup (pn_text_view_gui_install),  */
/*  reading the line snapshot + colours through the core's GTK-free      */
/*  accessors.  The headless runtime never loads this file's half, so    */
/*  the Text View logic runs without GTK.                               */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-text-view-gui.h"
#include "pn-text-view.h"

#include <gtk/gtk.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/*  Geometry (painter-side copy)                                       */
/* ------------------------------------------------------------------ */

#define PN_TEXT_VIEW_WIDTH   280.0
#define PN_TEXT_VIEW_INSET     6.0

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

static void
pn_text_view_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnTextView          *self    = PN_TEXT_VIEW (node);
    const double         scale   = MIN (1.0,
                                        (w / PN_TEXT_VIEW_WIDTH) * 0.6 + 0.4);
    PnTextViewPaintState ps;
    const gchar *const  *lines;
    guint                n_lines;
    double               font_px;
    double               line_h;
    const double         inset   = PN_TEXT_VIEW_INSET;
    const double         inner_x = x + inset;
    int                  visible_lines;
    int                  max_offset;
    int                  scroll_offset;
    guint                i;
    guint                shown;

    pn_text_view_get_paint_state (self, &ps);
    lines   = pn_text_view_peek_lines (self, &n_lines);
    font_px = ps.font_size         * scale;
    line_h  = (ps.font_size + 2.0) * scale;

    /* Background. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.background_color);
    cairo_fill (cr);
    cairo_restore (cr);

    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);

    cairo_select_font_face (cr, "monospace",
                            CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size (cr, font_px);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.text_color);

    /* Empty state: nothing received, or the latest message had no
     * string `output`.  Render a faint "waiting for output" hint in
     * italic so the sink reads as "waiting for input" rather than as
     * broken -- mirrors PnTableView's empty-state contract. */
    if (n_lines == 0)
    {
        cairo_select_font_face (cr, "monospace",
                                CAIRO_FONT_SLANT_ITALIC,
                                CAIRO_FONT_WEIGHT_NORMAL);
        cairo_move_to (cr, inner_x, y + font_px + 6.0);
        cairo_show_text (cr, "waiting for output");
        cairo_restore (cr);
        return;
    }

    visible_lines = (int) floor ((h - 2.0) / line_h);
    if (visible_lines < 0) visible_lines = 0;

    max_offset = (int) n_lines - visible_lines;
    if (max_offset < 0) max_offset = 0;
    /* The painter is the only place that knows the live extents, so it
     * clamps the core's stored offset here and reads the pinned value
     * back. */
    pn_text_view_clamp_scroll_offset (self, max_offset);
    scroll_offset = pn_text_view_get_scroll_offset (self);

    shown = 0;
    for (i = (guint) scroll_offset;
         i < n_lines && (int) shown < visible_lines;
         i++)
    {
        const gchar *line = lines[i];
        const double ly   = y + (shown + 1) * line_h;

        if (line == NULL)
            line = "";

        cairo_move_to (cr, inner_x, ly);
        cairo_show_text (cr, line);
        shown++;
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_text_view_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_TEXT_VIEW));

    node_class->paint_plot = pn_text_view_paint_plot;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot we
     * just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
