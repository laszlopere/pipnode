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
/*  Sample pipnode plugin: PnEcho — logic half (GTK-free).             */
/*                                                                     */
/*  Demonstrates the recommended two-tier plugin layout (TODO #23,     */
/*  Phase 6): the logic lives here, links only the GTK-free core, and  */
/*  is the only half a headless server installs.  A separate companion */
/*  module (pn-echo-gui.c, built as pn_echo-gui.so) carries the GTK    */
/*  settings-dialog customisations and is loaded by the editor alone.  */
/*                                                                     */
/*  This file defines a single #PnNode subclass, registers it with the */
/*  host's #PnNodeFactory through the standard pn_plugin_init() entry   */
/*  point declared in <pn-plugin.h>, and is used by                    */
/*  tests/test_plugin_load.py to verify the plugin discovery / load /  */
/*  instantiate path round-trips under pipnode-run with no GTK.        */
/*                                                                     */
/*  Behaviour:                                                         */
/*    Forwards every message it receives unchanged to its output       */
/*    port — a no-op pass-through.  Useful as a sanity check that      */
/*    the plugin's #GType is reachable through the worksheet load     */
/*    path and that the worker plumbing (input port, emit signal)     */
/*    survives the boundary between the host binary and the plugin    */
/*    .so.                                                             */
/* ------------------------------------------------------------------ */

#include <gmodule.h>

/* In-tree build: pull headers directly from $(top_srcdir)/lib so the
 * sample plugin compiles before libpipnode is installed.  An
 * out-of-tree third-party plugin would use the installed forms
 * <pipnode/pn-node.h>, <pipnode/pn-plugin.h>, … which are exposed
 * through the lib_HEADERS install set in lib/Makefile.am.
 *
 * Note this list carries no GTK / dialog headers: the logic half is
 * GTK-free, so it links libpipnode-core alone and loads without GTK. */
#include "pn-echo.h"
#include "pn-message.h"
#include "pn-node-factory.h"
#include "pn-plugin.h"

struct _PnEcho
{
    PnNode parent_instance;

    /* Token for the custom property editor demo: the auto-generated
     * tab renders this as a plain GtkEntry, but the companion module's
     * #PnNodeClass.build_property_editor override promotes it to a
     * two-item GtkComboBoxText so the dialog test can prove the vfunc
     * — installed from the companion .so — fired. */
    gchar *device;
};

enum {
    PROP_0,
    PROP_DEVICE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

G_DEFINE_TYPE (PnEcho, pn_echo, PN_TYPE_NODE)

static void
pn_echo_receive (PnNode *node, PnMessage *message)
{
    /* Plain pass-through; the sample plugin is not in the business of
     * mutating the message envelope. */
    pn_node_emit_message (node, message);
}

static void
pn_echo_get_property (GObject    *object,
                      guint       prop_id,
                      GValue     *value,
                      GParamSpec *pspec)
{
    PnEcho *self = PN_ECHO (object);

    switch (prop_id)
    {
    case PROP_DEVICE:
        g_value_set_string (value, self->device);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_echo_set_property (GObject      *object,
                      guint         prop_id,
                      const GValue *value,
                      GParamSpec   *pspec)
{
    PnEcho *self = PN_ECHO (object);

    switch (prop_id)
    {
    case PROP_DEVICE:
        g_free (self->device);
        self->device = g_value_dup_string (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_echo_finalize (GObject *object)
{
    PnEcho *self = PN_ECHO (object);

    g_clear_pointer (&self->device, g_free);

    G_OBJECT_CLASS (pn_echo_parent_class)->finalize (object);
}

static void
pn_echo_class_init (PnEchoClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_echo_get_property;
    object_class->set_property = pn_echo_set_property;
    object_class->finalize     = pn_echo_finalize;

    node_class->receive = pn_echo_receive;

    /* The dialog-extension vfuncs (build_property_editor,
     * build_extra_pages) are intentionally NOT set here — they touch
     * GTK and live in the companion module, which installs them onto
     * this class via pn_plugin_gui_init().  The headless host runs the
     * logic above with those slots NULL, so the node loads without
     * GTK and simply has no custom dialog there. */

    node_class->class_name = "Echo";
    node_class->icon       = "\xef\x81\xb1";  /* fa-share U+F064 */
    node_class->color      = (PnColor){ 0.55, 0.65, 0.95, 1.0 };
    node_class->category   = "Filters";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_DEVICE] = g_param_spec_string (
            "device", "Device",
            "Demo string property exercising the per-class custom "
            "property editor hook (rendered as a 2-item combo by the "
            "companion module's pn_echo_build_property_editor).",
            "loop",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_echo_init (PnEcho *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  blue = { 0.55, 0.65, 0.95, 1.0 };

    self->device = g_strdup ("loop");

    pn_node_set_class_name (node, "Echo");
    pn_node_set_icon       (node, "\xef\x81\xb1");
    pn_node_set_color      (node, &blue);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Plugin entry point (logic half)                                    */
/* ------------------------------------------------------------------ */

G_MODULE_EXPORT const PnPluginInfo *
pn_plugin_init (PnNodeFactory *factory)
{
    static const PnPluginInfo info = {
        .abi_version = PN_PLUGIN_ABI_VERSION,
        .name        = "EchoPlugin",
        .version     = "1.0.0",
        .description = "Sample plugin: forwards every input message "
                       "unchanged to its output (pass-through).",
    };

    pn_node_factory_register (factory, PN_TYPE_ECHO);
    return &info;
}
