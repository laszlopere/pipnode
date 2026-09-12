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
/*  PnFiat — gui tier.                                                */
/*                                                                    */
/*  The settings-dialog customisation for the Fiat Converter.  The     */
/*  node's GType, properties, the periodic fetch/cache logic and the   */
/*  receive()-time conversion live in the GTK-free core file           */
/*  pn-fiat.c; this file installs the build_property_editor vfunc slot */
/*  onto that class at editor startup (pn_fiat_gui_install).  The      */
/*  editors it hands back are the ones shared with the FX Converter    */
/*  and Bridge Quote in pn-currency-editors.c, and the dialog reads    */
/*  and writes the node's state purely through its GObject properties, */
/*  so the headless runtime never loads this half.                     */
/*                                                                    */
/*  Three groups of property want a non-default editor:                */
/*                                                                    */
/*    * `rate`, `last-update` and `status` are written by the node     */
/*      itself (cached fetch state) and the user is meant to read,     */
/*      not edit, them.                                               */
/*                                                                    */
/*    * `from` and `to` are #PnFiatCurrency enums.  They take the      */
/*      iconless picker: there is no bundled icon set for national     */
/*      currencies, and MYR or RON read far better with the name       */
/*      spelled out beside the code than they would as three bare      */
/*      letters in a thirty-row list.                                  */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-fiat-gui.h"
#include "pn-fiat.h"
#include "pn-currency-editors.h"

#include <gtk/gtk.h>

/* Adapter for PnCurrencyDescribeFunc, which speaks plain gint so one
 * combo builder can serve every currency enum. */
static const gchar *
fiat_describe (gint value)
{
    return pn_fiat_currency_get_name ((PnFiatCurrency) value);
}

static GtkWidget *
pn_fiat_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                               GParamSpec  *pspec,
                               GObject     *target,
                               GtkWindow   *parent    G_GNUC_UNUSED)
{
    const gchar *name  = pspec->name;
    GType        ptype = G_PARAM_SPEC_VALUE_TYPE (pspec);

    if (g_strcmp0 (name, "last-update") == 0)
        return pn_readonly_label_editor_new (target, pspec, "Never");

    if (g_strcmp0 (name, "rate")   == 0 ||
        g_strcmp0 (name, "status") == 0)
        return pn_readonly_label_editor_new (target, pspec, NULL);

    if ((g_strcmp0 (name, "from") == 0 || g_strcmp0 (name, "to") == 0) &&
        ptype == PN_TYPE_FIAT_CURRENCY)
        return pn_currency_editor_new_named (target, pspec, fiat_describe);

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_fiat_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_FIAT));

    node_class->build_property_editor = pn_fiat_build_property_editor;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid.  (One leaked ref on a singleton
     * class, mirroring pn_node_factory_register.) */
}
