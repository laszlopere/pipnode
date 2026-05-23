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
/*  PnMeshtastic — gui tier.                                           */
/*                                                                     */
/*  The settings-dialog customisation for the Meshtastic node.  The    */
/*  node's GType, properties, the serial worker / protobuf framing,    */
/*  the USB enumeration helpers and receive() logic all live in the    */
/*  GTK-free core file pn-meshtastic.c; this file installs the          */
/*  build_property_editor + build_class_tab vfuncs onto that class at   */
/*  editor startup (pn_meshtastic_gui_install).  The dialog reads the   */
/*  runtime status fields (busy / device / hw-model / last-busy-path /  */
/*  last-error) through their read-only GObject properties and the      */
/*  device/channel lists through the public pn_meshtastic_list_* /      */
/*  pn_meshtastic_get_channel_cache accessors, so it needs no extra     */
/*  core seam.  The headless runtime never loads this half, so the      */
/*  serial worker runs without GTK.                                     */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-meshtastic-gui.h"
#include "pn-meshtastic.h"
#include "pn-node-dialog-helpers.h"

#include <gtk/gtk.h>

/* Maps a Meshtastic HardwareModel enum value to its symbolic name.
 * Mirrors the format_hw_model() table the pip-mesh bash prototype keeps
 * (see /usr/bin/pip-mesh around line 1073) so the settings dialog can
 * label the local device with the same string the pip-mesh CLI prints.
 * Returns %NULL for unknown values; callers fall back to the raw int. */
static const gchar *
format_hw_model (guint32 model)
{
    switch (model)
    {
    case 1:   return "TLORA_V2";
    case 2:   return "TLORA_V1";
    case 3:   return "TLORA_V2_1_1P6";
    case 4:   return "TBEAM";
    case 5:   return "HELTEC_V2_0";
    case 6:   return "TBEAM_V0P7";
    case 7:   return "T_ECHO";
    case 8:   return "TLORA_V1_1P3";
    case 9:   return "RAK4631";
    case 10:  return "HELTEC_V2_1";
    case 11:  return "HELTEC_V1";
    case 12:  return "TBEAM_S3_CORE";
    case 13:  return "RAK11200";
    case 14:  return "NANO_G1";
    case 15:  return "TLORA_V2_1_1P8";
    case 16:  return "TLORA_T3_S3";
    case 17:  return "NANO_G1_EXPLORER";
    case 18:  return "NANO_G2_ULTRA";
    case 25:  return "STATION_G1";
    case 26:  return "RAK11310";
    case 33:  return "T_ECHO_PLUS";
    case 37:  return "PORTDUINO";
    case 43:  return "HELTEC_V3";
    case 44:  return "HELTEC_WSL_V3";
    case 47:  return "RPI_PICO";
    case 48:  return "HELTEC_WIRELESS_TRACKER";
    case 49:  return "HELTEC_WIRELESS_PAPER";
    case 50:  return "T_DECK";
    case 51:  return "T_WATCH_S3";
    case 53:  return "HELTEC_HT62";
    case 65:  return "HELTEC_CAPSULE_SENSOR_V3";
    case 69:  return "HELTEC_MESH_NODE_T114";
    case 70:  return "SENSECAP_INDICATOR";
    case 71:  return "TRACKER_T1000_E";
    case 79:  return "RPI_PICO2";
    case 84:  return "WISMESH_TAP";
    case 89:  return "THINKNODE_M1";
    case 91:  return "T_ETH_ELITE";
    case 94:  return "HELTEC_MESH_POCKET";
    case 95:  return "SEEED_SOLAR_NODE";
    case 102: return "T_DECK_PRO";
    case 103: return "T_LORA_PAGER";
    case 108: return "HELTEC_MESH_SOLAR";
    case 109: return "T_ECHO_LITE";
    case 110: return "HELTEC_V4";
    case 113: return "HELTEC_WIRELESS_TRACKER_V2";
    case 114: return "T_WATCH_ULTRA";
    case 255: return "PRIVATE_HW";
    default:  return NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_property_editor override        */
/*                                                                     */
/*  Two of #PnMeshtastic's three properties (`device`, `channel`)      */
/*  need a richer editor than the introspection-driven default would   */
/*  give them: the device path is a serial-port path that should pick  */
/*  from the live USB bus enumeration, and the channel name is a      */
/*  label whose valid set depends on which device the user just       */
/*  picked AND on the worker handshake having published the channel   */
/*  cache.  Pre-2.0 the host's pn-node-dialog.c carried these as       */
/*  PN_IS_MESHTASTIC special-case branches; the per-class vfunc lets   */
/*  the same code live next to the rest of #PnMeshtastic so a future   */
/*  board-table edit / handshake protocol bump touches one file       */
/*  instead of two and an out-of-tree node type with the same         */
/*  device-then-channel shape can model itself after this without      */
/*  patching libpipnode.                                               */
/* ------------------------------------------------------------------ */

static void
meshtastic_channel_repopulate (GObject         *target,
                               GtkComboBoxText *combo)
{
    PnMeshtastic         *node    = PN_MESHTASTIC (target);
    gchar                *device  = NULL;
    gchar                *current = NULL;
    gchar               **fallback = NULL;
    const gchar * const  *cached;
    const gchar * const  *list;
    gboolean              listed  = FALSE;

    g_object_get (target, "device", &device,  NULL);
    g_object_get (target, "channel", &current, NULL);

    /* Snapshot the user's current selection so a remove_all() does
     * not lose it before the new list arrives. */
    gtk_combo_box_text_remove_all (combo);

    cached = pn_meshtastic_get_channel_cache (node);
    if (cached != NULL && cached[0] != NULL)
    {
        list = cached;
    }
    else if (device != NULL && *device != '\0')
    {
        fallback = pn_meshtastic_list_channels (device);
        list     = (const gchar * const *) fallback;
    }
    else
    {
        list = NULL;
    }

    if (list != NULL)
    {
        for (gsize i = 0; list[i] != NULL; i++)
        {
            gtk_combo_box_text_append (combo, list[i], list[i]);
            if (g_strcmp0 (list[i], current) == 0)
                listed = TRUE;
        }
    }

    if (!listed && current != NULL && *current != '\0')
        gtk_combo_box_text_append (combo, current, current);

    /* Reapply the bound value: gtk_combo_box_text_remove_all() drops
     * the active-id, but the property still holds the user's choice
     * so push it back into the combo so the visual stays in sync. */
    if (current != NULL && *current != '\0')
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (combo), current);

    g_strfreev (fallback);
    g_free (device);
    g_free (current);
}

static void
on_meshtastic_device_notify (GObject    *target,
                             GParamSpec *pspec G_GNUC_UNUSED,
                             gpointer    user_data)
{
    meshtastic_channel_repopulate (target, GTK_COMBO_BOX_TEXT (user_data));
}

static GtkWidget *
pn_meshtastic_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                                     GParamSpec  *pspec,
                                     GObject     *target,
                                     GtkWindow   *parent    G_GNUC_UNUSED)
{
    const gchar  *name     = pspec->name;
    gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);

    /* Device path: combo populated from pn_meshtastic_list_devices()
     * (a sysfs walk for known vendor/product IDs).  The currently-
     * configured value is added to the list verbatim if it isn't
     * auto-detected so a previously-configured device whose USB cable
     * is currently unplugged still appears as the selection. */
    if (g_strcmp0 (name, "device") == 0)
    {
        GtkWidget   *combo   = gtk_combo_box_text_new ();
        gchar      **devices = pn_meshtastic_list_devices ();
        gchar       *current = NULL;
        gboolean     listed  = FALSE;

        g_object_get (target, name, &current, NULL);

        /* Sentinel "No Device" entry whose id is the empty string,
         * so the active-id binding selects it both for a fresh
         * unconfigured node and for a node whose worker just
         * cleared the device after an EBUSY conflict. */
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   "", "No Device");

        for (gchar **p = devices; p != NULL && *p != NULL; p++)
        {
            gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                       *p, *p);
            if (g_strcmp0 (*p, current) == 0)
                listed = TRUE;
        }
        if (!listed && current != NULL && *current != '\0')
            gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                       current, current);

        g_strfreev (devices);
        g_free (current);

        gtk_widget_set_hexpand   (combo, TRUE);
        gtk_widget_set_sensitive (combo, writable);
        g_object_bind_property (target, name, combo, "active-id", flags);
        return combo;
    }

    /* Channel name: combo populated from the worker-side cache when
     * the node is connected, falling back to a fresh handshake
     * against the configured device path when it isn't.  Repopulates
     * in place when the device combo changes so the user sees the
     * list update without having to reopen the dialog. */
    if (g_strcmp0 (name, "channel") == 0)
    {
        GtkWidget *combo = gtk_combo_box_text_new ();

        meshtastic_channel_repopulate (target,
                                       GTK_COMBO_BOX_TEXT (combo));

        /* Refresh the channel list whenever the user picks a
         * different device, AND whenever the worker's busy flag
         * transitions — the latter is the moment the worker just
         * finished publishing the channel cache after a fresh
         * handshake, so this is what makes the combo go from
         * empty (or stale) to fully populated as soon as the
         * "Initializing device…" spinner clears.  Tied to the
         * combo's lifetime via g_signal_connect_object so the
         * dialog can close without leaking the handlers. */
        g_signal_connect_object (target, "notify::device",
                                 G_CALLBACK (on_meshtastic_device_notify),
                                 combo, 0);
        g_signal_connect_object (target, "notify::busy",
                                 G_CALLBACK (on_meshtastic_device_notify),
                                 combo, 0);

        gtk_widget_set_hexpand   (combo, TRUE);
        gtk_widget_set_sensitive (combo, writable);
        g_object_bind_property (target, name, combo, "active-id", flags);
        return combo;
    }

    /* Any other property — currently none, but kept open for future
     * additions — falls back to the host default. */
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Settings dialog: status row                                        */
/*                                                                     */
/*  Layered on top of build_class_tab below.  The dialog's auto-       */
/*  generated tab would show only the two writable properties (device, */
/*  channel) — but the worker also publishes runtime state (busy,      */
/*  hw_model) that the user wants surfaced inline so the act of        */
/*  picking a device is visibly a two-step transaction: a red "Device  */
/*  /dev/ttyUSB0 is busy." while the worker handshakes, then a calmer  */
/*  "HELTEC_V3 is ready." once the model name comes back from the      */
/*  local NodeInfo.  The label markup is recomputed on every relevant  */
/*  notify, so the row also clears itself when the user picks "No      */
/*  Device" or when a worker stop wipes the cached hw_model.           */
/*                                                                     */
/*  The runtime state is read through the read-only GObject properties */
/*  (busy / hw-model / last-busy-path / last-error / device) so this   */
/*  gui tier never reaches into the core instance struct.              */
/* ------------------------------------------------------------------ */

static void
update_status_label (GObject    *obj,
                     GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer    user_data)
{
    GtkLabel *label = GTK_LABEL (user_data);
    gchar    *device         = NULL;
    gchar    *last_busy_path = NULL;
    gchar    *last_error     = NULL;
    gboolean  busy           = FALSE;
    guint     hw_model       = 0;
    gchar    *markup;

    g_object_get (obj,
                  "device",         &device,
                  "busy",           &busy,
                  "hw-model",       &hw_model,
                  "last-busy-path", &last_busy_path,
                  "last-error",     &last_error,
                  NULL);

    /* The sticky banner takes priority over both `busy` and the
     * (possibly just-cleared) `device` field — the worker sets it
     * right before it asks the main thread to revert the selection
     * to "No Device" after an EBUSY, and clears it itself once the
     * user picks a different device or a retry succeeds.  Without
     * this branch the "Device X is busy." line would vanish at the
     * exact moment it's most useful (the auto-revert moment), which
     * is what the user reported. */
    if (last_busy_path != NULL && *last_busy_path != '\0')
    {
        gchar *escaped = g_markup_escape_text (last_busy_path, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">Device %s is busy.</span>",
                escaped);
        g_free (escaped);
    }
    else if (last_error != NULL && *last_error != '\0')
    {
        /* Non-EBUSY failure: surface the errno-derived reason verbatim
         * (e.g. "No such file or directory" when the user unplugged the
         * cable before loading the worksheet) alongside the device path
         * so the user knows which port the message is about. */
        const gchar *path = (device != NULL && *device != '\0')
                            ? device
                            : "(none)";
        gchar *epath = g_markup_escape_text (path, -1);
        gchar *erez  = g_markup_escape_text (last_error, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">Device %s: %s</span>",
                epath, erez);
        g_free (epath);
        g_free (erez);
    }
    else if (busy)
    {
        const gchar *path = (device != NULL && *device != '\0')
                            ? device
                            : "(none)";
        gchar *escaped = g_markup_escape_text (path, -1);
        markup = g_strdup_printf (
                "<span foreground=\"red\">Device %s is busy.</span>",
                escaped);
        g_free (escaped);
    }
    else if (hw_model != 0)
    {
        const gchar *name = format_hw_model (hw_model);
        if (name != NULL)
        {
            markup = g_strdup_printf ("%s is ready.", name);
        }
        else
        {
            /* Unknown enum value — fall back to the integer so a new
             * board added on the device side before the format table
             * here is updated still produces a meaningful line. */
            markup = g_strdup_printf ("hw_model=%u is ready.",
                                      (guint) hw_model);
        }
    }
    else
    {
        markup = g_strdup ("");
    }

    gtk_label_set_markup (label, markup);
    g_free (markup);
    g_free (device);
    g_free (last_busy_path);
    g_free (last_error);
}

static GtkWidget *
pn_meshtastic_build_class_tab (PnNode    *self,
                               GtkWindow *parent)
{
    GObject    *target = G_OBJECT (self);
    GtkWidget  *grid   = pn_node_dialog_new_property_grid ();
    GtkWidget  *device_editor;
    GtkWidget  *channel_editor;
    GtkWidget  *status_label;
    GParamSpec *device_pspec;
    GParamSpec *channel_pspec;

    device_pspec  = g_object_class_find_property (G_OBJECT_GET_CLASS (self),
                                                  "device");
    channel_pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (self),
                                                  "channel");

    /* Recreate the two writable rows the introspection-driven default
     * tab would have built — routed through our own build_property_editor
     * so device and channel keep their custom combos.  Keeping the
     * order (device first, channel second) matches what users have
     * been seeing since the property tab landed. */
    device_editor = pn_meshtastic_build_property_editor (
            self, device_pspec, target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 0,
                               g_param_spec_get_nick (device_pspec),
                               device_editor);

    channel_editor = pn_meshtastic_build_property_editor (
            self, channel_pspec, target, parent);
    pn_node_dialog_attach_row (GTK_GRID (grid), 1,
                               g_param_spec_get_nick (channel_pspec),
                               channel_editor);

    status_label = gtk_label_new (NULL);
    gtk_label_set_xalign     (GTK_LABEL (status_label), 0.0);
    gtk_label_set_use_markup (GTK_LABEL (status_label), TRUE);
    pn_node_dialog_attach_row (GTK_GRID (grid), 2, "Status", status_label);

    /* Seed once and then keep in sync with every relevant runtime
     * change.  g_signal_connect_object ties the handler lifetime to
     * the label widget so the dialog can close without leaking the
     * connections. */
    update_status_label (target, NULL, status_label);
    g_signal_connect_object (target, "notify::busy",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::device",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::hw-model",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::last-busy-path",
                             G_CALLBACK (update_status_label),
                             status_label, 0);
    g_signal_connect_object (target, "notify::last-error",
                             G_CALLBACK (update_status_label),
                             status_label, 0);

    return grid;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_meshtastic_gui_install (void)
{
    PnNodeClass *node_class =
        PN_NODE_CLASS (g_type_class_ref (PN_TYPE_MESHTASTIC));

    node_class->build_property_editor = pn_meshtastic_build_property_editor;
    node_class->build_class_tab       = pn_meshtastic_build_class_tab;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slots
     * we just wrote stay valid. */
}
