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

#ifndef PN_MESHTASTIC_H
#define PN_MESHTASTIC_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnMeshtastic                                                       */
/*                                                                     */
/*  Bidirectional bridge between the pipnode message graph and a       */
/*  Meshtastic LoRa device attached over USB.  Speaks the device's     */
/*  framed-protobuf serial protocol natively in C — no helper          */
/*  subprocess — so the connection can be held open for the whole      */
/*  lifetime of the node and incoming text packets land on the         */
/*  output port the moment they arrive.                                */
/*                                                                     */
/*  Each instance owns one worker thread that holds the serial fd,     */
/*  runs the configuration handshake on startup, and then alternates   */
/*  between reading FromRadio frames (forwarded to the main loop as    */
/*  outgoing #PnMessage values with topic "meshtastic") and writing    */
/*  ToRadio frames built from messages that arrived on the input       */
/*  port (data.output is sent verbatim as a TEXT_MESSAGE_APP packet).  */
/*                                                                     */
/*  The :device property is a serial-port path (e.g. /dev/ttyUSB0)     */
/*  and :channel is the case-sensitive name of the channel to send    */
/*  on (the primary channel "LongFast" by default — empty resolves to  */
/*  channel index 0).                                                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MESHTASTIC (pn_meshtastic_get_type ())

G_DECLARE_FINAL_TYPE (PnMeshtastic, pn_meshtastic, PN, MESHTASTIC, PnNode)

PnMeshtastic *pn_meshtastic_new (void);

/**
 * pn_meshtastic_list_devices:
 *
 * Walks the host's USB bus and returns a NULL-terminated array of
 * serial-port paths (e.g. <literal>/dev/ttyACM0</literal>) belonging
 * to attached Meshtastic-compatible devices.  Matches the same
 * vendor/product table the <literal>pip-mesh</literal> bash prototype
 * keeps (Heltec V3, several Tracker variants).  The returned array
 * is sorted alphabetically and must be freed with g_strfreev().
 * Used by the settings dialog to populate the device combobox.
 */
gchar **pn_meshtastic_list_devices (void);

/**
 * pn_meshtastic_list_channels:
 * @device_path: a serial-port path returned by
 *               pn_meshtastic_list_devices(), or any other
 *               <literal>/dev/tty*</literal> path
 *
 * Briefly opens @device_path, runs the Meshtastic configuration
 * handshake, collects the channel names the device advertises, and
 * closes the port again.  Returns a NULL-terminated array of channel
 * name strings (sorted by channel index, primary channel first), or
 * %NULL if the device cannot be opened or does not respond within a
 * few seconds.  The result must be freed with g_strfreev().
 *
 * Intended for the settings dialog's channel combobox.  Calling this
 * while another #PnMeshtastic instance is already holding the same
 * device open will likely interleave traffic and fail; the dialog
 * works around that by reading the worker's cached channel list when
 * available — see pn_meshtastic_get_channel_cache().
 */
gchar **pn_meshtastic_list_channels (const gchar *device_path);

/**
 * pn_meshtastic_get_channel_cache:
 * @self: the node
 *
 * Returns the channel-name list captured by the node's worker during
 * its configuration handshake, or %NULL if the worker has not run a
 * successful handshake yet.  The dialog prefers this over
 * pn_meshtastic_list_channels() when the user is editing a node that
 * is already attached to a device, so the channel combo can be
 * populated without re-opening the (already-busy) port.
 *
 * The returned strv is owned by the node and remains valid until the
 * worker disconnects or the node is finalised; copy with g_strdupv()
 * if you need to outlive that.
 */
const gchar * const *pn_meshtastic_get_channel_cache (PnMeshtastic *self);

G_END_DECLS

#endif /* PN_MESHTASTIC_H */
