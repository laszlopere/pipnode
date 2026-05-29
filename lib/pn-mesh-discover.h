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
#include <sys/types.h>   /* pid_t */

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

    /* TRUE when @tty is currently held open by another process (most
     * commonly a Zigbee/serial daemon that grabbed a generic CP2102
     * dongle -- see TODO #33).  Such a device must never be opened or
     * written to; the UI shows it disabled.  @in_use_by is a
     * human-readable holder description ("zigbee2mqtt (pid 1768043)"),
     * NULL when the holder could not be named (e.g. owned by another
     * user, so its /proc is unreadable). */
    gboolean in_use;
    gchar   *in_use_by;
} PnMeshDevice;

void               pn_mesh_device_free (PnMeshDevice *device);
PnMeshDevice      *pn_mesh_device_copy (const PnMeshDevice *device);

/* Is @tty currently held open by a process OTHER than ourselves?  Walks
 * /proc/<pid>/fd looking for a descriptor that resolves to @tty.  When a
 * holder is found and @holder_out is non-NULL, *holder_out is set to a
 * newly-allocated "<comm> (pid N)" string (caller g_free()s); it is left
 * NULL otherwise.  Best-effort: holders owned by another user (whose
 * /proc/<pid>/fd we cannot read) are invisible, so a FALSE return means
 * "no detectable holder", not a hard guarantee -- the exclusive-open
 * (TIOCEXCL) in pn-mesh-serial.c is the second line of defence.  Safe to
 * call from a worker thread; pure /proc reads, never touches @tty itself. */
gboolean pn_mesh_tty_in_use (const gchar *tty, gchar **holder_out);

/* Lower-level helper behind pn_mesh_tty_in_use(): does process @pid hold
 * any open descriptor that resolves to @path?  Exposed for unit testing
 * (the public wrapper above skips our own pid, which makes self-checks
 * impossible to assert).  Returns FALSE on any access error. */
gboolean pn_mesh_path_held_by_pid (pid_t pid, const gchar *path);

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
