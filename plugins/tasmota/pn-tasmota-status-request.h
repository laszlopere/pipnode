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

#ifndef PN_TASMOTA_STATUS_REQUEST_H
#define PN_TASMOTA_STATUS_REQUEST_H

#include "pn-node.h"
#include "pn-tasmota-common.h"   /* PnTasmotaStatusKind + its GType */

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTasmotaStatusRequest                                             */
/*                                                                     */
/*  Read-side companion to #PnTasmotaRelayCommand.  Treats every       */
/*  inbound message as a bare trigger (the value is irrelevant) and    */
/*  emits the Tasmota `Status` query a downstream #PnMqttSink can       */
/*  publish to make a device report its configuration / info.  The     */
/*  envelope topic becomes `cmnd/<device>/Status` and `data.payload`   */
/*  the numeric section selected by the `request` property, so a bare   */
/*  `PnInject -> PnTasmotaStatusRequest -> PnMqttSink` chain asks a     */
/*  Tasmota device (or every device at once) for its status without an  */
/*  intervening Format / Rewrite node.  The reply arrives back through  */
/*  a #PnMqttSource subscribed to `stat/<device>/STATUS#`.             */
/*                                                                     */
/*  Unlike the write-side relay command this is a strictly read-only    */
/*  query, so there is no unsafe default to guard against: the         */
/*  `device` property defaults to Tasmota's built-in `tasmotas` group   */
/*  topic (every device that hears the broker) and an empty field is    */
/*  treated the same way -- the node always has a safe thing to do and  */
/*  never enters the red "needs configuration" state.                   */
/* ------------------------------------------------------------------ */

/* The Tasmota `Status <n>` section enum #PnTasmotaStatusKind and its
 * registered GType #PN_TYPE_TASMOTA_STATUS_KIND now live in
 * pn-tasmota-common.h, shared with #PnTasmotaProbe. */

#define PN_TYPE_TASMOTA_STATUS_REQUEST (pn_tasmota_status_request_get_type ())

G_DECLARE_FINAL_TYPE (PnTasmotaStatusRequest, pn_tasmota_status_request,
                      PN, TASMOTA_STATUS_REQUEST, PnNode)

PnTasmotaStatusRequest *pn_tasmota_status_request_new (void);

G_END_DECLS

#endif /* PN_TASMOTA_STATUS_REQUEST_H */
