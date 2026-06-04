#!/usr/bin/env python3
"""Functional test: node lifecycle by UUID over D-Bus (TODO #40.4).

Where #40.1 made every *read* and *wiring* call addressable by the
stable per-node UUID, #40.4 adds the *mutating* lifecycle verbs an agent
needs to author a graph from nothing and edit it in place:

  * AddNodeReturningUuid(type,x,y) -> uuid   create + hand back the handle
  * DeleteNode(uuid)                          remove a node AND its wires
  * MoveNode(uuid,x,y)                         reposition
  * RenameNode(uuid,name)                      relabel
  * SetNodeInputCount(uuid,n)                  grow a multi-input node
  * SetNodeInputName(uuid,index,name)          name an input port

This test drives all six through a freshly launched editor and asserts
both the happy path (the mutation is observable via the #40.1 by-UUID
read methods) and the error contract (#40.2 stable remote error names):

  1. ADD — AddNodeReturningUuid returns a fresh, non-empty handle that
     GetNodeByUuid then resolves to the node just created; a bogus type
     fails with UnknownNodeType.

  2. MOVE — MoveNode writes through; GetNodeByUuid reports the new (x,y).

  3. RENAME — RenameNode writes through; GetNodeByUuid reports the name.

  4. INPUTS — SetNodeInputCount grows the node so that an input index
     that was out of range becomes nameable; SetNodeInputName then
     succeeds for an in-range index and fails (BadPropertyValue) past the
     new count.  A count < 1 is rejected.

  5. DELETE — DeleteNode drops the node together with every wire incident
     on it (the whole point of routing through the shared delete path),
     and the handle no longer resolves.

  6. ERRORS — every verb rejects an unknown UUID with NodeNotFound.

Run directly:

    python3 tests/test_dbus_node_lifecycle.py

Set $PIPNODE to override the path to the binary; defaults to the in-tree
build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import os
import subprocess
import sys
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

ERR   = "org.pipas.pipnode.Worksheet.Error."
BOGUS = "00000000-0000-0000-0000-000000000000"


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


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


def add_node(bus, type_name: str, x: float, y: float) -> str:
    return call(bus, "AddNodeReturningUuid",
                GLib.Variant("(sdd)", (type_name, x, y)), "(s)").unpack()[0]


def get_node(bus, node_uuid: str):
    return call(bus, "GetNodeByUuid",
                GLib.Variant("(s)", (node_uuid,)), "(sssddbb)").unpack()


def node_count(bus) -> int:
    return call(bus, "GetNodeCount", None, "(u)").unpack()[0]


def wire_count(bus) -> int:
    return call(bus, "GetWireCount", None, "(u)").unpack()[0]


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

        # Start from a known-empty sheet so the counts below are exact.
        call(bus, "Clear", None, None)
        if node_count(bus) != 0:
            fail("Clear did not empty the worksheet")

        # --- 1. ADD --------------------------------------------------
        src = add_node(bus, "PnAutoRandom", 80.0, 120.0)
        dst = add_node(bus, "PnDial", 300.0, 120.0)
        if not src or not dst:
            fail("AddNodeReturningUuid returned an empty UUID")
        if src == dst:
            fail("AddNodeReturningUuid handed out the same UUID twice")
        if node_count(bus) != 2:
            fail(f"expected 2 nodes after two adds, got {node_count(bus)}")

        # The returned handle must resolve to the node just created.
        if get_node(bus, src)[0] != "AutoRandom":
            fail(f"src handle resolves to class {get_node(bus, src)[0]!r}, "
                 "want 'AutoRandom'")
        if get_node(bus, dst)[0] != "Dial":
            fail(f"dst handle resolves to class {get_node(bus, dst)[0]!r}, "
                 "want 'Dial'")

        expect(bus, "AddNodeReturningUuid",
               GLib.Variant("(sdd)", ("PnNoSuchType", 0.0, 0.0)), "(s)",
               ERR + "UnknownNodeType")

        # --- 2. MOVE -------------------------------------------------
        call(bus, "MoveNode", GLib.Variant("(sdd)", (dst, 540.0, 410.0)), None)
        row = get_node(bus, dst)
        if (row[3], row[4]) != (540.0, 410.0):
            fail(f"after MoveNode, position is {(row[3], row[4])!r}, "
                 "want (540.0, 410.0)")
        expect(bus, "MoveNode",
               GLib.Variant("(sdd)", (BOGUS, 1.0, 2.0)), None,
               ERR + "NodeNotFound")

        # --- 3. RENAME -----------------------------------------------
        call(bus, "RenameNode",
             GLib.Variant("(ss)", (dst, "Boiler Gauge")), None)
        if get_node(bus, dst)[1] != "Boiler Gauge":
            fail(f"after RenameNode, name is {get_node(bus, dst)[1]!r}, "
                 "want 'Boiler Gauge'")
        expect(bus, "RenameNode",
               GLib.Variant("(ss)", (BOGUS, "X")), None,
               ERR + "NodeNotFound")

        # --- 4. INPUTS -----------------------------------------------
        # A Dial starts with a single input, so index 2 is out of range.
        expect(bus, "SetNodeInputName",
               GLib.Variant("(sis)", (dst, 2, "gamma")), None,
               ERR + "BadPropertyValue")
        # Grow it to three inputs; now index 2 is nameable.
        call(bus, "SetNodeInputCount", GLib.Variant("(si)", (dst, 3)), None)
        call(bus, "SetNodeInputName",
             GLib.Variant("(sis)", (dst, 0, "alpha")), None)
        call(bus, "SetNodeInputName",
             GLib.Variant("(sis)", (dst, 2, "gamma")), None)
        # One past the new count is still rejected.
        expect(bus, "SetNodeInputName",
               GLib.Variant("(sis)", (dst, 3, "delta")), None,
               ERR + "BadPropertyValue")
        # A nonsensical count is rejected without disabling the input.
        expect(bus, "SetNodeInputCount",
               GLib.Variant("(si)", (dst, 0)), None,
               ERR + "BadPropertyValue")
        # Unknown-UUID forms.
        expect(bus, "SetNodeInputCount",
               GLib.Variant("(si)", (BOGUS, 2)), None,
               ERR + "NodeNotFound")
        expect(bus, "SetNodeInputName",
               GLib.Variant("(sis)", (BOGUS, 0, "x")), None,
               ERR + "NodeNotFound")

        # --- 5. DELETE (drops incident wires) ------------------------
        if not call(bus, "ConnectNodesByUuid",
                    GLib.Variant("(ss)", (src, dst)), "(b)").unpack()[0]:
            fail("ConnectNodesByUuid(src -> dst) returned False")
        if wire_count(bus) != 1:
            fail(f"expected 1 wire before delete, got {wire_count(bus)}")

        call(bus, "DeleteNode", GLib.Variant("(s)", (dst,)), None)
        if node_count(bus) != 1:
            fail(f"after DeleteNode, expected 1 node, got {node_count(bus)}")
        if wire_count(bus) != 0:
            fail(f"after DeleteNode, the incident wire survived: "
                 f"{wire_count(bus)} wire(s) left")

        # The handle no longer resolves, and a second delete is a clean
        # NodeNotFound rather than a crash or a silent success.
        expect(bus, "GetNodeByUuid",
               GLib.Variant("(s)", (dst,)), "(sssddbb)",
               ERR + "NodeNotFound")
        expect(bus, "DeleteNode",
               GLib.Variant("(s)", (dst,)), None,
               ERR + "NodeNotFound")
        # The other node is untouched.
        if get_node(bus, src)[0] != "AutoRandom":
            fail("deleting dst disturbed the surviving src node")

        print("PASS: the by-UUID node-lifecycle methods (AddNodeReturningUuid, "
              "MoveNode, RenameNode, SetNodeInputCount, SetNodeInputName, "
              "DeleteNode) mutate the graph observably, drop incident wires on "
              "delete, and reject bad UUIDs/values with the stable error names.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
