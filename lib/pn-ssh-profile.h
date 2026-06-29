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

#ifndef PN_SSH_PROFILE_H
#define PN_SSH_PROFILE_H

#include "pn-node-factory.h"

G_BEGIN_DECLS

/* The host-provisioned credential profile type for an SSH login: a
 * username / port / identity-file / passphrase / password / host-key-policy
 * bundle that says HOW to log into a remote host, kept apart from the
 * in-node "host" field that says WHERE.  Two bundled plugins reference it —
 * the shell plugin (Shell Command, df, free, lxc-ls, tmux-monitor) and the
 * host-monitoring plugin (CPU, Load, Memory, …) — so the type lives in
 * core, not behind one plugin's libtool BIND_LOCAL boundary, exactly as the
 * mqtt-broker profile does for the MQTT base class.  A node with no profile
 * keeps today's ambient-key behaviour. */
#define PN_PROFILE_SSH "ssh-login"

/**
 * pn_ssh_register_profile_type:
 * @factory: the process-wide factory
 *
 * Registers the "ssh-login" profile type with @factory.  Called once from
 * the host's built-in registration pass (alongside
 * #pn_mqtt_register_profile_type) so the type is visible to both the shell
 * and host-monitoring plugins before any SSH-using node is constructed.
 * Idempotent: re-registering is a no-op.
 */
void pn_ssh_register_profile_type (PnNodeFactory *factory);

/**
 * PnSshLogin:
 * @has_profile:     %TRUE when a profile applied; %FALSE means ambient
 *                   login (no -l/-p/-i, accept-new policy) and the other
 *                   fields are ignored
 * @username:        login name (ssh -l); never %NULL, "" when unset
 * @identity_file:   private-key path (ssh -i); never %NULL, "" when unset
 * @host_key_policy: the "ssh-login" host-key-policy token; never %NULL,
 *                   "" treated as accept-new
 * @port:            TCP port; only emitted (as -p) when it is not 22
 * @host:            the host the profile is for (its required "host" field);
 *                   never %NULL, "" when the profile leaves it blank.  When
 *                   non-empty it is the connection target — #pn_ssh_build_argv
 *                   uses it in place of the node's own host argument, so the
 *                   profile both says WHERE and HOW to log in.
 *
 * A flat, value-only snapshot of the login fields resolved from an
 * "ssh-login" profile.  #PnProfile is owned by the vault and only safe to
 * read on the main thread; the SSH nodes run their `ssh` on a worker
 * thread, so they resolve a #PnSshLogin on the main thread (under their own
 * mutex) and hand a copy of it to #pn_ssh_build_argv from the worker.  All
 * strings are owned by the struct.  A zero-initialised #PnSshLogin
 * (`{0}` / @has_profile %FALSE) is a valid "ambient" snapshot; clearing one
 * with #pn_ssh_login_clear returns it to that state.
 */
typedef struct {
    gboolean  has_profile;
    gchar    *username;
    gchar    *identity_file;
    gchar    *host_key_policy;
    gint      port;
    gchar    *host;
} PnSshLogin;

/**
 * pn_ssh_login_resolve:
 * @node:     a node carrying an "ssh-login" profile-ref string property
 * @ref_prop: the name of that property (e.g. "auth-profile")
 * @out:      (out caller-allocates): filled with the resolved snapshot;
 *            any previous contents are overwritten (not freed — pass a
 *            zeroed struct, or #pn_ssh_login_clear it first)
 *
 * MAIN-THREAD ONLY.  Resolves @node's referenced profile with
 * #pn_node_get_profile and copies its username / identity-file /
 * host-key-policy / port into @out as plain strings.  @out.has_profile is
 * %FALSE (an ambient snapshot) when no profile applies — i.e. the picker is
 * on "Default" and no primary "ssh-login" profile exists, or the ref is the
 * "Custom settings" sentinel.  The passphrase / password secrets are never
 * read (see #pn_ssh_build_argv).
 */
void pn_ssh_login_resolve (PnNode      *node,
                           const gchar *ref_prop,
                           PnSshLogin  *out);

/**
 * pn_ssh_login_copy:
 * @dst: (out caller-allocates): an uninitialised snapshot to fill
 * @src: the snapshot to deep-copy
 *
 * Deep-copies @src into @dst.  The SSH nodes call this under their mutex to
 * lift the cached snapshot into a worker-local copy before unlocking.
 */
void pn_ssh_login_copy (PnSshLogin       *dst,
                        const PnSshLogin *src);

/**
 * pn_ssh_login_clear:
 * @login: a snapshot (may be zeroed/ambient)
 *
 * Frees @login's owned strings and zeroes it back to an ambient snapshot.
 * Does not free @login itself.
 */
void pn_ssh_login_clear (PnSshLogin *login);

/**
 * pn_ssh_login_refresh:
 * @node:     a node carrying an "ssh-login" profile-ref property
 * @ref_prop: the name of that property (e.g. "auth-profile")
 * @mutex:    the node mutex guarding @slot
 * @slot:     the node's cached #PnSshLogin (worker-readable)
 *
 * MAIN-THREAD ONLY.  Re-resolves @node's login with #pn_ssh_login_resolve
 * (outside @mutex, since that re-enters the property getter) and then
 * swaps the result into @slot under @mutex, clearing the old snapshot.  The
 * one call each SSH node makes from its property setter and its
 * #PnVault::changed handler to keep the worker-side snapshot current.
 */
void pn_ssh_login_refresh (PnNode      *node,
                           const gchar *ref_prop,
                           GMutex      *mutex,
                           PnSshLogin  *slot);

/**
 * pn_ssh_login_dup_locked:
 * @mutex: the node mutex guarding @slot
 * @slot:  the node's cached #PnSshLogin
 * @out:   (out caller-allocates): an uninitialised snapshot to fill
 *
 * Deep-copies @slot into @out while holding @mutex.  The one call each SSH
 * node makes on its worker thread to lift the cached snapshot into a
 * worker-local copy it can read after unlocking; #pn_ssh_login_clear it
 * when done.
 */
void pn_ssh_login_dup_locked (GMutex           *mutex,
                              const PnSshLogin *slot,
                              PnSshLogin       *out);

/**
 * pn_ssh_auth_profile_param_spec:
 *
 * Builds the standard "auth-profile" #GParamSpec every SSH node installs: a
 * read-write string defaulting to "" and tagged with
 * pn_param_spec_set_profile_ref() for #PN_PROFILE_SSH, so the node-settings
 * dialog renders it as an SSH-Login profile picker.  "" follows the type's
 * primary profile; with no profile configured that resolves to ambient.
 *
 * Returns: (transfer full): a floating #GParamSpec; install it with
 *   g_object_class_install_property() (which sinks the ref).
 */
GParamSpec *pn_ssh_auth_profile_param_spec (void);

/**
 * pn_ssh_build_argv:
 * @host:            the remote host to reach (passed verbatim as the ssh
 *                   destination; the caller has already decided it is NOT
 *                   the local machine — see pn_shell_host_is_local)
 * @login:           (nullable): a resolved #PnSshLogin shaping the login,
 *                   or %NULL / an @has_profile %FALSE snapshot to keep
 *                   today's ambient-key behaviour (no -l/-p/-i, accept-new)
 * @connect_timeout: ConnectTimeout seconds; 0 falls back to 5
 * @base_argv:       (nullable): the NULL-terminated remote command, taken
 *                   verbatim as the ssh tail (the caller owns its quoting)
 *
 * The one shared SSH argv builder (TODO #47.3).  Turns @host plus the
 * resolved @login into the exact `ssh` argv every SSH-using node runs:
 *
 *     ssh -o BatchMode=yes -o StrictHostKeyChecking=<policy>
 *         -o ConnectTimeout=<n> [-l user] [-p port] [-i identity]
 *         <host> <base_argv...>
 *
 * The host-key-policy token maps onto OpenSSH's StrictHostKeyChecking:
 * accept-new → accept-new (today's default, trust on first use),
 * strict → yes (refuse an unknown or changed key), off → no (accept
 * anything).  A %NULL / ambient @login, or one with the field unset, yields
 * accept-new.  Username, port (when not the default 22) and identity-file
 * are only added when @login sets them, so an ambient snapshot reproduces
 * the fixed argv the nodes hard-coded before this type existed.
 *
 * BatchMode=yes is kept unconditionally: it disables every interactive
 * prompt, so the connection only succeeds via a pre-installed key or the
 * agent.  The profile's passphrase / password secrets are therefore not
 * part of #PnSshLogin and are never replayed.  This is the v1 DECISION for
 * item 47.5: auth is ssh-agent / unencrypted key only, password auth is
 * unsupported, and an encrypted key must be unlocked via ssh-add rather
 * than the stored passphrase.  Feeding a stored secret would need a
 * separate non-interactive path (the libssh session weighed in 47.7); the
 * secrets stay in the schema so that path can adopt them later.
 *
 * There is no local-host short-circuit: this always builds a remote ssh
 * argv.  The caller keeps its own local/remote test and only routes the
 * remote branch through here.
 *
 * Returns: (transfer full): a NULL-terminated argv suitable for
 *   g_spawn_sync() with %G_SPAWN_SEARCH_PATH.  An empty @base_argv yields
 *   an empty (argv[0] == NULL) vector.  Free with g_strfreev().
 */
gchar **pn_ssh_build_argv (const gchar        *host,
                           const PnSshLogin   *login,
                           guint               connect_timeout,
                           const gchar *const *base_argv);

G_END_DECLS

#endif /* PN_SSH_PROFILE_H */
