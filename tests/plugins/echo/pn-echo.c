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
/*  Sample pipnode plugin: PnEcho                                       */
/*                                                                     */
/*  Demonstrates the smallest viable plugin layout.  Defines a single  */
/*  #PnNode subclass, registers it with the host's #PnNodeFactory     */
/*  through the standard pn_plugin_init() entry point declared in     */
/*  <pn-plugin.h>, and is used by tests/test_plugin_load.py to verify  */
/*  the plugin discovery / load / instantiate path round-trips.        */
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
 * through the lib_HEADERS install set in lib/Makefile.am. */
#include "pn-node.h"
#include "pn-message.h"
#include "pn-node-factory.h"
#include "pn-node-dialog-helpers.h"
#include "pn-plugin.h"

#define PN_TYPE_ECHO (pn_echo_get_type ())

G_DECLARE_FINAL_TYPE (PnEcho, pn_echo, PN, ECHO, PnNode)

struct _PnEcho
{
    PnNode parent_instance;

    /* Token for the custom property editor demo: the auto-generated
     * tab would render this as a plain GtkEntry, but the
     * #PnNodeClass.build_property_editor override below promotes it
     * to a two-item GtkComboBoxText so the dialog test can prove the
     * vfunc fired. */
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

/* ------------------------------------------------------------------ */
/*  Dialog extension demo                                              */
/*                                                                     */
/*  The two overrides below exist purely to exercise the ABI v2        */
/*  dialog hooks from out-of-tree-shaped code so a regression in the   */
/*  vfunc dispatch / helpers surface lights up in CI.  They are not    */
/*  necessary for the pass-through behaviour above.                    */
/* ------------------------------------------------------------------ */

static GtkWidget *
pn_echo_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                               GParamSpec  *pspec,
                               GObject     *target,
                               GtkWindow   *parent    G_GNUC_UNUSED)
{
    if (g_strcmp0 (pspec->name, "device") == 0)
    {
        gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
        GBindingFlags flags    = G_BINDING_SYNC_CREATE
                                 | (writable ? G_BINDING_BIDIRECTIONAL : 0);
        GtkWidget    *combo    = gtk_combo_box_text_new ();

        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   "loop",   "Loopback");
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   "tee",    "Tee");

        gtk_widget_set_hexpand   (combo, TRUE);
        gtk_widget_set_sensitive (combo, writable);
        g_object_bind_property (target, "device", combo, "active-id", flags);
        return combo;
    }

    /* Other properties (none right now, but kept open for future) fall
     * back to the host default. */
    return NULL;
}

static void
pn_echo_build_extra_pages (PnNode      *self      G_GNUC_UNUSED,
                           GtkNotebook *notebook,
                           GtkWindow   *parent    G_GNUC_UNUSED)
{
    /* A trivial extra tab so the dialog has a "Stats" page beyond the
     * auto-generated property tab.  Real plugins would put live
     * counters / a logs view / a "scan again" button here. */
    GtkWidget *box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *label = gtk_label_new (
            "Echo plugin extra page — wired through "
            "PnNodeClass.build_extra_pages.");

    g_object_set (box,
                  "margin-start",  12,
                  "margin-end",    12,
                  "margin-top",    12,
                  "margin-bottom", 12,
                  NULL);
    gtk_widget_set_halign (label, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (box), label, FALSE, FALSE, 0);

    pn_node_dialog_append_page (notebook, box, "Stats");
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

    /* The ABI v2 dialog extension hooks. */
    node_class->build_property_editor = pn_echo_build_property_editor;
    node_class->build_extra_pages     = pn_echo_build_extra_pages;

    node_class->class_name = "Echo";
    node_class->icon       = "\xef\x81\xb1";  /* fa-share U+F064 */
    node_class->color      = (PnColor){ 0.55, 0.65, 0.95, 1.0 };
    node_class->category   = "Filters";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_DEVICE] = g_param_spec_string (
            "device", "Device",
            "Demo string property exercising the per-class custom "
            "property editor hook (rendered as a 2-item combo by "
            "pn_echo_build_property_editor).",
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
/*  Plugin entry point                                                 */
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
