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

/* ------------------------------------------------------------------ */
/*  PnMeshDiscover — sysfs walk for Meshtastic USB devices.            */
/*                                                                     */
/*  Direct C port of pip-mesh's detect_devices() + find_tty_device():  */
/*    /sys/bus/usb/devices/<port>/ -> skip if no `product` file        */
/*    read idVendor + idProduct, match against the table below         */
/*    for each interface <port>:<conf>.<iface>/, try tty/tty* and one  */
/*    level deeper (some drivers nest the tty subsystem) for the tty   */
/*                                                                     */
/*  Pure sysfs reads (idVendor/idProduct/product/manufacturer/serial); */
/*  never touches /dev, so a scan cannot disturb a device, hang on a   */
/*  stuck port, or wake one that is asleep.  No /dev/<tty> open, no    */
/*  serial handshake, no cache.  The expensive part (the                */
/*  want_config_id handshake) happens later, only after the user picks  */
/*  a row.                                                              */
/*                                                                     */
/*  VID:PID table is kept in lock-step with /usr/bin/pip-mesh.  Adding */
/*  a board means appending one row here and one to pip-mesh; nothing  */
/*  else.                                                              */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-mesh-discover.h"

/* ------------------------------------------------------------------ */
/*  PnMeshDevice                                                       */
/* ------------------------------------------------------------------ */

void
pn_mesh_device_free (PnMeshDevice *device)
{
    if (device == NULL)
        return;

    g_free (device->kind);
    g_free (device->vendor_id);
    g_free (device->product_id);
    g_free (device->manufacturer);
    g_free (device->product);
    g_free (device->serial);
    g_free (device->tty);
    g_slice_free (PnMeshDevice, device);
}

PnMeshDevice *
pn_mesh_device_copy (const PnMeshDevice *src)
{
    PnMeshDevice *out;

    if (src == NULL)
        return NULL;

    out = g_slice_new0 (PnMeshDevice);
    out->kind         = g_strdup (src->kind);
    out->vendor_id    = g_strdup (src->vendor_id);
    out->product_id   = g_strdup (src->product_id);
    out->manufacturer = g_strdup (src->manufacturer);
    out->product      = g_strdup (src->product);
    out->serial       = g_strdup (src->serial);
    out->tty          = g_strdup (src->tty);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Known-device table                                                  */
/* ------------------------------------------------------------------ */

/* Keep in lock-step with /usr/bin/pip-mesh's MESH_DEVICES array.  Both
 * files match by `vendor:product` against the hex strings sysfs prints
 * (lowercase, 4 chars each, no `0x` prefix). */
typedef struct
{
    const char *vendor_id;
    const char *product_id;
    const char *kind;
} KnownDevice;

static const KnownDevice KNOWN_DEVICES[] = {
    { "10c4", "ea60", "Heltec V3"        },
    { "239a", "0029", "Tracker"          },
    { "239a", "8027", "SenseCAP Tracker" },
    { "239a", "8029", "Tracker"          },
};

static const char *
match_known_device (const char *vendor_id, const char *product_id)
{
    gsize i;

    if (vendor_id == NULL || product_id == NULL)
        return NULL;

    for (i = 0; i < G_N_ELEMENTS (KNOWN_DEVICES); i++)
        if (g_ascii_strcasecmp (vendor_id,  KNOWN_DEVICES[i].vendor_id)  == 0
         && g_ascii_strcasecmp (product_id, KNOWN_DEVICES[i].product_id) == 0)
            return KNOWN_DEVICES[i].kind;

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  sysfs reads                                                         */
/* ------------------------------------------------------------------ */

/* Read a sysfs attribute as a single trimmed line, returning NULL if
 * the file is absent or empty.  pip-mesh's read_sysfs_attr does the
 * same (`echo $value` collapses whitespace; we strip both ends). */
static gchar *
read_sysfs_attr (const gchar *base, const gchar *name)
{
    gchar *path;
    gchar *raw = NULL;
    gchar *out;

    path = g_build_filename (base, name, NULL);
    if (!g_file_get_contents (path, &raw, NULL, NULL))
    {
        g_free (path);
        return NULL;
    }
    g_free (path);

    out = g_strstrip (raw);  /* in place, points into raw's buffer */
    if (*out == '\0')
    {
        g_free (raw);
        return NULL;
    }
    /* g_strstrip returns a pointer inside the original buffer, so the
     * caller-visible copy must duplicate before freeing the source. */
    {
        gchar *dup = g_strdup (out);
        g_free (raw);
        return dup;
    }
}

/* ------------------------------------------------------------------ */
/*  tty resolution                                                      */
/* ------------------------------------------------------------------ */

/* Look for a "tty*" entry inside @dir; first match wins.  Returns an
 * absolute /dev/<name> path, or NULL when no tty is found. */
static gchar *
pick_tty_in_dir (const gchar *dir)
{
    GDir       *d;
    const gchar *name;
    gchar      *out = NULL;

    d = g_dir_open (dir, 0, NULL);
    if (d == NULL)
        return NULL;

    while ((name = g_dir_read_name (d)) != NULL)
    {
        if (g_str_has_prefix (name, "tty"))
        {
            out = g_build_filename ("/dev", name, NULL);
            break;
        }
    }
    g_dir_close (d);
    return out;
}

/* pip-mesh's find_tty_device strategy:
 *   for each interface (subdir named "<busnum>:*") of the USB device:
 *     look for <iface>/tty/tty*           (cdc_acm style)
 *     else  <iface>/<anything>/tty/tty*   (driver-nested style)
 *
 * We follow the same two-deep search.  Returns the /dev/ttyXXX path or
 * NULL if no tty was found under any interface. */
static gchar *
find_tty_device (const gchar *usb_path)
{
    GDir        *iface_dir;
    const gchar *iface_name;
    gchar       *busnum;
    gchar       *iface_prefix;
    gchar       *out = NULL;

    busnum = g_path_get_basename (usb_path);
    iface_prefix = g_strconcat (busnum, ":", NULL);
    g_free (busnum);

    iface_dir = g_dir_open (usb_path, 0, NULL);
    if (iface_dir == NULL)
    {
        g_free (iface_prefix);
        return NULL;
    }

    while ((iface_name = g_dir_read_name (iface_dir)) != NULL)
    {
        gchar *iface_path;
        gchar *tty_dir;

        if (!g_str_has_prefix (iface_name, iface_prefix))
            continue;

        iface_path = g_build_filename (usb_path, iface_name, NULL);

        /* Direct: <iface>/tty/tty* */
        tty_dir = g_build_filename (iface_path, "tty", NULL);
        out = pick_tty_in_dir (tty_dir);
        g_free (tty_dir);

        /* Nested: <iface>/<subdir>/tty/tty* */
        if (out == NULL)
        {
            GDir        *sub;
            const gchar *sub_name;

            sub = g_dir_open (iface_path, 0, NULL);
            if (sub != NULL)
            {
                while ((sub_name = g_dir_read_name (sub)) != NULL)
                {
                    gchar *nested = g_build_filename (iface_path, sub_name,
                                                      "tty", NULL);
                    out = pick_tty_in_dir (nested);
                    g_free (nested);
                    if (out != NULL)
                        break;
                }
                g_dir_close (sub);
            }
        }

        g_free (iface_path);
        if (out != NULL)
            break;
    }

    g_dir_close (iface_dir);
    g_free (iface_prefix);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Synchronous scan                                                    */
/* ------------------------------------------------------------------ */

#define USB_DEVICES_PATH "/sys/bus/usb/devices"

GPtrArray *
pn_mesh_discover_sync (void)
{
    GPtrArray   *out;
    GDir        *bus;
    const gchar *entry;

    out = g_ptr_array_new_with_free_func ((GDestroyNotify) pn_mesh_device_free);

    bus = g_dir_open (USB_DEVICES_PATH, 0, NULL);
    if (bus == NULL)
        return out;   /* no USB subsystem visible — return empty, not NULL */

    while ((entry = g_dir_read_name (bus)) != NULL)
    {
        gchar        *usb_path;
        gchar        *vendor_id;
        gchar        *product_id;
        const gchar  *kind;
        PnMeshDevice *device;

        usb_path = g_build_filename (USB_DEVICES_PATH, entry, NULL);

        /* pip-mesh gates on the presence of `product` -- skipping
         * hubs / non-functional-device entries cheaply.  We mirror
         * that. */
        {
            gchar *probe = g_build_filename (usb_path, "product", NULL);
            gboolean ok  = g_file_test (probe, G_FILE_TEST_IS_REGULAR);
            g_free (probe);
            if (!ok)
            {
                g_free (usb_path);
                continue;
            }
        }

        vendor_id  = read_sysfs_attr (usb_path, "idVendor");
        product_id = read_sysfs_attr (usb_path, "idProduct");

        kind = match_known_device (vendor_id, product_id);
        if (kind == NULL)
        {
            g_free (vendor_id);
            g_free (product_id);
            g_free (usb_path);
            continue;
        }

        device = g_slice_new0 (PnMeshDevice);
        device->kind         = g_strdup (kind);
        device->vendor_id    = vendor_id;     /* now owned */
        device->product_id   = product_id;    /* now owned */
        device->manufacturer = read_sysfs_attr (usb_path, "manufacturer");
        device->product      = read_sysfs_attr (usb_path, "product");
        device->serial       = read_sysfs_attr (usb_path, "serial");
        device->tty          = find_tty_device (usb_path);

        g_free (usb_path);

        /* A device with no tty is not actionable from here -- drop it
         * silently rather than confronting the user with an
         * "address-less" row. */
        if (device->tty == NULL)
        {
            pn_mesh_device_free (device);
            continue;
        }

        g_ptr_array_add (out, device);
    }
    g_dir_close (bus);

    return out;
}

/* ------------------------------------------------------------------ */
/*  Async wrapper                                                       */
/* ------------------------------------------------------------------ */

static void
discover_thread_func (GTask        *task,
                      gpointer      source_object,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
    GPtrArray *result;

    (void) source_object;
    (void) task_data;

    if (g_task_return_error_if_cancelled (task))
        return;

    result = pn_mesh_discover_sync ();

    /* Cancellation is checked one more time before handing the result
     * across thread boundaries -- the caller may have already torn down
     * the dialog while we were walking sysfs. */
    if (cancellable != NULL && g_cancellable_is_cancelled (cancellable))
    {
        g_ptr_array_unref (result);
        g_task_return_error_if_cancelled (task);
        return;
    }

    g_task_return_pointer (task, result, (GDestroyNotify) g_ptr_array_unref);
}

void
pn_mesh_discover_async (GCancellable        *cancellable,
                        GAsyncReadyCallback  callback,
                        gpointer             user_data)
{
    GTask *task;

    task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_set_source_tag (task, pn_mesh_discover_async);
    g_task_run_in_thread (task, discover_thread_func);
    g_object_unref (task);
}

GPtrArray *
pn_mesh_discover_finish (GAsyncResult *result, GError **error)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
    return g_task_propagate_pointer (G_TASK (result), error);
}
