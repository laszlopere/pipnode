#!/usr/bin/env python3
"""Functional test: the UUID-addressed D-Bus surface (TODO #40.1).

The ``org.pipas.pipnode.Worksheet`` interface grew up index-addressed
(GetNode/SetNodeProperty/ConnectNodes by integer index), which is fine
for a single-shot test but unusable for a multi-step automated edit: a
node's index shifts the moment any earlier node is removed.  TODO #40.1
adds a UUID-addressed variant of every node method, keyed on the stable
``pn_node_get_uuid`` handle that is already serialized.  This test drives
those new methods through a freshly launched editor and asserts:

  1. ENUMERATION — GetNodesWithUuids returns every node together with
     its UUID, and the UUIDs agree with the legacy GetNodeUuid(index).

  2. BY-UUID READ — GetNodeByUuid resolves to the same node GetNode does
     for the matching index (class + name), and a bogus UUID fails
     cleanly rather than silently matching node 0.

  3. BY-UUID PROPERTY GET/SET — SetNodePropertyByUuid writes through and
     GetNodePropertyByUuid reads it back; the legacy index getter sees
     the same value (one node, two address spaces, one state).

  4. BY-UUID WIRING — ConnectNodesByUuid creates the wire the index form
     would; a self-loop is rejected.

  5. BY-UUID DIALOG — OpenNodeDialogByUuid opens the right node's dialog;
     a bogus UUID fails.

  6. STABILITY — after a Save/Load round-trip (the operation indices do
     NOT survive intact in general), the very same UUID strings still
     resolve to the very same nodes.  This is the whole point of UUID
     addressing.

Run directly:

    python3 tests/test_dbus_uuid_addressing.py

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

# Two stable UUIDs we mint up front and bake into the loaded worksheet,
# so the test addresses nodes by handles it knows independently of any
# enumeration order.
SRC_UUID = str(uuid.uuid4())
DST_UUID = str(uuid.uuid4())


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def build_worksheet_json() -> dict:
    """An AutoRandom source and a Dial sink, each with a known UUID."""
    return {
        "format":  "pipnode",
        "version": 1,
        "nodes": [
            {
                "type":       "PnAutoRandom",
                "name":       "Source",
                "uuid":       SRC_UUID,
                "position":   {"x": 80.0, "y": 120.0},
                "properties": {},
            },
            {
                "type":       "PnDial",
                "name":       "Gauge",
                "uuid":       DST_UUID,
                "position":   {"x": 300.0, "y": 120.0},
                "properties": {},
            },
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
    """Invoke @method on the worksheet interface synchronously."""
    return bus.call_sync(
        BUS_NAME, OBJECT_PATH, WS_IFACE, method,
        params,
        GLib.VariantType.new(reply_type) if reply_type else None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def expect_error(bus, method: str, params, reply_type: str | None,
                 what: str) -> None:
    """Assert that @method raises a D-Bus error (rather than silently
    succeeding on a bad address)."""
    try:
        call(bus, method, params, reply_type)
    except GLib.Error:
        return
    fail(f"{what}: {method} returned success for an invalid address")


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

        n = call(bus, "GetNodeCount", None, "(u)").unpack()[0]
        if n != 2:
            fail(f"expected 2 nodes after LoadFromFile, got {n}")

        # --- 1. ENUMERATION ------------------------------------------
        rows = call(bus, "GetNodesWithUuids",
                    None, "(a(sssddbbs))").unpack()[0]
        if len(rows) != 2:
            fail(f"GetNodesWithUuids returned {len(rows)} rows, expected 2")

        by_uuid = {row[7]: row for row in rows}      # uuid -> tuple
        if set(by_uuid) != {SRC_UUID, DST_UUID}:
            fail(f"GetNodesWithUuids UUIDs {set(by_uuid)!r} do not match the "
                 f"baked-in {{{SRC_UUID!r}, {DST_UUID!r}}}; UUIDs did not "
                 "round-trip through load")

        # The enumerated UUID must equal what GetNodeUuid(index) reports
        # for the same row — the two discovery paths must agree.
        for idx, row in enumerate(rows):
            legacy = call(bus, "GetNodeUuid",
                          GLib.Variant("(u)", (idx,)), "(s)").unpack()[0]
            if legacy != row[7]:
                fail(f"row {idx}: GetNodesWithUuids uuid {row[7]!r} != "
                     f"GetNodeUuid({idx}) {legacy!r}")

        # Class names came across as expected.
        if by_uuid[SRC_UUID][0] != "AutoRandom":
            fail(f"SRC node class is {by_uuid[SRC_UUID][0]!r}, want 'AutoRandom'")
        if by_uuid[DST_UUID][0] != "Dial":
            fail(f"DST node class is {by_uuid[DST_UUID][0]!r}, want 'Dial'")

        # --- 2. BY-UUID READ -----------------------------------------
        # GetNodeByUuid(DST) must equal GetNode(<index of DST>): same node.
        dst_index = next(i for i, r in enumerate(rows) if r[7] == DST_UUID)
        by_idx  = call(bus, "GetNode",
                       GLib.Variant("(u)", (dst_index,)),
                       "(sssddbb)").unpack()
        by_uid  = call(bus, "GetNodeByUuid",
                       GLib.Variant("(s)", (DST_UUID,)),
                       "(sssddbb)").unpack()
        if by_idx != by_uid:
            fail(f"GetNodeByUuid != GetNode for the same node: "
                 f"{by_uid!r} vs {by_idx!r}")

        expect_error(bus, "GetNodeByUuid",
                     GLib.Variant("(s)", ("no-such-uuid",)), "(sssddbb)",
                     "bogus GetNodeByUuid")

        # --- 3. BY-UUID PROPERTY GET/SET -----------------------------
        if not call(bus, "SetNodePropertyByUuid",
                    GLib.Variant("(sss)", (DST_UUID, "label", "Boiler")),
                    "(b)").unpack()[0]:
            fail("SetNodePropertyByUuid('label') returned False")

        echoed = call(bus, "GetNodePropertyByUuid",
                      GLib.Variant("(ss)", (DST_UUID, "label")),
                      "(s)").unpack()[0]
        if echoed != "Boiler":
            fail(f"GetNodePropertyByUuid('label') = {echoed!r}, want 'Boiler'")

        # The legacy index getter must see the same write — one node,
        # one state, addressed two ways.
        legacy_label = call(bus, "GetNodeProperty",
                            GLib.Variant("(us)", (dst_index, "label")),
                            "(s)").unpack()[0]
        if legacy_label != "Boiler":
            fail(f"index getter sees label {legacy_label!r}, want 'Boiler'; "
                 "UUID and index forms address different state")

        expect_error(bus, "GetNodePropertyByUuid",
                     GLib.Variant("(ss)", ("no-such-uuid", "label")), "(s)",
                     "bogus GetNodePropertyByUuid")
        expect_error(bus, "SetNodePropertyByUuid",
                     GLib.Variant("(sss)", ("no-such-uuid", "label", "x")),
                     "(b)", "bogus SetNodePropertyByUuid")

        # --- 4. BY-UUID WIRING ---------------------------------------
        if not call(bus, "ConnectNodesByUuid",
                    GLib.Variant("(ss)", (SRC_UUID, DST_UUID)),
                    "(b)").unpack()[0]:
            fail("ConnectNodesByUuid(src -> dst) returned False")

        wire_count = call(bus, "GetWireCount", None, "(u)").unpack()[0]
        if wire_count != 1:
            fail(f"expected 1 wire after ConnectNodesByUuid, got {wire_count}")

        # A self-loop must be rejected.
        expect_error(bus, "ConnectNodesByUuid",
                     GLib.Variant("(ss)", (SRC_UUID, SRC_UUID)), "(b)",
                     "self-loop ConnectNodesByUuid")

        # --- 5. BY-UUID DIALOG ---------------------------------------
        if not call(bus, "OpenNodeDialogByUuid",
                    GLib.Variant("(s)", (DST_UUID,)), "(b)").unpack()[0]:
            fail("OpenNodeDialogByUuid(dst) returned False")
        # The open dialog must belong to the Dial: its tab set is the
        # six-tab notebook only PnDial builds.
        titles = list(call(bus, "GetDialogPageTitles",
                           None, "(as)").unpack()[0])
        if titles[:2] != ["Class", "Node"] or "Scale" not in titles:
            fail(f"OpenNodeDialogByUuid opened the wrong node's dialog: "
                 f"tabs {titles!r}")
        call(bus, "CloseNodeDialog", None, "(b)")

        expect_error(bus, "OpenNodeDialogByUuid",
                     GLib.Variant("(s)", ("no-such-uuid",)), "(b)",
                     "bogus OpenNodeDialogByUuid")

        # --- 6. STABILITY across a Save/Load round-trip --------------
        with tempfile.NamedTemporaryFile(
                mode="w", suffix=".json", delete=False) as fp:
            saved_path = fp.name
        try:
            call(bus, "SaveToFile", GLib.Variant("(s)", (saved_path,)), None)
            call(bus, "Clear", None, None)
            if call(bus, "GetNodeCount", None, "(u)").unpack()[0] != 0:
                fail("Clear did not empty the worksheet")
            call(bus, "LoadFromFile", GLib.Variant("(s)", (saved_path,)), None)
        finally:
            os.unlink(saved_path)

        # The very same UUID strings must still resolve, and the label we
        # set earlier must have been saved and reloaded with the node.
        reloaded = call(bus, "GetNodeByUuid",
                        GLib.Variant("(s)", (DST_UUID,)),
                        "(sssddbb)").unpack()
        if reloaded[0] != "Dial":
            fail(f"after Save/Load, {DST_UUID!r} resolves to class "
                 f"{reloaded[0]!r}, want 'Dial'; UUID did not survive the "
                 "round-trip")
        reloaded_label = call(bus, "GetNodePropertyByUuid",
                              GLib.Variant("(ss)", (DST_UUID, "label")),
                              "(s)").unpack()[0]
        if reloaded_label != "Boiler":
            fail(f"after Save/Load, label is {reloaded_label!r}, want 'Boiler'")

        print("PASS: the UUID-addressed Worksheet methods (GetNodesWithUuids, "
              "GetNodeByUuid, Get/SetNodePropertyByUuid, ConnectNodesByUuid, "
              "OpenNodeDialogByUuid) address the same nodes as the index "
              "forms, reject bad UUIDs, and resolve stably across Save/Load.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
