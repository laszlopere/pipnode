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

#ifndef PN_MESH_DISCOVER_H
#define PN_MESH_DISCOVER_H

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* One discovered Meshtastic USB device.
 *
 * @kind is the human-friendly product family the VID:PID matched against
 * pip-mesh's table (e.g. "Heltec V3", "Tracker", "SenseCAP Tracker"); the
 * UI shows it as the device row's primary line.  @product / @manufacturer
 * / @serial are read straight off sysfs and may be NULL when the device
 * does not expose them (counterfeit / minimal-descriptor boards).  @tty
 * is the absolute /dev path; rows without a usable tty are dropped at
 * scan time so a row in the result list is always actionable. */
typedef struct
{
    gchar *kind;          /* "Heltec V3" etc. -- borrowed-style: g_free  */
    gchar *vendor_id;     /* 4-hex-char USB VID, e.g. "10c4"             */
    gchar *product_id;    /* 4-hex-char USB PID, e.g. "ea60"             */
    gchar *manufacturer;  /* sysfs `manufacturer`, may be NULL           */
    gchar *product;       /* sysfs `product` (USB-string), may be NULL   */
    gchar *serial;        /* sysfs `serial`, may be NULL                 */
    gchar *tty;           /* absolute /dev/ttyXXX path                   */
} PnMeshDevice;

void               pn_mesh_device_free (PnMeshDevice *device);
PnMeshDevice      *pn_mesh_device_copy (const PnMeshDevice *device);

/* Synchronous USB scan.  Walks /sys/bus/usb/devices/ matching against
 * the same VID:PID table pip-mesh uses; resolves each match's tty via
 * the same two-deep `tty/tty*` search.  Returns a GPtrArray of
 * PnMeshDevice* with pn_mesh_device_free as the element-free; empty
 * (length 0) when no compatible device is plugged in, never NULL.
 *
 * Pure sysfs reads, no /dev access -- safe to call without the device
 * being ready and without paying any handshake cost.  Designed to be
 * called from a worker thread (typically wrapped in pn_mesh_discover_async). */
GPtrArray *pn_mesh_discover_sync (void);

/* GTask-wrapped variant.  Runs pn_mesh_discover_sync() on a worker
 * thread and invokes @callback on the thread that scheduled the task
 * (typically the main loop).  @cancellable lets a dialog tear the scan
 * down if the user closes the window mid-walk.  Use
 * pn_mesh_discover_finish() in @callback to collect the result. */
void       pn_mesh_discover_async  (GCancellable        *cancellable,
                                    GAsyncReadyCallback  callback,
                                    gpointer             user_data);

/* Companion to pn_mesh_discover_async().  Returns the same GPtrArray
 * shape as pn_mesh_discover_sync(); NULL on cancellation (with @error
 * set to G_IO_ERROR_CANCELLED).  Caller owns the returned array. */
GPtrArray *pn_mesh_discover_finish (GAsyncResult        *result,
                                    GError             **error);

G_END_DECLS

#endif /* PN_MESH_DISCOVER_H */
