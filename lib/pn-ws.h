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

#ifndef PN_WS_H
#define PN_WS_H

#include "pn-node.h"
#include "pn-vault.h"   /* for PnProfile */

#include <libsoup/soup.h>   /* for SoupMessage, GTlsCertificate{,Flags} */

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnWebsocket                                                        */
/*                                                                     */
/*  Derivable base class for source nodes whose data feed arrives over */
/*  a long-lived WebSocket connection (ws:// or wss://).  The class    */
/*  owns the SoupSession + SoupWebsocketConnection, drives an          */
/*  automatic (re)connect loop, and dispatches the inbound text frames */
/*  to subclasses on the main thread.                                  */
/*                                                                     */
/*  Subclasses customise behaviour by overriding the class virtuals    */
/*  and class fields in their `class_init`.  Two virtuals are central: */
/*                                                                     */
/*    PnWebsocketClass.connected     — invoked once per successful     */
/*                                     handshake; subclasses typically */
/*                                     send their subscription RPC     */
/*                                     here via pn_ws_send_text().     */
/*    PnWebsocketClass.text_message  — invoked for every inbound text  */
/*                                     frame; subclasses parse it and  */
/*                                     emit a #PnMessage if it is a    */
/*                                     notification they care about.   */
/*                                                                     */
/*  The node has a "configuration required" state: while               */
/*  PnWebsocketClass.is_configured returns %FALSE the body flips to    */
/*  red ❗ and no connection is attempted.  Setting #PnWebsocket:url   */
/*  to a non-empty value flips it back and starts the connect loop.    */
/*                                                                     */
/*  Re-connect strategy: after a transport error or a "closed" signal  */
/*  the class waits a short backoff (capped at a minute) and retries,  */
/*  for as long as the node is still configured.                       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_WEBSOCKET (pn_websocket_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnWebsocket, pn_websocket,
                          PN, WEBSOCKET, PnNode)

struct _PnWebsocketClass
{
    PnNodeClass parent_class;

    /**
     * PnWebsocketClass.normal_icon:
     *
     * Glyph shown on the node body while it is configured.  %NULL
     * falls back to the base class's plug glyph.  The string is
     * owned by the class and must outlive every instance.
     */
    const gchar *normal_icon;

    /**
     * PnWebsocketClass.normal_color:
     *
     * Body colour used while the node is configured.  Subclasses pick
     * a colour distinctive from the base WebSocket teal.  Leaving this
     * zero-initialised falls back to the base class default.
     */
    PnColor normal_color;

    /**
     * PnWebsocketClass::is_configured:
     * @self: the node instance
     *
     * Return %TRUE when @self has enough configuration to start a
     * connection.  Called on the main thread before each connect
     * attempt; while it returns %FALSE the body is rendered in its
     * warning state and no socket is opened.
     *
     * The default implementation reports configured iff the inherited
     * #PnWebsocket:url is non-empty.
     */
    gboolean (*is_configured) (PnWebsocket *self);

    /**
     * PnWebsocketClass::connected:
     * @self: the node instance
     *
     * Called once per successful WebSocket handshake on the main
     * thread.  Subclasses typically use this to publish a
     * subscription request via pn_ws_send_text().  The default
     * implementation is a no-op.
     */
    void (*connected) (PnWebsocket *self);

    /**
     * PnWebsocketClass::text_message:
     * @self: the node instance
     * @text: NUL-terminated text payload of the inbound frame
     *
     * Called on the main thread for every inbound WebSocket text
     * frame.  Subclasses parse the payload (typically JSON) and emit
     * a #PnMessage when it is a notification they care about.  The
     * default implementation is a no-op.
     */
    void (*text_message) (PnWebsocket *self, const gchar *text);

    /**
     * PnWebsocketClass::profile_resolved:
     * @self: the node instance
     * @profile: (nullable): the resolved #PnProfile, or %NULL when the
     *           referenced profile does not exist and the type has no
     *           primary
     *
     * Invoked on the main thread whenever the credentials profile bound
     * via pn_ws_bind_profile() resolves to a (possibly different, possibly
     * %NULL) value: at initial load, when the bound property is set, and
     * when the vault emits ::changed.  The default implementation reads
     * the profile's "url" field (or "" when @profile is %NULL) and assigns
     * it to #PnWebsocket:url, which drives the existing connect /
     * reconnect loop.  Subclasses override to additionally stash secrets
     * (via pn_profile_get_secret()) or to compose the URL from multiple
     * fields, calling g_object_set (self, "url", composed, NULL) themselves.
     * Always main-thread.
     */
    void (*profile_resolved) (PnWebsocket *self, PnProfile *profile);

    /**
     * PnWebsocketClass::prepare_message:
     * @self: the node instance
     * @msg:  the #SoupMessage about to be handed to libsoup's
     *        websocket connect, freshly created and not yet sent
     *
     * Invoked on the main thread between soup_message_new() and
     * soup_session_websocket_connect_async() for every connect attempt.
     * Subclasses use this to influence the upgrade request before it
     * goes out: connect to #SoupMessage::accept-certificate (e.g. to
     * honour a profile's tls_insecure permission on wss:// against a
     * self-signed peer), replace request headers (e.g. set
     * Authorization for HTTP Basic auth on the upgrade), or attach
     * cookies.  Most general of the two TLS / header seams — closes
     * both the self-signed and the upgrade-auth gaps in one place.
     *
     * The default implementation is a no-op.  Always main-thread.
     */
    void (*prepare_message) (PnWebsocket *self, SoupMessage *msg);

    /**
     * PnWebsocketClass::accept_certificate:
     * @self:   the node instance
     * @cert:   the peer's TLS certificate
     * @errors: the validation errors against the system trust store
     *
     * Narrow seam mirroring #SoupMessage::accept-certificate.  The base
     * class connects to that signal on every connect attempt and
     * forwards to this vfunc — return %TRUE to accept the certificate
     * despite @errors, %FALSE to reject (the default returns %FALSE,
     * deferring to the system trust store).
     *
     * #PnWebsocketClass::prepare_message is the more general entry
     * point and can both hook the signal itself and tweak headers;
     * override this narrow form when only the TLS-trust decision needs
     * customising.  Either or both may be overridden — when both are
     * set the base class's hook (calling this vfunc) and any extra
     * accept-certificate handler the subclass connects from
     * prepare_message all run; a %TRUE from any one accepts the cert.
     *
     * Always main-thread.
     */
    gboolean (*accept_certificate) (PnWebsocket          *self,
                                    GTlsCertificate      *cert,
                                    GTlsCertificateFlags  errors);

    /**
     * PnWebsocketClass::connection_failed:
     * @self:   the node instance
     * @reason: a non-NULL, human-readable description of what failed
     *          (libsoup's #GError message, or a synthetic line like
     *          "invalid URL" when no #GError exists)
     *
     * Invoked on the main thread whenever an attempted connection or a
     * live connection fails: malformed #PnWebsocket:url, handshake
     * error (DNS, refused, TLS, 4xx upgrade), or a runtime
     * protocol/transport error on an established link.  This is the
     * counterpart to #PnWebsocketClass::connected.
     *
     * Subclasses override this to emit a #PnMessage with
     * `success = FALSE` and the reason on the `output` member so the
     * failure becomes visible on the canvas (see PLUGINS §12 — plugins
     * must not rely on stdout/stderr to communicate errors to the user).
     * The base class also logs the same reason via g_warning() for
     * developer diagnostics when pipnode is run from a terminal; that
     * is complementary, not a substitute.
     *
     * The base class continues to drive its own automatic backoff +
     * reconnect, so overrides do NOT need to schedule retries.  The
     * default implementation is a no-op.
     *
     * Always main-thread.
     */
    void (*connection_failed) (PnWebsocket *self, const gchar *reason);
};

PnWebsocket *pn_websocket_new (void);

/* ------------------------------------------------------------------ */
/*  Subclass helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * pn_ws_dup_url:
 * @self: a websocket node
 *
 * Return the current value of #PnWebsocket:url as a freshly-allocated
 * copy.  The caller frees with g_free().  May return %NULL when no
 * URL has been set.
 */
gchar *pn_ws_dup_url (PnWebsocket *self);

/**
 * pn_ws_apply_visual_state:
 * @self:       a websocket node
 * @configured: whether the node currently has the configuration it
 *              needs to attempt a connection
 *
 * Re-paint the node body for the given configuration state, using
 * the subclass's class-level normal_icon/normal_color when
 * @configured is %TRUE and the shared warning identity otherwise.
 * Subclasses with multi-field configuration call this whenever any
 * of their inputs change.  Main-thread only.
 */
void pn_ws_apply_visual_state (PnWebsocket *self, gboolean configured);

/**
 * pn_ws_send_text:
 * @self: a websocket node
 * @text: NUL-terminated UTF-8 payload
 *
 * Send @text on the live connection as a single WebSocket text
 * frame.  Silently no-ops while disconnected, so subclasses can call
 * it from any context without first checking the link state.  Main-
 * thread only.
 */
void pn_ws_send_text (PnWebsocket *self, const gchar *text);

/**
 * pn_ws_is_connected:
 * @self: a websocket node
 *
 * Returns %TRUE while the underlying connection has finished its
 * handshake and is open in either direction.  Main-thread only.
 */
gboolean pn_ws_is_connected (PnWebsocket *self);

/**
 * pn_ws_bind_profile:
 * @self:      a websocket node
 * @prop_name: the name of a string property on @self tagged via
 *             pn_param_spec_set_profile_ref() — typically declared by
 *             the subclass itself
 *
 * Wire @self up so its connection target is driven by a host-provisioned
 * credentials profile instead of (or in addition to) the inherited
 * #PnWebsocket:url.  After this call the base class:
 *
 *   - watches @self's `notify::@prop_name`, so changing the profile-ref
 *     re-resolves and reconnects;
 *   - watches the default #PnVault's ::changed, so an edit in the
 *     credentials manager takes effect live;
 *   - schedules one main-loop idle to perform the initial resolve once
 *     property loading has settled (works the same whether the property
 *     is the default "" — which follows the type's primary profile — or
 *     a deserialised id).
 *
 * Every resolve invokes #PnWebsocketClass::profile_resolved.  The default
 * implementation of that vfunc reads the profile's "url" field and pushes
 * it to #PnWebsocket:url, so a subclass whose schema names the endpoint
 * "url" needs no resolver code at all; subclasses that also need a
 * secret, or whose schema spreads the endpoint across multiple fields,
 * override the vfunc.
 *
 * Call once, in #_init or #constructed.  Calling twice replaces the
 * binding.  Main-thread only.
 */
void pn_ws_bind_profile (PnWebsocket *self, const gchar *prop_name);

G_END_DECLS

#endif /* PN_WS_H */
