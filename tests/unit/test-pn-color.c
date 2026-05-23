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

/* Unit tests + permanent guard for PnColor, the GTK-free colour type the
 * headless-core split (TODO #23) lifted out of #PnNodeClass's GdkRGBA
 * `color` slot.  The whole split rests on two properties this file pins:
 *
 *   1. PnColor is byte-identical in layout to GdkRGBA, so the GUI tier
 *      can cast `PnColor* <-> GdkRGBA*` for free and an already-compiled
 *      plugin's `color` slot keeps reading correctly.  Asserted at
 *      compile time below.
 *
 *   2. pn_color_parse() / pn_color_to_string() agree with
 *      gdk_rgba_parse() / gdk_rgba_to_string() for the colour strings the
 *      worksheet save format and preferences actually use — so saved
 *      files round-trip byte-for-byte across the GdkRGBA -> PnColor cut
 *      (risk register: "PnColor parser diverges from gdk_rgba_parse").
 *
 * This is the only unit test that includes GTK, and only to compare
 * against the reference implementation; it never calls gtk_init(). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-color.h"

#include <gtk/gtk.h>
#include <stddef.h>

/* (1) Layout identity — the cornerstone of the ABI-stable cast. */
G_STATIC_ASSERT (sizeof (PnColor) == sizeof (GdkRGBA));
G_STATIC_ASSERT (offsetof (PnColor, red)   == offsetof (GdkRGBA, red));
G_STATIC_ASSERT (offsetof (PnColor, green) == offsetof (GdkRGBA, green));
G_STATIC_ASSERT (offsetof (PnColor, blue)  == offsetof (GdkRGBA, blue));
G_STATIC_ASSERT (offsetof (PnColor, alpha) == offsetof (GdkRGBA, alpha));

/* The actual body colours pinned by node classes around the tree, plus a
 * couple of translucent values to exercise the rgba() branch. */
static const PnColor sample_colors[] = {
    { 0.62, 0.50, 0.78, 1.0 },   /* violet (load / default)        */
    { 0.92, 0.76, 0.27, 1.0 },   /* yellow (dial / graph / meter)  */
    { 0.27, 0.71, 0.85, 1.0 },   /* cyan   (table)                 */
    { 0.40, 0.70, 0.45, 1.0 },   /* green  (http base)             */
    { 0.30, 0.62, 0.70, 1.0 },   /* teal   (ws base)               */
    { 0.86, 0.30, 0.28, 1.0 },   /* red    (unconfigured flip)     */
    { 0.10, 0.20, 0.30, 0.5 },   /* translucent                    */
    { 0.0,  0.0,  0.0,  0.0 },   /* fully transparent              */
};

static void
test_layout_runtime_alias (void)
{
    /* The compile-time asserts above already pin the layout; this also
     * checks at runtime that writing through a GdkRGBA* alias of a
     * PnColor reads back through the PnColor members (and vice versa). */
    PnColor  c = { 0.0, 0.0, 0.0, 0.0 };
    GdkRGBA *g = (GdkRGBA *) &c;

    g->red = 0.25; g->green = 0.5; g->blue = 0.75; g->alpha = 1.0;
    PN_CHECK_NEAR (c.red,   0.25, 1e-12);
    PN_CHECK_NEAR (c.green, 0.5,  1e-12);
    PN_CHECK_NEAR (c.blue,  0.75, 1e-12);
    PN_CHECK_NEAR (c.alpha, 1.0,  1e-12);
}

static void
test_to_string_matches_gdk (void)
{
    for (gsize i = 0; i < G_N_ELEMENTS (sample_colors); i++)
    {
        PnColor c = sample_colors[i];
        GdkRGBA g = { c.red, c.green, c.blue, c.alpha };

        gchar *ours = pn_color_to_string (&c);
        gchar *ref  = gdk_rgba_to_string (&g);

        PN_CHECK_CMPSTR (ours, ==, ref);

        g_free (ours);
        g_free (ref);
    }
}

static void
test_roundtrip_through_string (void)
{
    for (gsize i = 0; i < G_N_ELEMENTS (sample_colors); i++)
    {
        PnColor c = sample_colors[i];
        gchar  *s = pn_color_to_string (&c);
        PnColor back;

        PN_CHECK (pn_color_parse (&back, s));
        /* gdk_rgba_to_string() quantises r/g/b to 8 bits, so allow one
         * step of 1/255; alpha keeps full %g precision. */
        PN_CHECK_NEAR (back.red,   c.red,   1.0 / 255.0);
        PN_CHECK_NEAR (back.green, c.green, 1.0 / 255.0);
        PN_CHECK_NEAR (back.blue,  c.blue,  1.0 / 255.0);
        PN_CHECK_NEAR (back.alpha, c.alpha, 1e-6);

        g_free (s);
    }
}

static void
test_parse_matches_gdk (void)
{
    /* The canonical string forms the loader / preferences hand us — gdk's
     * own rgb()/rgba() output and the hex forms GTK3 itself accepts.
     * pn_color_parse() must agree with gdk_rgba_parse() component-for-
     * component on every one of these (the round-trip guarantee). */
    static const char *specs[] = {
        "rgb(158,128,199)",
        "rgba(158,128,199,0.5)",
        "rgb(255,255,255)",
        "rgba(0,0,0,0)",
        "#ff8800",
        "#f80",
    };

    for (gsize i = 0; i < G_N_ELEMENTS (specs); i++)
    {
        PnColor ours;
        GdkRGBA ref;
        gboolean ok_ours = pn_color_parse (&ours, specs[i]);
        gboolean ok_ref  = gdk_rgba_parse (&ref, specs[i]);

        PN_CHECK (ok_ours);
        PN_CHECK (ok_ref);
        if (ok_ours && ok_ref)
        {
            PN_CHECK_NEAR (ours.red,   ref.red,   1e-9);
            PN_CHECK_NEAR (ours.green, ref.green, 1e-9);
            PN_CHECK_NEAR (ours.blue,  ref.blue,  1e-9);
            PN_CHECK_NEAR (ours.alpha, ref.alpha, 1e-9);
        }
    }
}

static void
test_parse_extra_forms (void)
{
    PnColor c;

    /* Forms pn_color_parse() accepts as a deliberate superset of GTK3's
     * gdk_rgba_parse() — the CSS Color 4 alpha-hex notations, surrounding
     * whitespace, and the "transparent" keyword.  Checked against fixed
     * expectations since the reference rejects them. */

    PN_CHECK (pn_color_parse (&c, "#11223344"));   /* #rrggbbaa */
    PN_CHECK_NEAR (c.red,   0x11 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.green, 0x22 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.blue,  0x33 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.alpha, 0x44 / 255.0, 1e-9);

    PN_CHECK (pn_color_parse (&c, "#1234"));       /* #rgba (nibbles ×17) */
    PN_CHECK_NEAR (c.red,   0x11 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.green, 0x22 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.blue,  0x33 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.alpha, 0x44 / 255.0, 1e-9);

    PN_CHECK (pn_color_parse (&c, "  rgb(10, 20, 30)  "));
    PN_CHECK_NEAR (c.red,   10 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.green, 20 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.blue,  30 / 255.0, 1e-9);
    PN_CHECK_NEAR (c.alpha, 1.0,        1e-9);

    PN_CHECK (pn_color_parse (&c, "transparent"));
    PN_CHECK_NEAR (c.alpha, 0.0, 1e-9);
}

static void
test_parse_rejects_garbage (void)
{
    PnColor c = { 0.1, 0.2, 0.3, 0.4 };

    PN_CHECK_FALSE (pn_color_parse (&c, ""));
    PN_CHECK_FALSE (pn_color_parse (&c, "not a colour"));
    PN_CHECK_FALSE (pn_color_parse (&c, "rgb(1,2)"));
    PN_CHECK_FALSE (pn_color_parse (&c, NULL));

    /* On failure @c is left untouched. */
    PN_CHECK_NEAR (c.red,   0.1, 1e-12);
    PN_CHECK_NEAR (c.alpha, 0.4, 1e-12);
}

static void
test_equal_and_boxed (void)
{
    PnColor a = { 0.5, 0.6, 0.7, 1.0 };
    PnColor b = { 0.5, 0.6, 0.7, 1.0 };
    PnColor c = { 0.5, 0.6, 0.7, 0.5 };

    PN_CHECK (pn_color_equal (&a, &b));
    PN_CHECK_FALSE (pn_color_equal (&a, &c));
    PN_CHECK (pn_color_equal (NULL, NULL));
    PN_CHECK_FALSE (pn_color_equal (&a, NULL));

    /* Boxed registration: a GValue can carry PnColor by value (this is
     * how the "color" property serialises and binds). */
    {
        GValue   v = G_VALUE_INIT;
        PnColor *got;

        g_value_init (&v, PN_TYPE_COLOR);
        g_value_set_boxed (&v, &a);
        got = g_value_get_boxed (&v);
        PN_CHECK (got != NULL && pn_color_equal (got, &a));
        g_value_unset (&v);
    }
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-color");
    pn_test_add ("layout_runtime_alias", test_layout_runtime_alias);
    pn_test_add ("to_string_matches_gdk", test_to_string_matches_gdk);
    pn_test_add ("roundtrip_through_string", test_roundtrip_through_string);
    pn_test_add ("parse_matches_gdk", test_parse_matches_gdk);
    pn_test_add ("parse_extra_forms", test_parse_extra_forms);
    pn_test_add ("parse_rejects_garbage", test_parse_rejects_garbage);
    pn_test_add ("equal_and_boxed", test_equal_and_boxed);
    return pn_test_run ();
}
