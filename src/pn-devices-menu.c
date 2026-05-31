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
/*  Devices menu — provider-driven (TODO #34 Phase D).                 */
/*                                                                     */
/*  One entry per registered #PnDeviceProvider, in registration order. */
/*  Built-in device kinds register from pn_gui_install_builtin_nodes() */
/*  and plugins from their GUI companions, so this file never names a  */
/*  particular device — it just enumerates the registry and routes     */
/*  each activation back through pn_device_provider_present().  That    */
/*  keeps the switch to plugin-supplied entries out of pn-window.c.    */
/* ------------------------------------------------------------------ */

#include "pn-devices-menu.h"

#include "pn-device-provider.h"

/* GTK 3 deprecates GtkImageMenuItem but the editor's menubar uses it
 * throughout for icon-bearing items (see create_image_menu_item in
 * pn-window.c) so the columns line up.  Match that pattern here. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

typedef struct
{
    GtkWindow *parent;   /* the editor window; not owned */
    gchar     *id;       /* provider id to present */
} DevicesMenuItem;

static void
devices_menu_item_free (gpointer data, GClosure *closure)
{
    DevicesMenuItem *item = data;

    (void) closure;
    g_free (item->id);
    g_slice_free (DevicesMenuItem, item);
}

/* Open (or raise) the configuration dialog for this entry's provider. */
static void
action_present_provider (GtkMenuItem *menu_item, gpointer user_data)
{
    DevicesMenuItem *item = user_data;

    (void) menu_item;
    pn_device_provider_present (item->id, item->parent);
}

/* Turn a provider's icon spec into a menu-sized #GtkImage.  An absolute
 * path is loaded from that file (scaled down to the menu icon size so a
 * large source image does not inflate the row); any other string is taken
 * as a themed icon name.  An absolute path can never be a valid themed
 * name, so the two namespaces never collide.  Returns NULL only when a
 * file path fails to load, in which case the entry shows no icon. */
static GtkWidget *
device_icon_image_new (const gchar *icon)
{
    GdkPixbuf *pixbuf;
    GtkWidget *image;
    GError    *error = NULL;
    gint       w = 16, h = 16;

    if (!g_path_is_absolute (icon))
        return gtk_image_new_from_icon_name (icon, GTK_ICON_SIZE_MENU);

    gtk_icon_size_lookup (GTK_ICON_SIZE_MENU, &w, &h);
    pixbuf = gdk_pixbuf_new_from_file_at_scale (icon, w, h, TRUE, &error);
    if (pixbuf == NULL)
    {
        g_warning ("Devices menu: cannot load icon file '%s': %s",
                   icon, error->message);
        g_clear_error (&error);
        return NULL;
    }

    image = gtk_image_new_from_pixbuf (pixbuf);
    g_object_unref (pixbuf);
    return image;
}

/* Build a menu item with an icon + label + activate handler.  @icon is a
 * themed icon name or an absolute icon-file path (see device_icon_image_new).
 * Mirrors the (file-private) helper in pn-window.c so the Devices entries
 * line up visually with File/Edit/View/Help items. */
static GtkWidget *
build_image_item (const gchar   *icon,
                  const gchar   *label,
                  GCallback      callback,
                  gpointer       user_data,
                  GClosureNotify notify)
{
    GtkWidget *item;

    item = gtk_image_menu_item_new_with_mnemonic (label);
    if (icon != NULL)
    {
        GtkWidget *image = device_icon_image_new (icon);
        if (image != NULL)
        {
            gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
            gtk_image_menu_item_set_always_show_image (
                    GTK_IMAGE_MENU_ITEM (item), TRUE);
        }
    }

    g_signal_connect_data (item, "activate", callback,
                           user_data, notify, 0);
    return item;
}

/* Append "…" to a provider's display name to signal it opens a dialog,
 * matching the editor's other dialog-opening menu items. */
static gchar *
ellipsized_label (const gchar *display_name)
{
    return g_strconcat (display_name, "…", NULL);
}

GtkWidget *
pn_devices_menu_new (GtkWindow *parent_window, GtkAccelGroup *accel_group)
{
    GtkWidget *menu;
    GList     *providers, *l;

    (void) accel_group;   /* no accelerators on these entries yet */

    menu = gtk_menu_new ();

    providers = pn_device_provider_list ();
    for (l = providers; l != NULL; l = l->next)
    {
        PnDeviceProviderInfo *info = l->data;
        DevicesMenuItem      *item;
        gchar                *label;

        item = g_slice_new0 (DevicesMenuItem);
        item->parent = parent_window;
        item->id     = g_strdup (info->id);

        label = ellipsized_label (info->display_name);
        gtk_menu_shell_append (
                GTK_MENU_SHELL (menu),
                build_image_item (info->icon_name, label,
                                  G_CALLBACK (action_present_provider),
                                  item, devices_menu_item_free));
        g_free (label);
    }
    g_list_free_full (providers,
                      (GDestroyNotify) pn_device_provider_info_free);

    return menu;
}

G_GNUC_END_IGNORE_DEPRECATIONS
