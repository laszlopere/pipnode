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

#ifndef PN_HTTPS_SERVER_H
#define PN_HTTPS_SERVER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnHttpsTunnelReceiver                                                      */
/*                                                                     */
/*  Source-only node that hosts an HTTPS endpoint and emits a          */
/*  #PnMessage for every JSON POST it receives.  Designed as the       */
/*  receiver side of a "pipnode-to-pipnode over HTTPS" link: a peer    */
/*  pipnode (or any HTTP client) POSTs a JSON document with the        */
/*  message envelope (`topic`, `data`, ...) to `/`, and this node      */
/*  rebuilds it as a #PnMessage and emits it on its output port.       */
/*                                                                     */
/*  TLS material:                                                      */
/*    - #PnHttpsTunnelReceiver:cert-path and #PnHttpsTunnelReceiver:key-path point at  */
/*      PEM files supplied by the user when they want a real cert.     */
/*    - When either path is empty the node falls back to a freshly     */
/*      minted, in-memory self-signed certificate (RSA 2048, CN=       */
/*      "pipnode") so the node works out of the box; clients have to   */
/*      pin or skip-verify, but no manual setup is required.           */
/*                                                                     */
/*  Optional HTTP Basic auth: when both #PnHttpsTunnelReceiver:username and    */
/*  #PnHttpsTunnelReceiver:password are non-empty the server installs a basic  */
/*  auth domain covering the entire URI space.  Either field empty    */
/*  leaves the endpoint open.                                          */
/* ------------------------------------------------------------------ */

#define PN_TYPE_HTTPS_TUNNEL_RECEIVER (pn_https_tunnel_receiver_get_type ())

G_DECLARE_FINAL_TYPE (PnHttpsTunnelReceiver, pn_https_tunnel_receiver, PN, HTTPS_SERVER, PnNode)

PnHttpsTunnelReceiver *pn_https_tunnel_receiver_new (void);

G_END_DECLS

#endif /* PN_HTTPS_SERVER_H */
