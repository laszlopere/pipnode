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

/* Unit tests for PnSettingsSchema — the GTK-free, declarative settings
 * description the headless split (TODO #23, Phase 7) puts in the core so a
 * settings-only node/plugin needs no GTK and no `-gui.so` companion.  This
 * file pins the builder-API → data-model mapping and the per-class qdata
 * storage; it links libpipnode-core ONLY (no GTK), proving the schema is
 * core-tier.  The schema → GtkWidget rendering lives in the gui tier and is
 * covered by the functional D-Bus dialog tests, not here. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-settings-schema.h"
#include "pn-node.h"

/* A throwaway concrete #PnNode subclass so the per-class attachment can be
 * exercised without dragging in a real node (and its dependencies). */
#define TEST_TYPE_NODE (test_node_get_type ())
G_DECLARE_FINAL_TYPE (TestNode, test_node, TEST, NODE, PnNode)
struct _TestNode { PnNode parent_instance; };
G_DEFINE_TYPE (TestNode, test_node, PN_TYPE_NODE)
static void test_node_class_init (TestNodeClass *klass) { (void) klass; }
static void test_node_init       (TestNode      *self)  { (void) self; }

/* A derivable base + leaf so chain resolution of row visibility (a subclass
 * overriding a base class's hidden row) can be exercised. */
typedef struct { PnNode      parent_instance; } TestBase;
typedef struct { PnNodeClass parent_class;    } TestBaseClass;
static GType test_base_get_type (void);
G_DEFINE_TYPE (TestBase, test_base, PN_TYPE_NODE)
static void test_base_class_init (TestBaseClass *k) { (void) k; }
static void test_base_init       (TestBase      *s) { (void) s; }
#define TEST_TYPE_BASE (test_base_get_type ())

typedef struct { TestBase      parent_instance; } TestLeaf;
typedef struct { TestBaseClass parent_class;    } TestLeafClass;
static GType test_leaf_get_type (void);
G_DEFINE_TYPE (TestLeaf, test_leaf, TEST_TYPE_BASE)
static void test_leaf_class_init (TestLeafClass *k) { (void) k; }
static void test_leaf_init       (TestLeaf      *s) { (void) s; }
#define TEST_TYPE_LEAF (test_leaf_get_type ())

static void
test_empty (void)
{
    PnSettingsSchema *s = pn_settings_schema_new ();

    PN_CHECK_FALSE (pn_settings_schema_has_tabs (s));
    PN_CHECK_CMPINT ((gint) pn_settings_schema_get_n_tabs (s), ==, 0);

    pn_settings_schema_unref (s);
}

/* Rows added before any pn_settings_schema_tab() land on an implicit,
 * unnamed default tab — and the schema reports it has NO named tabs, so the
 * renderer keeps the single auto-generated property tab. */
static void
test_default_tab (void)
{
    PnSettingsSchema *s = pn_settings_schema_new ();

    pn_settings_schema_row (s, "alpha", PN_EDITOR_AUTO);
    pn_settings_schema_row (s, "beta",  PN_EDITOR_SPIN);

    PN_CHECK_FALSE (pn_settings_schema_has_tabs (s));
    PN_CHECK_CMPINT ((gint) pn_settings_schema_get_n_tabs (s), ==, 1);
    PN_CHECK (pn_settings_schema_get_tab_title (s, 0) == NULL);

    PN_CHECK_CMPINT ((gint) pn_settings_schema_get_n_rows (s, 0), ==, 2);
    PN_CHECK_CMPSTR (pn_settings_schema_row_prop (s, 0, 0), ==, "alpha");
    PN_CHECK_CMPSTR (pn_settings_schema_row_prop (s, 0, 1), ==, "beta");
    PN_CHECK_CMPINT (pn_settings_schema_row_kind (s, 0, 0), ==, PN_EDITOR_AUTO);
    PN_CHECK_CMPINT (pn_settings_schema_row_kind (s, 0, 1), ==, PN_EDITOR_SPIN);

    pn_settings_schema_unref (s);
}

/* Named tabs replace the auto tab; rows attach to the most recent tab. */
static void
test_named_tabs (void)
{
    PnSettingsSchema *s = pn_settings_schema_new ();

    pn_settings_schema_tab (s, "Data");
    pn_settings_schema_row (s, "key",   PN_EDITOR_AUTO);
    pn_settings_schema_row (s, "label", PN_EDITOR_AUTO);
    pn_settings_schema_tab (s, "Scale");
    pn_settings_schema_row (s, "min-value", PN_EDITOR_SPIN);

    PN_CHECK (pn_settings_schema_has_tabs (s));
    PN_CHECK_CMPINT ((gint) pn_settings_schema_get_n_tabs (s), ==, 2);
    PN_CHECK_CMPSTR (pn_settings_schema_get_tab_title (s, 0), ==, "Data");
    PN_CHECK_CMPSTR (pn_settings_schema_get_tab_title (s, 1), ==, "Scale");

    PN_CHECK_CMPINT ((gint) pn_settings_schema_get_n_rows (s, 0), ==, 2);
    PN_CHECK_CMPINT ((gint) pn_settings_schema_get_n_rows (s, 1), ==, 1);
    PN_CHECK_CMPSTR (pn_settings_schema_row_prop (s, 1, 0), ==, "min-value");

    pn_settings_schema_unref (s);
}

/* An explicit choice list is copied and implies a combo for an AUTO row. */
static void
test_choices (void)
{
    PnSettingsSchema   *s = pn_settings_schema_new ();
    static const gchar *const units[] = { "C", "F", "K", NULL };
    const gchar *const *got;

    pn_settings_schema_row     (s, "unit", PN_EDITOR_AUTO);
    pn_settings_schema_choices (s, "unit", units);

    PN_CHECK_CMPINT (pn_settings_schema_row_kind (s, 0, 0), ==, PN_EDITOR_COMBO);

    got = pn_settings_schema_row_choices (s, 0, 0);
    PN_CHECK (got != NULL);
    if (got != NULL)
    {
        PN_CHECK_CMPSTR (got[0], ==, "C");
        PN_CHECK_CMPSTR (got[1], ==, "F");
        PN_CHECK_CMPSTR (got[2], ==, "K");
        PN_CHECK (got[3] == NULL);
    }

    pn_settings_schema_unref (s);
}

static void
test_flags (void)
{
    PnSettingsSchema *s = pn_settings_schema_new ();

    pn_settings_schema_row       (s, "expression", PN_EDITOR_MULTILINE);
    pn_settings_schema_row_flags (s, "expression",
                                  PN_ROW_FLAG_FULL_WIDTH | PN_ROW_FLAG_READONLY);

    PN_CHECK_CMPINT (pn_settings_schema_row_get_flags (s, 0, 0), ==,
                     PN_ROW_FLAG_FULL_WIDTH | PN_ROW_FLAG_READONLY);

    pn_settings_schema_unref (s);
}

/* Conditions: an equality target stores when_prop + when_value; a truthy
 * test stores when_prop with a NULL value (the documented discriminator). */
static void
test_conditions (void)
{
    PnSettingsSchema *s = pn_settings_schema_new ();

    pn_settings_schema_row (s, "hold-ms", PN_EDITOR_SPIN);
    pn_settings_schema_row (s, "label",   PN_EDITOR_AUTO);

    pn_settings_schema_enable_when_eq     (s, "hold-ms", "mode", "flash");
    pn_settings_schema_enable_when_truthy (s, "label",   "show-label");

    PN_CHECK_CMPSTR (pn_settings_schema_row_when_prop  (s, 0, 0), ==, "mode");
    PN_CHECK_CMPSTR (pn_settings_schema_row_when_value (s, 0, 0), ==, "flash");

    PN_CHECK_CMPSTR (pn_settings_schema_row_when_prop  (s, 0, 1), ==, "show-label");
    PN_CHECK (pn_settings_schema_row_when_value (s, 0, 1) == NULL);

    /* A row with no condition reports no controller. */
    {
        PnSettingsSchema *plain = pn_settings_schema_new ();
        pn_settings_schema_row (plain, "x", PN_EDITOR_AUTO);
        PN_CHECK (pn_settings_schema_row_when_prop (plain, 0, 0) == NULL);
        pn_settings_schema_unref (plain);
    }

    pn_settings_schema_unref (s);
}

/* Out-of-range accessors are safe and return benign defaults. */
static void
test_out_of_range (void)
{
    PnSettingsSchema *s = pn_settings_schema_new ();

    pn_settings_schema_row (s, "only", PN_EDITOR_AUTO);

    PN_CHECK (pn_settings_schema_get_tab_title (s, 5) == NULL);
    PN_CHECK_CMPINT ((gint) pn_settings_schema_get_n_rows (s, 5), ==, 0);
    PN_CHECK (pn_settings_schema_row_prop (s, 0, 9) == NULL);
    PN_CHECK_CMPINT (pn_settings_schema_row_kind (s, 9, 9), ==, PN_EDITOR_AUTO);

    pn_settings_schema_unref (s);
}

/* Boxed registration: a schema rides in a GValue (copy == ref), so it can
 * be carried by the GObject machinery. */
static void
test_boxed (void)
{
    PnSettingsSchema *s = pn_settings_schema_new ();
    GValue            v = G_VALUE_INIT;
    PnSettingsSchema *got;

    pn_settings_schema_row (s, "a", PN_EDITOR_AUTO);

    g_value_init (&v, PN_TYPE_SETTINGS_SCHEMA);
    g_value_set_boxed (&v, s);          /* copy func = ref → same pointer */
    got = g_value_get_boxed (&v);
    PN_CHECK (got == s);
    g_value_unset (&v);                 /* drops the ref the value held    */

    pn_settings_schema_unref (s);
}

/* Per-class attachment via GType qdata: get is NULL until set, returns the
 * exact schema after, and a second set replaces (unrefs) the first. */
static void
test_class_attach (void)
{
    PnNodeClass      *klass = g_type_class_ref (TEST_TYPE_NODE);
    PnSettingsSchema *first  = pn_settings_schema_new ();
    PnSettingsSchema *second = pn_settings_schema_new ();

    PN_CHECK (pn_node_class_get_settings_schema (klass) == NULL);

    pn_node_class_set_settings_schema (klass, first);   /* transfer full */
    PN_CHECK (pn_node_class_get_settings_schema (klass) == first);

    /* Replacing unrefs `first` internally; do not touch it afterwards. */
    pn_node_class_set_settings_schema (klass, second);
    PN_CHECK (pn_node_class_get_settings_schema (klass) == second);

    g_type_class_unref (klass);
}

/* PN_ROW_FLAG_HIDDEN is resolved across the class chain (leaf -> PnNode):
 * a base class can hide a row, a subclass inherits the hide, and a subclass
 * can re-show it by naming the same row without the flag (nearest wins). */
static void
test_row_hidden_chain (void)
{
    PnNodeClass      *base = g_type_class_ref (TEST_TYPE_BASE);
    PnNodeClass      *leaf = g_type_class_ref (TEST_TYPE_LEAF);
    PnSettingsSchema *bs   = pn_settings_schema_new ();

    /* No schema attached anywhere: nothing is hidden. */
    PN_CHECK_FALSE (pn_node_class_property_row_hidden (leaf, "topic"));

    /* Base hides "topic"; the leaf inherits the decision. */
    pn_settings_schema_row       (bs, "topic", PN_EDITOR_AUTO);
    pn_settings_schema_row_flags (bs, "topic", PN_ROW_FLAG_HIDDEN);
    pn_node_class_set_settings_schema (base, bs);          /* transfer full */

    PN_CHECK (pn_node_class_property_row_hidden (base, "topic"));
    PN_CHECK (pn_node_class_property_row_hidden (leaf, "topic"));

    /* A property no schema in the chain names is never hidden. */
    PN_CHECK_FALSE (pn_node_class_property_row_hidden (leaf, "name"));

    /* The leaf re-shows "topic" with a plain row: nearest (leaf) wins. */
    {
        PnSettingsSchema *ls = pn_settings_schema_new ();
        pn_settings_schema_row (ls, "topic", PN_EDITOR_AUTO);  /* no HIDDEN */
        pn_node_class_set_settings_schema (leaf, ls);

        PN_CHECK_FALSE (pn_node_class_property_row_hidden (leaf, "topic"));
        /* The base still hides it for its own instances. */
        PN_CHECK (pn_node_class_property_row_hidden (base, "topic"));
    }

    g_type_class_unref (leaf);
    g_type_class_unref (base);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-settings-schema");
    pn_test_add ("empty",          test_empty);
    pn_test_add ("default_tab",    test_default_tab);
    pn_test_add ("named_tabs",     test_named_tabs);
    pn_test_add ("choices",        test_choices);
    pn_test_add ("flags",          test_flags);
    pn_test_add ("conditions",     test_conditions);
    pn_test_add ("out_of_range",   test_out_of_range);
    pn_test_add ("boxed",          test_boxed);
    pn_test_add ("class_attach",   test_class_attach);
    pn_test_add ("row_hidden_chain", test_row_hidden_chain);
    return pn_test_run ();
}
