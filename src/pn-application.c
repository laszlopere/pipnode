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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-application.h"
#include "pn-window.h"
#include "pn-device-provider.h"
#include "pn-device-dialog.h"
#include "pn-worksheet.h"
#include "pn-node-factory.h"
#include "pn-node-store.h"
#include "pn-flow.h"
#include "pn-wire.h"
#include "pn-wire-store.h"
#include "pn-message.h"
#include "pn-panel-display.h"
#include "pn-panel-input.h"
#include "pn-countdown.h"
#include "pn-digital-clock.h"
#include "pn-inject.h"
#include "pn-label.h"
#include "pn-led.h"
#include "pn-matrix57.h"
#include "pn-numeric.h"
#include "pn-switch.h"
#include "pn-color.h"
#include "pn-panel-geometry.h"
#include "pn-desktop-geometry.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>

struct _PnApplication
{
    GtkApplication parent_instance;

    gboolean opt_version;
    gboolean opt_no_plugins;
    gchar   *opt_file;
    gchar   *opt_dbus_name;
    gchar  **opt_remaining;

    /** Path of the worksheet file selected on the command line (via
     *  --file or as a positional argument), to be loaded once the
     *  initial window is shown.  Owned by the application. */
    gchar *startup_file;

    /** Registration id for the org.pipas.pipnode.Worksheet D-Bus
     *  interface installed at the application's object path.  Zero
     *  while no interface is registered. */
    guint dbus_worksheet_reg_id;

    /** Registration id for the org.pipas.pipnode.Editor D-Bus interface
     *  (document / window / sheet lifecycle + API versioning), the
     *  sibling of .Worksheet on the same object path.  Zero while no
     *  interface is registered. */
    guint dbus_editor_reg_id;

    /** Registration id for the org.pipas.pipnode.Engine D-Bus interface
     *  (the panel-applet control surface), installed at the same object
     *  path.  Zero while no interface is registered. */
    guint dbus_engine_reg_id;

    /** Registration id for the org.pipas.pipnode.Devices D-Bus interface
     *  (the Devices-menu provider + open device-dialog control surface),
     *  the sibling of the others on the same object path.  Zero while no
     *  interface is registered. */
    guint dbus_devices_reg_id;

    /** Worksheets the engine is running for panel applets: file path
     *  (owned key) -> panel-backed #PnWindow (borrowed; owned by the
     *  GtkApplication).  %NULL until the first RunWorksheet. */
    GHashTable *worksheets;
};

G_DEFINE_TYPE (PnApplication, pn_application, GTK_TYPE_APPLICATION)

/* ------------------------------------------------------------------ */
/*  D-Bus interface for test access to PnWorksheet                     */
/*                                                                     */
/*  Mirrors the pattern pipevolution uses: the application registers   */
/*  a single object at its own object path and installs an interface  */
/*  per test-controlled widget.  Method handlers resolve the active   */
/*  widget at call time, so the interface tracks whichever worksheet  */
/*  is hosted by the application's currently active window.           */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Error contract (TODO #40.2)                                       */
/*                                                                    */
/*  Mutating calls used to fail by returning a bare FALSE / "" or a  */
/*  generic G_DBUS_ERROR_FAILED, which tells an automation client    */
/*  THAT a call failed but never WHY.  These codes are registered    */
/*  with GDBus so each maps to a stable, machine-readable D-Bus error */
/*  name (org.pipas.pipnode.Worksheet.Error.*) the client can branch */
/*  on with g_dbus_error_get_remote_error() — the human-readable     */
/*  message stays, but the name is the contract.                     */
/* ------------------------------------------------------------------ */

typedef enum
{
    PN_WORKSHEET_ERROR_NO_ACTIVE_SHEET,
    PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
    PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
    PN_WORKSHEET_ERROR_WIRE_NOT_FOUND,
    PN_WORKSHEET_ERROR_UNKNOWN_NODE_TYPE,
    PN_WORKSHEET_ERROR_UNKNOWN_PROPERTY,
    PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
    PN_WORKSHEET_ERROR_ILLEGAL_CONNECTION,
    PN_WORKSHEET_ERROR_SHEET_NOT_FOUND,
    PN_WORKSHEET_ERROR_FAILED,              /* generic fallback (I/O etc.) */
    PN_WORKSHEET_ERROR_GLOBAL_NOT_FOUND     /* document global by name (40.11) */
} PnWorksheetError;

#define PN_WORKSHEET_ERROR (pn_worksheet_error_quark ())

/* enum code -> stable D-Bus error name.  The .Error. infix matches the
 * convention GDBus itself uses (e.g. org.freedesktop.DBus.Error.*). */
static const GDBusErrorEntry pn_worksheet_error_entries[] = {
    { PN_WORKSHEET_ERROR_NO_ACTIVE_SHEET,
      "org.pipas.pipnode.Worksheet.Error.NoActiveSheet" },
    { PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
      "org.pipas.pipnode.Worksheet.Error.NoActiveWindow" },
    { PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
      "org.pipas.pipnode.Worksheet.Error.NodeNotFound" },
    { PN_WORKSHEET_ERROR_WIRE_NOT_FOUND,
      "org.pipas.pipnode.Worksheet.Error.WireNotFound" },
    { PN_WORKSHEET_ERROR_UNKNOWN_NODE_TYPE,
      "org.pipas.pipnode.Worksheet.Error.UnknownNodeType" },
    { PN_WORKSHEET_ERROR_UNKNOWN_PROPERTY,
      "org.pipas.pipnode.Worksheet.Error.UnknownProperty" },
    { PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
      "org.pipas.pipnode.Worksheet.Error.BadPropertyValue" },
    { PN_WORKSHEET_ERROR_ILLEGAL_CONNECTION,
      "org.pipas.pipnode.Worksheet.Error.IllegalConnection" },
    { PN_WORKSHEET_ERROR_SHEET_NOT_FOUND,
      "org.pipas.pipnode.Worksheet.Error.SheetNotFound" },
    { PN_WORKSHEET_ERROR_FAILED,
      "org.pipas.pipnode.Worksheet.Error.Failed" },
    { PN_WORKSHEET_ERROR_GLOBAL_NOT_FOUND,
      "org.pipas.pipnode.Worksheet.Error.GlobalNotFound" },
};

/** The Worksheet automation error domain, lazily registered with GDBus
 *  the first time it is needed so the codes above translate to their
 *  wire names on both ends of the bus. */
static GQuark
pn_worksheet_error_quark (void)
{
    static gsize quark = 0;
    g_dbus_error_register_error_domain ("pn-worksheet-error-quark",
                                        &quark,
                                        pn_worksheet_error_entries,
                                        G_N_ELEMENTS (pn_worksheet_error_entries));
    return (GQuark) quark;
}

static const gchar worksheet_introspection_xml[] =
    "<node>"
    "  <interface name='org.pipas.pipnode.Worksheet'>"
    "    <method name='GetNodeCount'>"
    "      <arg type='u' name='count' direction='out'/>"
    "    </method>"
    "    <method name='GetWireCount'>"
    "      <arg type='u' name='count' direction='out'/>"
    "    </method>"
    "    <method name='GetNode'>"
    "      <arg type='u' name='index' direction='in'/>"
    "      <arg type='s' name='class_name' direction='out'/>"
    "      <arg type='s' name='name' direction='out'/>"
    "      <arg type='s' name='icon' direction='out'/>"
    "      <arg type='d' name='x' direction='out'/>"
    "      <arg type='d' name='y' direction='out'/>"
    "      <arg type='b' name='has_input' direction='out'/>"
    "      <arg type='b' name='has_output' direction='out'/>"
    "    </method>"
    "    <method name='GetNodeByUuid'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "      <arg type='s' name='class_name' direction='out'/>"
    "      <arg type='s' name='name' direction='out'/>"
    "      <arg type='s' name='icon' direction='out'/>"
    "      <arg type='d' name='x' direction='out'/>"
    "      <arg type='d' name='y' direction='out'/>"
    "      <arg type='b' name='has_input' direction='out'/>"
    "      <arg type='b' name='has_output' direction='out'/>"
    "    </method>"
    "    <method name='GetNodes'>"
    "      <arg type='a(sssddbb)' name='nodes' direction='out'/>"
    "    </method>"
    "    <method name='GetNodesWithUuids'>"
    "      <arg type='a(sssddbbs)' name='nodes' direction='out'/>"
    "    </method>"
    "    <method name='GetWire'>"
    "      <arg type='u' name='index' direction='in'/>"
    "      <arg type='i' name='source_index' direction='out'/>"
    "      <arg type='i' name='target_index' direction='out'/>"
    "    </method>"
    "    <method name='GetWires'>"
    "      <arg type='a(ii)' name='wires' direction='out'/>"
    "    </method>"
    "    <method name='Clear'>"
    "    </method>"
    "    <method name='LoadFromFile'>"
    "      <arg type='s' name='path' direction='in'/>"
    "    </method>"
    "    <method name='SaveToFile'>"
    "      <arg type='s' name='path' direction='in'/>"
    "    </method>"
    "    <method name='GetNodeUuid'>"
    "      <arg type='u' name='index' direction='in'/>"
    "      <arg type='s' name='uuid'  direction='out'/>"
    "    </method>"
    "    <method name='InjectDebugMessage'>"
    "      <arg type='s' name='text'  direction='in'/>"
    "    </method>"
    "    <method name='GetDebugRowCount'>"
    "      <arg type='u' name='count' direction='out'/>"
    "    </method>"
    "    <method name='GetDebugRowFromId'>"
    "      <arg type='u' name='index'   direction='in'/>"
    "      <arg type='s' name='from_id' direction='out'/>"
    "    </method>"
    "    <method name='ClickDebugFromButton'>"
    "      <arg type='u' name='index'   direction='in'/>"
    "      <arg type='b' name='clicked' direction='out'/>"
    "    </method>"
    "    <method name='GetFocusPulseUuid'>"
    "      <arg type='s' name='uuid' direction='out'/>"
    "    </method>"
    "    <method name='GetWorksheetScroll'>"
    "      <arg type='d' name='hval'   direction='out'/>"
    "      <arg type='d' name='vval'   direction='out'/>"
    "      <arg type='d' name='hpage'  direction='out'/>"
    "      <arg type='d' name='vpage'  direction='out'/>"
    "      <arg type='d' name='hupper' direction='out'/>"
    "      <arg type='d' name='vupper' direction='out'/>"
    "    </method>"
    "    <method name='GrabWorksheetFocus'>"
    "    </method>"
    "    <method name='ResizeWindow'>"
    "      <arg type='u' name='width'  direction='in'/>"
    "      <arg type='u' name='height' direction='in'/>"
    "    </method>"
    "    <method name='GetDebugPaneAllocation'>"
    "      <arg type='i' name='width'  direction='out'/>"
    "      <arg type='i' name='height' direction='out'/>"
    "    </method>"
    "    <method name='GetNodeProperty'>"
    "      <arg type='u' name='index' direction='in'/>"
    "      <arg type='s' name='prop'  direction='in'/>"
    "      <arg type='s' name='value' direction='out'/>"
    "    </method>"
    "    <method name='GetNodePropertyByUuid'>"
    "      <arg type='s' name='uuid'  direction='in'/>"
    "      <arg type='s' name='prop'  direction='in'/>"
    "      <arg type='s' name='value' direction='out'/>"
    "    </method>"
    "    <method name='SetNodeProperty'>"
    "      <arg type='u' name='index' direction='in'/>"
    "      <arg type='s' name='prop'  direction='in'/>"
    "      <arg type='s' name='value' direction='in'/>"
    "      <arg type='b' name='ok'    direction='out'/>"
    "    </method>"
    "    <method name='SetNodePropertyByUuid'>"
    "      <arg type='s' name='uuid'  direction='in'/>"
    "      <arg type='s' name='prop'  direction='in'/>"
    "      <arg type='s' name='value' direction='in'/>"
    "      <arg type='b' name='ok'    direction='out'/>"
    "    </method>"
    "    <method name='SetNodeProperties'>"
    "      <arg type='s'     name='uuid'  direction='in'/>"
    "      <arg type='a{ss}' name='props' direction='in'/>"
    "    </method>"
    "    <method name='AddNode'>"
    "      <arg type='s' name='type'  direction='in'/>"
    "      <arg type='d' name='x'     direction='in'/>"
    "      <arg type='d' name='y'     direction='in'/>"
    "      <arg type='i' name='index' direction='out'/>"
    "    </method>"
    "    <method name='AddNodeReturningUuid'>"
    "      <arg type='s' name='type' direction='in'/>"
    "      <arg type='d' name='x'    direction='in'/>"
    "      <arg type='d' name='y'    direction='in'/>"
    "      <arg type='s' name='uuid' direction='out'/>"
    "    </method>"
    "    <method name='DeleteNode'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='MoveNode'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "      <arg type='d' name='x'    direction='in'/>"
    "      <arg type='d' name='y'    direction='in'/>"
    "    </method>"
    "    <method name='RenameNode'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "      <arg type='s' name='name' direction='in'/>"
    "    </method>"
    "    <method name='SetNodeInputCount'>"
    "      <arg type='s' name='uuid'  direction='in'/>"
    "      <arg type='i' name='count' direction='in'/>"
    "    </method>"
    "    <method name='SetNodeInputName'>"
    "      <arg type='s' name='uuid'  direction='in'/>"
    "      <arg type='i' name='index' direction='in'/>"
    "      <arg type='s' name='name'  direction='in'/>"
    "    </method>"
    "    <method name='ConnectNodes'>"
    "      <arg type='u' name='source'    direction='in'/>"
    "      <arg type='u' name='target'    direction='in'/>"
    "      <arg type='b' name='connected' direction='out'/>"
    "    </method>"
    "    <method name='ConnectNodesByUuid'>"
    "      <arg type='s' name='source'    direction='in'/>"
    "      <arg type='s' name='target'    direction='in'/>"
    "      <arg type='b' name='connected' direction='out'/>"
    "    </method>"
    "    <method name='Connect'>"
    "      <arg type='s' name='source'       direction='in'/>"
    "      <arg type='s' name='target'       direction='in'/>"
    "      <arg type='i' name='target_input' direction='in'/>"
    "      <arg type='s' name='wire_uuid'    direction='out'/>"
    "    </method>"
    "    <method name='Disconnect'>"
    "      <arg type='s' name='wire_uuid' direction='in'/>"
    "    </method>"
    "    <method name='ListWires'>"
    "      <arg type='a(ssis)' name='wires' direction='out'/>"
    "    </method>"
    "    <method name='GetNodeWires'>"
    "      <arg type='s'       name='uuid'  direction='in'/>"
    "      <arg type='a(ssis)' name='wires' direction='out'/>"
    "    </method>"
    "    <method name='OpenNodeDialog'>"
    "      <arg type='u' name='index'  direction='in'/>"
    "      <arg type='b' name='opened' direction='out'/>"
    "    </method>"
    "    <method name='OpenNodeDialogByUuid'>"
    "      <arg type='s' name='uuid'   direction='in'/>"
    "      <arg type='b' name='opened' direction='out'/>"
    "    </method>"
    "    <method name='CloseNodeDialog'>"
    "      <arg type='b' name='closed' direction='out'/>"
    "    </method>"
    "    <method name='GetDialogPageTitles'>"
    "      <arg type='as' name='titles' direction='out'/>"
    "    </method>"
    "    <method name='SelectDialogPage'>"
    "      <arg type='u' name='index'    direction='in'/>"
    "      <arg type='b' name='selected' direction='out'/>"
    "    </method>"
    "    <method name='GetDialogEditorText'>"
    "      <arg type='s' name='prop' direction='in'/>"
    "      <arg type='s' name='text' direction='out'/>"
    "    </method>"
    "    <method name='GetDialogEditorSensitive'>"
    "      <arg type='s' name='prop'      direction='in'/>"
    "      <arg type='b' name='sensitive' direction='out'/>"
    "    </method>"
    "    <method name='SetDialogEditorText'>"
    "      <arg type='s' name='prop' direction='in'/>"
    "      <arg type='s' name='text' direction='in'/>"
    "      <arg type='b' name='ok'   direction='out'/>"
    "    </method>"
    "    <method name='GetDeviceProviders'>"
    "      <arg type='as' name='ids' direction='out'/>"
    "    </method>"
    "    <method name='ListNodeTypes'>"
    "      <arg type='a(sssbbs)' name='types' direction='out'/>"
    "    </method>"
    "    <method name='GetNodeTypeInfo'>"
    "      <arg type='s' name='type'        direction='in'/>"
    "      <arg type='s' name='class_name'  direction='out'/>"
    "      <arg type='s' name='category'    direction='out'/>"
    "      <arg type='b' name='has_input'   direction='out'/>"
    "      <arg type='b' name='has_output'  direction='out'/>"
    "      <arg type='s' name='plugin_name' direction='out'/>"
    "      <arg type='s' name='icon'        direction='out'/>"
    "      <arg type='s' name='color'       direction='out'/>"
    "      <arg type='s' name='help_page'   direction='out'/>"
    "    </method>"
    "    <method name='ListNodeProperties'>"
    "      <arg type='s'          name='uuid'  direction='in'/>"
    "      <arg type='a(sssssb)'  name='props' direction='out'/>"
    "    </method>"
    /* --- 40.12 selection & view ------------------------------------- */
    "    <method name='SelectNodes'>"
    "      <arg type='as' name='uuids'   direction='in'/>"
    "      <arg type='u'  name='matched' direction='out'/>"
    "    </method>"
    "    <method name='GetSelection'>"
    "      <arg type='as' name='uuids' direction='out'/>"
    "    </method>"
    "    <method name='ClearSelection'>"
    "    </method>"
    "    <method name='FocusNode'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='CenterOn'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='FitToContent'>"
    "    </method>"
    "    <method name='GetNodeGeometry'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "      <arg type='d' name='x' direction='out'/>"
    "      <arg type='d' name='y' direction='out'/>"
    "      <arg type='d' name='w' direction='out'/>"
    "      <arg type='d' name='h' direction='out'/>"
    "    </method>"
    "    <method name='SetWorksheetScroll'>"
    "      <arg type='d' name='h' direction='in'/>"
    "      <arg type='d' name='v' direction='in'/>"
    "    </method>"
    /* --- 40.13 message injection + readback -------------------------- */
    "    <method name='InjectMessage'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "      <arg type='s' name='json' direction='in'/>"
    "    </method>"
    "    <method name='InjectMessageOnInput'>"
    "      <arg type='s' name='uuid'  direction='in'/>"
    "      <arg type='i' name='input' direction='in'/>"
    "      <arg type='s' name='json'  direction='in'/>"
    "    </method>"
    "    <method name='GetLastOutputMessage'>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "      <arg type='s' name='json' direction='out'/>"
    "    </method>"
    /* --- 40.14 live-observation signals (Worksheet) ------------------ */
    "    <signal name='NodeAdded'>"
    "      <arg type='s' name='uuid'/>"
    "      <arg type='s' name='type_name'/>"
    "    </signal>"
    "    <signal name='NodeRemoved'>"
    "      <arg type='s' name='uuid'/>"
    "    </signal>"
    "    <signal name='NodeMoved'>"
    "      <arg type='s' name='uuid'/>"
    "      <arg type='d' name='x'/>"
    "      <arg type='d' name='y'/>"
    "    </signal>"
    "    <signal name='NodeRenamed'>"
    "      <arg type='s' name='uuid'/>"
    "      <arg type='s' name='name'/>"
    "    </signal>"
    "    <signal name='NodePropertyChanged'>"
    "      <arg type='s' name='uuid'/>"
    "      <arg type='s' name='property'/>"
    "    </signal>"
    "    <signal name='WireAdded'>"
    "      <arg type='s' name='wire_uuid'/>"
    "      <arg type='s' name='source_uuid'/>"
    "      <arg type='s' name='target_uuid'/>"
    "      <arg type='i' name='target_input'/>"
    "    </signal>"
    "    <signal name='WireRemoved'>"
    "      <arg type='s' name='wire_uuid'/>"
    "    </signal>"
    "    <signal name='SelectionChanged'>"
    "      <arg type='as' name='uuids'/>"
    "    </signal>"
    "    <signal name='MessageEmitted'>"
    "      <arg type='s' name='node_uuid'/>"
    "      <arg type='s' name='topic'/>"
    "      <arg type='s' name='json'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static PnWorksheet *
get_worksheet (GApplication *app)
{
    GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (!win || !PN_IS_WINDOW (win))
        return NULL;
    return pn_window_get_worksheet (PN_WINDOW (win));
}

/** The active #PnWindow, or %NULL when no Pipnode window is current.
 *  The document-level automation surface (org.pipas.pipnode.Editor)
 *  works on the window's shared flow — always present, unlike the
 *  active *tab* get_worksheet() returns (which is %NULL while the
 *  panel-editor tab is selected). */
static PnWindow *
get_window (GApplication *app)
{
    GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (!win || !PN_IS_WINDOW (win))
        return NULL;
    return PN_WINDOW (win);
}

/** Light the active window's "automation active" badge (TODO #40.16) for
 *  any automation call that is not a pure read.  The mutating surface is
 *  always-on on the session bus (a same-user trust boundary); this is the
 *  visible counterpart to that decision, not a gate.  Readers are
 *  recognised by their verb prefix (Get* / List* / Is*) plus the read-only
 *  ValidateJson — every other method mutates the document, drives the
 *  running flow, or moves the view, and so counts as visible activity. */
static void
note_automation_activity (GApplication *app, const gchar *method_name)
{
    PnWindow *win;

    if (g_str_has_prefix (method_name, "Get")
        || g_str_has_prefix (method_name, "List")
        || g_str_has_prefix (method_name, "Is")
        || g_strcmp0 (method_name, "ValidateJson") == 0)
        return;

    win = get_window (app);
    if (win != NULL)
        pn_window_note_automation_activity (win);
}

/** Index of the sheet named @name in @flow's tab order, or -1 when no
 *  such sheet exists.  The flow has no public name->index lookup, so the
 *  Editor surface walks pn_flow_get_sheets() to validate a sheet handle
 *  before acting on it. */
static gint
sheet_index_in_flow (PnFlow *flow, const gchar *name)
{
    guint               n = 0, i;
    const gchar *const *sheets;

    if (name == NULL || *name == '\0')
        return -1;

    sheets = pn_flow_get_sheets (flow, &n);
    for (i = 0; i < n; i++)
        if (g_strcmp0 (sheets[i], name) == 0)
            return (gint) i;
    return -1;
}

static gint
node_index_of (PnNodeStore *nodes, PnNode *node)
{
    guint i;
    guint n = pn_node_store_get_length (nodes);

    for (i = 0; i < n; i++)
    {
        if (pn_node_store_get_node (nodes, i) == node)
            return (gint) i;
    }
    return -1;
}

/** The node in @nodes whose stable UUID equals @uuid, or %NULL.
 *
 *  This is the by-UUID companion to node_index_of() and the basis of
 *  the UUID-addressed D-Bus surface (TODO #40.1): a node's index shifts
 *  the moment any earlier node is deleted, which makes indices unusable
 *  across a multi-step automated edit, whereas pn_node_get_uuid() is
 *  stable for the life of the node and is what gets serialized.  An
 *  empty or %NULL @uuid never matches. */
static PnNode *
node_by_uuid (PnNodeStore *nodes, const gchar *uuid)
{
    guint i, n;

    if (uuid == NULL || *uuid == '\0')
        return NULL;

    n = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (nodes, i);
        if (node != NULL && g_strcmp0 (pn_node_get_uuid (node), uuid) == 0)
            return node;
    }
    return NULL;
}

/** The wire in @wires whose session handle equals @uuid, or %NULL.  The
 *  by-UUID companion to node_by_uuid() for the port-aware wiring surface
 *  (TODO #40.5): a wire's UUID is minted at construction and unique among
 *  live wires, so Disconnect/GetNodeWires can address one precisely.  An
 *  empty or %NULL @uuid never matches. */
static PnWire *
wire_by_uuid (PnWireStore *wires, const gchar *uuid)
{
    guint i, n;

    if (uuid == NULL || *uuid == '\0')
        return NULL;

    n = pn_wire_store_get_length (wires);
    for (i = 0; i < n; i++)
    {
        PnWire *wire = pn_wire_store_get_wire (wires, i);
        if (wire != NULL && g_strcmp0 (pn_wire_get_uuid (wire), uuid) == 0)
            return wire;
    }
    return NULL;
}

/** Append @wire to @builder as the {src_uuid, tgt_uuid, target_input,
 *  wire_uuid} tuple ListWires / GetNodeWires report.  A dangling
 *  endpoint (no source or target) contributes an empty UUID string so
 *  the row count still mirrors the store. */
static void
wire_append_to_builder (GVariantBuilder *builder, PnWire *wire)
{
    PnNode      *src      = pn_wire_get_source (wire);
    PnNode      *tgt      = pn_wire_get_target (wire);
    const gchar *src_uuid = src != NULL ? pn_node_get_uuid (src) : NULL;
    const gchar *tgt_uuid = tgt != NULL ? pn_node_get_uuid (tgt) : NULL;
    const gchar *wire_uuid = pn_wire_get_uuid (wire);

    g_variant_builder_add (builder, "(ssis)",
                           src_uuid  ? src_uuid  : "",
                           tgt_uuid  ? tgt_uuid  : "",
                           pn_wire_get_target_input (wire),
                           wire_uuid ? wire_uuid : "");
}

/** Render a node property's #GValue as a stable, locale-independent
 *  string for the GetNodeProperty test method.  Common fundamental
 *  types are formatted explicitly so a test can compare against an
 *  exact (numbers) or canonical (booleans) form; anything else falls
 *  back to g_strdup_value_contents(). */
static gchar *
node_value_to_string (const GValue *v)
{
    GType t = G_VALUE_TYPE (v);

    if (t == G_TYPE_STRING)
    {
        const gchar *s = g_value_get_string (v);
        return g_strdup (s != NULL ? s : "");
    }
    if (t == G_TYPE_BOOLEAN)
        return g_strdup (g_value_get_boolean (v) ? "true" : "false");
    if (t == G_TYPE_INT)
        return g_strdup_printf ("%d", g_value_get_int (v));
    if (t == G_TYPE_UINT)
        return g_strdup_printf ("%u", g_value_get_uint (v));
    if (t == G_TYPE_DOUBLE)
    {
        gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (buf, sizeof buf, g_value_get_double (v));
        return g_strdup (buf);
    }
    if (t == G_TYPE_FLOAT)
    {
        gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (buf, sizeof buf, (double) g_value_get_float (v));
        return g_strdup (buf);
    }
    if (G_TYPE_IS_ENUM (t))
    {
        /* Render an enum as its value-nick — the same spelling
         * node_value_from_string() parses back — so an enum property
         * round-trips through Get/SetNodeProperty and reads naturally
         * in the ListNodeProperties schema (TODO #40.8). */
        GEnumClass *ec = g_type_class_ref (t);
        GEnumValue *ev = g_enum_get_value (ec, g_value_get_enum (v));
        gchar      *s  = g_strdup (ev != NULL ? ev->value_nick : "");
        g_type_class_unref (ec);
        return s;
    }

    return g_strdup_value_contents (v);
}

/** Describe a #GParamSpec's value domain as a compact string for the
 *  ListNodeProperties schema (TODO #40.8): an enum becomes its
 *  pipe-separated value-nicks ("a|b|c"), a bounded numeric becomes
 *  "min..max", everything else an empty string.  Newly allocated. */
static gchar *
node_pspec_constraints (GParamSpec *pspec)
{
    GType vt = G_PARAM_SPEC_VALUE_TYPE (pspec);

    if (G_TYPE_IS_ENUM (vt))
    {
        GEnumClass *ec = g_type_class_ref (vt);
        GString    *s  = g_string_new (NULL);
        for (guint i = 0; i < ec->n_values; i++)
        {
            if (i > 0)
                g_string_append_c (s, '|');
            g_string_append (s, ec->values[i].value_nick);
        }
        g_type_class_unref (ec);
        return g_string_free (s, FALSE);
    }
    if (G_IS_PARAM_SPEC_INT (pspec))
    {
        GParamSpecInt *p = G_PARAM_SPEC_INT (pspec);
        return g_strdup_printf ("%d..%d", p->minimum, p->maximum);
    }
    if (G_IS_PARAM_SPEC_UINT (pspec))
    {
        GParamSpecUInt *p = G_PARAM_SPEC_UINT (pspec);
        return g_strdup_printf ("%u..%u", p->minimum, p->maximum);
    }
    if (G_IS_PARAM_SPEC_DOUBLE (pspec))
    {
        GParamSpecDouble *p = G_PARAM_SPEC_DOUBLE (pspec);
        gchar lo[G_ASCII_DTOSTR_BUF_SIZE], hi[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (lo, sizeof lo, p->minimum);
        g_ascii_dtostr (hi, sizeof hi, p->maximum);
        return g_strdup_printf ("%s..%s", lo, hi);
    }
    if (G_IS_PARAM_SPEC_FLOAT (pspec))
    {
        GParamSpecFloat *p = G_PARAM_SPEC_FLOAT (pspec);
        gchar lo[G_ASCII_DTOSTR_BUF_SIZE], hi[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (lo, sizeof lo, (double) p->minimum);
        g_ascii_dtostr (hi, sizeof hi, (double) p->maximum);
        return g_strdup_printf ("%s..%s", lo, hi);
    }
    return g_strdup ("");
}

/** Parse @text into @value (already inited to the target type) for the
 *  SetNodeProperty test method.  Handles the fundamental types a node
 *  property is likely to use; returns %FALSE for an unsupported type so
 *  the caller can report a clear error rather than set a bogus value. */
static gboolean
node_value_from_string (GValue *value, const gchar *text)
{
    GType t = G_VALUE_TYPE (value);

    if (text == NULL)
        text = "";

    if (t == G_TYPE_STRING)
        g_value_set_string (value, text);
    else if (t == G_TYPE_BOOLEAN)
        g_value_set_boolean (value,
                             g_ascii_strcasecmp (text, "true") == 0 ||
                             g_strcmp0 (text, "1") == 0);
    else if (t == G_TYPE_INT)
        g_value_set_int (value, (gint) g_ascii_strtoll (text, NULL, 10));
    else if (t == G_TYPE_UINT)
        g_value_set_uint (value, (guint) g_ascii_strtoull (text, NULL, 10));
    else if (t == G_TYPE_DOUBLE)
        g_value_set_double (value, g_ascii_strtod (text, NULL));
    else if (t == G_TYPE_FLOAT)
        g_value_set_float (value, (gfloat) g_ascii_strtod (text, NULL));
    else if (G_TYPE_IS_ENUM (t))
    {
        GEnumClass *ec  = g_type_class_ref (t);
        GEnumValue *ev  = g_enum_get_value_by_nick (ec, text);
        if (ev == NULL)
            ev = g_enum_get_value_by_name (ec, text);
        if (ev != NULL)
            g_value_set_enum (value, ev->value);
        g_type_class_unref (ec);
        if (ev == NULL)
            return FALSE;
    }
    else
        return FALSE;

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Message <-> JSON for the interactivity surface (TODO #40.13)      */
/*                                                                    */
/*  The wire form is the documented message envelope the Debug pane   */
/*  renders — {type, from, from_id, topic, id, created, data} — so an */
/*  agent reads back exactly the shape it already knows, and can post */
/*  the same shape (or just a bare data bag) to inject one.  The      */
/*  serialize/deserialize pair lives on PnMessage (TODO #43.5) so the */
/*  $pnvector markers survive the round-trip; inspection paths here   */
/*  ship descriptor-only (include_blobs = FALSE) and never base64     */
/*  megabytes by accident — a dedicated binary method would do that.  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Shared method bodies                                              */
/*                                                                    */
/*  Each of these takes an already-resolved node (or endpoints) and  */
/*  completes the invocation, so the index-addressed (legacy/test)   */
/*  and UUID-addressed (TODO #40.1) handlers differ only in how they */
/*  look the node up, never in what they then do with it.            */
/* ------------------------------------------------------------------ */

/** Complete GetNode / GetNodeByUuid: report @node's identity tuple. */
static void
worksheet_return_node_info (GDBusMethodInvocation *invocation, PnNode *node)
{
    const gchar   *class_name = pn_node_get_class_name (node);
    const gchar   *name       = pn_node_get_name       (node);
    const gchar   *icon       = pn_node_get_icon       (node);
    const PnPoint *pos        = pn_node_get_position   (node);

    g_dbus_method_invocation_return_value (
            invocation,
            g_variant_new ("(sssddbb)",
                           class_name ? class_name : "",
                           name       ? name       : "",
                           icon       ? icon       : "",
                           pos ? pos->x : 0.0,
                           pos ? pos->y : 0.0,
                           pn_node_get_has_input  (node),
                           pn_node_get_has_output (node)));
}

/** Complete GetNodeProperty / GetNodePropertyByUuid: render @node's
 *  @prop as a string, or fail if it has no such property. */
static void
worksheet_return_node_property (GDBusMethodInvocation *invocation,
                                PnNode *node, const gchar *prop)
{
    GParamSpec *pspec;
    GValue      value = G_VALUE_INIT;
    gchar      *str;

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node), prop);
    if (!pspec)
    {
        g_dbus_method_invocation_return_error (
                invocation, PN_WORKSHEET_ERROR,
                PN_WORKSHEET_ERROR_UNKNOWN_PROPERTY,
                "Node has no property '%s'", prop);
        return;
    }

    g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
    g_object_get_property (G_OBJECT (node), prop, &value);
    str = node_value_to_string (&value);
    g_value_unset (&value);

    g_dbus_method_invocation_return_value (
            invocation, g_variant_new ("(s)", str));
    g_free (str);
}

/** Complete SetNodeProperty / SetNodePropertyByUuid: parse @text into
 *  @node's writable @prop, or fail with a clear reason. */
static void
worksheet_set_node_property (GDBusMethodInvocation *invocation,
                             PnNode *node, const gchar *prop,
                             const gchar *text)
{
    GParamSpec *pspec;
    GValue      value = G_VALUE_INIT;

    pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node), prop);
    if (!pspec || (pspec->flags & G_PARAM_WRITABLE) == 0)
    {
        g_dbus_method_invocation_return_error (
                invocation, PN_WORKSHEET_ERROR,
                PN_WORKSHEET_ERROR_UNKNOWN_PROPERTY,
                "Node has no writable property '%s'", prop);
        return;
    }

    g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
    if (!node_value_from_string (&value, text))
    {
        g_value_unset (&value);
        g_dbus_method_invocation_return_error (
                invocation, PN_WORKSHEET_ERROR,
                PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                "Cannot parse '%s' for property '%s'", text, prop);
        return;
    }

    g_object_set_property (G_OBJECT (node), prop, &value);
    g_value_unset (&value);

    g_dbus_method_invocation_return_value (
            invocation, g_variant_new ("(b)", TRUE));
}

/** Complete SetNodeProperties (TODO #40.6): parse every (name,value) pair
 *  in @props and, only if they ALL parse into a writable property, apply
 *  them in one g_object_setv call.  All-or-nothing: the first unknown or
 *  unparseable entry fails the whole call with UnknownProperty /
 *  BadPropertyValue and the node is left exactly as it was, so an agent
 *  never ends up with a half-configured node.  @props strings are
 *  borrowed from the still-live method parameters. */
static void
worksheet_set_node_properties (GDBusMethodInvocation *invocation,
                               PnNode *node, GVariant *props)
{
    GVariantIter  iter;
    const gchar  *prop = NULL;
    const gchar  *text = NULL;
    GPtrArray    *names;
    GArray       *values;

    names  = g_ptr_array_new ();
    values = g_array_new (FALSE, FALSE, sizeof (GValue));
    g_array_set_clear_func (values, (GDestroyNotify) g_value_unset);

    g_variant_iter_init (&iter, props);
    while (g_variant_iter_next (&iter, "{&s&s}", &prop, &text))
    {
        GParamSpec *pspec;
        GValue      value = G_VALUE_INIT;

        pspec = g_object_class_find_property (G_OBJECT_GET_CLASS (node), prop);
        if (!pspec || (pspec->flags & G_PARAM_WRITABLE) == 0)
        {
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_UNKNOWN_PROPERTY,
                    "Node has no writable property '%s'", prop);
            goto out;
        }

        g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
        if (!node_value_from_string (&value, text))
        {
            g_value_unset (&value);
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "Cannot parse '%s' for property '%s'", text, prop);
            goto out;
        }

        /* The array takes ownership of the parsed value (shallow copy of
         * the GValue struct); its clear func unsets each on free. */
        g_ptr_array_add (names, (gpointer) prop);
        g_array_append_val (values, value);
    }

    /* Every pair validated — now nothing can fail, so apply atomically. */
    g_object_setv (G_OBJECT (node), names->len,
                   (const gchar **) names->pdata,
                   (const GValue *) values->data);

    g_dbus_method_invocation_return_value (invocation, NULL);

out:
    g_ptr_array_free (names, TRUE);
    g_array_free (values, TRUE);
}

/** Shared by AddNode (index form) and AddNodeReturningUuid (TODO #40.4):
 *  create a node of @type_name, place it at (@x,@y), and add it to the
 *  active flow via the same path a palette drop takes, so every view
 *  stays in sync.  On success the node is owned by the flow/store and a
 *  borrowed pointer is returned (still alive); on failure the invocation
 *  is completed with an UnknownNodeType / Failed error and %NULL is
 *  returned, so the caller need only `if (!node) return;`. */
static PnNode *
worksheet_add_node (GDBusMethodInvocation *invocation,
                    PnWorksheet *worksheet,
                    const gchar *type_name, gdouble x, gdouble y)
{
    GType   type;
    PnNode *node;
    PnPoint pos;

    type = pn_node_factory_lookup (
            pn_node_factory_get_default (), type_name);
    if (type == G_TYPE_INVALID || !g_type_is_a (type, PN_TYPE_NODE))
    {
        g_dbus_method_invocation_return_error (
                invocation,
                PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_UNKNOWN_NODE_TYPE,
                "Unknown node type '%s'", type_name);
        return NULL;
    }

    node = pn_node_factory_create_for_type (
            pn_node_factory_get_default (), type);
    if (!node)
    {
        g_dbus_method_invocation_return_error (
                invocation,
                PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                "Could not create node of type '%s'", type_name);
        return NULL;
    }

    pos.x = x;
    pos.y = y;
    pn_node_set_position (node, &pos);
    pn_flow_add_node (pn_worksheet_get_flow (worksheet), node);
    g_object_unref (node);      /* the flow/store now owns the reference */

    return node;                /* borrowed; still alive in the store */
}

/** Complete ConnectNodes / ConnectNodesByUuid: wire @source's output to
 *  @target's input and persist the wire on the canvas.  Both endpoints
 *  must already be resolved (callers report NodeNotFound for a missing
 *  one); this rejects only the self-loop.  Port-aware wiring is TODO
 *  #40.5. */
static void
worksheet_connect_nodes (GDBusMethodInvocation *invocation,
                         PnWireStore *wires, PnNode *source, PnNode *target)
{
    PnWire *wire;

    if (source == target)
    {
        g_dbus_method_invocation_return_error (
                invocation, PN_WORKSHEET_ERROR,
                PN_WORKSHEET_ERROR_ILLEGAL_CONNECTION,
                "Cannot connect a node to itself");
        return;
    }

    /* Constructing the wire wires up routing (it connects the source's
     * "message" signal); adding it to the store makes it persist and
     * show on the canvas. */
    wire = pn_wire_new (source, target);
    pn_wire_store_add (wires, wire);
    g_object_unref (wire);

    g_dbus_method_invocation_return_value (
            invocation, g_variant_new ("(b)", TRUE));
}

static void
handle_worksheet_method_call (
        GDBusConnection       *connection,
        const gchar           *sender,
        const gchar           *object_path,
        const gchar           *interface_name,
        const gchar           *method_name,
        GVariant              *parameters,
        GDBusMethodInvocation *invocation,
        gpointer               user_data)
{
    GApplication *app       = G_APPLICATION (user_data);
    PnWorksheet  *worksheet = get_worksheet (app);
    PnNodeStore  *nodes;
    PnWireStore  *wires;

    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;

    if (!worksheet)
    {
        g_dbus_method_invocation_return_error (
                invocation,
                PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_SHEET,
                "No active worksheet");
        return;
    }

    nodes = pn_worksheet_get_nodes (worksheet);
    wires = pn_worksheet_get_wires (worksheet);

    note_automation_activity (app, method_name);

    if (g_strcmp0 (method_name, "GetNodeCount") == 0)
    {
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(u)", pn_node_store_get_length (nodes)));
    }
    else if (g_strcmp0 (method_name, "GetWireCount") == 0)
    {
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(u)", pn_wire_store_get_length (wires)));
    }
    else if (g_strcmp0 (method_name, "GetNode") == 0)
    {
        guint   index;
        PnNode *node;

        g_variant_get (parameters, "(u)", &index);

        node = pn_node_store_get_node (nodes, index);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "Node index %u out of range", index);
            return;
        }

        worksheet_return_node_info (invocation, node);
    }
    else if (g_strcmp0 (method_name, "GetNodeByUuid") == 0)
    {
        const gchar *uuid = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(&s)", &uuid);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        worksheet_return_node_info (invocation, node);
    }
    else if (g_strcmp0 (method_name, "GetNodes") == 0)
    {
        GVariantBuilder builder;
        guint i;
        guint n = pn_node_store_get_length (nodes);

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssddbb)"));

        for (i = 0; i < n; i++)
        {
            PnNode        *node       = pn_node_store_get_node (nodes, i);
            const gchar   *class_name = pn_node_get_class_name (node);
            const gchar   *name       = pn_node_get_name       (node);
            const gchar   *icon       = pn_node_get_icon       (node);
            const PnPoint *pos        = pn_node_get_position   (node);

            g_variant_builder_add (
                    &builder, "(sssddbb)",
                    class_name ? class_name : "",
                    name       ? name       : "",
                    icon       ? icon       : "",
                    pos ? pos->x : 0.0,
                    pos ? pos->y : 0.0,
                    pn_node_get_has_input  (node),
                    pn_node_get_has_output (node));
        }

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(sssddbb))", &builder));
    }
    else if (g_strcmp0 (method_name, "GetNodesWithUuids") == 0)
    {
        /* The UUID-aware companion to GetNodes (TODO #40.1): one call an
         * automation client can make to enumerate every node together
         * with the stable handle it then addresses them by, instead of
         * a GetNodeUuid round-trip per node. */
        GVariantBuilder builder;
        guint i;
        guint n = pn_node_store_get_length (nodes);

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssddbbs)"));

        for (i = 0; i < n; i++)
        {
            PnNode        *node       = pn_node_store_get_node (nodes, i);
            const gchar   *class_name = pn_node_get_class_name (node);
            const gchar   *name       = pn_node_get_name       (node);
            const gchar   *icon       = pn_node_get_icon       (node);
            const PnPoint *pos        = pn_node_get_position   (node);
            const gchar   *uuid       = pn_node_get_uuid       (node);

            g_variant_builder_add (
                    &builder, "(sssddbbs)",
                    class_name ? class_name : "",
                    name       ? name       : "",
                    icon       ? icon       : "",
                    pos ? pos->x : 0.0,
                    pos ? pos->y : 0.0,
                    pn_node_get_has_input  (node),
                    pn_node_get_has_output (node),
                    uuid       ? uuid       : "");
        }

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(sssddbbs))", &builder));
    }
    else if (g_strcmp0 (method_name, "GetWire") == 0)
    {
        guint   index;
        PnWire *wire;

        g_variant_get (parameters, "(u)", &index);

        wire = pn_wire_store_get_wire (wires, index);
        if (!wire)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_WIRE_NOT_FOUND,
                    "Wire index %u out of range", index);
            return;
        }

        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(ii)",
                               node_index_of (nodes, pn_wire_get_source (wire)),
                               node_index_of (nodes, pn_wire_get_target (wire))));
    }
    else if (g_strcmp0 (method_name, "GetWires") == 0)
    {
        GVariantBuilder builder;
        guint i;
        guint n = pn_wire_store_get_length (wires);

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ii)"));

        for (i = 0; i < n; i++)
        {
            PnWire *wire = pn_wire_store_get_wire (wires, i);
            g_variant_builder_add (
                    &builder, "(ii)",
                    node_index_of (nodes, pn_wire_get_source (wire)),
                    node_index_of (nodes, pn_wire_get_target (wire)));
        }

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(ii))", &builder));
    }
    else if (g_strcmp0 (method_name, "Clear") == 0)
    {
        pn_worksheet_clear (worksheet);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "LoadFromFile") == 0)
    {
        const gchar *path = NULL;
        GError      *error = NULL;

        g_variant_get (parameters, "(&s)", &path);

        if (!pn_worksheet_load_from_file (worksheet, path, &error))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                    "Load failed: %s",
                    error ? error->message : "(no error message)");
            g_clear_error (&error);
            return;
        }

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "SaveToFile") == 0)
    {
        const gchar *path = NULL;
        GError      *error = NULL;

        g_variant_get (parameters, "(&s)", &path);

        if (!pn_worksheet_save_to_file (worksheet, path, &error))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                    "Save failed: %s",
                    error ? error->message : "(no error message)");
            g_clear_error (&error);
            return;
        }

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetNodeUuid") == 0)
    {
        guint        index;
        PnNode      *node;
        const gchar *uuid;

        g_variant_get (parameters, "(u)", &index);

        node = pn_node_store_get_node (nodes, index);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "Node index %u out of range", index);
            return;
        }

        uuid = pn_node_get_uuid (node);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", uuid != NULL ? uuid : ""));
    }
    else if (g_strcmp0 (method_name, "InjectDebugMessage") == 0)
    {
        GtkWindow   *win  = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        const gchar *text = NULL;

        g_variant_get (parameters, "(&s)", &text);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        pn_window_inject_debug_message (PN_WINDOW (win), text);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetDebugRowCount") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint      count;

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        count = pn_window_get_debug_row_count (PN_WINDOW (win));
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(u)", count));
    }
    else if (g_strcmp0 (method_name, "GetDebugRowFromId") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint      index;
        gchar     *uuid;

        g_variant_get (parameters, "(u)", &index);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        uuid = pn_window_get_debug_row_from_id (PN_WINDOW (win), index);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", uuid != NULL ? uuid : ""));
        g_free (uuid);
    }
    else if (g_strcmp0 (method_name, "ClickDebugFromButton") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint      index;
        gboolean   ok;

        g_variant_get (parameters, "(u)", &index);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        ok = pn_window_click_debug_from_button (PN_WINDOW (win), index);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", ok));
    }
    else if (g_strcmp0 (method_name, "GetFocusPulseUuid") == 0)
    {
        const gchar *uuid = pn_worksheet_get_focus_pulse_uuid (worksheet);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", uuid != NULL ? uuid : ""));
    }
    else if (g_strcmp0 (method_name, "GetWorksheetScroll") == 0)
    {
        /* Read the enclosing scrolled window's adjustments so a
         * test can verify focus_node_by_uuid actually moved the
         * viewport.  Reports zeros (rather than failing) when the
         * worksheet is somehow not packed inside a scrolled window
         * — keeps the call safe to make from any test. */
        GtkWidget     *anc;
        GtkAdjustment *hadj = NULL, *vadj = NULL;
        double         hval = 0, vval = 0;
        double         hpage = 0, vpage = 0;
        double         hupper = 0, vupper = 0;

        anc = gtk_widget_get_ancestor (
                GTK_WIDGET (worksheet), GTK_TYPE_SCROLLED_WINDOW);
        if (anc != NULL)
        {
            hadj = gtk_scrolled_window_get_hadjustment (
                    GTK_SCROLLED_WINDOW (anc));
            vadj = gtk_scrolled_window_get_vadjustment (
                    GTK_SCROLLED_WINDOW (anc));
        }
        if (hadj != NULL)
        {
            hval   = gtk_adjustment_get_value     (hadj);
            hpage  = gtk_adjustment_get_page_size (hadj);
            hupper = gtk_adjustment_get_upper     (hadj);
        }
        if (vadj != NULL)
        {
            vval   = gtk_adjustment_get_value     (vadj);
            vpage  = gtk_adjustment_get_page_size (vadj);
            vupper = gtk_adjustment_get_upper     (vadj);
        }

        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(dddddd)",
                               hval, vval, hpage, vpage, hupper, vupper));
    }
    else if (g_strcmp0 (method_name, "GrabWorksheetFocus") == 0)
    {
        /* Mirror the gtk_widget_grab_focus call that on_button_press
         * makes in pn-worksheet.c: a regression test can use this to
         * exercise GtkScrolledWindow's "ensure focused widget
         * visible" path without synthesising real pointer events. */
        gtk_widget_grab_focus (GTK_WIDGET (worksheet));
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "ResizeWindow") == 0)
    {
        /* Resize the active window through GTK so the size-allocate
         * cycle runs and downstream widgets (the debug paned in
         * particular) see a fresh allocation — going through the
         * window manager via xdotool/wmctrl can leave the GTK widget
         * tree out of sync, which is exactly the case the wide-window
         * Debug View regression test needs to exclude. */
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint w, h;

        g_variant_get (parameters, "(uu)", &w, &h);

        if (win == NULL)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        gtk_window_resize (win, (gint) w, (gint) h);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetDebugPaneAllocation") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        gint       w = 0, h = 0;

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        pn_window_get_debug_pane_allocation (PN_WINDOW (win), &w, &h);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(ii)", w, h));
    }
    else if (g_strcmp0 (method_name, "GetNodeProperty") == 0)
    {
        guint        index;
        const gchar *prop = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(u&s)", &index, &prop);

        node = pn_node_store_get_node (nodes, index);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "Node index %u out of range", index);
            return;
        }

        worksheet_return_node_property (invocation, node, prop);
    }
    else if (g_strcmp0 (method_name, "GetNodePropertyByUuid") == 0)
    {
        const gchar *uuid = NULL;
        const gchar *prop = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(&s&s)", &uuid, &prop);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        worksheet_return_node_property (invocation, node, prop);
    }
    else if (g_strcmp0 (method_name, "SetNodeProperty") == 0)
    {
        guint        index;
        const gchar *prop = NULL;
        const gchar *text = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(u&s&s)", &index, &prop, &text);

        node = pn_node_store_get_node (nodes, index);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "Node index %u out of range", index);
            return;
        }

        worksheet_set_node_property (invocation, node, prop, text);
    }
    else if (g_strcmp0 (method_name, "SetNodePropertyByUuid") == 0)
    {
        const gchar *uuid = NULL;
        const gchar *prop = NULL;
        const gchar *text = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(&s&s&s)", &uuid, &prop, &text);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        worksheet_set_node_property (invocation, node, prop, text);
    }
    else if (g_strcmp0 (method_name, "SetNodeProperties") == 0)
    {
        const gchar *uuid  = NULL;
        GVariant    *props = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(&s@a{ss})", &uuid, &props);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            g_variant_unref (props);
            return;
        }

        worksheet_set_node_properties (invocation, node, props);
        g_variant_unref (props);
    }
    else if (g_strcmp0 (method_name, "AddNode") == 0)
    {
        const gchar *type_name = NULL;
        gdouble      x, y;
        PnNode      *node;

        g_variant_get (parameters, "(&sdd)", &type_name, &x, &y);

        node = worksheet_add_node (invocation, worksheet, type_name, x, y);
        if (!node)
            return;

        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(i)", node_index_of (nodes, node)));
    }
    else if (g_strcmp0 (method_name, "AddNodeReturningUuid") == 0)
    {
        const gchar *type_name = NULL;
        gdouble      x, y;
        PnNode      *node;
        const gchar *uuid;

        g_variant_get (parameters, "(&sdd)", &type_name, &x, &y);

        node = worksheet_add_node (invocation, worksheet, type_name, x, y);
        if (!node)
            return;

        /* A node minted via the factory always carries a fresh UUID; the
         * stable handle is what an agent addresses every later call by. */
        uuid = pn_node_get_uuid (node);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", uuid ? uuid : ""));
    }
    else if (g_strcmp0 (method_name, "DeleteNode") == 0)
    {
        const gchar *uuid = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(&s)", &uuid);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        /* The shared delete path drops the node together with every wire
         * incident on it, so the graph is never left dangling. */
        pn_worksheet_delete_node (worksheet, node);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "MoveNode") == 0)
    {
        const gchar *uuid = NULL;
        gdouble      x, y;
        PnNode      *node;
        PnPoint      pos;

        g_variant_get (parameters, "(&sdd)", &uuid, &x, &y);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        pos.x = x;
        pos.y = y;
        pn_node_set_position (node, &pos);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "RenameNode") == 0)
    {
        const gchar *uuid = NULL;
        const gchar *name = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(&s&s)", &uuid, &name);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        pn_node_set_name (node, name);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "SetNodeInputCount") == 0)
    {
        const gchar *uuid = NULL;
        gint         count;
        PnNode      *node;

        g_variant_get (parameters, "(&si)", &uuid, &count);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        /* A multi-input node needs at least one input; reject nonsense
         * rather than silently turning the node's input off. */
        if (count < 1)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "Input count must be >= 1, got %d", count);
            return;
        }

        pn_node_set_n_inputs (node, count);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "SetNodeInputName") == 0)
    {
        const gchar *uuid = NULL;
        gint         index;
        const gchar *name = NULL;
        PnNode      *node;

        g_variant_get (parameters, "(&si&s)", &uuid, &index, &name);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        if (index < 0 || index >= pn_node_get_n_inputs (node))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "Input index %d out of range (node has %d input(s))",
                    index, pn_node_get_n_inputs (node));
            return;
        }

        pn_node_set_input_name (node, index, name);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "ConnectNodes") == 0)
    {
        guint   src, dst;
        PnNode *source, *target;

        g_variant_get (parameters, "(uu)", &src, &dst);

        source = pn_node_store_get_node (nodes, src);
        target = pn_node_store_get_node (nodes, dst);
        if (!source || !target)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "Node index %u out of range", !source ? src : dst);
            return;
        }

        worksheet_connect_nodes (invocation, wires, source, target);
    }
    else if (g_strcmp0 (method_name, "ConnectNodesByUuid") == 0)
    {
        const gchar *src_uuid = NULL;
        const gchar *dst_uuid = NULL;
        PnNode      *source, *target;

        g_variant_get (parameters, "(&s&s)", &src_uuid, &dst_uuid);

        source = node_by_uuid (nodes, src_uuid);
        target = node_by_uuid (nodes, dst_uuid);
        if (!source || !target)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'",
                    !source ? (src_uuid ? src_uuid : "")
                            : (dst_uuid ? dst_uuid : ""));
            return;
        }

        worksheet_connect_nodes (invocation, wires, source, target);
    }
    else if (g_strcmp0 (method_name, "Connect") == 0)
    {
        const gchar *src_uuid = NULL;
        const gchar *dst_uuid = NULL;
        gint         target_input;
        PnNode      *source, *target;
        PnWire      *wire;
        guint        i, n;

        g_variant_get (parameters, "(&s&si)",
                       &src_uuid, &dst_uuid, &target_input);

        source = node_by_uuid (nodes, src_uuid);
        target = node_by_uuid (nodes, dst_uuid);
        if (!source || !target)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'",
                    !source ? (src_uuid ? src_uuid : "")
                            : (dst_uuid ? dst_uuid : ""));
            return;
        }

        /* Reject the connections the model cannot route, each with the
         * stable IllegalConnection name so an agent can tell them apart
         * by message: self-loop, output->non-input, and an input port
         * that does not exist on the target. */
        if (source == target)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_ILLEGAL_CONNECTION,
                    "Cannot connect a node to itself");
            return;
        }
        if (!pn_node_get_has_output (source))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_ILLEGAL_CONNECTION,
                    "Source node '%s' has no output", src_uuid);
            return;
        }
        if (!pn_node_get_has_input (target))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_ILLEGAL_CONNECTION,
                    "Target node '%s' has no input", dst_uuid);
            return;
        }
        if (target_input < 0 || target_input >= pn_node_get_n_inputs (target))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_ILLEGAL_CONNECTION,
                    "Target input %d out of range (node has %d input(s))",
                    target_input, pn_node_get_n_inputs (target));
            return;
        }

        /* A duplicate (same source, same target, same port) would only
         * double-deliver; reject it so ListWires stays a faithful map. */
        n = pn_wire_store_get_length (wires);
        for (i = 0; i < n; i++)
        {
            PnWire *w = pn_wire_store_get_wire (wires, i);
            if (pn_wire_get_source (w) == source &&
                pn_wire_get_target (w) == target &&
                pn_wire_get_target_input (w) == target_input)
            {
                g_dbus_method_invocation_return_error (
                        invocation,
                        PN_WORKSHEET_ERROR,
                        PN_WORKSHEET_ERROR_ILLEGAL_CONNECTION,
                        "A wire already feeds input %d of the target",
                        target_input);
                return;
            }
        }

        wire = pn_wire_new_full (source, target, target_input);
        pn_wire_store_add (wires, wire);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", pn_wire_get_uuid (wire)));
        g_object_unref (wire);
    }
    else if (g_strcmp0 (method_name, "Disconnect") == 0)
    {
        const gchar *wire_uuid = NULL;
        PnWire      *wire;

        g_variant_get (parameters, "(&s)", &wire_uuid);

        wire = wire_by_uuid (wires, wire_uuid);
        if (!wire)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_WIRE_NOT_FOUND,
                    "No wire with uuid '%s'", wire_uuid ? wire_uuid : "");
            return;
        }

        /* Removal severs routing (pn_wire_store_remove -> pn_wire_disconnect)
         * and drops the canvas line. */
        pn_wire_store_remove (wires, wire);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "ListWires") == 0)
    {
        GVariantBuilder builder;
        guint           i, n;

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ssis)"));
        n = pn_wire_store_get_length (wires);
        for (i = 0; i < n; i++)
            wire_append_to_builder (&builder,
                                    pn_wire_store_get_wire (wires, i));

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(ssis))", &builder));
    }
    else if (g_strcmp0 (method_name, "GetNodeWires") == 0)
    {
        const gchar    *uuid = NULL;
        PnNode         *node;
        GVariantBuilder builder;
        guint           i, n;

        g_variant_get (parameters, "(&s)", &uuid);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ssis)"));
        n = pn_wire_store_get_length (wires);
        for (i = 0; i < n; i++)
        {
            PnWire *wire = pn_wire_store_get_wire (wires, i);
            if (pn_wire_get_source (wire) == node ||
                pn_wire_get_target (wire) == node)
                wire_append_to_builder (&builder, wire);
        }

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(ssis))", &builder));
    }
    else if (g_strcmp0 (method_name, "OpenNodeDialog") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint      index;
        gboolean   ok;

        g_variant_get (parameters, "(u)", &index);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        ok = pn_window_open_node_dialog (PN_WINDOW (win), index);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", ok));
    }
    else if (g_strcmp0 (method_name, "OpenNodeDialogByUuid") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        const gchar *uuid = NULL;
        PnNode      *node;
        gboolean     ok;

        g_variant_get (parameters, "(&s)", &uuid);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        /* The window dialog path is index-addressed; resolve the stable
         * uuid to the node's current index for the call. */
        ok = pn_window_open_node_dialog (PN_WINDOW (win),
                                         node_index_of (nodes, node));
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", ok));
    }
    else if (g_strcmp0 (method_name, "CloseNodeDialog") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        gboolean   ok;

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        ok = pn_window_close_node_dialog (PN_WINDOW (win));
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", ok));
    }
    else if (g_strcmp0 (method_name, "GetDialogPageTitles") == 0)
    {
        GtkWindow       *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        gchar          **titles;
        GVariantBuilder  builder;
        gchar          **p;

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        titles = pn_window_get_dialog_page_titles (PN_WINDOW (win));

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
        for (p = titles; p != NULL && *p != NULL; p++)
            g_variant_builder_add (&builder, "s", *p);
        g_strfreev (titles);

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(as)", &builder));
    }
    else if (g_strcmp0 (method_name, "SelectDialogPage") == 0)
    {
        GtkWindow *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        guint      index;
        gboolean   ok;

        g_variant_get (parameters, "(u)", &index);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        ok = pn_window_select_dialog_page (PN_WINDOW (win), index);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", ok));
    }
    else if (g_strcmp0 (method_name, "GetDialogEditorText") == 0)
    {
        GtkWindow   *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        const gchar *prop = NULL;
        gchar       *text;

        g_variant_get (parameters, "(&s)", &prop);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        text = pn_window_get_dialog_editor_text (PN_WINDOW (win), prop);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", text != NULL ? text : ""));
        g_free (text);
    }
    else if (g_strcmp0 (method_name, "GetDialogEditorSensitive") == 0)
    {
        GtkWindow   *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        const gchar *prop = NULL;
        gboolean     found = FALSE;
        gboolean     sensitive;

        g_variant_get (parameters, "(&s)", &prop);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        sensitive = pn_window_get_dialog_editor_sensitive (
                PN_WINDOW (win), prop, &found);
        if (!found)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_UNKNOWN_PROPERTY,
                    "No editor for property '%s'", prop);
            return;
        }

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", sensitive));
    }
    else if (g_strcmp0 (method_name, "SetDialogEditorText") == 0)
    {
        GtkWindow   *win = gtk_application_get_active_window (
                GTK_APPLICATION (app));
        const gchar *prop = NULL;
        const gchar *text = NULL;
        gboolean     ok;

        g_variant_get (parameters, "(&s&s)", &prop, &text);

        if (win == NULL || !PN_IS_WINDOW (win))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                    "No active window");
            return;
        }

        ok = pn_window_set_dialog_editor_text (PN_WINDOW (win), prop, text);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", ok));
    }
    else if (g_strcmp0 (method_name, "ListNodeTypes") == 0)
    {
        /* The install-accurate node palette (TODO #40.7): one tuple per
         * type the factory knows about, in registration/palette order,
         * including any types contributed by loaded plugins.  This is the
         * runtime mirror of the pipnode-nodes skill — the skill carries
         * the prose, this carries the live catalog. */
        PnNodeFactory   *factory = pn_node_factory_get_default ();
        guint            n       = pn_node_factory_get_n_types (factory);
        GVariantBuilder  builder;
        guint            i;

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssbbs)"));
        for (i = 0; i < n; i++)
        {
            GType        t   = pn_node_factory_get_type_at (factory, i);
            const gchar *cls = pn_node_factory_get_class_name (factory, t);
            const gchar *cat = pn_node_factory_get_category (factory, t);
            const gchar *plg = pn_node_factory_get_plugin_name (factory, t);

            g_variant_builder_add (
                    &builder, "(sssbbs)",
                    g_type_name (t),
                    cls != NULL ? cls : "",
                    cat != NULL ? cat : "",
                    pn_node_factory_get_has_input  (factory, t),
                    pn_node_factory_get_has_output (factory, t),
                    plg != NULL ? plg : "");
        }

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(sssbbs))", &builder));
    }
    else if (g_strcmp0 (method_name, "GetNodeTypeInfo") == 0)
    {
        /* Per-type detail the palette row omits (TODO #40.7): icon glyph,
         * body colour, and the per-class help-page basename (PnXxx.html,
         * the same anchor the in-app help browser resolves). */
        PnNodeFactory *factory   = pn_node_factory_get_default ();
        const gchar   *type_name = NULL;
        GType          t;

        g_variant_get (parameters, "(&s)", &type_name);

        t = pn_node_factory_lookup (factory, type_name);
        if (t == G_TYPE_INVALID)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_UNKNOWN_NODE_TYPE,
                    "No such node type '%s'", type_name ? type_name : "");
        }
        else
        {
            const gchar   *cls   = pn_node_factory_get_class_name (factory, t);
            const gchar   *cat   = pn_node_factory_get_category (factory, t);
            const gchar   *plg   = pn_node_factory_get_plugin_name (factory, t);
            const gchar   *icon  = pn_node_factory_get_icon (factory, t);
            const PnColor *color = pn_node_factory_get_color (factory, t);
            gchar         *colstr = color != NULL ? pn_color_to_string (color)
                                                  : g_strdup ("");
            gchar         *help   = g_strconcat (g_type_name (t), ".html", NULL);

            g_dbus_method_invocation_return_value (
                    invocation,
                    g_variant_new ("(ssbbssss)",
                                   cls  != NULL ? cls  : "",
                                   cat  != NULL ? cat  : "",
                                   pn_node_factory_get_has_input  (factory, t),
                                   pn_node_factory_get_has_output (factory, t),
                                   plg  != NULL ? plg  : "",
                                   icon != NULL ? icon : "",
                                   colstr,
                                   help));
            g_free (colstr);
            g_free (help);
        }
    }
    else if (g_strcmp0 (method_name, "ListNodeProperties") == 0)
    {
        /* The configurable surface of one live node (TODO #40.8): every
         * installed GParamSpec rendered as
         *   (name, value-type, current, default, constraints, writable)
         * so an agent learns which properties exist, their types, their
         * current and default values, the allowed enum nicks / numeric
         * range, and whether SetNodeProperty can touch them — without
         * reading the node's C source. */
        const gchar  *uuid = NULL;
        PnNode       *node;
        GParamSpec  **specs;
        guint         n_specs, i;
        GVariantBuilder builder;

        g_variant_get (parameters, "(&s)", &uuid);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        specs = g_object_class_list_properties (
                    G_OBJECT_GET_CLASS (node), &n_specs);
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssssb)"));
        for (i = 0; i < n_specs; i++)
        {
            GParamSpec *ps  = specs[i];
            GType       vt  = G_PARAM_SPEC_VALUE_TYPE (ps);
            gboolean    wr  = (ps->flags & G_PARAM_WRITABLE) != 0 &&
                              (ps->flags & G_PARAM_CONSTRUCT_ONLY) == 0;
            gchar      *cur;
            gchar      *def;
            gchar      *con = node_pspec_constraints (ps);

            /* current value (only if readable) */
            if (ps->flags & G_PARAM_READABLE)
            {
                GValue v = G_VALUE_INIT;
                g_value_init (&v, vt);
                g_object_get_property (G_OBJECT (node), ps->name, &v);
                cur = node_value_to_string (&v);
                g_value_unset (&v);
            }
            else
            {
                cur = g_strdup ("");
            }

            /* default value from the pspec */
            {
                GValue dv = G_VALUE_INIT;
                g_value_init (&dv, vt);
                g_param_value_set_default (ps, &dv);
                def = node_value_to_string (&dv);
                g_value_unset (&dv);
            }

            g_variant_builder_add (&builder, "(sssssb)",
                                   ps->name,
                                   g_type_name (vt),
                                   cur, def, con, wr);
            g_free (cur);
            g_free (def);
            g_free (con);
        }
        g_free (specs);

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(sssssb))", &builder));
    }
    else if (g_strcmp0 (method_name, "SelectNodes") == 0)
    {
        /* Replace the selection with the named nodes so the agent can
         * highlight what it just built (TODO #40.12). */
        GVariantIter *iter = NULL;
        GPtrArray    *list = g_ptr_array_new ();
        const gchar  *u;
        guint         matched;

        g_variant_get (parameters, "(as)", &iter);
        while (iter != NULL && g_variant_iter_next (iter, "&s", &u))
            g_ptr_array_add (list, (gpointer) u);
        if (iter != NULL)
            g_variant_iter_free (iter);

        matched = pn_worksheet_select_nodes_by_uuid (
                worksheet, (const gchar *const *) list->pdata, list->len);
        g_ptr_array_free (list, TRUE);

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(u)", matched));
    }
    else if (g_strcmp0 (method_name, "GetSelection") == 0)
    {
        gchar          **uuids = pn_worksheet_get_selection_uuids (worksheet);
        GVariantBuilder  builder;
        gchar          **p;

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
        for (p = uuids; p != NULL && *p != NULL; p++)
            g_variant_builder_add (&builder, "s", *p);
        g_strfreev (uuids);

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(as)", &builder));
    }
    else if (g_strcmp0 (method_name, "ClearSelection") == 0)
    {
        pn_worksheet_clear_selection (worksheet);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "FocusNode") == 0)
    {
        const gchar *uuid = NULL;

        g_variant_get (parameters, "(&s)", &uuid);

        if (node_by_uuid (nodes, uuid) == NULL)
        {
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        pn_worksheet_focus_node_by_uuid (worksheet, uuid);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "CenterOn") == 0)
    {
        const gchar *uuid = NULL;

        g_variant_get (parameters, "(&s)", &uuid);

        if (!pn_worksheet_center_on_uuid (worksheet, uuid))
        {
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "FitToContent") == 0)
    {
        pn_worksheet_fit_to_content (worksheet);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetNodeGeometry") == 0)
    {
        /* The node's full worksheet-space bounding box (position + size),
         * so an agent can lay out, align, or frame what it built
         * (TODO #40.12). */
        const gchar   *uuid = NULL;
        PnNode        *node;
        const PnPoint *pos;
        double         w = 0.0, h = 0.0;

        g_variant_get (parameters, "(&s)", &uuid);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        pos = pn_node_get_position (node);
        pn_node_get_size (node, &w, &h);

        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(dddd)",
                               pos ? pos->x : 0.0, pos ? pos->y : 0.0,
                               w, h));
    }
    else if (g_strcmp0 (method_name, "SetWorksheetScroll") == 0)
    {
        gdouble h, v;

        g_variant_get (parameters, "(dd)", &h, &v);
        pn_worksheet_set_scroll (worksheet, h, v);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "InjectMessage") == 0 ||
             g_strcmp0 (method_name, "InjectMessageOnInput") == 0)
    {
        /* Deliver a message to a node's input as if a wire carried it
         * (TODO #40.13), so an agent can drive a flow it just built and
         * read the result back via GetLastOutputMessage. */
        const gchar *uuid  = NULL;
        const gchar *json  = NULL;
        gint         input = 0;
        PnNode      *node;
        PnMessage   *msg;
        GError      *err   = NULL;

        if (g_strcmp0 (method_name, "InjectMessageOnInput") == 0)
            g_variant_get (parameters, "(&si&s)", &uuid, &input, &json);
        else
            g_variant_get (parameters, "(&s&s)", &uuid, &json);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }
        if (input < 0 || input >= pn_node_get_n_inputs (node))
        {
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "Input index %d out of range [0,%d)",
                    input, pn_node_get_n_inputs (node));
            return;
        }

        msg = pn_message_deserialize (json, &err);
        if (msg == NULL)
        {
            /* Keep the documented Worksheet error on the D-Bus surface —
             * the deserializer raises in its own PN_MESSAGE_ERROR domain. */
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "%s", err != NULL ? err->message : "bad message JSON");
            g_clear_error (&err);
            return;
        }

        pn_node_receive_message_on_input (node, msg, input);
        g_object_unref (msg);

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetLastOutputMessage") == 0)
    {
        const gchar *uuid = NULL;
        PnNode      *node;
        PnMessage   *last;
        gchar       *json;

        g_variant_get (parameters, "(&s)", &uuid);

        node = node_by_uuid (nodes, uuid);
        if (!node)
        {
            g_dbus_method_invocation_return_error (
                    invocation, PN_WORKSHEET_ERROR,
                    PN_WORKSHEET_ERROR_NODE_NOT_FOUND,
                    "No node with uuid '%s'", uuid ? uuid : "");
            return;
        }

        last = pn_node_get_last_output_message (node);
        json = last != NULL ? pn_message_serialize (last, FALSE)
                            : g_strdup ("");

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", json));
        g_free (json);
    }
    else if (g_strcmp0 (method_name, "GetDeviceProviders") == 0)
    {
        /* Snapshot the Devices-menu provider registry (TODO #34 Phase D).
         * The Devices menu is built by enumerating exactly this list, so
         * a provider's presence here is its presence in the menu. */
        GList           *providers, *l;
        GVariantBuilder  builder;

        providers = pn_device_provider_list ();
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
        for (l = providers; l != NULL; l = l->next)
        {
            PnDeviceProviderInfo *info = l->data;
            g_variant_builder_add (&builder, "s", info->id);
        }
        g_list_free_full (providers,
                          (GDestroyNotify) pn_device_provider_info_free);

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(as)", &builder));
    }
    else
    {
        g_dbus_method_invocation_return_error (
                invocation,
                G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                "Unknown method '%s'", method_name);
    }
}

static const GDBusInterfaceVTable worksheet_vtable = {
    handle_worksheet_method_call,
    NULL,
    NULL
};

static GDBusNodeInfo *worksheet_introspection_data = NULL;

/* ================================================================== */
/*  org.pipas.pipnode.Editor — document / lifecycle automation         */
/*                                                                     */
/*  The sibling of .Worksheet on the SAME object path (TODO #40.3).    */
/*  .Worksheet owns active-sheet graph operations; .Editor owns the    */
/*  things that sit *around* a single sheet — the document, the        */
/*  window, the set of sheets, and (the part that lands now) the API   */
/*  version a client uses to feature-detect.  The document/sheet/      */
/*  globals methods themselves arrive in 40.9-40.11; this commit       */
/*  establishes the interface and its version handshake so a client    */
/*  can tell a UUID-aware, error-contract-bearing build apart from the */
/*  older index-only one before it makes a call.                       */
/*                                                                     */
/*  Versioning: MAJOR bumps on a breaking change to either automation  */
/*  interface (a method/signature removed or repurposed); MINOR bumps  */
/*  when methods are added back-compatibly.  Read it cheaply via the   */
/*  Version property (major only) or precisely via GetApiVersion.      */
/* ================================================================== */

#define PN_AUTOMATION_API_VERSION_MAJOR 1u
#define PN_AUTOMATION_API_VERSION_MINOR 2u

static const gchar editor_introspection_xml[] =
    "<node>"
    "  <interface name='org.pipas.pipnode.Editor'>"
    "    <method name='GetApiVersion'>"
    "      <arg type='u' name='major' direction='out'/>"
    "      <arg type='u' name='minor' direction='out'/>"
    "    </method>"
    /* --- 40.9  whole-document / active-sheet JSON round-trip --------- */
    "    <method name='GetDocumentJson'>"
    "      <arg type='s' name='json' direction='out'/>"
    "    </method>"
    "    <method name='SetDocumentJson'>"
    "      <arg type='s' name='json' direction='in'/>"
    "    </method>"
    "    <method name='GetWorksheetJson'>"
    "      <arg type='s' name='json' direction='out'/>"
    "    </method>"
    "    <method name='SetWorksheetJson'>"
    "      <arg type='s' name='json' direction='in'/>"
    "    </method>"
    "    <method name='ValidateJson'>"
    "      <arg type='s' name='json'  direction='in'/>"
    "      <arg type='b' name='ok'    direction='out'/>"
    "      <arg type='s' name='error' direction='out'/>"
    "    </method>"
    /* --- 40.10 sheets + document lifecycle --------------------------- */
    "    <method name='ListSheets'>"
    "      <arg type='as' name='sheets' direction='out'/>"
    "    </method>"
    "    <method name='GetActiveSheet'>"
    "      <arg type='s' name='name' direction='out'/>"
    "    </method>"
    "    <method name='SelectSheet'>"
    "      <arg type='s' name='name' direction='in'/>"
    "    </method>"
    "    <method name='AddSheet'>"
    "      <arg type='s' name='name'   direction='in'/>"
    "      <arg type='s' name='actual' direction='out'/>"
    "    </method>"
    "    <method name='RemoveSheet'>"
    "      <arg type='s' name='name' direction='in'/>"
    "    </method>"
    "    <method name='RenameSheet'>"
    "      <arg type='s' name='from' direction='in'/>"
    "      <arg type='s' name='to'   direction='in'/>"
    "    </method>"
    "    <method name='New'>"
    "    </method>"
    "    <method name='Open'>"
    "      <arg type='s' name='path' direction='in'/>"
    "    </method>"
    "    <method name='Save'>"
    "    </method>"
    "    <method name='SaveAs'>"
    "      <arg type='s' name='path' direction='in'/>"
    "    </method>"
    "    <method name='GetCurrentPath'>"
    "      <arg type='s' name='path' direction='out'/>"
    "    </method>"
    "    <method name='IsModified'>"
    "      <arg type='b' name='modified' direction='out'/>"
    "    </method>"
    /* --- 40.11 document globals -------------------------------------- */
    "    <method name='ListGlobals'>"
    "      <arg type='a(sss)' name='globals' direction='out'/>"
    "    </method>"
    "    <method name='GetGlobal'>"
    "      <arg type='s' name='name'  direction='in'/>"
    "      <arg type='s' name='type'  direction='out'/>"
    "      <arg type='s' name='value' direction='out'/>"
    "    </method>"
    "    <method name='SetGlobal'>"
    "      <arg type='s' name='name'  direction='in'/>"
    "      <arg type='s' name='type'  direction='in'/>"
    "      <arg type='s' name='value' direction='in'/>"
    "    </method>"
    "    <method name='RemoveGlobal'>"
    "      <arg type='s' name='name' direction='in'/>"
    "    </method>"
    "    <property name='Version' type='u' access='read'/>"
    "  </interface>"
    "</node>";

/* --- Document globals: D-Bus <-> GValue marshalling (40.11) ---------
 *
 * The wire form is a (type-nick, value-string) pair, using the same four
 * scalar type nicks the on-disk format records ("boolean", "integer",
 * "double", "string") so an agent can copy a value straight from a saved
 * worksheet.  pn_flow_*_global work in #GValue, so these helpers bridge
 * the two. */

static GType
editor_gtype_from_global_nick (const gchar *nick)
{
    if (g_strcmp0 (nick, "boolean") == 0) return G_TYPE_BOOLEAN;
    if (g_strcmp0 (nick, "integer") == 0) return G_TYPE_INT64;
    if (g_strcmp0 (nick, "double")  == 0) return G_TYPE_DOUBLE;
    if (g_strcmp0 (nick, "string")  == 0) return G_TYPE_STRING;
    return G_TYPE_INVALID;
}

static const gchar *
editor_global_type_nick (GType type)
{
    if (type == G_TYPE_BOOLEAN) return "boolean";
    if (type == G_TYPE_INT64)   return "integer";
    if (type == G_TYPE_DOUBLE)  return "double";
    if (type == G_TYPE_STRING)  return "string";
    return "";
}

/** Render a document-global #GValue to the string form ListGlobals /
 *  GetGlobal report.  Booleans render "true"/"false"; the locale-stable
 *  C representation is used for the numeric types so values round-trip
 *  through SetGlobal regardless of the user's locale. */
static gchar *
editor_global_value_to_string (const GValue *value)
{
    GType t = G_VALUE_TYPE (value);

    if (t == G_TYPE_BOOLEAN)
        return g_strdup (g_value_get_boolean (value) ? "true" : "false");
    if (t == G_TYPE_INT64)
        return g_strdup_printf ("%" G_GINT64_FORMAT, g_value_get_int64 (value));
    if (t == G_TYPE_DOUBLE)
    {
        gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
        g_ascii_dtostr (buf, sizeof buf, g_value_get_double (value));
        return g_strdup (buf);
    }
    if (t == G_TYPE_STRING)
    {
        const gchar *s = g_value_get_string (value);
        return g_strdup (s != NULL ? s : "");
    }
    return g_strdup ("");
}

/** Parse @text into @out (uninitialised; the caller g_value_unset()s it
 *  on success) according to @type.  Returns %FALSE with @error set on an
 *  unparseable numeric or an unsupported type. */
static gboolean
editor_global_string_to_value (GType         type,
                               const gchar  *text,
                               GValue       *out,
                               GError      **error)
{
    if (type == G_TYPE_BOOLEAN)
    {
        gboolean v = (g_ascii_strcasecmp (text, "true") == 0
                      || g_ascii_strcasecmp (text, "1") == 0
                      || g_ascii_strcasecmp (text, "on") == 0
                      || g_ascii_strcasecmp (text, "yes") == 0);
        g_value_init (out, G_TYPE_BOOLEAN);
        g_value_set_boolean (out, v);
        return TRUE;
    }
    if (type == G_TYPE_INT64)
    {
        gchar  *end = NULL;
        gint64  v   = g_ascii_strtoll (text, &end, 10);
        if (end == text || (end != NULL && *end != '\0'))
        {
            g_set_error (error, PN_WORKSHEET_ERROR,
                         PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                         "'%s' is not a valid integer", text);
            return FALSE;
        }
        g_value_init (out, G_TYPE_INT64);
        g_value_set_int64 (out, v);
        return TRUE;
    }
    if (type == G_TYPE_DOUBLE)
    {
        gchar   *end = NULL;
        gdouble  v   = g_ascii_strtod (text, &end);
        if (end == text || (end != NULL && *end != '\0'))
        {
            g_set_error (error, PN_WORKSHEET_ERROR,
                         PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                         "'%s' is not a valid number", text);
            return FALSE;
        }
        g_value_init (out, G_TYPE_DOUBLE);
        g_value_set_double (out, v);
        return TRUE;
    }
    if (type == G_TYPE_STRING)
    {
        g_value_init (out, G_TYPE_STRING);
        g_value_set_string (out, text);
        return TRUE;
    }

    g_set_error (error, PN_WORKSHEET_ERROR,
                 PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                 "unsupported global type");
    return FALSE;
}

/** Serialise just the active sheet's nodes (and the wires among them) to
 *  the clipboard-shaped JSON pn_flow_serialize_nodes() produces — the
 *  cheap "read one sheet" half of the 40.9 round-trip. */
static gchar *
editor_serialize_active_sheet (PnFlow *flow)
{
    const gchar *active = pn_flow_get_active_sheet (flow);
    PnNodeStore *nodes  = pn_flow_get_nodes (flow);
    GList       *subset = NULL;
    guint        i, n   = pn_node_store_get_length (nodes);
    gchar       *json;

    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (nodes, i);
        if (g_strcmp0 (pn_node_get_worksheet (node), active) == 0)
            subset = g_list_prepend (subset, node);
    }
    subset = g_list_reverse (subset);

    json = pn_flow_serialize_nodes (flow, subset);
    g_list_free (subset);
    return json;
}

/** Replace the active sheet's contents with the nodes described in @json
 *  (the write half of the 40.9 round-trip).  The payload is parse-checked
 *  up front so a malformed string is rejected before anything is removed;
 *  then the sheet's current nodes are dropped, the payload pasted in, and
 *  every pasted node retagged onto the active sheet (so a payload captured
 *  from a differently-named sheet still lands here). */
static gboolean
editor_replace_active_sheet (PnFlow      *flow,
                             const gchar *json,
                             GError     **error)
{
    const gchar *active = pn_flow_get_active_sheet (flow);
    PnNodeStore *nodes  = pn_flow_get_nodes (flow);
    GPtrArray   *doomed;
    GList       *pasted;
    GList       *l;
    guint        i, n;

    /* Parse-check without applying: catches a malformed payload while the
     * sheet is still intact.  Any parser failure is re-reported in the
     * worksheet error domain so it maps to a stable .Error.Failed name
     * rather than leaking json-glib's private quark onto the bus. */
    {
        JsonParser *parser = json_parser_new ();
        JsonNode   *root;
        GError     *perr   = NULL;
        if (!json_parser_load_from_data (parser, json, -1, &perr))
        {
            g_set_error (error, PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                         "Could not parse worksheet JSON: %s",
                         perr ? perr->message : "(no error message)");
            g_clear_error (&perr);
            g_object_unref (parser);
            return FALSE;
        }
        root = json_parser_get_root (parser);
        if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
        {
            g_set_error_literal (error, PN_WORKSHEET_ERROR,
                                 PN_WORKSHEET_ERROR_FAILED,
                                 "worksheet JSON root is not an object");
            g_object_unref (parser);
            return FALSE;
        }
        g_object_unref (parser);
    }

    /* Drop the current sheet's nodes (incident wires cascade away via the
     * node-store's "node-removed" signal). */
    doomed = g_ptr_array_new ();
    n = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (nodes, i);
        if (g_strcmp0 (pn_node_get_worksheet (node), active) == 0)
            g_ptr_array_add (doomed, node);
    }
    for (i = 0; i < doomed->len; i++)
        pn_node_store_remove (nodes, doomed->pdata[i]);
    g_ptr_array_unref (doomed);

    {
        GError *perr = NULL;
        pasted = pn_flow_paste_from_string (flow, json, 0.0, 0.0, &perr);
        if (pasted == NULL && perr != NULL)
        {
            g_set_error (error, PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                         "Could not apply worksheet JSON: %s", perr->message);
            g_clear_error (&perr);
            return FALSE;
        }
    }

    for (l = pasted; l != NULL; l = l->next)
        pn_node_set_worksheet (PN_NODE (l->data), active);
    g_list_free (pasted);

    pn_flow_set_modified (flow, TRUE);
    return TRUE;
}

static void
handle_editor_method_call (
        GDBusConnection       *connection,
        const gchar           *sender,
        const gchar           *object_path,
        const gchar           *interface_name,
        const gchar           *method_name,
        GVariant              *parameters,
        GDBusMethodInvocation *invocation,
        gpointer               user_data)
{
    GApplication *app = G_APPLICATION (user_data);
    PnWindow     *win;
    PnFlow       *flow;

    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;

    /* The version handshake is the one method that does not need a live
     * document — answer it before the window guard. */
    if (g_strcmp0 (method_name, "GetApiVersion") == 0)
    {
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(uu)",
                               PN_AUTOMATION_API_VERSION_MAJOR,
                               PN_AUTOMATION_API_VERSION_MINOR));
        return;
    }

    win = get_window (app);
    if (win == NULL)
    {
        g_dbus_method_invocation_return_error (
                invocation,
                PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_NO_ACTIVE_WINDOW,
                "No active window");
        return;
    }
    flow = pn_window_get_flow (win);

    note_automation_activity (app, method_name);

    /* ---- 40.9  whole-document / active-sheet JSON round-trip -------- */
    if (g_strcmp0 (method_name, "GetDocumentJson") == 0)
    {
        gchar *json = pn_flow_to_string (flow);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", json != NULL ? json : ""));
        g_free (json);
    }
    else if (g_strcmp0 (method_name, "SetDocumentJson") == 0)
    {
        const gchar *json  = NULL;
        GError      *lerror = NULL;

        g_variant_get (parameters, "(&s)", &json);

        if (!pn_flow_load_from_data (flow, json, &lerror))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                    "Could not load document: %s",
                    lerror ? lerror->message : "(no error message)");
            g_clear_error (&lerror);
            return;
        }
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetWorksheetJson") == 0)
    {
        gchar *json = editor_serialize_active_sheet (flow);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", json != NULL ? json : ""));
        g_free (json);
    }
    else if (g_strcmp0 (method_name, "SetWorksheetJson") == 0)
    {
        const gchar *json  = NULL;
        GError      *lerror = NULL;

        g_variant_get (parameters, "(&s)", &json);

        if (!editor_replace_active_sheet (flow, json, &lerror))
        {
            g_dbus_method_invocation_return_gerror (invocation, lerror);
            g_clear_error (&lerror);
            return;
        }
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "ValidateJson") == 0)
    {
        const gchar *json   = NULL;
        GError      *lerror  = NULL;
        PnFlow      *scratch;
        gboolean     ok;

        g_variant_get (parameters, "(&s)", &json);

        /* Validate by loading into a throwaway flow: the live document is
         * never touched, but the answer reflects the real loader. */
        scratch = pn_flow_new ();
        ok = pn_flow_load_from_data (scratch, json, &lerror);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(bs)", ok,
                               ok ? "" : (lerror ? lerror->message : "invalid")));
        g_clear_error (&lerror);
        g_object_unref (scratch);
    }
    /* ---- 40.10 sheets + document lifecycle -------------------------- */
    else if (g_strcmp0 (method_name, "ListSheets") == 0)
    {
        GVariantBuilder builder;
        guint           n = 0, i;
        const gchar *const *sheets = pn_flow_get_sheets (flow, &n);

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
        for (i = 0; i < n; i++)
            g_variant_builder_add (&builder, "s", sheets[i]);

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(as)", &builder));
    }
    else if (g_strcmp0 (method_name, "GetActiveSheet") == 0)
    {
        const gchar *active = pn_flow_get_active_sheet (flow);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", active != NULL ? active : ""));
    }
    else if (g_strcmp0 (method_name, "SelectSheet") == 0)
    {
        const gchar *name = NULL;
        g_variant_get (parameters, "(&s)", &name);

        if (sheet_index_in_flow (flow, name) < 0)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_SHEET_NOT_FOUND,
                    "No sheet named '%s'", name ? name : "");
            return;
        }
        pn_flow_set_active_sheet (flow, name);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "AddSheet") == 0)
    {
        const gchar        *name = NULL;
        guint               n    = 0;
        const gchar *const *sheets;

        g_variant_get (parameters, "(&s)", &name);

        if (!pn_flow_add_sheet (flow, (name && *name) ? name : NULL))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "A sheet named '%s' already exists", name ? name : "");
            return;
        }
        /* add_sheet appends; the freshly-created sheet (with its possibly
         * auto-generated "Sheet N" name) is the last one. */
        sheets = pn_flow_get_sheets (flow, &n);
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(s)", n > 0 ? sheets[n - 1] : ""));
    }
    else if (g_strcmp0 (method_name, "RemoveSheet") == 0)
    {
        const gchar *name = NULL;
        guint        n    = 0;

        g_variant_get (parameters, "(&s)", &name);

        if (sheet_index_in_flow (flow, name) < 0)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_SHEET_NOT_FOUND,
                    "No sheet named '%s'", name ? name : "");
            return;
        }
        (void) pn_flow_get_sheets (flow, &n);
        if (n <= 1)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "Cannot remove the only sheet");
            return;
        }
        pn_flow_remove_sheet (flow, name);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "RenameSheet") == 0)
    {
        const gchar *from = NULL;
        const gchar *to   = NULL;

        g_variant_get (parameters, "(&s&s)", &from, &to);

        if (sheet_index_in_flow (flow, from) < 0)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_SHEET_NOT_FOUND,
                    "No sheet named '%s'", from ? from : "");
            return;
        }
        if (to == NULL || *to == '\0')
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "New sheet name must not be empty");
            return;
        }
        /* A collision with a *different* existing sheet is the only way
         * rename can still fail now that from/to are validated. */
        if (g_strcmp0 (from, to) != 0 && sheet_index_in_flow (flow, to) >= 0)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "A sheet named '%s' already exists", to);
            return;
        }
        pn_flow_rename_sheet (flow, from, to);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "New") == 0)
    {
        pn_window_new_document (win);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "Open") == 0)
    {
        const gchar *path  = NULL;
        GError      *lerror = NULL;

        g_variant_get (parameters, "(&s)", &path);

        if (!pn_window_load_file (win, path, &lerror))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                    "Could not open '%s': %s", path ? path : "",
                    lerror ? lerror->message : "(no error message)");
            g_clear_error (&lerror);
            return;
        }
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "Save") == 0)
    {
        GError *lerror = NULL;

        if (!pn_window_save_current (win, &lerror))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                    "Could not save: %s",
                    lerror ? lerror->message : "(no error message)");
            g_clear_error (&lerror);
            return;
        }
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "SaveAs") == 0)
    {
        const gchar *path  = NULL;
        GError      *lerror = NULL;

        g_variant_get (parameters, "(&s)", &path);

        if (!pn_window_save_to (win, path, &lerror))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                    "Could not save to '%s': %s", path ? path : "",
                    lerror ? lerror->message : "(no error message)");
            g_clear_error (&lerror);
            return;
        }
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "GetCurrentPath") == 0)
    {
        const gchar *path = pn_window_get_current_path (win);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", path != NULL ? path : ""));
    }
    else if (g_strcmp0 (method_name, "IsModified") == 0)
    {
        g_dbus_method_invocation_return_value (
                invocation,
                g_variant_new ("(b)", pn_flow_is_modified (flow)));
    }
    /* ---- 40.11 document globals ------------------------------------- */
    else if (g_strcmp0 (method_name, "ListGlobals") == 0)
    {
        GVariantBuilder builder;
        GList          *names = pn_flow_list_globals (flow);
        GList          *l;

        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sss)"));
        for (l = names; l != NULL; l = l->next)
        {
            const gchar *name = l->data;
            GValue       v    = G_VALUE_INIT;

            if (!pn_flow_get_global (flow, name, &v))
                continue;
            {
                gchar *str = editor_global_value_to_string (&v);
                g_variant_builder_add (
                        &builder, "(sss)", name,
                        editor_global_type_nick (G_VALUE_TYPE (&v)), str);
                g_free (str);
            }
            g_value_unset (&v);
        }
        g_list_free_full (names, g_free);

        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(sss))", &builder));
    }
    else if (g_strcmp0 (method_name, "GetGlobal") == 0)
    {
        const gchar *name = NULL;
        GValue       v    = G_VALUE_INIT;

        g_variant_get (parameters, "(&s)", &name);

        if (!pn_flow_get_global (flow, name, &v))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_GLOBAL_NOT_FOUND,
                    "No document global named '%s'", name ? name : "");
            return;
        }
        {
            gchar *str = editor_global_value_to_string (&v);
            g_dbus_method_invocation_return_value (
                    invocation,
                    g_variant_new ("(ss)",
                                   editor_global_type_nick (G_VALUE_TYPE (&v)),
                                   str));
            g_free (str);
        }
        g_value_unset (&v);
    }
    else if (g_strcmp0 (method_name, "SetGlobal") == 0)
    {
        const gchar *name   = NULL;
        const gchar *type   = NULL;
        const gchar *value  = NULL;
        GType        gtype;
        GValue       v      = G_VALUE_INIT;
        GError      *lerror = NULL;

        g_variant_get (parameters, "(&s&s&s)", &name, &type, &value);

        if (name == NULL || *name == '\0')
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "Global name must not be empty");
            return;
        }
        gtype = editor_gtype_from_global_nick (type);
        if (gtype == G_TYPE_INVALID)
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_BAD_PROPERTY_VALUE,
                    "Unknown global type '%s' (want boolean|integer|double|"
                    "string)", type ? type : "");
            return;
        }
        if (!editor_global_string_to_value (gtype, value, &v, &lerror))
        {
            g_dbus_method_invocation_return_gerror (invocation, lerror);
            g_clear_error (&lerror);
            return;
        }
        pn_flow_set_global (flow, name, &v);
        g_value_unset (&v);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "RemoveGlobal") == 0)
    {
        const gchar *name = NULL;
        GValue       v    = G_VALUE_INIT;

        g_variant_get (parameters, "(&s)", &name);

        if (!pn_flow_get_global (flow, name, &v))
        {
            g_dbus_method_invocation_return_error (
                    invocation,
                    PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_GLOBAL_NOT_FOUND,
                    "No document global named '%s'", name ? name : "");
            return;
        }
        g_value_unset (&v);
        pn_flow_remove_global (flow, name);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else
    {
        g_dbus_method_invocation_return_error (
                invocation,
                G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                "Unknown method '%s'", method_name);
    }
}

static GVariant *
handle_editor_get_property (
        GDBusConnection  *connection,
        const gchar      *sender,
        const gchar      *object_path,
        const gchar      *interface_name,
        const gchar      *property_name,
        GError          **error,
        gpointer          user_data)
{
    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;
    (void) user_data;

    if (g_strcmp0 (property_name, "Version") == 0)
        return g_variant_new_uint32 (PN_AUTOMATION_API_VERSION_MAJOR);

    g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                 "Unknown property '%s'", property_name);
    return NULL;
}

static const GDBusInterfaceVTable editor_vtable = {
    handle_editor_method_call,
    handle_editor_get_property,
    NULL
};

static GDBusNodeInfo *editor_introspection_data = NULL;

/* ================================================================== */
/*  org.pipas.pipnode.Engine — XFCE panel applet control surface       */
/*                                                                     */
/*  A panel applet runs its worksheet in this process (the background  */
/*  engine, pipnode-editor --gapplication-service) and drives it over  */
/*  D-Bus, keyed by the worksheet's file path.  Each worksheet lives   */
/*  in a panel-backed PnWindow whose flow runs off the main loop while */
/*  the window stays hidden; PresentEditor present()s it for editing.  */
/*  Panel I/O nodes bridge the running flow to the applet: a           */
/*  PnPanelInput is driven with a value (SetInput) or told of an       */
/*  applet mouse event (SendEvent, fanned out to every Panel Input).   */
/*  A click on an interactive widget (a Switch toggle) is addressed to */
/*  that one node by UUID (ActivateWidget), which flips it.            */
/*                                                                     */
/*  The display side mirrors the visual layout editor (PnLayoutEditor) */
/*  headlessly, once per surface it lays out for:                       */
/*    - GetLayout / LayoutChanged carry the PANEL band — every          */
/*      PnCountdown / PnLed the user snapped onto it, in band order;    */
/*    - GetDesktopLayout / DesktopLayoutChanged carry the DESKTOP       */
/*      window — the widgets placed inside it, each at its own          */
/*      window-relative (x, y), plus the window's size and title.       */
/*  Both are consumed by dumb viewers (the XFCE applet, the standalone  */
/*  pipnode-desktop) through the shared PnWidgetMirror.  Each node's    */
/*  live state is pushed per widget, keyed by node UUID (WidgetChanged) */
/*  as JSON, and serves both surfaces.  The older single-value          */
/*  PnPanelDisplay bridge (GetDisplayValue + ValueChanged) is kept as a */
/*  legacy channel alongside them.                                      */
/* ================================================================== */

static const gchar engine_introspection_xml[] =
    "<node>"
    "  <interface name='org.pipas.pipnode.Engine'>"
    "    <method name='RunWorksheet'>"
    "      <arg type='s' name='path'  direction='in'/>"
    "      <arg type='s' name='value' direction='out'/>"
    "    </method>"
    "    <method name='GetDisplayValue'>"
    "      <arg type='s' name='path'  direction='in'/>"
    "      <arg type='s' name='value' direction='out'/>"
    "    </method>"
    "    <method name='GetLayout'>"
    "      <arg type='s' name='path'   direction='in'/>"
    "      <arg type='s' name='layout' direction='out'/>"
    "    </method>"
    "    <method name='GetDesktopLayout'>"
    "      <arg type='s' name='path'   direction='in'/>"
    "      <arg type='s' name='layout' direction='out'/>"
    "    </method>"
    "    <method name='SetInput'>"
    "      <arg type='s' name='path'  direction='in'/>"
    "      <arg type='d' name='value' direction='in'/>"
    "    </method>"
    "    <method name='SendEvent'>"
    "      <arg type='s' name='path'   direction='in'/>"
    "      <arg type='s' name='event'  direction='in'/>"
    "      <arg type='u' name='button' direction='in'/>"
    "    </method>"
    "    <method name='ActivateWidget'>"
    "      <arg type='s' name='path' direction='in'/>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='PresentEditor'>"
    "      <arg type='s' name='path'  direction='in'/>"
    "    </method>"
    "    <method name='CloseWorksheet'>"
    "      <arg type='s' name='path'  direction='in'/>"
    "    </method>"
    "    <method name='Quit'/>"
    "    <signal name='ValueChanged'>"
    "      <arg type='s' name='path'/>"
    "      <arg type='s' name='value'/>"
    "    </signal>"
    "    <signal name='LayoutChanged'>"
    "      <arg type='s' name='path'/>"
    "      <arg type='s' name='layout'/>"
    "    </signal>"
    "    <signal name='DesktopLayoutChanged'>"
    "      <arg type='s' name='path'/>"
    "      <arg type='s' name='layout'/>"
    "    </signal>"
    "    <signal name='WidgetChanged'>"
    "      <arg type='s' name='path'/>"
    "      <arg type='s' name='uuid'/>"
    "      <arg type='s' name='state'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

/* Path stashed on each PnPanelDisplay so its value-changed handler
 * knows which worksheet to name in the ValueChanged D-Bus signal. */
#define PN_ENGINE_PATH_QDATA  "pn-engine-worksheet-path"

/** First node of @type in @win's document, or %NULL.  Reads the flow's
 *  node store (spanning all sheets) rather than the active tab, so it works
 *  even when the panel-editor tab is the one on screen. */
static PnNode *
engine_find_node (PnWindow *win, GType type)
{
    PnFlow      *flow;
    PnNodeStore *nodes;
    guint        i, n;

    if (win == NULL)
        return NULL;

    flow = pn_window_get_flow (win);
    if (flow == NULL)
        return NULL;

    nodes = pn_flow_get_nodes (flow);
    n     = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (nodes, i);
        if (node != NULL && g_type_is_a (G_OBJECT_TYPE (node), type))
            return node;
    }
    return NULL;
}

/** The node with @uuid in @win's document, or %NULL.  Used to address a
 *  single mirrored widget — e.g. the switch the user clicked on the panel —
 *  rather than the type-first lookup engine_find_node does. */
static PnNode *
engine_find_node_by_uuid (PnWindow *win, const gchar *uuid)
{
    PnFlow      *flow;
    PnNodeStore *nodes;
    guint        i, n;

    if (win == NULL || uuid == NULL)
        return NULL;

    flow = pn_window_get_flow (win);
    if (flow == NULL)
        return NULL;

    nodes = pn_flow_get_nodes (flow);
    n     = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (nodes, i);
        if (node != NULL
            && g_strcmp0 (pn_node_get_uuid (node), uuid) == 0)
            return node;
    }
    return NULL;
}

/** Report a mouse event on the applet to every Panel Input in @win, so a
 *  worksheet with more than one input all see the click. */
static void
engine_send_event_to_inputs (PnWindow    *win,
                             const gchar *event,
                             guint        button)
{
    PnFlow      *flow;
    PnNodeStore *nodes;
    guint        i, n;

    if (win == NULL)
        return;

    flow = pn_window_get_flow (win);
    if (flow == NULL)
        return;

    nodes = pn_flow_get_nodes (flow);
    n     = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (nodes, i);
        if (PN_IS_PANEL_INPUT (node))
            pn_panel_input_send_event (PN_PANEL_INPUT (node), event, button);
    }
}

/** Current Panel Display string for @win; "" when there is none.
 *  Caller frees. */
static gchar *
engine_display_value (PnWindow *win)
{
    PnNode *node = engine_find_node (win, PN_TYPE_PANEL_DISPLAY);

    if (node == NULL)
        return g_strdup ("");
    return pn_panel_display_dup_text (PN_PANEL_DISPLAY (node));
}

/** Forward a Panel Display change to the applet as a ValueChanged
 *  signal, naming the worksheet by the path stashed on the node. */
static void
on_panel_display_value_changed (
        PnPanelDisplay *display,
        const gchar    *text,
        gpointer        user_data)
{
    GApplication    *app  = G_APPLICATION (user_data);
    GDBusConnection *conn = g_application_get_dbus_connection (app);
    const gchar     *obj  = g_application_get_dbus_object_path (app);
    const gchar     *path;

    if (conn == NULL || obj == NULL)
        return;

    path = g_object_get_data (G_OBJECT (display), PN_ENGINE_PATH_QDATA);
    if (path == NULL)
        return;

    g_dbus_connection_emit_signal (
            conn, NULL, obj, "org.pipas.pipnode.Engine", "ValueChanged",
            g_variant_new ("(ss)", path, text != NULL ? text : ""),
            NULL);
}

/** Tag and connect every Panel Display in @win so its changes reach the
 *  applet over D-Bus. */
static void
engine_wire_displays (
        PnApplication *self,
        PnWindow      *win,
        const gchar   *path)
{
    PnFlow      *flow = pn_window_get_flow (win);
    PnNodeStore *nodes;
    guint        i, n;

    if (flow == NULL)
        return;

    nodes = pn_flow_get_nodes (flow);
    n     = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (nodes, i);

        if (!PN_IS_PANEL_DISPLAY (node))
            continue;

        g_object_set_data_full (G_OBJECT (node), PN_ENGINE_PATH_QDATA,
                                g_strdup (path), g_free);
        g_signal_connect (node, "value-changed",
                          G_CALLBACK (on_panel_display_value_changed), self);
    }
}

/* ------------------------------------------------------------------ */
/*  Per-node panel widget mirroring                                     */
/*                                                                     */
/*  The engine plays the headless role PnLayoutEditor plays in the     */
/*  editor: it watches the flow's nodes and pushes each representable   */
/*  one (a PnCountdown -> seven-segment readout, a PnLed -> lamp) to a  */
/*  counterpart widget in the real applet.  A widget is "on the panel"  */
/*  when the layout editor snapped it onto the band; band membership is */
/*  derived from each node's saved (x, y) using the shared geometry, so */
/*  it stays in lock-step with the editor's snap target.               */
/* ------------------------------------------------------------------ */

/* Context carried into the per-worksheet signal handlers (node-store
 * add/remove, the flow's panel-layout-changed, and each widget node's
 * repaint-needed): everything they need to name the worksheet and rebuild
 * its layout.  Owned by the window it is stashed on; freed with it. */
typedef struct
{
    PnApplication *app;   /* the engine; not reffed (outlives the worksheet) */
    PnWindow      *win;   /* the worksheet window that owns this ctx         */
    gchar         *path;  /* worksheet file path (owned)                     */
} EngineWidgetCtx;

#define PN_ENGINE_WIDGET_CTX_QDATA  "pn-engine-widget-ctx"

static void on_engine_window_destroy (GtkWidget *win, gpointer user_data);

static void
engine_widget_ctx_free (gpointer data)
{
    EngineWidgetCtx *ctx = data;

    g_free (ctx->path);
    g_slice_free (EngineWidgetCtx, ctx);
}

/** A node the applet mirrors: a countdown readout, an indicator lamp, a
 *  slide toggle, or a fire-button (the two interactive kinds — see
 *  ActivateWidget). */
static gboolean
engine_is_widget_node (PnNode *node)
{
    return PN_IS_COUNTDOWN (node)
        || PN_IS_DIGITAL_CLOCK (node)
        || PN_IS_INJECT (node)
        || PN_IS_LABEL (node)
        || PN_IS_LED (node)
        || PN_IS_MATRIX57 (node)
        || PN_IS_NUMERIC (node)
        || PN_IS_SWITCH (node);
}

/** A node the DESKTOP viewer shows as a plot area: it installs a
 *  PnNodeClass::paint_plot painter and has no dedicated widget above.
 *  That is the whole test — Graph, XY Graph, Plot, Oscilloscope, the
 *  clocks and meters, the weather card, Table View, Text View, and any
 *  plugin node that paints one, without a type list to keep in step.
 *
 *  Countdown and Label paint plots too, which is why the dedicated-widget
 *  test comes first: their seven-segment and text faces are drawn as
 *  vectors by the panel widgets and look better for it.
 *
 *  Desktop only.  A panel band is one icon tall, and the panel layout has
 *  nowhere to record the size a plot needs. */
static gboolean
engine_is_plot_node (PnNode *node)
{
    if (engine_is_widget_node (node))
        return FALSE;

    return PN_NODE_GET_CLASS (node)->paint_plot != NULL;
}

/** Any node either viewer mirrors — what the engine wires up and watches.
 *  The panel builder narrows this back to engine_is_widget_node. */
static gboolean
engine_is_mirrored_node (PnNode *node)
{
    return engine_is_widget_node (node) || engine_is_plot_node (node);
}

/** The pixel area @node's plot is drawn at: what the layout editor's
 *  resize grip stored, or the default a freshly-placed plot starts at. */
static void
engine_plot_size (PnFlow *flow, PnNode *node, gint *out_w, gint *out_h)
{
    gdouble w = PN_DE_PLOT_DEFAULT_WIDTH;
    gdouble h = PN_DE_PLOT_DEFAULT_HEIGHT;

    if (flow != NULL)
        pn_flow_get_desktop_size (flow, pn_node_get_uuid (node), &w, &h);

    *out_w = (gint) w;
    *out_h = (gint) h;
}

/** cairo_surface_write_to_png_stream sink: append to a #GByteArray. */
static cairo_status_t
engine_png_write (void                *closure,
                  const unsigned char *data,
                  unsigned int         length)
{
    g_byte_array_append ((GByteArray *) closure, data, length);
    return CAIRO_STATUS_SUCCESS;
}

/** Render @node's own paint_plot painter to a @w x @h PNG and return it
 *  base64-encoded, or "" when there is nothing to draw.
 *
 *  This is the bridge across the tier boundary.  The viewer links no
 *  pipnode runtime and no plotting stack — that is what keeps a node
 *  crash out of its address space — so it cannot call a painter that
 *  lives in PLplot, MathGL and Pango.  The engine can, so it draws the
 *  picture once and ships the pixels; the viewer's #PnPlotDisplay blits
 *  them.  What the window shows is therefore the node's own painter
 *  output, not a second implementation of it that could drift. */
static gchar *
engine_render_plot_png (PnNode *node, gint w, gint h)
{
    PnNodeClass     *klass = PN_NODE_GET_CLASS (node);
    cairo_surface_t *surface;
    cairo_t         *cr;
    GByteArray      *buf;
    gchar           *out = NULL;

    if (klass->paint_plot == NULL || w <= 0 || h <= 0)
        return g_strdup ("");

    surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, w, h);
    cr      = cairo_create (surface);
    klass->paint_plot (node, cr, 0.0, 0.0, (gdouble) w, (gdouble) h);
    cairo_destroy (cr);

    buf = g_byte_array_new ();
    if (cairo_surface_write_to_png_stream (surface, engine_png_write, buf)
        == CAIRO_STATUS_SUCCESS)
        out = g_base64_encode (buf->data, buf->len);

    g_byte_array_unref (buf);
    cairo_surface_destroy (surface);

    return out != NULL ? out : g_strdup ("");
}

/** Emit an Engine signal, sinking @params even when the bus is absent. */
static void
engine_emit_signal (PnApplication *app,
                    const gchar   *signal_name,
                    GVariant      *params)
{
    GApplication    *gapp = G_APPLICATION (app);
    GDBusConnection *conn = g_application_get_dbus_connection (gapp);
    const gchar     *obj  = g_application_get_dbus_object_path (gapp);

    if (conn == NULL || obj == NULL)
    {
        /* No bus (e.g. a non-service run): drop the floating @params. */
        g_variant_unref (g_variant_ref_sink (params));
        return;
    }

    g_dbus_connection_emit_signal (
            conn, NULL, obj, "org.pipas.pipnode.Engine", signal_name,
            params, NULL);
}

/** Append an [r, g, b] array.  Used for widget colours that always paint
 *  opaque on the applet side (the lamp dome, the matrix bezel/pixels). */
static void
engine_add_color_array (JsonBuilder *b, const PnColor *c)
{
    json_builder_begin_array (b);
    json_builder_add_double_value (b, c->red);
    json_builder_add_double_value (b, c->green);
    json_builder_add_double_value (b, c->blue);
    json_builder_end_array (b);
}

/** Append an [r, g, b, a] array — for widget colours where the alpha
 *  controls the look (the Label box background, the seven-segment lit /
 *  unlit bars that fade into the panel's transparent background). */
static void
engine_add_rgba_array (JsonBuilder *b, const PnColor *c)
{
    json_builder_begin_array (b);
    json_builder_add_double_value (b, c->red);
    json_builder_add_double_value (b, c->green);
    json_builder_add_double_value (b, c->blue);
    json_builder_add_double_value (b, c->alpha);
    json_builder_end_array (b);
}

/** Fill the open object @b with @node's render state, mirroring exactly
 *  what PnLayoutEditor pushes into the corresponding panel widget.  Returns
 *  the widget kind ("countdown"/"led"), or %NULL when @node is not a
 *  representable widget.
 *
 *  @flow is needed only by the plot kind, whose state carries the size the
 *  desktop layout gave it and the picture rendered at that size; %NULL is
 *  accepted and falls back to the default plot size. */
static const gchar *
engine_add_widget_state (JsonBuilder *b, PnFlow *flow, PnNode *node)
{
    if (PN_IS_COUNTDOWN (node))
    {
        PnCountdownPaintState st;
        gint64                seconds;

        pn_countdown_get_paint_state (PN_COUNTDOWN (node), &st);
        seconds = st.days * G_GINT64_CONSTANT (86400)
                + (gint64) st.hours * 3600
                + (gint64) st.minutes * 60
                + st.seconds;

        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "countdown");
        json_builder_set_member_name (b, "seconds");
        json_builder_add_int_value (b, seconds);
        json_builder_set_member_name (b, "day_digits");
        json_builder_add_int_value (b, st.day_digits);
        /* RGBA so the panel widget can fade the segments into the panel's
         * transparent background when the user picks translucent colours. */
        json_builder_set_member_name (b, "segment_color");
        engine_add_rgba_array (b, &st.segment_color);
        json_builder_set_member_name (b, "unlit_color");
        engine_add_rgba_array (b, &st.unlit_segment_color);
        return "countdown";
    }

    if (PN_IS_DIGITAL_CLOCK (node))
    {
        /* The clock is the Countdown without a days field: mirror it onto
         * the same applet readout ("countdown" kind) but with zero day
         * digits, so the panel widget draws a bare HH:MM:SS.  The seconds
         * are already a within-a-day count. */
        PnDigitalClockPaintState st;
        gint64                   seconds;

        pn_digital_clock_get_paint_state (PN_DIGITAL_CLOCK (node), &st);
        seconds = (gint64) st.hours * 3600
                + (gint64) st.minutes * 60
                + st.seconds;

        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "countdown");
        json_builder_set_member_name (b, "seconds");
        json_builder_add_int_value (b, seconds);
        json_builder_set_member_name (b, "day_digits");
        json_builder_add_int_value (b, 0);
        /* RGBA so the panel widget honours a translucent segment colour. */
        json_builder_set_member_name (b, "segment_color");
        engine_add_rgba_array (b, &st.segment_color);
        json_builder_set_member_name (b, "unlit_color");
        engine_add_rgba_array (b, &st.unlit_segment_color);
        return "countdown";
    }

    if (PN_IS_LABEL (node))
    {
        PnLabelPaintState st;

        pn_label_get_paint_state (PN_LABEL (node), &st);

        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "text");
        json_builder_set_member_name (b, "text");
        json_builder_add_string_value (b, st.text);
        json_builder_set_member_name (b, "lines");
        json_builder_add_int_value (b, st.lines);
        json_builder_set_member_name (b, "font");
        json_builder_add_string_value (b, st.font_family);
        json_builder_set_member_name (b, "scale");
        json_builder_add_int_value (b, st.font_scale);
        json_builder_set_member_name (b, "weight");
        json_builder_add_int_value (b, st.weight);
        json_builder_set_member_name (b, "italic");
        json_builder_add_boolean_value (b, st.italic);
        json_builder_set_member_name (b, "align");
        json_builder_add_int_value (b, (gint) st.align);
        json_builder_set_member_name (b, "color");
        engine_add_color_array (b, &st.text_color);
        json_builder_set_member_name (b, "bg");
        engine_add_rgba_array (b, &st.background_color);
        return "text";
    }

    if (PN_IS_LED (node))
    {
        PnColor color;

        pn_led_peek_color (PN_LED (node), &color);

        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "led");
        json_builder_set_member_name (b, "lit");
        json_builder_add_boolean_value (b, pn_led_get_lit (PN_LED (node)));
        json_builder_set_member_name (b, "color");
        engine_add_color_array (b, &color);
        return "led";
    }

    if (PN_IS_MATRIX57 (node))
    {
        PnMatrix57PaintState st;

        pn_matrix57_get_paint_state (PN_MATRIX57 (node), &st);

        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "matrix57");
        json_builder_set_member_name (b, "text");
        json_builder_add_string_value (b, st.text);
        json_builder_set_member_name (b, "cells");
        json_builder_add_int_value (b, st.cells);
        json_builder_set_member_name (b, "lines");
        json_builder_add_int_value (b, st.lines);
        json_builder_set_member_name (b, "bg");
        engine_add_color_array (b, &st.background_color);
        json_builder_set_member_name (b, "pixel");
        engine_add_color_array (b, &st.pixel_color);
        json_builder_set_member_name (b, "unlit");
        engine_add_color_array (b, &st.unlit_pixel_color);
        return "matrix57";
    }

    if (PN_IS_NUMERIC (node))
    {
        PnNumericPaintState st;

        pn_numeric_get_paint_state (PN_NUMERIC (node), &st);

        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "numeric");
        json_builder_set_member_name (b, "value");
        json_builder_add_double_value (b, st.value);
        json_builder_set_member_name (b, "has_value");
        json_builder_add_boolean_value (b, st.has_value);
        json_builder_set_member_name (b, "digits");
        json_builder_add_int_value (b, st.digits);
        json_builder_set_member_name (b, "decimal_places");
        json_builder_add_int_value (b, st.decimal_places);
        /* RGBA so the panel widget can fade the segments into the panel's
         * transparent background when the user picks translucent colours. */
        json_builder_set_member_name (b, "segment_color");
        engine_add_rgba_array (b, &st.segment_color);
        json_builder_set_member_name (b, "unlit_color");
        engine_add_rgba_array (b, &st.unlit_segment_color);
        return "numeric";
    }

    if (PN_IS_SWITCH (node))
    {
        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "switch");
        json_builder_set_member_name (b, "on");
        json_builder_add_boolean_value (b, pn_switch_get_on (PN_SWITCH (node)));
        return "switch";
    }

    if (PN_IS_INJECT (node))
    {
        const gchar *icon = pn_inject_get_button_icon (PN_INJECT (node));

        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "injector");
        /* Empty string when no icon is configured — the panel widget
         * falls back to its cairo-drawn play triangle, mirroring the
         * worksheet's compact tab. */
        json_builder_set_member_name (b, "icon");
        json_builder_add_string_value (b, icon != NULL ? icon : "");
        return "injector";
    }

    if (engine_is_plot_node (node))
    {
        gint   w, h;
        gchar *png;

        engine_plot_size (flow, node, &w, &h);
        png = engine_render_plot_png (node, w, h);

        json_builder_set_member_name (b, "kind");
        json_builder_add_string_value (b, "plot");
        /* The size travels with the picture rather than with the layout
         * entry, so the widget reserves the right area even before the
         * first render arrives, and a resized plot can never be shown
         * with its old picture stretched into a new frame. */
        json_builder_set_member_name (b, "w");
        json_builder_add_int_value (b, w);
        json_builder_set_member_name (b, "h");
        json_builder_add_int_value (b, h);
        json_builder_set_member_name (b, "png");
        json_builder_add_string_value (b, png);
        g_free (png);
        return "plot";
    }

    return NULL;
}

/** Serialize the JsonBuilder's root to a string and tear it all down. */
static gchar *
engine_builder_to_string (JsonBuilder *b)
{
    JsonNode      *root = json_builder_get_root (b);
    JsonGenerator *gen  = json_generator_new ();
    gchar         *out;

    json_generator_set_root (gen, root);
    out = json_generator_to_data (gen, NULL);

    json_node_free (root);
    g_object_unref (gen);
    g_object_unref (b);
    return out;
}

/** @node's render state as a standalone JSON object string (the
 *  WidgetChanged payload, and the per-widget "state" in the layout). */
static gchar *
engine_widget_state_json (PnFlow *flow, PnNode *node)
{
    JsonBuilder *b = json_builder_new ();

    json_builder_begin_object (b);
    if (engine_add_widget_state (b, flow, node) == NULL)
    {
        json_builder_end_object (b);
        g_object_unref (b);
        return g_strdup ("{}");
    }
    json_builder_end_object (b);

    return engine_builder_to_string (b);
}

/* One representable node placed for a layout.  The panel builder sorts by
 * @x and leaves @y at 0; the desktop builder fills both with the node's
 * window-relative placement. */
typedef struct
{
    PnNode      *node;
    const gchar *uuid;
    gdouble      x;
    gdouble      y;
} EngineLayoutEntry;

/** Left-to-right by x; ties broken by UUID so the order is stable. */
static gint
engine_layout_entry_cmp (gconstpointer a, gconstpointer b)
{
    const EngineLayoutEntry *ea = a;
    const EngineLayoutEntry *eb = b;

    if (ea->x < eb->x) return -1;
    if (ea->x > eb->x) return 1;
    return g_strcmp0 (ea->uuid, eb->uuid);
}

/** The applet's widget set, order and placement for @win, as JSON:
 *    { "positioned": <bool>,
 *      "widgets": [ { "uuid", "order", "x", "state": {…} }, … ] }
 *
 *  Widgets are the representable nodes the layout editor snapped onto the
 *  panel band (a saved position whose y sits within the band), left to
 *  right by x.  "positioned" is %TRUE when the placement is the user's own
 *  band layout: then "x" is the widget's band x (offset so the leftmost is
 *  0), so the applet can mirror the spacing and grouping, not just order.
 *  When the document carries no panel layout at all (never arranged), fall
 *  back to every representable node in node-store order, "positioned" is
 *  %FALSE and "x" is 0 (the applet packs them) so the applet is not blank. */
static gchar *
engine_build_layout_json (PnWindow *win)
{
    PnFlow      *flow = win != NULL ? pn_window_get_flow (win) : NULL;
    PnNodeStore *nodes;
    GArray      *entries;
    GList       *placed;
    gboolean     have_layout;
    gdouble      min_x = 0.0;
    JsonBuilder *b;
    guint        i, n;

    if (flow == NULL)
        return g_strdup ("{\"widgets\":[]}");

    nodes = pn_flow_get_nodes (flow);

    placed      = pn_flow_list_panel_positions (flow);
    have_layout = (placed != NULL);
    g_list_free_full (placed, g_free);

    entries = g_array_new (FALSE, FALSE, sizeof (EngineLayoutEntry));
    n       = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode           *node = pn_node_store_get_node (nodes, i);
        EngineLayoutEntry e;
        gdouble           x = 0.0, y = 0.0;

        if (!engine_is_widget_node (node))
            continue;

        if (have_layout)
        {
            /* On the panel iff snapped to the band — same geometry the
             * editor snaps to (band height == widget height). */
            if (!pn_flow_get_panel_position (flow, pn_node_get_uuid (node),
                                             &x, &y))
                continue;
            if (ABS (y - PN_PE_PANEL_TOP) > PN_PE_PANEL_HEIGHT / 2.0)
                continue;
        }
        else
        {
            x = (gdouble) i;   /* fallback: keep node-store order */
        }

        e.node = node;
        e.uuid = pn_node_get_uuid (node);
        e.x    = x;
        e.y    = 0.0;          /* unused on the panel: the band fixes y */
        g_array_append_val (entries, e);
    }

    if (have_layout)
        g_array_sort (entries, engine_layout_entry_cmp);

    /* After the sort the first entry carries the smallest band x; offset
     * every placement by it so the leftmost widget lands at 0 and no panel
     * space is wasted on a left margin. */
    if (have_layout && entries->len > 0)
        min_x = g_array_index (entries, EngineLayoutEntry, 0).x;

    b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "positioned");
    json_builder_add_boolean_value (b, have_layout);
    json_builder_set_member_name (b, "widgets");
    json_builder_begin_array (b);
    for (i = 0; i < entries->len; i++)
    {
        EngineLayoutEntry *e = &g_array_index (entries, EngineLayoutEntry, i);

        json_builder_begin_object (b);
        json_builder_set_member_name (b, "uuid");
        json_builder_add_string_value (b, e->uuid);
        json_builder_set_member_name (b, "order");
        json_builder_add_int_value (b, i);
        json_builder_set_member_name (b, "x");
        json_builder_add_double_value (b, have_layout ? e->x - min_x : 0.0);
        json_builder_set_member_name (b, "state");
        json_builder_begin_object (b);
        engine_add_widget_state (b, flow, e->node);
        json_builder_end_object (b);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    json_builder_end_object (b);

    g_array_free (entries, TRUE);
    return engine_builder_to_string (b);
}

/** The desktop viewer's widget set for @win, as JSON:
 *    { "positioned": <bool>,
 *      "window": { "width", "height", "title", "widget_height" },
 *      "widgets": [ { "uuid", "order", "x", "y", "state": {…} }, … ] }
 *
 *  The desktop twin of engine_build_layout_json above.  Membership is
 *  simpler than the panel's band test: a node is in the window iff the
 *  desktop layout editor stored a placement for it, and "x"/"y" are that
 *  placement verbatim — window-relative, so the viewer paints with them
 *  directly.  When the document carries no desktop layout at all (never
 *  arranged), fall back to every representable node cascaded down the
 *  window the way the editor would place them, with "positioned" %FALSE,
 *  so a viewer opened on an unarranged worksheet is not blank.
 *
 *  "title" is what the document set, or "" — the viewer falls back to the
 *  worksheet's own file name, which is what the editor's placeholder
 *  promises. */
static gchar *
engine_build_desktop_layout_json (PnWindow *win)
{
    PnFlow      *flow = win != NULL ? pn_window_get_flow (win) : NULL;
    PnNodeStore *nodes;
    GArray      *entries;
    GList       *placed;
    gboolean     have_layout;
    JsonBuilder *b;
    gint         win_w = PN_DE_WINDOW_DEFAULT_WIDTH;
    gint         win_h = PN_DE_WINDOW_DEFAULT_HEIGHT;
    guint        i, n;
    guint        cascade = 0;

    if (flow == NULL)
        return g_strdup ("{\"widgets\":[]}");

    nodes = pn_flow_get_nodes (flow);
    pn_flow_get_desktop_window (flow, &win_w, &win_h);

    placed      = pn_flow_list_desktop_positions (flow);
    have_layout = (placed != NULL);
    g_list_free_full (placed, g_free);

    entries = g_array_new (FALSE, FALSE, sizeof (EngineLayoutEntry));
    n       = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode           *node = pn_node_store_get_node (nodes, i);
        EngineLayoutEntry e;
        gdouble           x = 0.0, y = 0.0;

        if (!engine_is_mirrored_node (node))
            continue;

        if (have_layout)
        {
            /* In the window iff the desktop editor placed it there. */
            if (!pn_flow_get_desktop_position (flow, pn_node_get_uuid (node),
                                               &x, &y))
                continue;
        }
        else
        {
            /* The editor's own cascade: down a column, wrapping to the
             * next once one fills, so the fallback matches what opening
             * the Desktop Layout tab would show. */
            const gint step_y  = PN_DE_WIDGET_HEIGHT + 28;
            const gint step_x  = 170;
            gint       per_col = MAX (1, win_h / step_y);

            x = (cascade / per_col) * step_x;
            y = (cascade % per_col) * step_y;
            cascade++;
        }

        e.node = node;
        e.uuid = pn_node_get_uuid (node);
        e.x    = x;
        e.y    = y;
        g_array_append_val (entries, e);
    }

    b = json_builder_new ();
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "positioned");
    json_builder_add_boolean_value (b, have_layout);

    json_builder_set_member_name (b, "window");
    json_builder_begin_object (b);
    json_builder_set_member_name (b, "width");
    json_builder_add_int_value (b, win_w);
    json_builder_set_member_name (b, "height");
    json_builder_add_int_value (b, win_h);
    json_builder_set_member_name (b, "title");
    json_builder_add_string_value (b, pn_flow_get_desktop_title (flow));
    /* The height every widget is drawn at.  Published rather than shared
     * as a header constant so the viewer stays a pure protocol client with
     * no pipnode headers at all — and it cannot drift from the editor,
     * which places widgets at this same size. */
    json_builder_set_member_name (b, "widget_height");
    json_builder_add_int_value (b, PN_DE_WIDGET_HEIGHT);
    json_builder_end_object (b);

    json_builder_set_member_name (b, "widgets");
    json_builder_begin_array (b);
    for (i = 0; i < entries->len; i++)
    {
        EngineLayoutEntry *e = &g_array_index (entries, EngineLayoutEntry, i);

        json_builder_begin_object (b);
        json_builder_set_member_name (b, "uuid");
        json_builder_add_string_value (b, e->uuid);
        json_builder_set_member_name (b, "order");
        json_builder_add_int_value (b, i);
        json_builder_set_member_name (b, "x");
        json_builder_add_double_value (b, e->x);
        json_builder_set_member_name (b, "y");
        json_builder_add_double_value (b, e->y);
        json_builder_set_member_name (b, "state");
        json_builder_begin_object (b);
        engine_add_widget_state (b, flow, e->node);
        json_builder_end_object (b);
        json_builder_end_object (b);
    }
    json_builder_end_array (b);
    json_builder_end_object (b);

    g_array_free (entries, TRUE);
    return engine_builder_to_string (b);
}

/** Push @node's fresh state to every viewer of ctx's worksheet. */
static void
engine_push_widget_state (EngineWidgetCtx *ctx, PnNode *node)
{
    PnFlow *flow = ctx->win != NULL ? pn_window_get_flow (ctx->win) : NULL;
    gchar  *json = engine_widget_state_json (flow, node);

    engine_emit_signal (ctx->app, "WidgetChanged",
                        g_variant_new ("(sss)", ctx->path,
                                       pn_node_get_uuid (node), json));
    g_free (json);
}

/* ------------------------------------------------------------------ */
/*  Coalescing plot pushes                                             */
/*                                                                     */
/*  Every other widget state is a handful of numbers, so a repaint can  */
/*  be forwarded the moment it happens.  A plot state carries a         */
/*  rendered PNG: the engine runs the node's PLplot / MathGL / Pango    */
/*  painter over an image surface and base64s the result.  That is far  */
/*  too expensive to do once per message on a busy feed, and pointless  */
/*  besides — nobody can read more than a few frames a second.          */
/*                                                                     */
/*  So a plot's repaint schedules one push and swallows every repaint   */
/*  until it fires.  The pending source id lives on the node itself, so */
/*  a node destroyed mid-wait cancels its own push rather than leaving  */
/*  a timeout holding a freed pointer.                                  */
/* ------------------------------------------------------------------ */

/* Longest a plot may lag the data behind it.  ~5 frames a second: fast
 * enough to read as live, slow enough that a graph fed at message rate
 * costs a bounded amount of CPU. */
#define PN_ENGINE_PLOT_PUSH_MS  200

#define PN_ENGINE_PLOT_PENDING_QDATA  "pn-engine-plot-pending"

typedef struct
{
    EngineWidgetCtx *ctx;   /* not owned; outlives the timeout */
    PnNode          *node;  /* not owned; cancels this on destroy */
} EnginePlotPush;

/* Whether a desktop window would actually show @node's plot: it was
 * placed in the desktop layout, or the document has no desktop layout at
 * all and the viewer is therefore falling back to every widget node. */
static gboolean
engine_plot_is_shown (EngineWidgetCtx *ctx, PnNode *node)
{
    PnFlow   *flow = ctx->win != NULL ? pn_window_get_flow (ctx->win) : NULL;
    GList    *placed;
    gboolean  have_layout;

    if (flow == NULL)
        return FALSE;

    placed      = pn_flow_list_desktop_positions (flow);
    have_layout = (placed != NULL);
    g_list_free_full (placed, g_free);

    if (!have_layout)
        return TRUE;

    return pn_flow_get_desktop_position (flow, pn_node_get_uuid (node),
                                         NULL, NULL);
}

/* The timeout's data destroy — runs whether it fired or was cancelled. */
static void
engine_plot_push_free (EnginePlotPush *push)
{
    g_slice_free (EnginePlotPush, push);
}

/* Set as the pending id's destroy notify: a node finalized (or unwired)
 * while a push is queued drops the timeout with it. */
static void
engine_plot_pending_cancel (gpointer data)
{
    guint id = GPOINTER_TO_UINT (data);

    if (id != 0)
        g_source_remove (id);
}

static gboolean
on_engine_plot_push (gpointer user_data)
{
    EnginePlotPush *push = user_data;

    /* Steal rather than set-to-NULL: stealing skips the destroy notify,
     * which would try to remove the source we are currently inside. */
    g_object_steal_data (G_OBJECT (push->node),
                         PN_ENGINE_PLOT_PENDING_QDATA);

    engine_push_widget_state (push->ctx, push->node);
    return G_SOURCE_REMOVE;
}

/** repaint-needed on a widget node → push its fresh state to the applet
 *  and to any desktop viewer.  Plots go through the coalescer above; every
 *  other kind is cheap enough to forward straight away. */
static void
on_panel_widget_repaint (PnNode *node, gpointer user_data)
{
    EngineWidgetCtx *ctx = user_data;
    EnginePlotPush  *push;
    guint            id;

    if (!engine_is_plot_node (node))
    {
        engine_push_widget_state (ctx, node);
        return;
    }

    /* Rendering costs real work, so do not do it for a plot no window
     * shows.  A document with a desktop layout shows exactly the nodes it
     * placed; one with no layout at all falls back to showing everything,
     * and then every plot is on screen. */
    if (!engine_plot_is_shown (ctx, node))
        return;

    /* A push is already queued: this repaint is folded into it. */
    if (g_object_get_data (G_OBJECT (node),
                           PN_ENGINE_PLOT_PENDING_QDATA) != NULL)
        return;

    push       = g_slice_new0 (EnginePlotPush);
    push->ctx  = ctx;
    push->node = node;

    id = g_timeout_add_full (G_PRIORITY_DEFAULT, PN_ENGINE_PLOT_PUSH_MS,
                             on_engine_plot_push, push,
                             (GDestroyNotify) engine_plot_push_free);
    g_object_set_data_full (G_OBJECT (node), PN_ENGINE_PLOT_PENDING_QDATA,
                            GUINT_TO_POINTER (id),
                            engine_plot_pending_cancel);
}

/** Re-publish the whole layout for ctx's worksheet to the applet. */
static void
engine_emit_layout_changed (EngineWidgetCtx *ctx)
{
    gchar *json = engine_build_layout_json (ctx->win);

    engine_emit_signal (ctx->app, "LayoutChanged",
                        g_variant_new ("(ss)", ctx->path, json));
    g_free (json);
}

/** Re-publish the whole desktop layout for ctx's worksheet to any viewer. */
static void
engine_emit_desktop_layout_changed (EngineWidgetCtx *ctx)
{
    gchar *json = engine_build_desktop_layout_json (ctx->win);

    engine_emit_signal (ctx->app, "DesktopLayoutChanged",
                        g_variant_new ("(ss)", ctx->path, json));
    g_free (json);
}

/** Follow a widget node's live value to the applet. */
static void
engine_wire_widget_node (EngineWidgetCtx *ctx, PnNode *node)
{
    g_signal_connect (node, "repaint-needed",
                      G_CALLBACK (on_panel_widget_repaint), ctx);
}

/* A node appeared at runtime: start mirroring it and refresh the layout. */
static void
on_engine_node_added (PnNodeStore *store,
                      PnNode      *node,
                      guint        index,
                      gpointer     user_data)
{
    EngineWidgetCtx *ctx = user_data;

    (void) store; (void) index;
    if (!engine_is_mirrored_node (node))
        return;
    engine_wire_widget_node (ctx, node);
    engine_emit_layout_changed (ctx);
    engine_emit_desktop_layout_changed (ctx);
}

/* A node went away: its repaint handler dies with it; refresh the layout. */
static void
on_engine_node_removed (PnNodeStore *store,
                        PnNode      *node,
                        guint        index,
                        gpointer     user_data)
{
    EngineWidgetCtx *ctx = user_data;

    (void) store; (void) index;
    if (!engine_is_mirrored_node (node))
        return;
    engine_emit_layout_changed (ctx);
    engine_emit_desktop_layout_changed (ctx);
}

/* The user dragged a widget in the presented editor (it shares this same
 * hidden flow), changing what is on the band: re-publish the layout. */
static void
on_engine_panel_layout_changed (PnFlow *flow, gpointer user_data)
{
    (void) flow;
    engine_emit_layout_changed ((EngineWidgetCtx *) user_data);
}

/* The desktop window changed in the presented editor (it shares this same
 * hidden flow): a widget moved, or the window was resized or retitled.
 * Re-publish the desktop layout. */
static void
on_engine_desktop_layout_changed (PnFlow *flow, gpointer user_data)
{
    (void) flow;
    engine_emit_desktop_layout_changed ((EngineWidgetCtx *) user_data);
}

/** Wire @win so every viewer mirrors it: per-node widget state, both
 *  surfaces' layout changes, plus the legacy single-value Panel Display
 *  channel. */
static void
engine_wire_panel_widgets (
        PnApplication *self,
        PnWindow      *win,
        const gchar   *path)
{
    PnFlow          *flow = pn_window_get_flow (win);
    PnNodeStore     *nodes;
    EngineWidgetCtx *ctx;
    guint            i, n;

    if (flow == NULL)
        return;

    nodes = pn_flow_get_nodes (flow);

    /* Legacy Panel Display -> ValueChanged channel, kept alongside. */
    engine_wire_displays (self, win, path);

    ctx       = g_slice_new0 (EngineWidgetCtx);
    ctx->app  = self;
    ctx->win  = win;
    ctx->path = g_strdup (path);
    g_object_set_data_full (G_OBJECT (win), PN_ENGINE_WIDGET_CTX_QDATA,
                            ctx, engine_widget_ctx_free);

    n = pn_node_store_get_length (nodes);
    for (i = 0; i < n; i++)
    {
        PnNode *node = pn_node_store_get_node (nodes, i);
        if (engine_is_mirrored_node (node))
            engine_wire_widget_node (ctx, node);
    }

    g_signal_connect (nodes, "node-added",
                      G_CALLBACK (on_engine_node_added), ctx);
    g_signal_connect (nodes, "node-removed",
                      G_CALLBACK (on_engine_node_removed), ctx);
    g_signal_connect (flow, "panel-layout-changed",
                      G_CALLBACK (on_engine_panel_layout_changed), ctx);
    g_signal_connect (flow, "desktop-layout-changed",
                      G_CALLBACK (on_engine_desktop_layout_changed), ctx);

    g_signal_connect (win, "destroy",
                      G_CALLBACK (on_engine_window_destroy), NULL);
}

/** Stop mirroring @win before it is torn down: drop the store/flow
 *  subscriptions so node-removed emitted during teardown cannot fire into
 *  a half-destroyed worksheet.  Per-node repaint handlers die with the
 *  nodes.  The ctx itself is freed when the window's qdata is cleared.
 *  Idempotent: safe to call from CloseWorksheet and again on destroy. */
static void
engine_unwire_panel_widgets (PnWindow *win)
{
    PnFlow          *flow = pn_window_get_flow (win);
    EngineWidgetCtx *ctx  = g_object_get_data (G_OBJECT (win),
                                               PN_ENGINE_WIDGET_CTX_QDATA);

    if (flow == NULL || ctx == NULL)
        return;

    g_signal_handlers_disconnect_by_data (pn_flow_get_nodes (flow), ctx);
    g_signal_handlers_disconnect_by_data (flow,                     ctx);

    /* Drop any queued plot render: the ctx it would push through is about
     * to go, and nothing is left to show the picture anyway.  Clearing the
     * qdata runs its destroy notify, which removes the source. */
    {
        PnNodeStore *nodes = pn_flow_get_nodes (flow);
        guint        i, n  = pn_node_store_get_length (nodes);

        for (i = 0; i < n; i++)
            g_object_set_data (G_OBJECT (pn_node_store_get_node (nodes, i)),
                               PN_ENGINE_PLOT_PENDING_QDATA, NULL);
    }
}

/* Cover the app-shutdown path, where windows are destroyed without going
 * through CloseWorksheet. */
static void
on_engine_window_destroy (GtkWidget *win, gpointer user_data)
{
    (void) user_data;
    engine_unwire_panel_widgets (PN_WINDOW (win));
}

/** Return the panel-backed window for @path, creating and loading it on
 *  first use.  %NULL with @error set when the worksheet fails to load. */
static PnWindow *
engine_ensure_worksheet (
        PnApplication *self,
        const gchar   *path,
        GError       **error)
{
    PnWindow *win = g_hash_table_lookup (self->worksheets, path);

    if (win != NULL)
        return win;

    win = pn_window_new_panel (self);
    if (!pn_window_load_file (win, path, error))
    {
        gtk_widget_destroy (GTK_WIDGET (win));
        return NULL;
    }

    g_hash_table_insert (self->worksheets, g_strdup (path), win);
    engine_wire_panel_widgets (self, win, path);
    return win;
}

/* Deferred quit for the Quit method: run once control is back in the main
 * loop, so the method reply and the D-Bus name release have flushed before
 * the process exits.  g_application_quit returns g_application_run despite
 * the service hold taken in startup(). */
static gboolean
engine_quit_idle (gpointer user_data)
{
    g_application_quit (G_APPLICATION (user_data));
    return G_SOURCE_REMOVE;
}

static void
handle_engine_method_call (
        GDBusConnection       *connection,
        const gchar           *sender,
        const gchar           *object_path,
        const gchar           *interface_name,
        const gchar           *method_name,
        GVariant              *parameters,
        GDBusMethodInvocation *invocation,
        gpointer               user_data)
{
    PnApplication *self = PN_APPLICATION (user_data);
    const gchar   *path = NULL;

    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;

    if (g_strcmp0 (method_name, "RunWorksheet") == 0)
    {
        GError   *err = NULL;
        PnWindow *win;
        gchar    *value;

        g_variant_get (parameters, "(&s)", &path);
        win = engine_ensure_worksheet (self, path, &err);
        if (win == NULL)
        {
            g_dbus_method_invocation_return_error (
                    invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Could not run worksheet '%s': %s", path,
                    err != NULL ? err->message : "(unknown error)");
            g_clear_error (&err);
            return;
        }

        value = engine_display_value (win);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", value));
        g_free (value);
    }
    else if (g_strcmp0 (method_name, "GetDisplayValue") == 0)
    {
        PnWindow *win;
        gchar    *value;

        g_variant_get (parameters, "(&s)", &path);
        win   = g_hash_table_lookup (self->worksheets, path);
        value = engine_display_value (win);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", value));
        g_free (value);
    }
    else if (g_strcmp0 (method_name, "GetLayout") == 0)
    {
        PnWindow *win;
        gchar    *layout;

        g_variant_get (parameters, "(&s)", &path);
        win    = g_hash_table_lookup (self->worksheets, path);
        layout = engine_build_layout_json (win);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", layout));
        g_free (layout);
    }
    else if (g_strcmp0 (method_name, "GetDesktopLayout") == 0)
    {
        PnWindow *win;
        gchar    *layout;

        g_variant_get (parameters, "(&s)", &path);
        win    = g_hash_table_lookup (self->worksheets, path);
        layout = engine_build_desktop_layout_json (win);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", layout));
        g_free (layout);
    }
    else if (g_strcmp0 (method_name, "SetInput") == 0)
    {
        PnWindow *win;
        PnNode   *node;
        gdouble   value;

        g_variant_get (parameters, "(&sd)", &path, &value);
        win  = g_hash_table_lookup (self->worksheets, path);
        node = engine_find_node (win, PN_TYPE_PANEL_INPUT);
        if (node != NULL)
            pn_panel_input_send (PN_PANEL_INPUT (node), value);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "SendEvent") == 0)
    {
        PnWindow    *win;
        const gchar *event  = NULL;
        guint        button = 0;

        g_variant_get (parameters, "(&s&su)", &path, &event, &button);
        win = g_hash_table_lookup (self->worksheets, path);
        engine_send_event_to_inputs (win, event, button);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "ActivateWidget") == 0)
    {
        PnWindow    *win;
        PnNode      *node;
        const gchar *uuid = NULL;

        g_variant_get (parameters, "(&s&s)", &path, &uuid);
        win  = g_hash_table_lookup (self->worksheets, path);
        node = engine_find_node_by_uuid (win, uuid);
        /* The two interactive kinds.  A switch toggle flips and re-emits a
         * repaint-needed → WidgetChanged carrying the new "on" state; an
         * injector fires its message once (no state to mirror back). */
        if (PN_IS_SWITCH (node))
            pn_switch_toggle (PN_SWITCH (node));
        else if (PN_IS_INJECT (node))
            pn_inject_fire (PN_INJECT (node));
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "PresentEditor") == 0)
    {
        GError   *err = NULL;
        PnWindow *win;

        g_variant_get (parameters, "(&s)", &path);
        win = engine_ensure_worksheet (self, path, &err);
        if (win == NULL)
        {
            g_dbus_method_invocation_return_error (
                    invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED,
                    "Could not open worksheet '%s': %s", path,
                    err != NULL ? err->message : "(unknown error)");
            g_clear_error (&err);
            return;
        }

        gtk_window_present (GTK_WINDOW (win));
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "CloseWorksheet") == 0)
    {
        PnWindow *win;

        g_variant_get (parameters, "(&s)", &path);
        win = g_hash_table_lookup (self->worksheets, path);
        if (win != NULL)
        {
            /* Drop it for good: autosave, forget, then destroy (which
             * stops the flow).  Remove from the table first so nothing
             * re-resolves it mid-teardown, and drop the mirroring
             * subscriptions so the node-removed cascade during teardown
             * cannot fire into a half-destroyed worksheet. */
            pn_window_autosave (win);
            engine_unwire_panel_widgets (win);
            g_hash_table_remove (self->worksheets, path);
            gtk_widget_destroy (GTK_WIDGET (win));
        }
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "Quit") == 0)
    {
        GHashTableIter it;
        gpointer       key, val;

        /* Persist every running worksheet first, so bouncing the engine
         * (e.g. to pick up a freshly installed binary) loses nothing. */
        g_hash_table_iter_init (&it, self->worksheets);
        while (g_hash_table_iter_next (&it, &key, &val))
            pn_window_autosave (PN_WINDOW (val));

        /* Answer before exiting; defer the actual quit so the reply and the
         * name release flush, letting clients re-activate a fresh engine. */
        g_dbus_method_invocation_return_value (invocation, NULL);
        g_idle_add (engine_quit_idle, self);
    }
    else
    {
        g_dbus_method_invocation_return_error (
                invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD,
                "Unknown method %s", method_name);
    }
}

static const GDBusInterfaceVTable engine_vtable = {
    handle_engine_method_call,
    NULL,
    NULL
};

static GDBusNodeInfo *engine_introspection_data = NULL;

/* ================================================================== */
/*  Live-observation signal bridge (TODO #40.14)                       */
/*                                                                     */
/*  What makes the automation interface INTERACTIVE: instead of        */
/*  polling, a client subscribes to D-Bus signals and reacts to edits  */
/*  — its own or the user's.  This bridge translates the model's       */
/*  GObject notifications into the .Worksheet / .Editor signals         */
/*  declared in the introspection XML.                                 */
/*                                                                     */
/*  The flow owns one shared node/wire store across every sheet, so a  */
/*  single set of store + flow connections per window covers the whole */
/*  document; per-node "notify"/"message" handlers are attached as      */
/*  nodes are added and dropped as they are removed.  One bridge lives */
/*  per #PnWindow for its lifetime (attached on window-added, torn down */
/*  on window-removed).                                                */
/* ================================================================== */

#define PN_WS_IFACE "org.pipas.pipnode.Worksheet"
#define PN_ED_IFACE "org.pipas.pipnode.Editor"

typedef struct
{
    PnApplication *app;     /* borrowed */
    PnWindow      *window;  /* borrowed */
    PnFlow        *flow;    /* borrowed */
    PnNodeStore   *nodes;   /* borrowed */
    PnWireStore   *wires;   /* borrowed */
    gulong         node_added_id;
    gulong         node_removed_id;
    gulong         wire_added_id;
    gulong         wire_removed_id;
    gulong         active_sheet_id;
    gulong         modified_id;
    PnWorksheet   *sel_ws;  /* weak pointer; the selection-bridged sheet */
    gulong         sel_id;
} PnSignalBridge;

/** Emit @signal_name on @iface at the application object path, consuming
 *  the floating @params.  A no-op (but still consuming @params) when the
 *  app is not on a bus — keeps every caller a single unconditional line. */
static void
automation_emit_signal (PnApplication *self,
                        const gchar   *iface,
                        const gchar   *signal_name,
                        GVariant      *params)
{
    GApplication    *app  = G_APPLICATION (self);
    GDBusConnection *conn = g_application_get_dbus_connection (app);
    const gchar     *obj  = g_application_get_dbus_object_path (app);

    if (conn == NULL || obj == NULL)
    {
        if (params != NULL)
            g_variant_unref (g_variant_ref_sink (params));
        return;
    }

    g_dbus_connection_emit_signal (conn, NULL, obj, iface, signal_name,
                                   params, NULL);
}

/* --- per-node notifications ------------------------------------------ */

static void
on_bridge_node_notify (GObject    *object,
                       GParamSpec *pspec,
                       gpointer    user_data)
{
    PnSignalBridge *b     = user_data;
    PnNode         *node  = PN_NODE (object);
    const gchar    *uuid  = pn_node_get_uuid (node);
    const gchar    *pname = pspec != NULL ? g_param_spec_get_name (pspec) : "";

    if (uuid == NULL)
        uuid = "";

    /* "position" and "name" get their own, richer signals; everything
     * else collapses onto the generic NodePropertyChanged so each model
     * change maps to exactly one D-Bus signal. */
    if (g_strcmp0 (pname, "position") == 0)
    {
        const PnPoint *p = pn_node_get_position (node);
        automation_emit_signal (
                b->app, PN_WS_IFACE, "NodeMoved",
                g_variant_new ("(sdd)", uuid,
                               p ? p->x : 0.0, p ? p->y : 0.0));
    }
    else if (g_strcmp0 (pname, "name") == 0)
    {
        const gchar *nm = pn_node_get_name (node);
        automation_emit_signal (
                b->app, PN_WS_IFACE, "NodeRenamed",
                g_variant_new ("(ss)", uuid, nm ? nm : ""));
    }
    else
    {
        automation_emit_signal (
                b->app, PN_WS_IFACE, "NodePropertyChanged",
                g_variant_new ("(ss)", uuid, pname ? pname : ""));
    }
}

static void
on_bridge_node_message (PnNode    *node,
                        PnMessage *message,
                        gpointer   user_data)
{
    PnSignalBridge *b     = user_data;
    const gchar    *uuid  = pn_node_get_uuid (node);
    const gchar    *topic = pn_message_get_topic (message);
    gchar          *json  = pn_message_serialize (message, FALSE);

    automation_emit_signal (
            b->app, PN_WS_IFACE, "MessageEmitted",
            g_variant_new ("(sss)", uuid ? uuid : "",
                           topic ? topic : "", json));
    g_free (json);
}

static void
bridge_connect_node (PnSignalBridge *b, PnNode *node)
{
    g_signal_connect (node, "notify",
                      G_CALLBACK (on_bridge_node_notify), b);
    g_signal_connect (node, "message",
                      G_CALLBACK (on_bridge_node_message), b);
}

/* --- store + flow notifications -------------------------------------- */

static void
on_bridge_node_added (PnNodeStore *store, PnNode *node,
                      guint index, gpointer user_data)
{
    PnSignalBridge *b    = user_data;
    const gchar    *uuid = pn_node_get_uuid (node);
    const gchar    *type = G_OBJECT_TYPE_NAME (node);

    (void) store; (void) index;

    bridge_connect_node (b, node);
    automation_emit_signal (
            b->app, PN_WS_IFACE, "NodeAdded",
            g_variant_new ("(ss)", uuid ? uuid : "", type ? type : ""));
}

static void
on_bridge_node_removed (PnNodeStore *store, PnNode *node,
                        guint index, gpointer user_data)
{
    PnSignalBridge *b    = user_data;
    const gchar    *uuid = pn_node_get_uuid (node);

    (void) store; (void) index;

    g_signal_handlers_disconnect_by_data (node, b);
    automation_emit_signal (
            b->app, PN_WS_IFACE, "NodeRemoved",
            g_variant_new ("(s)", uuid ? uuid : ""));
}

static void
on_bridge_wire_added (PnWireStore *store, PnWire *wire,
                      guint index, gpointer user_data)
{
    PnSignalBridge *b   = user_data;
    PnNode         *src = pn_wire_get_source (wire);
    PnNode         *tgt = pn_wire_get_target (wire);
    const gchar    *wu  = pn_wire_get_uuid (wire);
    const gchar    *su  = src != NULL ? pn_node_get_uuid (src) : NULL;
    const gchar    *tu  = tgt != NULL ? pn_node_get_uuid (tgt) : NULL;

    (void) store; (void) index;

    automation_emit_signal (
            b->app, PN_WS_IFACE, "WireAdded",
            g_variant_new ("(sssi)", wu ? wu : "", su ? su : "",
                           tu ? tu : "", pn_wire_get_target_input (wire)));
}

static void
on_bridge_wire_removed (PnWireStore *store, PnWire *wire,
                        guint index, gpointer user_data)
{
    PnSignalBridge *b  = user_data;
    const gchar    *wu = pn_wire_get_uuid (wire);

    (void) store; (void) index;

    automation_emit_signal (
            b->app, PN_WS_IFACE, "WireRemoved",
            g_variant_new ("(s)", wu ? wu : ""));
}

static void
on_bridge_selection_changed (PnWorksheet *ws, gpointer user_data)
{
    PnSignalBridge  *b     = user_data;
    gchar          **uuids = pn_worksheet_get_selection_uuids (ws);
    GVariantBuilder  builder;
    gchar          **p;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
    for (p = uuids; p != NULL && *p != NULL; p++)
        g_variant_builder_add (&builder, "s", *p);
    g_strfreev (uuids);

    automation_emit_signal (
            b->app, PN_WS_IFACE, "SelectionChanged",
            g_variant_new ("(as)", &builder));
}

/** Move the selection-changed connection onto the window's current active
 *  worksheet (each sheet tab is its own #PnWorksheet widget).  A weak
 *  pointer guards the bridged widget so a sheet closing under us nulls it
 *  rather than leaving a dangling handle to disconnect. */
static void
bridge_rebind_selection (PnSignalBridge *b)
{
    PnWorksheet *ws = pn_window_get_worksheet (b->window);

    if (ws == b->sel_ws)
        return;

    if (b->sel_ws != NULL)
    {
        if (b->sel_id != 0)
            g_signal_handler_disconnect (b->sel_ws, b->sel_id);
        g_object_remove_weak_pointer (G_OBJECT (b->sel_ws),
                                      (gpointer *) &b->sel_ws);
        b->sel_ws = NULL;
        b->sel_id = 0;
    }

    if (ws != NULL)
    {
        b->sel_ws = ws;
        g_object_add_weak_pointer (G_OBJECT (ws), (gpointer *) &b->sel_ws);
        b->sel_id = g_signal_connect (
                ws, "selection-changed",
                G_CALLBACK (on_bridge_selection_changed), b);
    }
}

static void
on_bridge_active_sheet_changed (PnFlow *flow, const gchar *name,
                                gpointer user_data)
{
    PnSignalBridge *b = user_data;

    (void) flow;

    automation_emit_signal (
            b->app, PN_ED_IFACE, "SheetChanged",
            g_variant_new ("(s)", name ? name : ""));
    bridge_rebind_selection (b);
}

static void
on_bridge_modified_changed (PnFlow *flow, gboolean modified,
                            gpointer user_data)
{
    PnSignalBridge *b = user_data;

    (void) flow;

    automation_emit_signal (
            b->app, PN_ED_IFACE, "DocumentModified",
            g_variant_new ("(b)", modified));
}

/* --- attach / detach ------------------------------------------------- */

static void
on_signal_bridge_window_removed (GtkApplication *gtkapp,
                                 GtkWindow      *window,
                                 gpointer        user_data);

/** Attach a fresh bridge to @window's flow and remember it on the window. */
static void
signal_bridge_attach (PnApplication *self, PnWindow *window)
{
    PnSignalBridge *b;
    PnFlow         *flow;
    guint           i, n;

    if (g_object_get_data (G_OBJECT (window), "pn-signal-bridge") != NULL)
        return;     /* already attached */

    flow = pn_window_get_flow (window);
    if (flow == NULL)
        return;

    b         = g_new0 (PnSignalBridge, 1);
    b->app    = self;
    b->window = window;
    b->flow   = flow;
    b->nodes  = pn_flow_get_nodes (flow);
    b->wires  = pn_flow_get_wires (flow);

    b->node_added_id = g_signal_connect (
            b->nodes, "node-added",
            G_CALLBACK (on_bridge_node_added), b);
    b->node_removed_id = g_signal_connect (
            b->nodes, "node-removed",
            G_CALLBACK (on_bridge_node_removed), b);
    b->wire_added_id = g_signal_connect (
            b->wires, "wire-added",
            G_CALLBACK (on_bridge_wire_added), b);
    b->wire_removed_id = g_signal_connect (
            b->wires, "wire-removed",
            G_CALLBACK (on_bridge_wire_removed), b);
    b->active_sheet_id = g_signal_connect (
            b->flow, "active-sheet-changed",
            G_CALLBACK (on_bridge_active_sheet_changed), b);
    b->modified_id = g_signal_connect (
            b->flow, "modified-changed",
            G_CALLBACK (on_bridge_modified_changed), b);

    /* Connect any nodes already present (e.g. a document opened at
     * launch); these pre-exist, so no NodeAdded is emitted for them. */
    n = pn_node_store_get_length (b->nodes);
    for (i = 0; i < n; i++)
        bridge_connect_node (b, pn_node_store_get_node (b->nodes, i));

    bridge_rebind_selection (b);

    g_object_set_data (G_OBJECT (window), "pn-signal-bridge", b);
}

/** Disconnect everything the bridge wired up and free it.  Called from
 *  window-removed while the flow is still alive. */
static void
signal_bridge_detach (PnWindow *window)
{
    PnSignalBridge *b = g_object_get_data (G_OBJECT (window),
                                           "pn-signal-bridge");
    guint i, n;

    if (b == NULL)
        return;

    g_signal_handler_disconnect (b->nodes, b->node_added_id);
    g_signal_handler_disconnect (b->nodes, b->node_removed_id);
    g_signal_handler_disconnect (b->wires, b->wire_added_id);
    g_signal_handler_disconnect (b->wires, b->wire_removed_id);
    g_signal_handler_disconnect (b->flow,  b->active_sheet_id);
    g_signal_handler_disconnect (b->flow,  b->modified_id);

    n = pn_node_store_get_length (b->nodes);
    for (i = 0; i < n; i++)
        g_signal_handlers_disconnect_by_data (
                pn_node_store_get_node (b->nodes, i), b);

    if (b->sel_ws != NULL)
    {
        if (b->sel_id != 0)
            g_signal_handler_disconnect (b->sel_ws, b->sel_id);
        g_object_remove_weak_pointer (G_OBJECT (b->sel_ws),
                                      (gpointer *) &b->sel_ws);
    }

    g_object_set_data (G_OBJECT (window), "pn-signal-bridge", NULL);
    g_free (b);
}

static void
on_signal_bridge_window_removed (GtkApplication *gtkapp,
                                 GtkWindow      *window,
                                 gpointer        user_data)
{
    (void) gtkapp; (void) user_data;
    if (PN_IS_WINDOW (window))
        signal_bridge_detach (PN_WINDOW (window));
}

static void
on_signal_bridge_window_added (GtkApplication *gtkapp,
                               GtkWindow      *window,
                               gpointer        user_data)
{
    (void) user_data;
    if (PN_IS_WINDOW (window))
        signal_bridge_attach (PN_APPLICATION (gtkapp), PN_WINDOW (window));
}

/* ================================================================== */
/*  org.pipas.pipnode.Devices — Devices-menu + open-dialog control     */
/*                                                                     */
/*  The Devices menu and every device-configuration dialog mirrored    */
/*  onto D-Bus.  ListProviders enumerates the menu; Present opens (or   */
/*  raises) a provider's dialog; the rest address an OPEN dialog by its */
/*  provider id through the open-dialog registry in pn-device-dialog.c. */
/*  Generic across every provider (Tasmota, Zigbee, Meshtastic, …) —    */
/*  the host names no device.  GetDeviceDetail forwards to the dialog's */
/*  optional plugin detail seam.  DevicesChanged fires whenever a       */
/*  dialog's device set / status / selection changes, so a client can   */
/*  watch a slow discovery (e.g. Tasmota) fill in live.                 */
/* ================================================================== */

#define PN_DEVICES_IFACE "org.pipas.pipnode.Devices"

static const gchar devices_introspection_xml[] =
    "<node>"
    "  <interface name='org.pipas.pipnode.Devices'>"
    "    <method name='ListProviders'>"
    "      <arg type='a(sss)' name='providers' direction='out'/>"
    "    </method>"
    "    <method name='Present'>"
    "      <arg type='s' name='id'    direction='in'/>"
    "      <arg type='b' name='shown' direction='out'/>"
    "    </method>"
    "    <method name='Close'>"
    "      <arg type='s' name='id'     direction='in'/>"
    "      <arg type='b' name='closed' direction='out'/>"
    "    </method>"
    "    <method name='ListDevices'>"
    "      <arg type='s'       name='id'      direction='in'/>"
    "      <arg type='a(ssss)' name='devices' direction='out'/>"
    "    </method>"
    "    <method name='GetStatus'>"
    "      <arg type='s' name='id'     direction='in'/>"
    "      <arg type='s' name='status' direction='out'/>"
    "    </method>"
    "    <method name='GetSelected'>"
    "      <arg type='s' name='id'       direction='in'/>"
    "      <arg type='s' name='deviceId' direction='out'/>"
    "    </method>"
    "    <method name='SelectDevice'>"
    "      <arg type='s' name='id'       direction='in'/>"
    "      <arg type='s' name='deviceId' direction='in'/>"
    "      <arg type='b' name='ok'       direction='out'/>"
    "    </method>"
    "    <method name='Scan'>"
    "      <arg type='s' name='id' direction='in'/>"
    "      <arg type='b' name='ok' direction='out'/>"
    "    </method>"
    "    <method name='GetDeviceDetail'>"
    "      <arg type='s' name='id'       direction='in'/>"
    "      <arg type='s' name='deviceId' direction='in'/>"
    "      <arg type='s' name='detail'   direction='out'/>"
    "    </method>"
    "    <signal name='DevicesChanged'>"
    "      <arg type='s' name='id'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

/** Resolve a provider id to its open dialog, or return an error to the
 *  caller and NULL when no dialog for that id is open. */
static PnDeviceDialog *
devices_require_open (const gchar *id, GDBusMethodInvocation *invocation)
{
    PnDeviceDialog *dialog = pn_device_dialog_lookup_open (id);

    if (dialog == NULL)
        g_dbus_method_invocation_return_error (
                invocation,
                PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
                "No open device dialog for provider '%s' "
                "(call Present first)", id != NULL ? id : "");
    return dialog;
}

static void
handle_devices_method_call (
        GDBusConnection       *connection,
        const gchar           *sender,
        const gchar           *object_path,
        const gchar           *interface_name,
        const gchar           *method_name,
        GVariant              *parameters,
        GDBusMethodInvocation *invocation,
        gpointer               user_data)
{
    GApplication *app = G_APPLICATION (user_data);

    (void) connection; (void) sender; (void) object_path;
    (void) interface_name;

    note_automation_activity (app, method_name);

    if (g_strcmp0 (method_name, "ListProviders") == 0)
    {
        GVariantBuilder b;
        GList          *providers, *l;

        g_variant_builder_init (&b, G_VARIANT_TYPE ("a(sss)"));
        providers = pn_device_provider_list ();
        for (l = providers; l != NULL; l = l->next)
        {
            PnDeviceProviderInfo *info = l->data;
            g_variant_builder_add (&b, "(sss)",
                                   info->id,
                                   info->display_name,
                                   info->icon_name != NULL
                                       ? info->icon_name : "");
        }
        g_list_free_full (providers,
                          (GDestroyNotify) pn_device_provider_info_free);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(sss))", &b));
        return;
    }

    if (g_strcmp0 (method_name, "Present") == 0)
    {
        const gchar *id = NULL;
        GtkWindow   *parent;
        gboolean     shown;

        g_variant_get (parameters, "(&s)", &id);
        /* Parent to the active editor window when there is one; a
         * parentless dialog is still fine (e.g. headless service). */
        parent = gtk_application_get_active_window (GTK_APPLICATION (app));
        shown  = pn_device_provider_present (id, parent);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", shown));
        return;
    }

    if (g_strcmp0 (method_name, "Close") == 0)
    {
        const gchar    *id = NULL;
        PnDeviceDialog *dialog;
        gboolean        closed = FALSE;

        g_variant_get (parameters, "(&s)", &id);
        dialog = pn_device_dialog_lookup_open (id);
        if (dialog != NULL)
        {
            gtk_widget_destroy (pn_device_dialog_get_dialog (dialog));
            closed = TRUE;
        }
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", closed));
        return;
    }

    if (g_strcmp0 (method_name, "ListDevices") == 0)
    {
        const gchar    *id = NULL;
        PnDeviceDialog *dialog;
        GVariantBuilder b;
        GPtrArray      *rows;
        guint           i;

        g_variant_get (parameters, "(&s)", &id);
        dialog = devices_require_open (id, invocation);
        if (dialog == NULL)
            return;

        g_variant_builder_init (&b, G_VARIANT_TYPE ("a(ssss)"));
        rows = pn_device_dialog_get_rows (dialog);
        for (i = 0; i < rows->len; i++)
        {
            PnDeviceRow *r = g_ptr_array_index (rows, i);
            g_variant_builder_add (&b, "(ssss)",
                                   r->id        != NULL ? r->id        : "",
                                   r->primary   != NULL ? r->primary   : "",
                                   r->secondary != NULL ? r->secondary : "",
                                   r->disabled_reason != NULL
                                       ? r->disabled_reason : "");
        }
        g_ptr_array_unref (rows);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(a(ssss))", &b));
        return;
    }

    if (g_strcmp0 (method_name, "GetStatus") == 0)
    {
        const gchar    *id = NULL;
        PnDeviceDialog *dialog;
        const gchar    *status;

        g_variant_get (parameters, "(&s)", &id);
        dialog = devices_require_open (id, invocation);
        if (dialog == NULL)
            return;
        status = pn_device_dialog_get_status (dialog);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", status != NULL ? status : ""));
        return;
    }

    if (g_strcmp0 (method_name, "GetSelected") == 0)
    {
        const gchar    *id = NULL;
        PnDeviceDialog *dialog;
        const gchar    *sel;

        g_variant_get (parameters, "(&s)", &id);
        dialog = devices_require_open (id, invocation);
        if (dialog == NULL)
            return;
        sel = pn_device_dialog_get_selected_id (dialog);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", sel != NULL ? sel : ""));
        return;
    }

    if (g_strcmp0 (method_name, "SelectDevice") == 0)
    {
        const gchar    *id = NULL, *device_id = NULL;
        PnDeviceDialog *dialog;
        gboolean        ok;

        g_variant_get (parameters, "(&s&s)", &id, &device_id);
        dialog = devices_require_open (id, invocation);
        if (dialog == NULL)
            return;
        pn_device_dialog_reselect_device (dialog, device_id);
        ok = (g_strcmp0 (pn_device_dialog_get_selected_id (dialog),
                         device_id) == 0);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", ok));
        return;
    }

    if (g_strcmp0 (method_name, "Scan") == 0)
    {
        const gchar    *id = NULL;
        PnDeviceDialog *dialog;

        g_variant_get (parameters, "(&s)", &id);
        dialog = devices_require_open (id, invocation);
        if (dialog == NULL)
            return;
        pn_device_dialog_scan (dialog);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(b)", TRUE));
        return;
    }

    if (g_strcmp0 (method_name, "GetDeviceDetail") == 0)
    {
        const gchar    *id = NULL, *device_id = NULL;
        PnDeviceDialog *dialog;
        gchar          *detail;

        g_variant_get (parameters, "(&s&s)", &id, &device_id);
        dialog = devices_require_open (id, invocation);
        if (dialog == NULL)
            return;
        detail = pn_device_dialog_get_device_detail (dialog, device_id);
        g_dbus_method_invocation_return_value (
                invocation, g_variant_new ("(s)", detail != NULL ? detail : ""));
        g_free (detail);
        return;
    }

    g_dbus_method_invocation_return_error (
            invocation, PN_WORKSHEET_ERROR, PN_WORKSHEET_ERROR_FAILED,
            "Unknown Devices method '%s'", method_name);
}

static const GDBusInterfaceVTable devices_vtable = {
    handle_devices_method_call,
    NULL,
    NULL
};

static GDBusNodeInfo *devices_introspection_data = NULL;

/** Per-dialog change hook: re-emit DevicesChanged so a client can watch a
 *  dialog's device set / status / selection evolve without polling. */
static void
on_device_dialog_changed (PnDeviceDialog *dialog, gpointer user_data)
{
    PnApplication *self = user_data;
    const gchar   *id   = pn_device_dialog_get_provider_id (dialog);

    if (id != NULL)
        automation_emit_signal (self, PN_DEVICES_IFACE, "DevicesChanged",
                                g_variant_new ("(s)", id));
}

/** Open-dialog registry observer: hook each dialog's change callback as it
 *  opens, and emit one DevicesChanged across the open/close transition so a
 *  client learns a dialog appeared or went away. */
static void
on_device_dialog_observed (const gchar    *provider_id,
                           PnDeviceDialog *dialog,
                           gboolean        opened,
                           gpointer        user_data)
{
    PnApplication *self = user_data;

    if (opened)
        pn_device_dialog_set_changed_callback (dialog,
                                               on_device_dialog_changed, self);
    automation_emit_signal (self, PN_DEVICES_IFACE, "DevicesChanged",
                            g_variant_new ("(s)", provider_id));
}

static gboolean
pn_application_dbus_register (
        GApplication    *app,
        GDBusConnection *connection,
        const gchar     *object_path,
        GError         **error)
{
    PnApplication *self = PN_APPLICATION (app);

    if (!G_APPLICATION_CLASS (pn_application_parent_class)->
                dbus_register (app, connection, object_path, error))
        return FALSE;

    if (!worksheet_introspection_data)
        worksheet_introspection_data =
                g_dbus_node_info_new_for_xml (
                        worksheet_introspection_xml, NULL);

    self->dbus_worksheet_reg_id =
        g_dbus_connection_register_object (
                connection,
                object_path,
                worksheet_introspection_data->interfaces[0],
                &worksheet_vtable,
                app,
                NULL,
                error);

    if (self->dbus_worksheet_reg_id == 0)
        return FALSE;

    /* Sibling automation interface on the same object: document /
     * lifecycle ops + the API version handshake (TODO #40.3). */
    if (!editor_introspection_data)
        editor_introspection_data =
                g_dbus_node_info_new_for_xml (
                        editor_introspection_xml, NULL);

    self->dbus_editor_reg_id =
        g_dbus_connection_register_object (
                connection,
                object_path,
                editor_introspection_data->interfaces[0],
                &editor_vtable,
                app,
                NULL,
                error);

    if (self->dbus_editor_reg_id == 0)
        return FALSE;

    /* Second interface on the same object: the panel-applet engine. */
    if (!engine_introspection_data)
        engine_introspection_data =
                g_dbus_node_info_new_for_xml (
                        engine_introspection_xml, NULL);

    self->dbus_engine_reg_id =
        g_dbus_connection_register_object (
                connection,
                object_path,
                engine_introspection_data->interfaces[0],
                &engine_vtable,
                app,
                NULL,
                error);

    if (self->dbus_engine_reg_id == 0)
        return FALSE;

    /* Sibling interface: the Devices menu + open device-dialog control
     * surface.  Install the open-dialog observer so every dialog's change
     * callback is hooked to DevicesChanged as it opens. */
    if (!devices_introspection_data)
        devices_introspection_data =
                g_dbus_node_info_new_for_xml (
                        devices_introspection_xml, NULL);

    self->dbus_devices_reg_id =
        g_dbus_connection_register_object (
                connection,
                object_path,
                devices_introspection_data->interfaces[0],
                &devices_vtable,
                app,
                NULL,
                error);

    if (self->dbus_devices_reg_id == 0)
        return FALSE;

    pn_device_dialog_set_observer (on_device_dialog_observed, self);

    return TRUE;
}

static void
pn_application_dbus_unregister (
        GApplication    *app,
        GDBusConnection *connection,
        const gchar     *object_path)
{
    PnApplication *self = PN_APPLICATION (app);

    if (self->dbus_worksheet_reg_id)
    {
        g_dbus_connection_unregister_object (
                connection, self->dbus_worksheet_reg_id);
        self->dbus_worksheet_reg_id = 0;
    }

    if (self->dbus_editor_reg_id)
    {
        g_dbus_connection_unregister_object (
                connection, self->dbus_editor_reg_id);
        self->dbus_editor_reg_id = 0;
    }

    if (self->dbus_engine_reg_id)
    {
        g_dbus_connection_unregister_object (
                connection, self->dbus_engine_reg_id);
        self->dbus_engine_reg_id = 0;
    }

    if (self->dbus_devices_reg_id)
    {
        pn_device_dialog_set_observer (NULL, NULL);
        g_dbus_connection_unregister_object (
                connection, self->dbus_devices_reg_id);
        self->dbus_devices_reg_id = 0;
    }

    G_APPLICATION_CLASS (pn_application_parent_class)->
            dbus_unregister (app, connection, object_path);
}

/* Started as a background service (--gapplication-service): hold the
 * application so it lives on as the panel engine even with no windows on
 * screen, ready to serve applets over D-Bus.  A normally-launched editor
 * leaves the flag clear and quits with its last window as before. */
static void
pn_application_startup (GApplication *app)
{
    G_APPLICATION_CLASS (pn_application_parent_class)->startup (app);

    if (g_application_get_flags (app) & G_APPLICATION_IS_SERVICE)
        g_application_hold (app);

    /* Attach the live-observation signal bridge (TODO #40.14) to every
     * editor window for its lifetime, so model edits reach D-Bus
     * subscribers without polling. */
    g_signal_connect (app, "window-added",
                      G_CALLBACK (on_signal_bridge_window_added), NULL);
    g_signal_connect (app, "window-removed",
                      G_CALLBACK (on_signal_bridge_window_removed), NULL);
}

/* --- Application lifecycle --- */

static gint
pn_application_handle_local_options (
        GApplication *app,
        GVariantDict *options)
{
    PnApplication *self = PN_APPLICATION (app);

    (void) options;

    if (self->opt_version)
    {
#ifdef GIT_VERSION
        g_print ("pipnode-editor %s (%s)\n", PACKAGE_VERSION, GIT_VERSION);
#else
        g_print ("pipnode-editor %s\n", PACKAGE_VERSION);
#endif
        return 0;  /* exit with success */
    }

    /*  Allow the functional tests (and the user) to run several
     *  pipnode instances side by side on the same session bus by
     *  overriding the well-known name they register under.  The
     *  GApplication object path follows the application id, so
     *  callers must derive it the same way (replace '.' with '/',
     *  prepend a leading '/'). */
    if (self->opt_dbus_name && *self->opt_dbus_name)
    {
        if (!g_application_id_is_valid (self->opt_dbus_name))
        {
            g_printerr ("pipnode-editor: invalid D-Bus name '%s'\n",
                        self->opt_dbus_name);
            return 1;
        }
        g_application_set_application_id (app, self->opt_dbus_name);
    }

    /* --no-plugins is also parsed here so it appears in --help and is
     * accepted rather than rejected as unknown, but it is acted on in
     * main() before g_application_run(): plugin discovery has to finish
     * before the first window builds its palette, which is well ahead of
     * this local-options hook.  main() therefore pre-scans argv for the
     * flag; nothing more is needed here. */

    g_clear_pointer (&self->startup_file, g_free);

    if (self->opt_file && *self->opt_file)
        self->startup_file = g_strdup (self->opt_file);
    else if (self->opt_remaining && self->opt_remaining[0])
    {
        if (self->opt_remaining[1])
        {
            g_printerr ("pipnode-editor: too many arguments\n");
            return 1;
        }
        self->startup_file = g_strdup (self->opt_remaining[0]);
    }

    return -1;  /* continue normal processing */
}

static void
pn_application_activate (GApplication *app)
{
    PnApplication *self = PN_APPLICATION (app);
    PnWindow *window;

    window = pn_window_new (PN_APPLICATION (app));
    gtk_window_present (GTK_WINDOW (window));

    if (self->startup_file)
    {
        GError *error = NULL;
        if (!pn_window_load_file (window, self->startup_file, &error))
        {
            g_printerr ("pipnode-editor: could not open '%s': %s\n",
                        self->startup_file,
                        error ? error->message : "(no error message)");
            g_clear_error (&error);
        }
        g_clear_pointer (&self->startup_file, g_free);
    }
}

static void
pn_application_finalize (GObject *object)
{
    PnApplication *self = PN_APPLICATION (object);

    g_clear_pointer (&self->opt_file,      g_free);
    g_clear_pointer (&self->opt_dbus_name, g_free);
    g_clear_pointer (&self->opt_remaining, g_strfreev);
    g_clear_pointer (&self->startup_file,  g_free);
    g_clear_pointer (&self->worksheets,    g_hash_table_destroy);

    G_OBJECT_CLASS (pn_application_parent_class)->finalize (object);
}

static void
pn_application_class_init (PnApplicationClass *klass)
{
    GObjectClass      *object_class = G_OBJECT_CLASS (klass);
    GApplicationClass *app_class    = G_APPLICATION_CLASS (klass);

    object_class->finalize = pn_application_finalize;

    app_class->handle_local_options = pn_application_handle_local_options;
    app_class->startup              = pn_application_startup;
    app_class->activate             = pn_application_activate;
    app_class->dbus_register        = pn_application_dbus_register;
    app_class->dbus_unregister      = pn_application_dbus_unregister;
}

static void
pn_application_init (PnApplication *self)
{
    /* path -> panel-backed PnWindow.  Keys owned; values borrowed (the
     * GtkApplication owns the windows). */
    self->worksheets = g_hash_table_new_full (
            g_str_hash, g_str_equal, g_free, NULL);
}

PnApplication *
pn_application_new (gboolean unique)
{
    PnApplication    *app;
    GApplicationFlags flags = G_APPLICATION_DEFAULT_FLAGS;

    /* A normal launch is non-unique: it runs as its own independent
     * process, registers no shared name, and handles its own options, so
     * opening a file is never forwarded to (or pops a window in) the
     * background engine.  Only the engine (--gapplication-service) and
     * the test instances (--dbus-name) take the well-known name as a
     * single, addressable instance. */
    if (!unique)
        flags |= G_APPLICATION_NON_UNIQUE;

    app = g_object_new (PN_TYPE_APPLICATION,
                        "application-id", "org.pipas.pipnode",
                        "flags", flags,
                        NULL);

    {
        static GOptionEntry entries[] = {
            { "version", 'V', 0, G_OPTION_ARG_NONE, NULL,
              "Print version and exit", NULL },
            { "no-plugins", 0, 0, G_OPTION_ARG_NONE, NULL,
              "Do not load any plugins for this session", NULL },
            { "file", 'f', 0, G_OPTION_ARG_FILENAME, NULL,
              "Open the worksheet at FILE on startup", "FILE" },
            { "dbus-name", 'D', 0, G_OPTION_ARG_STRING, NULL,
              "Register the application under NAME on the session bus "
              "instead of the default 'org.pipas.pipnode' "
              "(useful for running several instances in parallel, "
              "e.g. from the functional tests)", "NAME" },
            { G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_FILENAME_ARRAY, NULL,
              NULL, "[FILE]" },
            { NULL }
        };

        entries[0].arg_data = &app->opt_version;
        entries[1].arg_data = &app->opt_no_plugins;
        entries[2].arg_data = &app->opt_file;
        entries[3].arg_data = &app->opt_dbus_name;
        entries[4].arg_data = &app->opt_remaining;

        g_application_add_main_option_entries (G_APPLICATION (app), entries);
    }

    return app;
}
