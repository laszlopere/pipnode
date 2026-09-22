/*
 * Copyright (C) 2024-2026 Laszlo Pere
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * figure-preview: render a PnFigure's client area straight to a PNG,
 * through the exact shipped paint_plot code path, with no GTK window and
 * no running editor.  A styling-iteration harness in the same spirit as
 * osc-preview: edit lib/pn-figure-gui.c, rebuild, run this, look at the
 * PNG.  Sub-second loop instead of launch-app-and-drag-a-node-out.
 *
 *   ./figure-preview [out.png] [W] [H] [program-file]
 *
 * Defaults: figure.png at 280x210 (the node's own client area, 80.9e)
 * drawing the reference specimen from the head of TODO #80 — a lever
 * tilted by its input `a`.  Wind the lever with PN_FIGURE_A (radians).
 */

#include <cairo.h>
#include <stdlib.h>

#include "pn-figure.h"
#include "pn-figure-gui.h"
#include "pn-message.h"
#include "pn-node.h"

/* The reference specimen of TODO #80, verbatim. */
static const char SPECIMEN[] =
    "view -60, -25, 60, 35\n"
    "color \"#202020\"\n"
    "width 2\n"
    "\n"
    "# the beam, tilted by the input angle\n"
    "dx = 50 * cos(value1)\n"
    "dy = 50 * sin(value1)\n"
    "line -dx, -dy, dx, dy\n"
    "\n"
    "# fulcrum\n"
    "fill \"#808080\"\n"
    "poly 0,-2, -8,-14, 8,-14\n"
    "nofill\n"
    "\n"
    "# weight hanging off the left end\n"
    "rect -dx-6, -dy-16, 12, 10\n"
    "\n"
    "text -dx, -dy+6, \"A\"\n"
    "text  dx,  dy+6, \"B\"\n"
    "text 0, 22, \"%.1f deg\", value1 * 57.2958\n";

int
main (int argc, char **argv)
{
    const char      *out     = (argc > 1) ? argv[1] : "figure.png";
    int              w       = (argc > 2) ? atoi (argv[2]) : 280;
    int              h       = (argc > 3) ? atoi (argv[3]) : 210;
    const char      *file    = (argc > 4) ? argv[4] : NULL;
    const char      *angle   = g_getenv ("PN_FIGURE_A");
    gchar           *program = NULL;
    PnFigure        *figure;
    PnNode          *node;
    PnNodeClass     *klass;
    cairo_surface_t *surf;
    cairo_t         *cr;

    /* Install the cairo painter onto the class (editor startup does this). */
    pn_figure_gui_install ();

    figure = pn_figure_new ();
    node   = PN_NODE (figure);

    if (file != NULL && !g_file_get_contents (file, &program, NULL, NULL))
    {
        g_printerr ("cannot read %s\n", file);
        return 1;
    }
    g_object_set (figure, "program",
                  program != NULL ? program : SPECIMEN, NULL);
    g_free (program);

    /* Wind the one input, the way a knob on the worksheet would. */
    {
        PnMessage *message = pn_message_new (NULL, NULL);

        pn_message_set_double (message, "value",
                               angle != NULL ? g_ascii_strtod (angle, NULL)
                                             : 0.20);
        pn_node_receive_message (node, message);
        g_object_unref (message);
    }

    klass = PN_NODE_GET_CLASS (node);
    if (klass->paint_plot == NULL)
    {
        g_printerr ("paint_plot not installed\n");
        return 1;
    }

    surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cr   = cairo_create (surf);

    /* A neutral mid backdrop, so the figure's own background colour and
     * the extent of the client rectangle are both visible. */
    cairo_set_source_rgb (cr, 0.25, 0.25, 0.27);
    cairo_paint (cr);

    klass->paint_plot (node, cr, 0.0, 0.0, (double) w, (double) h);

    cairo_destroy (cr);
    cairo_surface_flush (surf);
    if (cairo_surface_write_to_png (surf, out) != CAIRO_STATUS_SUCCESS)
    {
        g_printerr ("failed to write %s\n", out);
        return 1;
    }
    cairo_surface_destroy (surf);

    g_print ("%s (%dx%d)  error=[%s]\n", out, w, h,
             pn_figure_get_error (figure));

    g_object_unref (node);
    return 0;
}
