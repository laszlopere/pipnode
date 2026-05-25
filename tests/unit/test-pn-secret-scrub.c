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

/* Guards the credentials feature's core promise (plugin ABI v5): a property
 * tagged with pn_param_spec_set_secret() is NEVER written to the workflow
 * file, while ordinary properties still round-trip.  Uses a throwaway PnNode
 * subclass so the test stays in libpipnode-core (the bundled nodes that carry
 * real secrets live in plugins, out of the unit-test link). */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-flow.h"
#include "pn-node.h"

#include <glib/gstdio.h>
#include <string.h>

/* --- a throwaway node with one ordinary and one secret string prop --- */

#define SECRET_TYPE_NODE (secret_node_get_type ())
G_DECLARE_FINAL_TYPE (SecretNode, secret_node, SECRET, NODE, PnNode)

struct _SecretNode
{
    PnNode  parent_instance;
    gchar  *label;
    gchar  *password;
};

G_DEFINE_TYPE (SecretNode, secret_node, PN_TYPE_NODE)

enum { PROP_0, PROP_LABEL, PROP_PASSWORD, N_PROPS };
static GParamSpec *secret_node_props[N_PROPS];

static void
secret_node_get_property (GObject *o, guint id, GValue *v, GParamSpec *p)
{
    SecretNode *self = SECRET_NODE (o);
    switch (id)
    {
    case PROP_LABEL:    g_value_set_string (v, self->label);    break;
    case PROP_PASSWORD: g_value_set_string (v, self->password); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, p);
    }
}

static void
secret_node_set_property (GObject *o, guint id, const GValue *v, GParamSpec *p)
{
    SecretNode *self = SECRET_NODE (o);
    switch (id)
    {
    case PROP_LABEL:
        g_free (self->label);
        self->label = g_value_dup_string (v);
        break;
    case PROP_PASSWORD:
        g_free (self->password);
        self->password = g_value_dup_string (v);
        break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, p);
    }
}

static void
secret_node_finalize (GObject *o)
{
    SecretNode *self = SECRET_NODE (o);
    g_free (self->label);
    g_free (self->password);
    G_OBJECT_CLASS (secret_node_parent_class)->finalize (o);
}

static void
secret_node_class_init (SecretNodeClass *klass)
{
    GObjectClass *oc = G_OBJECT_CLASS (klass);
    PnNodeClass  *nc = PN_NODE_CLASS (klass);

    oc->get_property = secret_node_get_property;
    oc->set_property = secret_node_set_property;
    oc->finalize     = secret_node_finalize;

    nc->class_name = "Secret Node";
    nc->category   = "Test";

    secret_node_props[PROP_LABEL] = g_param_spec_string (
            "label", "Label", "Ordinary serialised property.",
            "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    secret_node_props[PROP_PASSWORD] = g_param_spec_string (
            "password", "Password", "Secret — must not be serialised.",
            "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (oc, N_PROPS, secret_node_props);

    pn_param_spec_set_secret (secret_node_props[PROP_PASSWORD]);
}

static void
secret_node_init (SecretNode *self)
{
    self->label    = g_strdup ("");
    self->password = g_strdup ("");
    pn_node_set_class_name (PN_NODE (self), "Secret Node");
}

/* --- the test ------------------------------------------------------- */

static void
test_secret_not_serialized (void)
{
    PnFlow     *flow = pn_flow_new ();
    SecretNode *n    = g_object_new (SECRET_TYPE_NODE, NULL);
    gchar      *dir  = g_dir_make_tmp ("pn-scrub-XXXXXX", NULL);
    gchar      *path = g_build_filename (dir, "flow.json", NULL);
    gchar      *contents = NULL;

    g_object_set (n,
                  "label",    "VISIBLE-LABEL-VALUE",
                  "password", "TOP-SECRET-PASSWORD",
                  NULL);
    pn_flow_add_node (flow, PN_NODE (n));

    PN_CHECK (pn_flow_save_to_file (flow, path, NULL));
    PN_CHECK (g_file_get_contents (path, &contents, NULL, NULL));

    if (contents != NULL)
    {
        /* The ordinary property round-trips... */
        PN_CHECK (strstr (contents, "VISIBLE-LABEL-VALUE") != NULL);
        /* ...the secret is scrubbed, both value and key. */
        PN_CHECK (strstr (contents, "TOP-SECRET-PASSWORD") == NULL);
        PN_CHECK (strstr (contents, "password") == NULL);
    }

    g_free (contents);
    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
    g_object_unref (flow);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-secret-scrub");
    pn_test_add ("secret_not_serialized", test_secret_not_serialized);
    return pn_test_run ();
}
