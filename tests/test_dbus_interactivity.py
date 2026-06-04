#!/usr/bin/env python3
"""Functional test: interactivity over D-Bus (TODO #40.12-40.14).

Phase E of the automation interface lets a client *observe and drive a
running flow* rather than just author it blind:

  40.12 Selection & view
        SelectNodes/GetSelection/ClearSelection, FocusNode, CenterOn,
        FitToContent, GetNodeGeometry, SetWorksheetScroll (+ the existing
        GetWorksheetScroll).

  40.13 Message injection + readback
        InjectMessage(uuid, json) / InjectMessageOnInput(uuid, input, json)
        deliver a message to a node's input as if a wire carried it;
        GetLastOutputMessage(uuid) reads back the last message a node
        emitted — so a flow can be exercised and its result read without a
        Debug node.

  40.14 Signals for live observation
        NodeAdded/NodeRemoved/NodeMoved/NodeRenamed/NodePropertyChanged,
        WireAdded/WireRemoved, SelectionChanged, MessageEmitted (on
        .Worksheet) and SheetChanged/DocumentModified (on .Editor),
        bridged from the model's GObject notifications.

Run directly:

    python3 tests/test_dbus_interactivity.py

Set $PIPNODE to override the path to the binary; defaults to the in-tree
build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import json
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
ED_IFACE    = "org.pipas.pipnode.Editor"

ERR = "org.pipas.pipnode.Worksheet.Error."


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


def call(bus, iface: str, method: str, params, reply_type: str | None):
    return bus.call_sync(
        BUS_NAME, OBJECT_PATH, iface, method,
        params,
        GLib.VariantType.new(reply_type) if reply_type else None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def expect(bus, iface: str, method: str, params, reply_type: str | None,
           want: str) -> None:
    try:
        call(bus, iface, method, params, reply_type)
    except GLib.Error as e:
        got = Gio.DBusError.get_remote_error(e)
        if got != want:
            fail(f"{method}: expected error {want!r}, got {got!r} "
                 f"(message: {e.message!r})")
        return
    fail(f"{method}: expected error {want!r} but the call succeeded")


def ws(bus, method, params=None, reply=None):
    return call(bus, WS_IFACE, method, params, reply)


def ed(bus, method, params=None, reply=None):
    return call(bus, ED_IFACE, method, params, reply)


def add_node(bus, type_name: str, x: float, y: float) -> str:
    return ws(bus, "AddNodeReturningUuid",
              GLib.Variant("(sdd)", (type_name, x, y)), "(s)").unpack()[0]


# --- signal collection ------------------------------------------------

EVENTS: list[tuple[str, tuple]] = []


def on_signal(_conn, _sender, _path, iface, name, params):
    EVENTS.append((name, params.unpack()))


def pump(timeout: float = 0.6) -> None:
    """Iterate the default main context so queued D-Bus signals arrive."""
    ctx = GLib.MainContext.default()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        while ctx.pending():
            ctx.iteration(False)
        time.sleep(0.02)


def seen(name: str, pred=None) -> bool:
    for n, args in EVENTS:
        if n == name and (pred is None or pred(args)):
            return True
    return False


def require(name: str, pred=None, ctx: str = "") -> None:
    if not seen(name, pred):
        fail(f"expected D-Bus signal {name!r} {ctx} but it was not emitted "
             f"(saw: {sorted({n for n, _ in EVENTS})})")


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

        ed(bus, "New")

        # Subscribe to every signal on our object before doing any work, so
        # the live-observation bridge (40.14) can be checked end to end.
        for iface in (WS_IFACE, ED_IFACE):
            bus.signal_subscribe(
                BUS_NAME, iface, None, OBJECT_PATH, None,
                Gio.DBusSignalFlags.NONE, on_signal,
            )
        pump(0.3)
        EVENTS.clear()

        # --- 1. NodeAdded / WireAdded signals on the build (40.14) ------
        a = add_node(bus, "PnKnob", 100.0, 100.0)
        b = add_node(bus, "PnTopic", 360.0, 100.0)
        c = add_node(bus, "PnDebug", 620.0, 100.0)
        pump()
        require("NodeAdded", lambda args: args[0] == a and args[1] == "PnKnob",
                "for the Knob")
        require("NodeAdded", lambda args: args[0] == b,
                "for the Topic")
        # The first edit after New flips the document's modified flag,
        # which the bridge forwards as DocumentModified(true) (40.14).
        require("DocumentModified", lambda args: args[0] is True,
                "after the first structural edit")

        wire = ws(bus, "Connect",
                  GLib.Variant("(ssi)", (b, c, 0)), "(s)").unpack()[0]
        pump()
        require("WireAdded",
                lambda args: args[0] == wire and args[1] == b and args[2] == c,
                "for the Topic->Debug connection")

        # --- 2. Selection (40.12) ---------------------------------------
        EVENTS.clear()
        matched = ws(bus, "SelectNodes",
                     GLib.Variant("(as)", ([a, b, "no-such-uuid"],)),
                     "(u)").unpack()[0]
        if matched != 2:
            fail(f"SelectNodes matched {matched}, expected 2 (one bogus uuid)")

        sel = set(ws(bus, "GetSelection", None, "(as)").unpack()[0])
        if sel != {a, b}:
            fail(f"GetSelection returned {sel!r}, expected {{a, b}}")

        pump()
        require("SelectionChanged",
                lambda args: set(args[0]) == {a, b},
                "after SelectNodes")

        ws(bus, "ClearSelection")
        if ws(bus, "GetSelection", None, "(as)").unpack()[0]:
            fail("GetSelection not empty after ClearSelection")

        # --- 3. View geometry & scroll (40.12) --------------------------
        gx, gy, gw, gh = ws(bus, "GetNodeGeometry",
                            GLib.Variant("(s)", (a,)), "(dddd)").unpack()
        if gw <= 0.0 or gh <= 0.0:
            fail(f"GetNodeGeometry gave non-positive size {gw}x{gh}")
        # Geometry's (x, y) must agree with the node's reported position.
        _, _, _, nx, ny, _, _ = ws(bus, "GetNodeByUuid",
                                    GLib.Variant("(s)", (a,)),
                                    "(sssddbb)").unpack()
        if abs(gx - nx) > 0.01 or abs(gy - ny) > 0.01:
            fail(f"GetNodeGeometry pos ({gx},{gy}) != GetNodeByUuid "
                 f"({nx},{ny})")

        # Unknown uuids are rejected with the stable error name.
        expect(bus, WS_IFACE, "GetNodeGeometry",
               GLib.Variant("(s)", ("nope",)), "(dddd)", ERR + "NodeNotFound")
        expect(bus, WS_IFACE, "FocusNode",
               GLib.Variant("(s)", ("nope",)), None, ERR + "NodeNotFound")
        expect(bus, WS_IFACE, "CenterOn",
               GLib.Variant("(s)", ("nope",)), None, ERR + "NodeNotFound")

        # These drive the live viewport; they must at least not error.
        ws(bus, "FocusNode", GLib.Variant("(s)", (a,)))
        ws(bus, "CenterOn", GLib.Variant("(s)", (c,)))
        ws(bus, "FitToContent")
        ws(bus, "SetWorksheetScroll", GLib.Variant("(dd)", (0.0, 0.0)))
        # GetWorksheetScroll returns six doubles; just confirm the shape.
        scroll = ws(bus, "GetWorksheetScroll", None, "(dddddd)").unpack()
        if len(scroll) != 6:
            fail("GetWorksheetScroll did not return six doubles")

        # --- 4. Message injection + readback (40.13) --------------------
        # Topic re-emits whatever it receives (stamping its own topic), so
        # injecting a value bag lets us read it straight back off the node.
        before = ws(bus, "GetLastOutputMessage",
                    GLib.Variant("(s)", (b,)), "(s)").unpack()[0]
        if before != "":
            fail("GetLastOutputMessage should be empty before any emission")

        EVENTS.clear()
        ws(bus, "InjectMessage",
           GLib.Variant("(ss)", (b, json.dumps({"value": 42.5,
                                                "label": "hi"}))))
        out = ws(bus, "GetLastOutputMessage",
                 GLib.Variant("(s)", (b,)), "(s)").unpack()[0]
        env = json.loads(out)
        if env.get("data", {}).get("value") != 42.5:
            fail(f"GetLastOutputMessage lost the injected value: {out!r}")
        if env.get("data", {}).get("label") != "hi":
            fail(f"GetLastOutputMessage lost the injected label: {out!r}")
        # An injected message stands in for a wire delivery, so it has no
        # source node; Topic re-emits it unchanged, hence an empty from_id.
        if env.get("from_id") != "":
            fail(f"injected message gained a source: {env.get('from_id')!r}")

        # Injection drove an emission, so MessageEmitted must have fired.
        pump()
        require("MessageEmitted",
                lambda args: args[0] == b
                and json.loads(args[2]).get("data", {}).get("value") == 42.5,
                "after InjectMessage")

        # Envelope form is accepted too (explicit data sub-object + topic).
        ws(bus, "InjectMessage",
           GLib.Variant("(ss)", (b, json.dumps({"topic": "ignored-by-topic",
                                                "data": {"value": 7.0}}))))
        out2 = json.loads(ws(bus, "GetLastOutputMessage",
                             GLib.Variant("(s)", (b,)), "(s)").unpack()[0])
        if out2.get("data", {}).get("value") != 7.0:
            fail(f"envelope-form InjectMessage lost its value: {out2!r}")

        # Malformed JSON and unknown node are rejected by the contract.
        expect(bus, WS_IFACE, "InjectMessage",
               GLib.Variant("(ss)", (b, "{ not json")), None,
               ERR + "BadPropertyValue")
        expect(bus, WS_IFACE, "InjectMessage",
               GLib.Variant("(ss)", ("nope", "{}")), None,
               ERR + "NodeNotFound")
        expect(bus, WS_IFACE, "GetLastOutputMessage",
               GLib.Variant("(s)", ("nope",)), "(s)", ERR + "NodeNotFound")
        # Out-of-range input port.
        expect(bus, WS_IFACE, "InjectMessageOnInput",
               GLib.Variant("(sis)", (b, 5, "{}")), None,
               ERR + "BadPropertyValue")

        # --- 5. NodeMoved / NodeRenamed / NodeRemoved signals (40.14) ---
        EVENTS.clear()
        ws(bus, "MoveNode", GLib.Variant("(sdd)", (a, 200.0, 220.0)))
        ws(bus, "RenameNode", GLib.Variant("(ss)", (a, "Dial One")))
        pump()
        require("NodeMoved",
                lambda args: args[0] == a and args[1] == 200.0
                and args[2] == 220.0, "after MoveNode")
        require("NodeRenamed",
                lambda args: args[0] == a and args[1] == "Dial One",
                "after RenameNode")

        EVENTS.clear()
        ws(bus, "Disconnect", GLib.Variant("(s)", (wire,)))
        ws(bus, "DeleteNode", GLib.Variant("(s)", (c,)))
        pump()
        require("WireRemoved", lambda args: args[0] == wire,
                "after Disconnect")
        require("NodeRemoved", lambda args: args[0] == c, "after DeleteNode")

        # --- 6. Editor-level signals: SheetChanged / DocumentModified ---
        EVENTS.clear()
        actual = ed(bus, "AddSheet", GLib.Variant("(s)", ("Logic",)),
                    "(s)").unpack()[0]
        ed(bus, "SelectSheet", GLib.Variant("(s)", (actual,)))
        pump()
        require("SheetChanged", lambda args: args[0] == actual,
                "after SelectSheet")

        print("PASS: D-Bus interactivity surface (TODO #40.12-40.14) — "
              "selection/view, message inject+readback, and live signals "
              "all behave")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    run_test()
