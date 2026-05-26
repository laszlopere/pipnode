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

/* Unit tests for the PnWebsocket profile-binding helper
 * (pn_ws_bind_profile + profile_resolved vfunc, plugin ABI v6).  Covers:
 *   - the default vfunc reads p["url"] and pushes it to PnWebsocket:url
 *     after the initial-resolve idle fires, even when the bound property
 *     is the default "" (follows the type's primary profile);
 *   - setting the profile-ref to a different id re-resolves to that
 *     profile's url;
 *   - a vault edit (pn_profile_set_field) emits ::changed and the bound
 *     node re-syncs its url;
 *   - a subclass that overrides profile_resolved is called instead of
 *     the default — and gets the same #PnProfile the default would have.
 *
 * The tests force PnWebsocketClass::is_configured back to FALSE in the
 * subclass so the base never actually attempts a network connect; the
 * url propagation is what we're verifying.  Headless — no display, no
 * sockets opened. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-ws.h"
#include "pn-vault.h"
#include "pn-profile-schema.h"
#include "pn-node.h"
#include "pn-node-factory.h"

#include <glib/gstdio.h>

/* --- subclass of PnWebsocket with a profile-ref property --------------- */

#define TEST_TYPE_WS (test_ws_get_type ())
G_DECLARE_FINAL_TYPE (TestWs, test_ws, TEST, WS, PnWebsocket)

struct _TestWs
{
    PnWebsocket  parent_instance;
    gchar       *endpoint_profile;

    /* When set, the override profile_resolved stashes the resolved
     * profile's "url" field here so the test can verify the vfunc was
     * called and which profile it received. */
    gchar       *last_resolved_url;
    guint        resolve_calls;
    gboolean     use_override;

    /* Set when the base class hands a failure reason through the
     * connection_failed vfunc.  The malformed-URL test reads these
     * to verify the seam reached the subclass. */
    gchar       *last_failure_reason;
    guint        failure_calls;

    /* When TRUE, the test wants is_configured to report TRUE so the
     * base class's connect path runs (instead of the default FALSE that
     * the profile-binding tests need to keep the network out). */
    gboolean     configured;
};

G_DEFINE_TYPE (TestWs, test_ws, PN_TYPE_WEBSOCKET)

enum { PROP_0, PROP_ENDPOINT_PROFILE, N_PROPS };
static GParamSpec *test_ws_props[N_PROPS];

static void
test_ws_get_property (GObject *o, guint id, GValue *v, GParamSpec *p)
{
    TestWs *self = TEST_WS (o);
    if (id == PROP_ENDPOINT_PROFILE)
        g_value_set_string (v, self->endpoint_profile);
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, p);
}

static void
test_ws_set_property (GObject *o, guint id, const GValue *v, GParamSpec *p)
{
    TestWs *self = TEST_WS (o);
    if (id == PROP_ENDPOINT_PROFILE)
    {
        g_free (self->endpoint_profile);
        self->endpoint_profile = g_value_dup_string (v);
    }
    else
        G_OBJECT_WARN_INVALID_PROPERTY_ID (o, id, p);
}

static void
test_ws_finalize (GObject *o)
{
    TestWs *self = TEST_WS (o);
    g_free (self->endpoint_profile);
    g_free (self->last_resolved_url);
    g_free (self->last_failure_reason);
    G_OBJECT_CLASS (test_ws_parent_class)->finalize (o);
}

/* Profile-binding tests need is_configured FALSE so the base never tries
 * to open a socket; the connection-failure test flips this on so the
 * base class runs through start_connect() and hits the new vfunc. */
static gboolean
test_ws_is_configured (PnWebsocket *self)
{
    return TEST_WS (self)->configured;
}

/* Override the failure seam: record the reason the base hands us so the
 * test can assert that the vfunc fired and what message it carried. */
static void
test_ws_connection_failed (PnWebsocket *base, const gchar *reason)
{
    TestWs *self = TEST_WS (base);
    g_free (self->last_failure_reason);
    self->last_failure_reason = g_strdup (reason);
    self->failure_calls++;
}

/* Override that records the resolved profile's URL alongside (still)
 * pushing the URL into the base via g_object_set, so the rest of the
 * pipeline behaves the same as the default impl. */
static void
test_ws_profile_resolved (PnWebsocket *base, PnProfile *p)
{
    TestWs *self = TEST_WS (base);
    gchar  *url;

    if (!self->use_override)
    {
        /* Delegate to the default impl when the test hasn't opted in. */
        PnWebsocketClass *parent =
                g_type_class_peek (g_type_parent (G_OBJECT_TYPE (base)));
        if (parent && parent->profile_resolved)
            parent->profile_resolved (base, p);
        self->resolve_calls++;
        return;
    }

    url = (p != NULL) ? pn_profile_get_string (p, "url") : g_strdup ("");
    g_free (self->last_resolved_url);
    self->last_resolved_url = url;

    /* Still propagate the URL so pn_ws_dup_url() reflects it. */
    g_object_set (base, "url", url != NULL ? url : "", NULL);
    self->resolve_calls++;
}

static void
test_ws_class_init (TestWsClass *klass)
{
    GObjectClass     *oc = G_OBJECT_CLASS (klass);
    PnWebsocketClass *wc = PN_WEBSOCKET_CLASS (klass);

    oc->get_property = test_ws_get_property;
    oc->set_property = test_ws_set_property;
    oc->finalize     = test_ws_finalize;

    wc->is_configured     = test_ws_is_configured;
    wc->profile_resolved  = test_ws_profile_resolved;
    wc->connection_failed = test_ws_connection_failed;

    test_ws_props[PROP_ENDPOINT_PROFILE] = g_param_spec_string (
            "endpoint-profile", "Endpoint profile",
            "Id of the test-ws-endpoint credential profile.",
            "", G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    g_object_class_install_property (oc, PROP_ENDPOINT_PROFILE,
                                     test_ws_props[PROP_ENDPOINT_PROFILE]);
    pn_param_spec_set_profile_ref (test_ws_props[PROP_ENDPOINT_PROFILE],
                                   "test-ws-endpoint");
}

static void
test_ws_init (TestWs *self)
{
    self->endpoint_profile     = g_strdup ("");
    self->last_resolved_url    = NULL;
    self->resolve_calls        = 0;
    self->use_override         = FALSE;
    self->last_failure_reason  = NULL;
    self->failure_calls        = 0;
    self->configured           = FALSE;

    pn_ws_bind_profile (PN_WEBSOCKET (self), "endpoint-profile");
}

/* Drain the main loop until no pending sources remain or @max_iters is
 * reached.  Used to let the initial-resolve idle (and any follow-ups)
 * fire deterministically. */
static void
spin (guint max_iters)
{
    GMainContext *ctx = g_main_context_default ();
    guint         i;
    for (i = 0; i < max_iters; i++)
    {
        if (!g_main_context_pending (ctx))
            break;
        g_main_context_iteration (ctx, FALSE);
    }
}

/* --- shared temp store ---------------------------------------------- */

static gchar *tmp_dir = NULL;

/* Wipe every test-ws-endpoint profile from the singleton vault so each
 * test starts from a known-empty state.  pn_node_get_profile() goes
 * through the singleton — there is no per-test vault to swap in. */
static void
reset_vault (void)
{
    PnVault *v = pn_vault_get_default ();
    GList   *profiles = pn_vault_list_profiles (v, "test-ws-endpoint");
    GList   *l;
    GList   *ids = NULL;

    /* Two passes because delete_profile invalidates the list. */
    for (l = profiles; l != NULL; l = l->next)
        ids = g_list_prepend (ids, g_strdup (pn_profile_get_id (l->data)));
    g_list_free (profiles);

    for (l = ids; l != NULL; l = l->next)
        pn_vault_delete_profile (v, l->data);
    g_list_free_full (ids, g_free);
}

/* --- tests ---------------------------------------------------------- */

static void
test_default_resolves_to_primary (void)
{
    TestWs *n;
    gchar  *u;

    reset_vault ();

    /* With nothing yet in the vault the resolver runs and produces "". */
    n = g_object_new (TEST_TYPE_WS, NULL);

    spin (16);
    u = pn_ws_dup_url (PN_WEBSOCKET (n));
    PN_CHECK_CMPSTR (u, ==, "");
    g_free (u);

    /* Create a primary profile; vault::changed fires and the bound node
     * picks it up. */
    {
        PnVault   *v = pn_vault_get_default ();
        PnProfile *p = pn_vault_create_profile (v, "test-ws-endpoint", "Primary");
        pn_profile_set_field (p, "url", "wss://primary.example/feed");
    }
    spin (16);

    u = pn_ws_dup_url (PN_WEBSOCKET (n));
    PN_CHECK_CMPSTR (u, ==, "wss://primary.example/feed");
    g_free (u);

    g_object_unref (n);
}

static void
test_explicit_ref_overrides_primary (void)
{
    PnVault   *v;
    PnProfile *home, *work;
    TestWs    *n;
    gchar     *u;

    reset_vault ();

    v    = pn_vault_get_default ();
    home = pn_vault_create_profile (v, "test-ws-endpoint", "Home");
    work = pn_vault_create_profile (v, "test-ws-endpoint", "Work");
    n    = g_object_new (TEST_TYPE_WS, NULL);

    pn_profile_set_field (home, "url", "wss://home/feed");
    pn_profile_set_field (work, "url", "wss://work/feed");
    spin (16);

    /* Empty ref follows the primary (Home, created first). */
    u = pn_ws_dup_url (PN_WEBSOCKET (n));
    PN_CHECK_CMPSTR (u, ==, "wss://home/feed");
    g_free (u);

    /* Explicit ref overrides — and the notify::endpoint-profile triggers
     * an immediate resync; no idle spin needed. */
    g_object_set (n, "endpoint-profile", pn_profile_get_id (work), NULL);
    u = pn_ws_dup_url (PN_WEBSOCKET (n));
    PN_CHECK_CMPSTR (u, ==, "wss://work/feed");
    g_free (u);

    g_object_unref (n);
}

static void
test_vault_change_resyncs (void)
{
    PnVault   *v;
    PnProfile *p;
    TestWs    *n;
    gchar     *u;

    reset_vault ();

    v = pn_vault_get_default ();
    p = pn_vault_create_profile (v, "test-ws-endpoint", "Live");

    pn_profile_set_field (p, "url", "wss://before/feed");
    n = g_object_new (TEST_TYPE_WS, NULL);
    spin (16);

    u = pn_ws_dup_url (PN_WEBSOCKET (n));
    PN_CHECK_CMPSTR (u, ==, "wss://before/feed");
    g_free (u);

    /* Manager-style edit: pn_profile_set_field emits ::changed which the
     * binding observes and re-resolves on the next main-loop tick. */
    pn_profile_set_field (p, "url", "wss://after/feed");
    spin (16);

    u = pn_ws_dup_url (PN_WEBSOCKET (n));
    PN_CHECK_CMPSTR (u, ==, "wss://after/feed");
    g_free (u);

    g_object_unref (n);
}

static void
test_connection_failed_on_malformed_url (void)
{
    TestWs *n;

    reset_vault ();

    /* Drive the base's start_connect path: opt in to is_configured and
     * set a URL that g_uri_parse rejects (whitespace is not legal in a
     * URI), so soup_message_new() in start_connect() returns NULL and
     * the synchronous "invalid URL" failure path runs.  The base no
     * longer logs the failure itself — the subclass's connection_failed
     * override is the sole channel, so this test simply asserts that
     * the override saw the right reason. */
    n = g_object_new (TEST_TYPE_WS, NULL);
    n->configured = TRUE;

    g_object_set (n, "url", "not a real url", NULL);

    /* The subclass-facing vfunc must have fired with the synthetic
     * reason — that is the seam plugin authors need so they can emit a
     * success=FALSE PnMessage on the canvas. */
    PN_CHECK_CMPINT (n->failure_calls, ==, 1);
    PN_CHECK_CMPSTR (n->last_failure_reason, ==, "invalid URL");

    /* Flip the node off so its lingering reconnect timer doesn't fire
     * again during teardown. */
    n->configured = FALSE;
    g_object_unref (n);
}

static void
test_override_vfunc_is_called (void)
{
    PnVault   *v;
    PnProfile *p;
    TestWs    *n;

    reset_vault ();

    v = pn_vault_get_default ();
    p = pn_vault_create_profile (v, "test-ws-endpoint", "Override");
    n = g_object_new (TEST_TYPE_WS, NULL);

    n->use_override = TRUE;
    pn_profile_set_field (p, "url", "wss://override/feed");
    spin (16);

    /* The override stored the URL it observed AND propagated it. */
    PN_CHECK (n->resolve_calls > 0);
    PN_CHECK_CMPSTR (n->last_resolved_url, ==, "wss://override/feed");

    g_object_unref (n);
}

static void
register_test_schema (void)
{
    PnProfileSchema *s = pn_profile_schema_new ("test-ws-endpoint", "Test WS");
    pn_profile_schema_field (s, "url", "URL", PN_FIELD_STRING);
    pn_node_factory_register_profile_type (pn_node_factory_get_default (), s);
}

static void
isolate_vault_singleton (void)
{
    gchar *cred;

    tmp_dir = g_dir_make_tmp ("pn-ws-XXXXXX", NULL);
    cred    = g_build_filename (tmp_dir, "singleton.json", NULL);
    g_setenv ("PIPNODE_CREDENTIALS_FILE", cred, TRUE);
    g_free (cred);
}

int
main (int argc, char **argv)
{
    /* Isolate before the first pn_vault_get_default(), the same pattern
     * test-pn-vault.c uses. */
    isolate_vault_singleton ();

    pn_test_init (&argc, &argv, "pn-ws");
    register_test_schema ();

    pn_test_add ("default_resolves_to_primary",  test_default_resolves_to_primary);
    pn_test_add ("explicit_ref_overrides_primary", test_explicit_ref_overrides_primary);
    pn_test_add ("vault_change_resyncs",         test_vault_change_resyncs);
    pn_test_add ("override_vfunc_is_called",     test_override_vfunc_is_called);
    pn_test_add ("connection_failed_on_malformed_url",
                 test_connection_failed_on_malformed_url);

    return pn_test_run ();
}
