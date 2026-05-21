#!/usr/bin/env python3
"""Functional test: clicking a debug-pane row's `from` shortcut must
focus the originating node on the worksheet.

Drives a freshly launched pipnode through the
``org.pipas.pipnode.Worksheet`` D-Bus interface — no synthesised
input events.  The flow:

  1. Spawn the pipnode binary.
  2. Load a worksheet with two nodes placed far apart on the canvas
     so a focus-pulse on either one is visible to the test as a
     non-zero scroll-adjustment delta.
  3. Read each node's persistent UUID via the DBus #GetNodeUuid
     method.
  4. Inject a synthesised JSON-shaped debug message claiming to
     originate from node 0; check that #GetDebugRowFromId returns
     that node's UUID.
  5. Trigger the row's `from`-button click via #ClickDebugFromButton;
     poll #GetFocusPulseUuid for ~250 ms and assert it returns node
     0's UUID.  Also assert the worksheet's scroll adjustments moved
     to a value that brackets the node's centre.
  6. Repeat for node 1 to confirm the pulse re-arms cleanly and the
     scroll moves to the other node.

This test exists to catch the regression where the from-button's
click was absorbed by the enclosing #GtkExpander and never reached
the focus API — the from_id round-trip via the listbox row is the
load-bearing assertion.

Run directly:

    python3 tests/test_debug_focus.py

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

# Randomised per test run so several pipnode instances can share a
# session bus; passed to the spawned binary via --dbus-name.
BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"


# Two nodes both placed *off-screen* relative to the default
# starting viewport — so a successful focus_node_by_uuid must
# actually move the scroll adjustments in order to bring either
# one into view.  The ~140×40 footprint of a #PnInject node sits
# well outside the default 841×576 viewport in both axes.
NODE_A = {"name": "Alpha", "x":  900.0, "y":  700.0}
NODE_B = {"name": "Bravo", "x": 1700.0, "y": 1300.0}

# Default node footprint — see PN_NODE_DEFAULT_{WIDTH,HEIGHT} in
# pn-node.c.  Used here to compute each node's centre when
# asserting the final viewport contains it.
NODE_W = 140.0
NODE_H = 40.0


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def build_worksheet_json() -> dict:
    """Two #PnInject nodes — type irrelevant, we never trigger them.

    Inject is chosen because it has no required configuration and
    therefore loads cleanly without any property errors.
    """
    return {
        "format":      "pipnode",
        "version":     1,
        "nodes": [
            {
                "type":       "PnInject",
                "name":       NODE_A["name"],
                "position":   {"x": NODE_A["x"], "y": NODE_A["y"]},
                "properties": {},
            },
            {
                "type":       "PnInject",
                "name":       NODE_B["name"],
                "position":   {"x": NODE_B["x"], "y": NODE_B["y"]},
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


def wait_focus_pulse(bus, expected_uuid: str, timeout: float = 0.6) -> str:
    """Poll #GetFocusPulseUuid until it matches @expected_uuid or the
    timeout fires.  Returns whatever the final value was so callers
    can assert against it for clearer failure messages."""
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        v = call(bus, "GetFocusPulseUuid", None, "(s)")
        last = v.unpack()[0]
        if last == expected_uuid:
            return last
        time.sleep(0.02)
    return last


def assert_node_in_viewport(bus, node, label: str) -> None:
    """Read the worksheet's scroll adjustments and confirm @node's
    centre lands inside the currently visible region.

    The worksheet runs at 1.0 zoom by default, so widget-space and
    worksheet-space coincide.  The viewport's visible band on each
    axis is [adj.value, adj.value + adj.page_size]; we expect
    @node's centre to be inside that band on both axes.
    """
    cx = node["x"] + NODE_W / 2.0
    cy = node["y"] + NODE_H / 2.0

    hval, vval, hpage, vpage, _hupper, _vupper = call(
        bus, "GetWorksheetScroll", None, "(dddddd)").unpack()

    in_x = hval <= cx <= hval + hpage
    in_y = vval <= cy <= vval + vpage
    if not (in_x and in_y):
        fail(
            f"{label}: node centre ({cx}, {cy}) is outside the visible "
            f"viewport (x in [{hval}, {hval + hpage}], "
            f"y in [{vval}, {vval + vpage}]); the focus API ran but "
            f"did not bring the node into view"
        )


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

        # Load the two-node worksheet.
        with tempfile.NamedTemporaryFile(
                mode="w", suffix=".json", delete=False) as fp:
            json.dump(build_worksheet_json(), fp)
            tmp_path = fp.name
        try:
            call(bus, "LoadFromFile",
                 GLib.Variant("(s)", (tmp_path,)), None)
        finally:
            os.unlink(tmp_path)

        # Confirm the load took.
        n = call(bus, "GetNodeCount", None, "(u)").unpack()[0]
        if n != 2:
            fail(f"expected 2 nodes after LoadFromFile, got {n}")

        # Pull the persistent UUIDs.  These survive any in-process
        # rename / move so they are the right shape to plumb through
        # a synthesised debug envelope below.
        uuid_a = call(bus, "GetNodeUuid",
                      GLib.Variant("(u)", (0,)), "(s)").unpack()[0]
        uuid_b = call(bus, "GetNodeUuid",
                      GLib.Variant("(u)", (1,)), "(s)").unpack()[0]
        if not uuid_a or not uuid_b:
            fail(f"empty UUID returned (A={uuid_a!r}, B={uuid_b!r})")
        if uuid_a == uuid_b:
            fail("the two nodes returned identical UUIDs")

        # Inject a synthesised message claiming to originate from
        # node A, then read it back through the row API.
        envelope_a = {
            "from":    NODE_A["name"],
            "from_id": uuid_a,
            "topic":   "synthetic",
            "id":      "test-msg-1",
            "created": "2026-05-08T10:00:00+00",
            "data":    {"output": "ping A"},
        }
        call(bus, "InjectDebugMessage",
             GLib.Variant("(s)", (json.dumps(envelope_a),)), None)

        rows = call(bus, "GetDebugRowCount", None, "(u)").unpack()[0]
        if rows != 1:
            fail(f"expected 1 debug row, got {rows}")

        seen_a = call(bus, "GetDebugRowFromId",
                      GLib.Variant("(u)", (0,)), "(s)").unpack()[0]
        if seen_a != uuid_a:
            fail(f"row 0 from_id is {seen_a!r}, expected {uuid_a!r}; "
                 "the JSON envelope did not survive the round-trip")

        # The actual unit under test: synthesise the from-button
        # click and confirm the pulse lands on node A.
        clicked = call(bus, "ClickDebugFromButton",
                       GLib.Variant("(u)", (0,)), "(b)").unpack()[0]
        if not clicked:
            fail("ClickDebugFromButton returned False — the from-"
                 "button could not be found or was not sensitive")

        pulsed = wait_focus_pulse(bus, uuid_a)
        if pulsed != uuid_a:
            fail("clicked from-button on row 0 did not arm the focus "
                 f"pulse on node A (uuid={uuid_a!r}); "
                 f"GetFocusPulseUuid returned {pulsed!r} after the "
                 "click — the click never reached the worksheet "
                 "focus API")

        # Both nodes were placed off-screen on purpose, so a
        # successful focus must have scrolled node A into view.
        assert_node_in_viewport(bus, NODE_A, "after clicking row 0")

        # Inject node B's message and click that row — the pulse
        # should re-arm to B and the scroll should move again.
        envelope_b = {
            "from":    NODE_B["name"],
            "from_id": uuid_b,
            "topic":   "synthetic",
            "id":      "test-msg-2",
            "created": "2026-05-08T10:00:01+00",
            "data":    {"output": "ping B"},
        }
        call(bus, "InjectDebugMessage",
             GLib.Variant("(s)", (json.dumps(envelope_b),)), None)

        rows = call(bus, "GetDebugRowCount", None, "(u)").unpack()[0]
        if rows != 2:
            fail(f"expected 2 debug rows after second inject, got {rows}")

        seen_b = call(bus, "GetDebugRowFromId",
                      GLib.Variant("(u)", (1,)), "(s)").unpack()[0]
        if seen_b != uuid_b:
            fail(f"row 1 from_id is {seen_b!r}, expected {uuid_b!r}")

        call(bus, "ClickDebugFromButton",
             GLib.Variant("(u)", (1,)), "(b)")

        pulsed_b = wait_focus_pulse(bus, uuid_b)
        if pulsed_b != uuid_b:
            fail("clicked from-button on row 1 did not arm the focus "
                 f"pulse on node B (uuid={uuid_b!r}); got {pulsed_b!r}")

        # And the same end-to-end check for node B.
        assert_node_in_viewport(bus, NODE_B, "after clicking row 1")

        # Regression check for the "clicking the worksheet jumps to
        # the top" bug: after focus_node_by_uuid has scrolled the
        # canvas to node B, simulating the gtk_widget_grab_focus
        # call that on_button_press makes on every click must NOT
        # cause GtkScrolledWindow to re-scroll back to (0, 0).
        # Capture the scroll, grab focus on the worksheet, and
        # confirm node B is still in view afterwards.
        before = call(bus, "GetWorksheetScroll",
                      None, "(dddddd)").unpack()
        call(bus, "GrabWorksheetFocus", None, None)
        # Spin the GLib main loop briefly so any deferred scroll
        # adjustment from focus-in lands before we read.
        ctx = GLib.MainContext.default()
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline:
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.01)
        after = call(bus, "GetWorksheetScroll",
                     None, "(dddddd)").unpack()
        assert_node_in_viewport(
                bus, NODE_B,
                "after grab_focus on the worksheet "
                f"(scroll before={before}, after={after})")

        print("PASS: clicking a debug-pane row's from-button arms "
              "the worksheet focus pulse on the right UUID, scrolls "
              "the originating node into view, and the scroll "
              "survives a subsequent grab_focus on the worksheet.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
