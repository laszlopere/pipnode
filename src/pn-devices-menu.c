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
/*  Devices menu — Phase 1 scaffold.                                   */
/*                                                                     */
/*  Today: one hardcoded entry that launches the Meshtastic            */
/*  configuration dialog.  Phase 8 of TODO #29 turns this into a       */
/*  plugin-registered list (PnDeviceProvider), so other plugins        */
/*  (Tasmota, Zigbee, Kodi) can append their own entries.  Keeping the */
/*  menu in its own file means that switch will not touch pn-window.c. */
/* ------------------------------------------------------------------ */

#include "pn-devices-menu.h"

#include "pn-mesh-dialog.h"

/* GTK 3 deprecates GtkImageMenuItem but the editor's menubar uses it
 * throughout for icon-bearing items (see create_image_menu_item in
 * pn-window.c) so the columns line up.  Match that pattern here. */
G_GNUC_BEGIN_IGNORE_DEPRECATIONS

typedef struct
{
    GtkWindow *parent;
} DevicesMenuCtx;

static void
devices_menu_ctx_free (gpointer data, GClosure *closure)
{
    (void) closure;
    g_slice_free (DevicesMenuCtx, data);
}

/* Open (or raise) the Meshtastic configuration dialog. */
static void
action_meshtastic (GtkMenuItem *item, gpointer user_data)
{
    DevicesMenuCtx *ctx = user_data;

    (void) item;
    pn_mesh_dialog_present (ctx->parent);
}

/* Build a menu item with a themed icon + label + activate handler.
 * Mirrors the (file-private) helper in pn-window.c so the Devices entries
 * line up visually with File/Edit/View/Help items. */
static GtkWidget *
build_image_item (const gchar *icon_name,
                  const gchar *label,
                  GCallback    callback,
                  gpointer     user_data,
                  GClosureNotify notify)
{
    GtkWidget *item;
    GtkWidget *image;

    item  = gtk_image_menu_item_new_with_mnemonic (label);
    image = gtk_image_new_from_icon_name (icon_name, GTK_ICON_SIZE_MENU);
    gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM (item), image);
    gtk_image_menu_item_set_always_show_image (GTK_IMAGE_MENU_ITEM (item),
                                               TRUE);

    g_signal_connect_data (item, "activate", callback,
                           user_data, notify, 0);
    return item;
}

GtkWidget *
pn_devices_menu_new (GtkWindow *parent_window, GtkAccelGroup *accel_group)
{
    GtkWidget      *menu;
    DevicesMenuCtx *ctx;

    (void) accel_group;   /* no accelerators on these entries yet */

    menu = gtk_menu_new ();

    ctx = g_slice_new0 (DevicesMenuCtx);
    ctx->parent = parent_window;

    /* "network-wireless" reads as "talk to a radio device" and is shipped
     * by every common icon theme; the dialog itself shows the per-device
     * branding once it is open. */
    gtk_menu_shell_append (
            GTK_MENU_SHELL (menu),
            build_image_item ("network-wireless", "_Meshtastic…",
                              G_CALLBACK (action_meshtastic),
                              ctx, devices_menu_ctx_free));

    return menu;
}

G_GNUC_END_IGNORE_DEPRECATIONS
