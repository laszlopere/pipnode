/*
 * Copyright (C) 2024-2026 Laszlo Pere
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * osc-preview: render the PnOscilloscope MAXIMIZED panel (CRT + knob
 * panel) straight to a PNG, using the exact shipped paint_plot code path,
 * with no GTK window and no running editor.  This is a styling-iteration
 * harness: edit lib/pn-oscilloscope-gui.c, rebuild, run this, look at the
 * PNG.  Sub-second loop instead of launch-app-and-maximize.
 *
 *   ./osc-preview [out.png] [W] [H]
 *
 * Defaults: osc-panel.png at 900x620.
 */

#include <cairo.h>
#include <math.h>

#include "pn-node.h"
#include "pn-oscilloscope.h"
#include "pn-oscilloscope-gui.h"
#include "pn-message.h"
#include "pn-vector.h"

/* Feed a representative waveform so the CRT shows a real trace and the
 * auto-fitted knobs read sensible live values.  A couple of sine cycles
 * over a sample-index X, like the worked-example worksheet. */
static void
feed_demo_wave (PnNode *node)
{
    enum { N = 256 };
    gdouble    y[N];
    int        i;
    PnMessage *msg;
    PnVector  *v;

    for (i = 0; i < N; i++)
        y[i] = 2.5 * sin (i * (4.0 * G_PI / N))
             + 0.4 * sin (i * (24.0 * G_PI / N));

    msg = pn_message_new (NULL, "t");
    v   = pn_vector_new_copy (y, N);
    pn_message_set_vector (msg, "value", v);
    g_object_unref (v);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

int
main (int argc, char **argv)
{
    const char     *out = (argc > 1) ? argv[1] : "osc-panel.png";
    int             w   = (argc > 2) ? atoi (argv[2]) : 900;
    int             h   = (argc > 3) ? atoi (argv[3]) : 620;
    PnOscilloscope *osc;
    PnNode         *node;
    PnNodeClass    *klass;
    cairo_surface_t *surf;
    cairo_t        *cr;

    /* Install the cairo painter onto the class (editor startup does this). */
    pn_oscilloscope_gui_install ();

    osc  = pn_oscilloscope_new ();
    node = PN_NODE (osc);
    feed_demo_wave (node);

    /* Show two vertical + two horizontal measurement cursors so the dashed
     * green reference lines, their readouts, and the lit V/H keys are all
     * visible in the preview.  (V1;V2;H1;H2 in data units; override with
     * PN_OSC_CURSORS, e.g. "70;;1.6;" to leave V2/H2 hidden.) */
    {
        const char *cur = g_getenv ("PN_OSC_CURSORS");

        g_object_set (osc, "cursors", cur ? cur : "70;185;1.6;-1.1", NULL);
    }

    pn_oscilloscope_set_maximized (osc, TRUE);

    klass = PN_NODE_GET_CLASS (node);
    if (klass->paint_plot == NULL)
    {
        g_printerr ("paint_plot not installed\n");
        return 1;
    }

    surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cr   = cairo_create (surf);

    /* A neutral mid backdrop so any panel edge/shadow is visible. */
    cairo_set_source_rgb (cr, 0.25, 0.25, 0.27);
    cairo_paint (cr);

    /* Small inset so the whole bezel silhouette is in frame. */
    klass->paint_plot (node, cr, 12.0, 12.0, w - 24.0, h - 24.0);

    cairo_destroy (cr);
    cairo_surface_flush (surf);
    if (cairo_surface_write_to_png (surf, out) != CAIRO_STATUS_SUCCESS)
    {
        g_printerr ("failed to write %s\n", out);
        return 1;
    }
    cairo_surface_destroy (surf);

    g_object_unref (node);
    g_print ("wrote %s (%dx%d)\n", out, w, h);
    return 0;
}
