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

#ifndef PN_HTTPS_CLIENT_H
#define PN_HTTPS_CLIENT_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnHttpsTunnelSender                                                      */
/*                                                                     */
/*  Sink-only node that POSTs every #PnMessage arriving on its input  */
/*  port to a configured HTTPS URL as a JSON envelope.  Designed as   */
/*  the sender side of a "pipnode-to-pipnode over HTTPS" link: wire a */
/*  source into this node, and any peer pipnode running a             */
/*  #PnHttpsTunnelReceiver at the configured URL receives the same message    */
/*  on its output port.                                                */
/*                                                                     */
/*  Configuration:                                                     */
/*    - #PnHttpsTunnelSender:url        — full target URL (e.g.              */
/*      "https://peer.example:8443/")                                  */
/*    - #PnHttpsTunnelSender:username,                                       */
/*      #PnHttpsTunnelSender:password   — optional HTTP Basic credentials   */
/*      (sent only when both are non-empty)                            */
/*    - #PnHttpsTunnelSender:verify-tls — when %FALSE (the default) accept   */
/*      self-signed certificates so the node pairs with the server's  */
/*      out-of-the-box self-signed cert without manual setup; flip to */
/*      %TRUE when targeting a real cert.                              */
/* ------------------------------------------------------------------ */

#define PN_TYPE_HTTPS_TUNNEL_SENDER (pn_https_tunnel_sender_get_type ())

G_DECLARE_FINAL_TYPE (PnHttpsTunnelSender, pn_https_tunnel_sender, PN, HTTPS_CLIENT, PnNode)

PnHttpsTunnelSender *pn_https_tunnel_sender_new (void);

G_END_DECLS

#endif /* PN_HTTPS_CLIENT_H */
