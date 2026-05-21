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

#include <gdk/gdk.h>

#include "pn-node.h"

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
    GdkRGBA normal_color;

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

G_END_DECLS

#endif /* PN_WS_H */
