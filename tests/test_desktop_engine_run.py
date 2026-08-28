#!/usr/bin/env python3
"""Functional test: the background engine serves a worksheet's DESKTOP
layout to the standalone viewer over ``org.pipas.pipnode.Engine``.

The desktop twin of ``test_panel_engine_run.py``.  ``pipnode-desktop`` is
a dumb D-Bus client of the same engine the XFCE panel applet talks to: it
runs nothing itself, it asks for the layout (``GetDesktopLayout``),
follows it (``DesktopLayoutChanged``) and pushes each node's live state
(``WidgetChanged``) into its widget.  This test pins the engine half of
that contract, which is the half the viewer cannot be wrong about:

  * ``GetDesktopLayout`` lists exactly the nodes the Desktop Layout editor
    placed, each with its window-relative ``x``/``y`` verbatim, alongside
    the ``window`` block (size, title, widget height) the viewer builds its
    window from;
  * the panel and desktop layouts of one document are independent — a node
    on the panel band does not appear in the window, and vice versa;
  * ``WidgetChanged`` carries live per-node state, serving both surfaces;
  * a worksheet with no desktop layout falls back to every representable
    node, so a viewer opened on an unarranged worksheet is not blank.

Driven over a private bus name (``--dbus-name``) so the test never
collides with a real engine.  Run directly:

    python3 tests/test_desktop_engine_run.py
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
PIPNODE = os.environ.get("PIPNODE",
                         os.path.join(ROOT, "src", "pipnode-editor"))

ENGINE_IFACE = "org.pipas.pipnode.Engine"

# A worksheet whose Panel Input drives a Countdown and a steady LED.  The
# Countdown is placed in the DESKTOP window only, the LED on the PANEL band
# only, and a second Countdown nowhere — so one document exercises both
# layouts and their independence in a single run.
CD_WIN   = "33333333-3333-3333-3333-333333333333"
LED_BAND = "44444444-4444-4444-4444-444444444444"
CD_NONE  = "55555555-5555-5555-5555-555555555555"
IN_UUID  = "66666666-6666-6666-6666-666666666666"

WORKSHEET = {
    "format": "pipnode",
    "version": 1,
    "nodes": [
        {
            "type": "PnPanelInput",
            "uuid": IN_UUID,
            "name": "Panel Input 1",
            "worksheet": "Worksheet",
            "position": {"x": 60.0, "y": 60.0},
            "properties": {"value": 0.0},
        },
        {
            "type": "PnCountdown",
            "uuid": CD_WIN,
            "name": "Countdown window",
            "worksheet": "Worksheet",
            "position": {"x": 360.0, "y": 60.0},
            "properties": {},
        },
        {
            "type": "PnLed",
            "uuid": LED_BAND,
            "name": "LED band",
            "worksheet": "Worksheet",
            "position": {"x": 360.0, "y": 200.0},
            "properties": {"mode": "Steady (level)"},
        },
        {
            "type": "PnCountdown",
            "uuid": CD_NONE,
            "name": "Countdown unplaced",
            "worksheet": "Worksheet",
            "position": {"x": 360.0, "y": 340.0},
            "properties": {},
        },
    ],
    "connections": [
        {"source_id": IN_UUID, "target_id": CD_WIN},
        {"source_id": IN_UUID, "target_id": LED_BAND},
    ],
    # The band sits at PN_PE_PANEL_TOP (24): only the LED is on the panel.
    "panel_layout": {
        LED_BAND: {"x": 100.0, "y": 24.0},
    },
    # Only the Countdown is in the window, at its own coordinates.
    "desktop_layout": {
        CD_WIN: {"x": 40.0, "y": 210.0},
    },
    "desktop_window": {"width": 800, "height": 480, "title": "Hallway"},
    # Saved with the desktop-editor tab open: the engine must still report
    # the layout, reading the flow rather than the (then non-worksheet)
    # active tab.
    "desktop_editor_open": True,
    "sheets": ["Worksheet"],
    "active_sheet": "Worksheet",
}

# A representable node with NO saved desktop layout: the engine falls back
# to showing every representable node so a never-arranged worksheet does
# not open a blank window.
CD_FALLBACK = "77777777-7777-7777-7777-777777777777"
FALLBACK_WORKSHEET = {
    "format": "pipnode",
    "version": 1,
    "nodes": [
        {
            "type": "PnCountdown",
            "uuid": CD_FALLBACK,
            "name": "Countdown",
            "worksheet": "Worksheet",
            "position": {"x": 120.0, "y": 80.0},
            "properties": {},
        },
    ],
    "connections": [],
    "sheets": ["Worksheet"],
    "active_sheet": "Worksheet",
}


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


class Engine:
    """A pipnode-editor running as a background D-Bus service under a
    private bus name."""

    def __init__(self) -> None:
        self.bus_name = (f"org.pipas.pipnode."
                         f"t{os.getpid()}_{uuid.uuid4().hex[:8]}")
        self.obj_path = "/" + self.bus_name.replace(".", "/")
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.proc = subprocess.Popen(
            [PIPNODE, f"--dbus-name={self.bus_name}",
             "--gapplication-service", "--no-plugins"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def wait_ready(self) -> bool:
        return wait_for_bus_name(self.bus, self.bus_name, timeout=15.0)

    def call(self, method: str, args: GLib.Variant, reply_type: str):
        rt = GLib.VariantType.new(reply_type) if reply_type else None
        reply = self.bus.call_sync(
            self.bus_name, self.obj_path, ENGINE_IFACE, method,
            args, rt, Gio.DBusCallFlags.NONE, 5000, None,
        )
        return reply.unpack() if reply_type else None

    def run_worksheet(self, path: str) -> str:
        return self.call("RunWorksheet", GLib.Variant("(s)", (path,)), "(s)")[0]

    def get_layout(self, path: str) -> dict:
        raw = self.call("GetLayout", GLib.Variant("(s)", (path,)), "(s)")[0]
        return json.loads(raw)

    def get_desktop_layout(self, path: str) -> dict:
        raw = self.call("GetDesktopLayout",
                        GLib.Variant("(s)", (path,)), "(s)")[0]
        return json.loads(raw)

    def set_input(self, path: str, value: float) -> None:
        self.call("SetInput", GLib.Variant("(sd)", (path, value)), "")

    def close_worksheet(self, path: str) -> None:
        self.call("CloseWorksheet", GLib.Variant("(s)", (path,)), "")

    def subscribe(self, signal_name: str, on_signal) -> int:
        return self.bus.signal_subscribe(
            self.bus_name, ENGINE_IFACE, signal_name, self.obj_path,
            None, Gio.DBusSignalFlags.NONE,
            lambda c, s, o, i, sig, params, user: on_signal(*params.unpack()),
            None,
        )

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def wait_for(predicate, timeout: float) -> bool:
    """Run the GLib main loop until predicate() is true or @timeout
    elapses.  Pumping the loop is what delivers the engine's D-Bus
    signals to our subscription."""
    if predicate():
        return True

    loop = GLib.MainLoop()
    state = {"ok": False}

    def check():
        if predicate():
            state["ok"] = True
            loop.quit()
            return False
        return True

    GLib.timeout_add(50, check)
    GLib.timeout_add(int(timeout * 1000), loop.quit)
    loop.run()
    return state["ok"] or predicate()


def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode editor binary not found or not executable: {PIPNODE}")
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    tmpdir = tempfile.mkdtemp(prefix="pn-desktop-")
    path = os.path.join(tmpdir, "sheet.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(WORKSHEET, fh)

    widgets: list[tuple] = []
    layouts: list[tuple] = []
    engine = Engine()
    try:
        if not engine.wait_ready():
            fail("engine never registered its D-Bus name")

        engine.subscribe(
            "WidgetChanged",
            lambda p, u, s: widgets.append((p, u, json.loads(s))))
        engine.subscribe(
            "DesktopLayoutChanged",
            lambda p, l: layouts.append((p, json.loads(l))))

        engine.run_worksheet(path)

        # --- the window block the viewer builds its window from ---------
        layout = engine.get_desktop_layout(path)
        window = layout.get("window", {})
        if window.get("width") != 800 or window.get("height") != 480:
            fail(f"GetDesktopLayout window size wrong: {window!r}")
        if window.get("title") != "Hallway":
            fail(f"GetDesktopLayout window title wrong: {window!r}")
        if not isinstance(window.get("widget_height"), int) \
                or window["widget_height"] <= 0:
            fail("GetDesktopLayout did not publish a widget_height: "
                 f"{window!r}")
        if layout.get("positioned") is not True:
            fail(f"a designed desktop layout must be positioned: {layout!r}")

        # --- membership and coordinates ---------------------------------
        placed = {w["uuid"]: w for w in layout.get("widgets", [])}
        if list(placed) != [CD_WIN]:
            fail("GetDesktopLayout should list only the placed Countdown; "
                 f"got {list(placed)!r}")
        if placed[CD_WIN].get("x") != 40.0 or placed[CD_WIN].get("y") != 210.0:
            fail("desktop placement must be reported verbatim "
                 f"(window-relative): {placed[CD_WIN]!r}")
        if placed[CD_WIN]["state"].get("kind") != "countdown":
            fail(f"desktop widget kind wrong: {placed[CD_WIN]!r}")

        # --- the two layouts of one document are independent -------------
        band = [w["uuid"] for w in engine.get_layout(path).get("widgets", [])]
        if band != [LED_BAND]:
            fail(f"panel band membership changed: {band!r}")
        if CD_WIN in band:
            fail("a window widget leaked onto the panel band")
        if LED_BAND in placed:
            fail("a band widget leaked into the desktop window")
        if CD_NONE in placed or CD_NONE in band:
            fail("an unplaced node appeared in a layout")

        # --- live state serves both surfaces -----------------------------
        engine.set_input(path, 120.0)

        def countdown_seen() -> bool:
            return any(p == path and u == CD_WIN and s.get("seconds") == 120
                       for p, u, s in widgets)

        if not wait_for(countdown_seen, timeout=8.0):
            fail("no WidgetChanged(seconds=120) for the window's Countdown; "
                 f"got {widgets!r}")

        # --- the fallback for a never-arranged worksheet ------------------
        fb_path = os.path.join(tmpdir, "fallback.json")
        with open(fb_path, "w", encoding="utf-8") as fh:
            json.dump(FALLBACK_WORKSHEET, fh)
        engine.run_worksheet(fb_path)
        fb = engine.get_desktop_layout(fb_path)
        fb_order = [w["uuid"] for w in fb.get("widgets", [])]
        if fb_order != [CD_FALLBACK]:
            fail(f"fallback layout did not list the lone Countdown: {fb_order!r}")
        if fb.get("positioned") is not False:
            fail(f"fallback layout should not be positioned: {fb!r}")

        engine.close_worksheet(path)
        engine.close_worksheet(fb_path)
    finally:
        engine.stop()

    print("OK: engine serves the desktop layout "
          "(window block, placements, independence from the panel band, "
          "live state, and the unarranged fallback)")


if __name__ == "__main__":
    run_test()
