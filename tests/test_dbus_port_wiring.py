#!/usr/bin/env python3
"""Functional test: port-aware wiring by UUID over D-Bus (TODO #40.5).

The legacy ConnectNodes(src,tgt) always feeds input port 0, which is
useless for a multi-input node (a Calculator, a grown sink) where the
agent must say *which* port a wire lands on.  #40.5 adds the port-aware,
wire-addressable surface on org.pipas.pipnode.Worksheet:

  * Connect(src_uuid,tgt_uuid,target_input) -> wire_uuid
  * Disconnect(wire_uuid)
  * ListWires           -> a(ssis) {src,tgt,target_input,wire_uuid}
  * GetNodeWires(uuid)  -> a(ssis) (wires incident on one node)

This test drives them through a freshly launched editor and asserts:

  1. CONNECT — Connect returns a fresh wire handle; ListWires reports the
     wire as {src, tgt, 0, handle}; GetNodeWires sees it from both ends.

  2. PORTS — after growing the target to three inputs, Connect to input 2
     succeeds and ListWires reports target_input == 2 for that wire.

  3. DISCONNECT — Disconnect drops exactly that wire (the other survives),
     and a second Disconnect of the same handle is a clean WireNotFound.

  4. ILLEGAL — self-loop, output->output (target has no input), an input
     index past the target's port count, and a duplicate wire are each
     rejected with IllegalConnection; bad node/wire UUIDs raise
     NodeNotFound / WireNotFound (the #40.2 contract).

Run directly:

    python3 tests/test_dbus_port_wiring.py

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


def connect(bus, src: str, tgt: str, target_input: int) -> str:
    return call(bus, "Connect",
                GLib.Variant("(ssi)", (src, tgt, target_input)),
                "(s)").unpack()[0]


def list_wires(bus):
    return call(bus, "ListWires", None, "(a(ssis))").unpack()[0]


def node_wires(bus, node_uuid: str):
    return call(bus, "GetNodeWires",
                GLib.Variant("(s)", (node_uuid,)), "(a(ssis))").unpack()[0]


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

        call(bus, "Clear", None, None)

        # Two sources (output, no input) and a sink (input, no output).
        src  = add_node(bus, "PnAutoRandom", 80.0, 80.0)
        src2 = add_node(bus, "PnAutoRandom", 80.0, 260.0)
        dst  = add_node(bus, "PnDial", 360.0, 160.0)

        # Sanity-check the capabilities the illegal-connection rules lean on.
        if not get_node(bus, src)[6]:
            fail("AutoRandom should report has_output=True")
        if get_node(bus, src)[5]:
            fail("AutoRandom should report has_input=False")
        if not get_node(bus, dst)[5]:
            fail("Dial should report has_input=True")

        # --- 1. CONNECT ----------------------------------------------
        w0 = connect(bus, src, dst, 0)
        if not w0:
            fail("Connect returned an empty wire UUID")

        wires = list_wires(bus)
        if len(wires) != 1:
            fail(f"ListWires returned {len(wires)} rows, expected 1")
        row = wires[0]
        if (row[0], row[1], row[2], row[3]) != (src, dst, 0, w0):
            fail(f"ListWires row {row!r} != expected ({src}, {dst}, 0, {w0})")

        # Both endpoints see the wire.
        if len(node_wires(bus, src)) != 1:
            fail("GetNodeWires(src) did not report the wire")
        if node_wires(bus, dst)[0][3] != w0:
            fail("GetNodeWires(dst) reported a different wire handle")

        # --- 2. PORTS (multi-input target) ---------------------------
        # Grow the Dial to three inputs, then land a wire on port 2.
        call(bus, "SetNodeInputCount", GLib.Variant("(si)", (dst, 3)), None)
        w2 = connect(bus, src2, dst, 2)
        if not w2 or w2 == w0:
            fail("Connect to input 2 returned a bad/duplicate wire UUID")

        by_handle = {r[3]: r for r in list_wires(bus)}
        if len(by_handle) != 2:
            fail(f"expected 2 wires after the second Connect, "
                 f"got {len(by_handle)}")
        if by_handle[w2][2] != 2:
            fail(f"wire {w2} reports target_input {by_handle[w2][2]}, want 2")
        if by_handle[w0][2] != 0:
            fail(f"wire {w0} reports target_input {by_handle[w0][2]}, want 0")

        # The Dial is now fed by two wires; the second source by one.
        if len(node_wires(bus, dst)) != 2:
            fail("GetNodeWires(dst) should report both incoming wires")
        if len(node_wires(bus, src2)) != 1:
            fail("GetNodeWires(src2) should report its single wire")

        # --- 3. DISCONNECT -------------------------------------------
        call(bus, "Disconnect", GLib.Variant("(s)", (w0,)), None)
        remaining = list_wires(bus)
        if len(remaining) != 1 or remaining[0][3] != w2:
            fail(f"after Disconnect(w0), wires are {remaining!r}; "
                 f"expected only {w2}")
        # A second disconnect of the same handle is a clean WireNotFound.
        expect(bus, "Disconnect", GLib.Variant("(s)", (w0,)), None,
               ERR + "WireNotFound")

        # --- 4. ILLEGAL CONNECTIONS ----------------------------------
        # self-loop
        expect(bus, "Connect", GLib.Variant("(ssi)", (dst, dst, 0)), "(s)",
               ERR + "IllegalConnection")
        # target has no input (output -> output)
        expect(bus, "Connect", GLib.Variant("(ssi)", (src, src2, 0)), "(s)",
               ERR + "IllegalConnection")
        # input index past the target's port count (Dial now has 3)
        expect(bus, "Connect", GLib.Variant("(ssi)", (src, dst, 7)), "(s)",
               ERR + "IllegalConnection")
        # duplicate (src2 -> dst @2 already exists as w2)
        expect(bus, "Connect", GLib.Variant("(ssi)", (src2, dst, 2)), "(s)",
               ERR + "IllegalConnection")
        # unknown node / wire UUIDs
        expect(bus, "Connect", GLib.Variant("(ssi)", (BOGUS, dst, 0)), "(s)",
               ERR + "NodeNotFound")
        expect(bus, "Connect", GLib.Variant("(ssi)", (src, BOGUS, 0)), "(s)",
               ERR + "NodeNotFound")
        expect(bus, "GetNodeWires", GLib.Variant("(s)", (BOGUS,)),
               "(a(ssis))", ERR + "NodeNotFound")
        expect(bus, "Disconnect", GLib.Variant("(s)", (BOGUS,)), None,
               ERR + "WireNotFound")

        # The surviving wire is untouched by all the rejected calls.
        if len(list_wires(bus)) != 1:
            fail("a rejected Connect leaked a wire into the store")

        print("PASS: the port-aware wiring methods (Connect, Disconnect, "
              "ListWires, GetNodeWires) land wires on the named input port, "
              "report them by stable handle, disconnect precisely, and reject "
              "self-loops/no-input/out-of-range/duplicate with IllegalConnection.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
