/*
 * graph-line-preview: render the multi-series TIME_SERIES (3D line) view
 * of a PnGraph through the real node painter, no GTK window.  Spreads
 * samples across real time bins with in-process sleeps so the line has
 * genuine multi-bin history (not a single cluster at x=0).
 *
 *   ./tools/graph-line-preview [out.png] [rounds]
 */
#include <cairo.h>
#include <glib.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

#include "pn-node.h"
#include "pn-message.h"
#include "pn-graph.h"
#include "pn-graph-gui.h"

static void
feed (PnNode *node, const char *topic, double value)
{
    PnMessage *msg = pn_message_new (NULL, topic);
    pn_message_set_double (msg, "value", value);
    pn_node_receive_message (node, msg);
    g_object_unref (msg);
}

int
main (int argc, char **argv)
{
    const char *out    = (argc > 1) ? argv[1] : "graph-3d-line.png";
    int         rounds = (argc > 2) ? atoi (argv[2]) : 24;
    int         w = 900, h = 640, i;
    PnGraph        *g;
    PnNode         *node;
    PnNodeClass    *klass;
    cairo_surface_t *surf;
    cairo_t        *cr;
    gint64          binw;

    pn_graph_gui_install ();
    g    = pn_graph_new ();
    node = PN_NODE (g);
    g_object_set (g, "data-view", PN_GRAPH_VIEW_TIME_SERIES, NULL);
    {
        const char *lw = g_getenv ("PN_GRAPH_LW");
        if (lw)
            g_object_set (g, "line-width", (guint) atoi (lw), NULL);
    }

    binw = pn_graph_bin_width_us (g);
    g_print ("bin_width = %" G_GINT64_FORMAT " us, window = %u s\n",
             binw, pn_graph_resolution_seconds (PN_GRAPH_RES_MINUTE));

    for (i = 0; i < rounds; i++)
    {
        feed (node, "kitchen", 20.0 + 4.0 * sin (i * 0.5));
        feed (node, "living",  60.0 + 5.0 * cos (i * 0.4));
        g_usleep (binw);          /* advance one time bin */
    }

    {
        guint n = 0;
        PnGraphSeriesView *v = pn_graph_collect_series_sorted (g, &n);
        g_print ("series = %u\n", n);
        g_free (v);
    }

    klass = PN_NODE_GET_CLASS (node);
    surf = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cr   = cairo_create (surf);
    cairo_set_source_rgb (cr, 0.25, 0.25, 0.27);
    cairo_paint (cr);
    klass->paint_plot (node, cr, 12.0, 12.0, w - 24.0, h - 24.0);
    cairo_destroy (cr);
    cairo_surface_flush (surf);
    cairo_surface_write_to_png (surf, out);
    cairo_surface_destroy (surf);
    g_object_unref (node);
    g_print ("wrote %s (%dx%d), %d rounds\n", out, w, h, rounds);
    return 0;
}
