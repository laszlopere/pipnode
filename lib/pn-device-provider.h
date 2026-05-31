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

#ifndef PN_DEVICE_PROVIDER_H
#define PN_DEVICE_PROVIDER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* The Devices-menu provider registry.
 *
 * The third piece of the device-dialog toolkit (alongside the form
 * helpers in pn-device-form.h and the dialog shell in
 * pn-device-dialog.h).  Where those two help a plugin BUILD a device
 * dialog, this lets it ADVERTISE one: each device kind the editor can
 * configure registers a provider, and the host's "Devices" menu is
 * built by enumerating the registry rather than hardcoding entries.
 *
 * A built-in node installs its provider from
 * pn_gui_install_builtin_nodes(); an out-of-tree plugin does so from
 * its GUI companion's pn_plugin_gui_init().  Either way the menu picks
 * the entry up with no change to the host.
 *
 * The registry is process-global and GTK-thread affine (the menu and
 * every present callback run on the main thread); it is not
 * thread-safe.  Providers live for the process lifetime -- there is no
 * unregister, matching the resident lifetime of the GUI modules that
 * register them. */

/* Called to open (or raise) the configuration dialog for a provider.
 * @parent is the editor window the menu was activated from (may be
 * NULL); @user_data is the pointer passed to _register(). */
typedef void (*PnDeviceProviderPresentFunc) (GtkWindow *parent,
                                             gpointer   user_data);

/* Register a device provider.
 *
 * @id            stable machine identifier, e.g. "meshtastic"; used to
 *                dedup and to address pn_device_provider_present().
 * @display_name  human label for the menu entry, e.g. "Meshtastic".
 * @icon_name     icon for the entry, or NULL for none.  A themed icon
 *                name (e.g. "network-wireless") is looked up in the icon
 *                theme; an ABSOLUTE path (e.g. "/usr/share/.../foo.png")
 *                is loaded from that file and scaled to the menu icon
 *                size.  A plugin shipping its own icon should pass an
 *                absolute path to its installed file.
 * @present       callback that opens the provider's dialog (required).
 * @user_data     opaque pointer handed back to @present.
 * @destroy       optional notifier run on @user_data if a later
 *                registration with the same @id replaces this one.
 *
 * Registering an @id that already exists REPLACES the previous entry
 * in place (its @destroy, if any, runs); the menu reflects whichever
 * registration was last.  This keeps a reloaded companion idempotent. */
void pn_device_provider_register (const gchar                 *id,
                                  const gchar                 *display_name,
                                  const gchar                 *icon_name,
                                  PnDeviceProviderPresentFunc  present,
                                  gpointer                     user_data,
                                  GDestroyNotify               destroy);

/* A snapshot of one registered provider, as returned by _list(). */
typedef struct
{
    gchar *id;
    gchar *display_name;
    gchar *icon_name;     /* may be NULL */
} PnDeviceProviderInfo;

/* Free a #PnDeviceProviderInfo returned by pn_device_provider_list(). */
void pn_device_provider_info_free (PnDeviceProviderInfo *info);

/* Enumerate the registry in registration order.  Returns a newly
 * allocated #GList of owned #PnDeviceProviderInfo* (deep copies, so the
 * caller may outlive the registry); free with
 * g_list_free_full (list, (GDestroyNotify) pn_device_provider_info_free).
 * Returns NULL when nothing is registered. */
GList *pn_device_provider_list (void);

/* Present the dialog for provider @id, parented to @parent.  Returns
 * TRUE if a provider with that id was found and its callback invoked,
 * FALSE for an unknown id. */
gboolean pn_device_provider_present (const gchar *id,
                                     GtkWindow   *parent);

G_END_DECLS

#endif /* PN_DEVICE_PROVIDER_H */
