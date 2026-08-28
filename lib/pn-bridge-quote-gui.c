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
/*  PnBridgeQuote — gui tier.                                          */
/*                                                                     */
/*  The settings-dialog customisation for the Bridge Quote node.  The   */
/*  GType, the two provider back-ends and the fetch/cache logic live in */
/*  the GTK-free core file pn-bridge-quote.c; this file installs the    */
/*  build_property_editor vfunc slot onto that class at editor startup. */
/*                                                                     */
/*  Six of the node's properties are fetched state — the user reads     */
/*  them, the node writes them — and every one of them is a double the  */
/*  default spinbutton would mangle: an effective PLS/BNB rate of       */
/*  1.79e-08 renders as "0.00" at two decimal places, and a `quote` is  */
/*  not something to type over by accident.  They all get the shared    */
/*  read-only monospace row instead.                                   */
/*                                                                     */
/*  `from` and `to` reuse the FX Converter's icon+ticker combo, so the  */
/*  two currency nodes pick their pair the same way.  `bridge` is a     */
/*  plain two-entry enum and the dialog's default combo suits it.       */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-bridge-quote-gui.h"
#include "pn-bridge-quote.h"
#include "pn-currency-editors.h"

#include <gtk/gtk.h>

/* Fetched state: shown, never edited. */
static const gchar *const readonly_props[] = {
    "quote", "rate", "fee", "min-amount", "max-amount", "status",
};

static GtkWidget *
pn_bridge_quote_build_property_editor (PnNode      *self   G_GNUC_UNUSED,
                                       GParamSpec  *pspec,
                                       GObject     *target,
                                       GtkWindow   *parent G_GNUC_UNUSED)
{
    const gchar *name  = pspec->name;
    GType        ptype = G_PARAM_SPEC_VALUE_TYPE (pspec);
    guint        i;

    /* An empty timestamp means no quote has landed yet — say so rather
     * than leaving the row blank. */
    if (g_strcmp0 (name, "last-update") == 0)
        return pn_readonly_label_editor_new (target, pspec, "Never");

    for (i = 0; i < G_N_ELEMENTS (readonly_props); i++)
        if (g_strcmp0 (name, readonly_props[i]) == 0)
            return pn_readonly_label_editor_new (target, pspec, NULL);

    if ((g_strcmp0 (name, "from") == 0 || g_strcmp0 (name, "to") == 0) &&
        ptype == PN_TYPE_CURRENCY)
        return pn_currency_editor_new (target, pspec);

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_bridge_quote_gui_install (void)
{
    PnNodeClass *node_class =
            PN_NODE_CLASS (g_type_class_ref (PN_TYPE_BRIDGE_QUOTE));

    node_class->build_property_editor = pn_bridge_quote_build_property_editor;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
