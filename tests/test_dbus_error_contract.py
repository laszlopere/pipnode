#!/usr/bin/env python3
"""Functional test: the Worksheet D-Bus error contract (TODO #40.2).

The mutating methods on ``org.pipas.pipnode.Worksheet`` used to fail by
returning a bare FALSE / empty string or a generic G_DBUS_ERROR_FAILED,
which tells an automation client THAT a call failed but never WHY.  TODO
#40.2 replaces those with a registered error domain whose codes map to
stable, machine-readable D-Bus error names under
``org.pipas.pipnode.Worksheet.Error.*``.  An agent branches on the NAME
(via g_dbus_error_get_remote_error); the human message is just a hint.

This test drives a freshly launched editor, provokes each failure mode,
and asserts the exact remote error name that comes back:

  * NodeNotFound      — bad index / bad UUID on every node method
  * WireNotFound      — bad wire index
  * UnknownNodeType   — AddNode with a type the factory does not know
  * UnknownProperty   — get/set a property the node does not have
  * BadPropertyValue  — set an enum property to an unparseable value
  * IllegalConnection — connect a node to itself

NoActiveSheet / NoActiveWindow / SheetNotFound are part of the same
domain but need a window-less / sheet-less editor (or sheets, TODO
#40.10) to provoke, so they are exercised by code paths rather than here.

Run directly:

    python3 tests/test_dbus_error_contract.py

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import uuid

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402


ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE = os.environ.get("PIPNODE", os.path.join(ROOT, "src", "pipnode-editor"))

BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"

ERR = "org.pipas.pipnode.Worksheet.Error."

# Baked-in UUIDs so the by-UUID error paths can be probed with handles we
# know exist (the valid endpoint of a connection) and ones we know do not.
SRC_UUID = str(uuid.uuid4())   # an AutoRandom (has the 'distribution' enum)
DST_UUID = str(uuid.uuid4())   # a Dial       (has 'label')
BOGUS    = "00000000-0000-0000-0000-000000000000"


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def build_worksheet_json() -> dict:
    return {
        "format":  "pipnode",
        "version": 1,
        "nodes": [
            {"type": "PnAutoRandom", "name": "Source", "uuid": SRC_UUID,
             "position": {"x": 80.0, "y": 120.0}, "properties": {}},
            {"type": "PnDial", "name": "Gauge", "uuid": DST_UUID,
             "position": {"x": 300.0, "y": 120.0}, "properties": {}},
        ],
        "connections": [],
    }


def wait_for_bus_name(bus, name: str, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            bus.call_sync(
                "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "GetNameOwner",
                GLib.Variant("(s)", (name,)),
                GLib.VariantType.new("(s)"),
                Gio.DBusCallFlags.NONE, 1000, None,
            )
            return True
        except GLib.Error:
            time.sleep(0.1)
    return False


def call(bus, method: str, params, reply_type: str | None):
    return bus.call_sync(
        BUS_NAME, OBJECT_PATH, WS_IFACE, method,
        params,
        GLib.VariantType.new(reply_type) if reply_type else None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def expect(bus, method: str, params, reply_type: str | None,
           want: str) -> None:
    """Assert @method raises a D-Bus error whose remote name is @want."""
    try:
        call(bus, method, params, reply_type)
    except GLib.Error as e:
        got = Gio.DBusError.get_remote_error(e)
        if got != want:
            fail(f"{method}: expected error {want!r}, got {got!r} "
                 f"(message: {e.message!r})")
        return
    fail(f"{method}: expected error {want!r} but the call succeeded")


def load_worksheet(bus) -> None:
    with tempfile.NamedTemporaryFile(
            mode="w", suffix=".json", delete=False) as fp:
        json.dump(build_worksheet_json(), fp)
        tmp_path = fp.name
    try:
        call(bus, "LoadFromFile", GLib.Variant("(s)", (tmp_path,)), None)
    finally:
        os.unlink(tmp_path)


# ----------------------------------------------------------------------
# Test
# ----------------------------------------------------------------------

def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    proc = subprocess.Popen(
        [PIPNODE, f"--dbus-name={BUS_NAME}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        if not wait_for_bus_name(bus, BUS_NAME, timeout=15.0):
            fail("pipnode never registered its DBus name")

        load_worksheet(bus)
        if call(bus, "GetNodeCount", None, "(u)").unpack()[0] != 2:
            fail("setup: expected 2 nodes after LoadFromFile")

        BAD_IDX = 99   # beyond the 2 loaded nodes

        # --- NodeNotFound, every node method ------------------------
        node_not_found = ERR + "NodeNotFound"
        expect(bus, "GetNode",
               GLib.Variant("(u)", (BAD_IDX,)), "(sssddbb)", node_not_found)
        expect(bus, "GetNodeUuid",
               GLib.Variant("(u)", (BAD_IDX,)), "(s)", node_not_found)
        expect(bus, "GetNodeByUuid",
               GLib.Variant("(s)", (BOGUS,)), "(sssddbb)", node_not_found)
        expect(bus, "GetNodeProperty",
               GLib.Variant("(us)", (BAD_IDX, "label")), "(s)", node_not_found)
        expect(bus, "GetNodePropertyByUuid",
               GLib.Variant("(ss)", (BOGUS, "label")), "(s)", node_not_found)
        expect(bus, "SetNodeProperty",
               GLib.Variant("(uss)", (BAD_IDX, "label", "x")), "(b)",
               node_not_found)
        expect(bus, "SetNodePropertyByUuid",
               GLib.Variant("(sss)", (BOGUS, "label", "x")), "(b)",
               node_not_found)
        expect(bus, "ConnectNodes",
               GLib.Variant("(uu)", (BAD_IDX, 0)), "(b)", node_not_found)
        expect(bus, "ConnectNodesByUuid",
               GLib.Variant("(ss)", (BOGUS, DST_UUID)), "(b)", node_not_found)
        expect(bus, "OpenNodeDialogByUuid",
               GLib.Variant("(s)", (BOGUS,)), "(b)", node_not_found)

        # --- WireNotFound -------------------------------------------
        expect(bus, "GetWire",
               GLib.Variant("(u)", (BAD_IDX,)), "(ii)", ERR + "WireNotFound")

        # --- UnknownNodeType ----------------------------------------
        expect(bus, "AddNode",
               GLib.Variant("(sdd)", ("PnDefinitelyNotARealNode", 0.0, 0.0)),
               "(i)", ERR + "UnknownNodeType")

        # --- UnknownProperty (read + write, index + uuid) -----------
        unknown_prop = ERR + "UnknownProperty"
        expect(bus, "GetNodeProperty",
               GLib.Variant("(us)", (1, "no-such-prop")), "(s)", unknown_prop)
        expect(bus, "SetNodeProperty",
               GLib.Variant("(uss)", (1, "no-such-prop", "x")), "(b)",
               unknown_prop)
        expect(bus, "GetNodePropertyByUuid",
               GLib.Variant("(ss)", (DST_UUID, "no-such-prop")), "(s)",
               unknown_prop)

        # --- BadPropertyValue ---------------------------------------
        # The AutoRandom 'distribution' is an enum: a value that is not a
        # known nick parses to nothing and must be rejected as bad value,
        # NOT as an unknown property (the property exists and is writable).
        expect(bus, "SetNodePropertyByUuid",
               GLib.Variant("(sss)", (SRC_UUID, "distribution", "not-a-shape")),
               "(b)", ERR + "BadPropertyValue")

        # --- IllegalConnection (self-loop, index + uuid) ------------
        illegal = ERR + "IllegalConnection"
        expect(bus, "ConnectNodes",
               GLib.Variant("(uu)", (0, 0)), "(b)", illegal)
        expect(bus, "ConnectNodesByUuid",
               GLib.Variant("(ss)", (SRC_UUID, SRC_UUID)), "(b)", illegal)

        # Sanity: a self-loop attempt must NOT have left a stray wire.
        if call(bus, "GetWireCount", None, "(u)").unpack()[0] != 0:
            fail("a rejected self-loop connection still created a wire")

        print("PASS: the Worksheet error contract returns stable "
              "org.pipas.pipnode.Worksheet.Error.* names — NodeNotFound, "
              "WireNotFound, UnknownNodeType, UnknownProperty, "
              "BadPropertyValue and IllegalConnection — for each failure mode.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
