/*
 * Copyright (C) 2024-2026 Laszlo Pere
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * graph-preview: render a multi-series PnGraph straight to a PNG through
 * the REAL node painter (pn_graph_gui_install + the class paint_plot
 * vfunc), with no GTK window and no running editor — the end-to-end
 * check for the MathGL 3D views (TODO #48).  Feeds four topics of
 * gaussian samples and renders the distribution view, which fans out to
 * the 3D bar painter for nseries > 1.
 *
 * Build (libs must be built first: make -C lib):
 *   gcc tools/graph-preview.c -o tools/graph-preview -I lib \
 *       $(pkg-config --cflags gtk+-3.0 json-glib-1.0) \
 *       -L lib/.libs -lpipnode-gui -lpipnode-core \
 *       -Wl,-rpath,$PWD/lib/.libs \
 *       $(pkg-config --libs gtk+-3.0 cairo glib-2.0) -lm
 *
 * Run: ./tools/graph-preview [out.png] [W] [H]
 */

#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>

#include "pn-node.h"
#include "pn-message.h"
#include "pn-graph.h"
#include "pn-graph-gui.h"

static void
feed (PnNode *node, PnNode *source, const char *topic, double value)
{
    PnMessage *msg = pn_message_new (source, topic);
    pn_message_set_double (msg, "value", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

int
main (int argc, char **argv)
{
    const char  *out = (argc > 1) ? argv[1] : "graph-3d-bars.png";
    int          w   = (argc > 2) ? atoi (argv[2]) : 900;
    int          h   = (argc > 3) ? atoi (argv[3]) : 640;
    const char  *topics[4] = { "kitchen", "living", "outdoor", "attic" };
    const char  *froms[4]  = { "Eth5 Load", "Mini06 CPU", "Outdoor Temp",
                               "Attic Humidity" };
    const double mean[4]   = { 20.0, 26.0, 12.0, 18.0 };
    const double sd[4]     = {  2.0,  3.0,  1.5,  4.0 };
    PnNode          *srcs[4];
    PnGraph         *g;
    PnNode          *node;
    PnNodeClass     *klass;
    cairo_surface_t *surf;
    cairo_t         *cr;
    GRand           *r;
    int              t, i;

    pn_graph_gui_install ();

    g    = pn_graph_new ();
    node = PN_NODE (g);
    g_object_set (g, "data-view", PN_GRAPH_VIEW_DISTRIBUTION, NULL);

    /* Four topics, each a gaussian cloud with its own mean/spread, so
     * the distribution view shows four distinct ribbons of bar towers. */
    {
        const char *ns = g_getenv ("PN_GRAPH_NSAMPLES");
        int nsamples = ns ? atoi (ns) : 180;

    /* A distinctly-named source node per topic so each series gets a real
     * message "from" label for the colour key. */
    for (t = 0; t < 4; t++)
    {
        srcs[t] = PN_NODE (pn_graph_new ());
        pn_node_set_name (srcs[t], froms[t]);
    }

    r = g_rand_new_with_seed (12345);
    for (t = 0; t < 4; t++)
        for (i = 0; i < nsamples; i++)
        {
            double u1 = g_rand_double (r) + 1e-12;
            double u2 = g_rand_double (r);
            double z  = sqrt (-2.0 * log (u1)) * cos (2.0 * G_PI * u2);
            feed (node, srcs[t], topics[t], mean[t] + sd[t] * z);
        }
    g_rand_free (r);
    }

    klass = PN_NODE_GET_CLASS (node);
    if (klass->paint_plot == NULL)
    {
        g_printerr ("paint_plot not installed\n");
        return 1;
    }

    surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cr   = cairo_create (surf);
    cairo_set_source_rgb (cr, 0.25, 0.25, 0.27);
    cairo_paint (cr);

    klass->paint_plot (node, cr, 12.0, 12.0, w - 24.0, h - 24.0);

    cairo_destroy (cr);
    cairo_surface_flush (surf);
    if (cairo_surface_write_to_png (surf, out) != CAIRO_STATUS_SUCCESS)
    {
        g_printerr ("failed to write %s\n", out);
        return 1;
    }
    cairo_surface_destroy (surf);

    for (t = 0; t < 4; t++)
        g_object_unref (srcs[t]);
    g_object_unref (node);
    g_print ("wrote %s (%dx%d)\n", out, w, h);
    return 0;
}
