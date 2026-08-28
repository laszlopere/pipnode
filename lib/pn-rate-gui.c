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
/*  PnRate — gui tier.                                                  */
/*                                                                     */
/*  The settings-dialog customisation for the Rate node.  The node's    */
/*  GType, properties, the periodic fetch/cache logic and the           */
/*  receive()-time conversion live in the GTK-free core file pn-rate.c; */
/*  this file installs the build_property_editor vfunc slot onto that    */
/*  class at editor startup (pn_rate_gui_install).  The two editors it   */
/*  hands back are shared with the Bridge Quote node and live in         */
/*  pn-currency-editors.c; the dialog reads and writes the node's state   */
/*  purely through its GObject properties, so the headless runtime never  */
/*  loads this half and the Rate logic runs without GTK.                */
/*                                                                     */
/*  Three of #PnRate's properties want a non-default editor:            */
/*                                                                     */
/*    * `rate`, `last-update` and `status` are written by the node       */
/*      itself (cached fetch state) and the user is meant to read, not   */
/*      edit them.                                                      */
/*                                                                     */
/*    * `from` and `to` are #PnCurrency enums, better picked from a      */
/*      combo that shows each currency's icon next to its ticker.        */
/*                                                                     */
/*  Pre-2.0 all three lived as PN_IS_RATE branches in the host's        */
/*  pn-node-dialog.c.                                                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-rate-gui.h"
#include "pn-rate.h"
#include "pn-currency-editors.h"

#include <gtk/gtk.h>

static GtkWidget *
pn_rate_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
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
        ptype == PN_TYPE_CURRENCY)
        return pn_currency_editor_new (target, pspec);

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_rate_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_RATE));

    node_class->build_property_editor = pn_rate_build_property_editor;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
