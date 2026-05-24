#!/usr/bin/env python3
"""Functional test: the background engine runs a worksheet for an XFCE
panel applet over the ``org.pipas.pipnode.Engine`` D-Bus interface.

``pipnode-editor --gapplication-service`` is the engine a panel applet
talks to.  Each worksheet runs in a panel-backed window kept hidden — its
flow ticks off the main loop — while the applet reads and drives it by
file path.  This test pins that contract end to end:

  * ``RunWorksheet`` loads a worksheet whose Panel Input feeds a Panel
    Display; the Panel Input's startup announce propagates, so
    ``GetDisplayValue`` settles on the input's saved value;
  * ``SetInput`` drives the Panel Input and the new value reaches the
    display, both via ``GetDisplayValue`` and the ``ValueChanged`` signal
    (which carries the worksheet path so one engine can serve many
    applets);
  * ``CloseWorksheet`` autosaves the worksheet (the last driven value is
    persisted to the file) so a panel applet survives an engine restart.

Driven over a private bus name (``--dbus-name``) so the test never
collides with a real engine.  Run directly:

    python3 tests/test_panel_engine_run.py
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

# A worksheet whose Panel Input (saved value 7) feeds a Panel Display.
WORKSHEET = {
    "format": "pipnode",
    "version": 1,
    "nodes": [
        {
            "type": "PnPanelInput",
            "uuid": "11111111-1111-1111-1111-111111111111",
            "name": "Panel Input 1",
            "worksheet": "Worksheet",
            "position": {"x": 120.0, "y": 80.0},
            "properties": {"value": 7.0},
        },
        {
            "type": "PnPanelDisplay",
            "uuid": "22222222-2222-2222-2222-222222222222",
            "name": "Panel Display 1",
            "worksheet": "Worksheet",
            "position": {"x": 420.0, "y": 80.0},
            "properties": {},
        },
    ],
    "connections": [
        {
            "source_id": "11111111-1111-1111-1111-111111111111",
            "target_id": "22222222-2222-2222-2222-222222222222",
        }
    ],
    "sheets": ["Worksheet"],
    "active_sheet": "Worksheet",
}


# --- Per-node panel widget mirroring -------------------------------------
#
# A worksheet that drives a Countdown and a steady LED from one Panel
# Input, with a panel layout that snaps both onto the band but parks a
# second Countdown off it.  Exercises GetLayout (band membership + order)
# and the WidgetChanged per-node state signal.
CD_BAND  = "33333333-3333-3333-3333-333333333333"
LED_BAND = "44444444-4444-4444-4444-444444444444"
CD_OFF   = "55555555-5555-5555-5555-555555555555"
IN_UUID  = "66666666-6666-6666-6666-666666666666"

# The band sits at PN_PE_PANEL_TOP (24) with the widget height (36); a
# placement at y == 24 is on the band, y == 200 is well clear of it.
LAYOUT_WORKSHEET = {
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
            "uuid": CD_BAND,
            "name": "Countdown band",
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
            "uuid": CD_OFF,
            "name": "Countdown off-band",
            "worksheet": "Worksheet",
            "position": {"x": 360.0, "y": 340.0},
            "properties": {},
        },
    ],
    "connections": [
        {"source_id": IN_UUID, "target_id": CD_BAND},
        {"source_id": IN_UUID, "target_id": LED_BAND},
    ],
    "panel_layout": {
        CD_BAND:  {"x": 0.0,   "y": 24.0},
        LED_BAND: {"x": 100.0, "y": 24.0},
        CD_OFF:   {"x": 0.0,   "y": 200.0},
    },
    # Saved with the panel-editor tab open: the engine must still report the
    # layout, reading the flow rather than the (then non-worksheet) active
    # tab — a regression that once blanked the applet to its icon.
    "panel_editor_open": True,
    "sheets": ["Worksheet"],
    "active_sheet": "Worksheet",
}

# A representable node with NO saved panel layout: the engine falls back to
# showing every representable node so a never-arranged worksheet is not
# blank.
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

    def get_display(self, path: str) -> str:
        return self.call("GetDisplayValue",
                         GLib.Variant("(s)", (path,)), "(s)")[0]

    def get_layout(self, path: str) -> dict:
        raw = self.call("GetLayout", GLib.Variant("(s)", (path,)), "(s)")[0]
        return json.loads(raw)

    def set_input(self, path: str, value: float) -> None:
        self.call("SetInput", GLib.Variant("(sd)", (path, value)), "")

    def send_event(self, path: str, event: str, button: int) -> None:
        self.call("SendEvent",
                  GLib.Variant("(ssu)", (path, event, button)), "")

    def present_editor(self, path: str) -> None:
        self.call("PresentEditor", GLib.Variant("(s)", (path,)), "")

    def close_worksheet(self, path: str) -> None:
        self.call("CloseWorksheet", GLib.Variant("(s)", (path,)), "")

    def subscribe_value_changed(self, on_signal) -> int:
        return self.subscribe("ValueChanged", on_signal)

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


def check_panel_widgets(engine: "Engine", tmpdir: str) -> None:
    """GetLayout reports the band-snapped Countdown/LED in order (the
    off-band one excluded), WidgetChanged carries each node's live state,
    and a layout-less worksheet falls back to all representable nodes."""
    layout_path = os.path.join(tmpdir, "layout.json")
    with open(layout_path, "w", encoding="utf-8") as fh:
        json.dump(LAYOUT_WORKSHEET, fh)

    widgets: list[tuple] = []
    engine.subscribe(
        "WidgetChanged",
        lambda p, u, s: widgets.append((p, u, json.loads(s))))

    engine.run_worksheet(layout_path)

    # Only the two band-snapped widgets, left to right by x; the off-band
    # Countdown must not appear.
    layout = engine.get_layout(layout_path)
    order  = [w["uuid"] for w in layout.get("widgets", [])]
    if order != [CD_BAND, LED_BAND]:
        fail(f"GetLayout band membership/order wrong: {order!r}")
    kinds = {w["uuid"]: w["state"]["kind"] for w in layout["widgets"]}
    if kinds.get(CD_BAND) != "countdown" or kinds.get(LED_BAND) != "led":
        fail(f"GetLayout widget kinds wrong: {kinds!r}")
    if CD_OFF in order:
        fail("off-band Countdown leaked into the layout")

    # A designed layout is "positioned" and carries each widget's band x,
    # offset so the leftmost sits at 0 — the applet uses these to mirror the
    # editor's spacing/grouping (band x 0 and 100 -> x 0 and 100).
    if layout.get("positioned") is not True:
        fail(f"GetLayout did not mark a designed layout positioned: {layout!r}")
    xs = {w["uuid"]: w.get("x") for w in layout["widgets"]}
    if xs.get(CD_BAND) != 0.0 or xs.get(LED_BAND) != 100.0:
        fail(f"GetLayout widget x positions wrong (expected 0 and 100): {xs!r}")

    # Drive the single Panel Input: the Countdown shows 120 s and the
    # steady LED latches on, each pushed as a WidgetChanged keyed by UUID.
    engine.set_input(layout_path, 120.0)

    def countdown_seen() -> bool:
        return any(p == layout_path and u == CD_BAND
                   and s.get("seconds") == 120
                   for p, u, s in widgets)

    def led_seen() -> bool:
        return any(p == layout_path and u == LED_BAND
                   and s.get("lit") is True
                   for p, u, s in widgets)

    if not wait_for(lambda: countdown_seen() and led_seen(), timeout=8.0):
        fail("WidgetChanged for the countdown (seconds=120) and the lit LED "
             f"were not both seen; got {widgets!r}")

    # A worksheet with a representable node but no saved layout: the engine
    # falls back to showing it rather than leaving the applet blank.
    fb_path = os.path.join(tmpdir, "fallback.json")
    with open(fb_path, "w", encoding="utf-8") as fh:
        json.dump(FALLBACK_WORKSHEET, fh)
    engine.run_worksheet(fb_path)
    fb_layout = engine.get_layout(fb_path)
    fb_order = [w["uuid"] for w in fb_layout.get("widgets", [])]
    if fb_order != [CD_FALLBACK]:
        fail(f"fallback layout did not list the lone Countdown: {fb_order!r}")
    if fb_layout.get("positioned") is not False:
        fail(f"fallback layout should not be positioned: {fb_layout!r}")

    engine.close_worksheet(layout_path)
    engine.close_worksheet(fb_path)


def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode editor binary not found or not executable: {PIPNODE}")
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    tmpdir = tempfile.mkdtemp(prefix="pn-panel-")
    path = os.path.join(tmpdir, "sheet.json")
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(WORKSHEET, fh)

    signals: list[tuple] = []
    engine = Engine()
    try:
        if not engine.wait_ready():
            fail("engine never registered its D-Bus name")

        engine.subscribe_value_changed(lambda p, v: signals.append((p, v)))

        engine.run_worksheet(path)

        # The Panel Input's startup announce (value 7) propagates to the
        # display shortly after load.
        if not wait_for(lambda: engine.get_display(path) == "7", timeout=8.0):
            fail("display did not settle on the input's startup value 7; "
                 f"got {engine.get_display(path)!r}")

        # Drive the input; the ValueChanged signal must carry this
        # worksheet's path and the new value (one engine multiplexes many
        # applets), and the display must read it back.
        engine.set_input(path, 99.0)
        if not wait_for(lambda: (path, "99") in signals, timeout=8.0):
            fail(f"no ValueChanged(path, '99') signal seen; got {signals!r}")
        if engine.get_display(path) != "99":
            fail(f"SetInput(99) did not reach the display; "
                 f"got {engine.get_display(path)!r}")

        # A mouse click reaches the Panel Input, which emits the button
        # number on data.value (and a human-readable sentence on data.output
        # for a Text to Speech node); the wired Panel Display reads the value
        # back as "3" for a right click.
        engine.send_event(path, "click", 3)
        if not wait_for(lambda: engine.get_display(path) == "3", timeout=8.0):
            fail("SendEvent(click, right) did not reach the display; "
                 f"got {engine.get_display(path)!r}")

        # PresentEditor must succeed (opens the running flow for editing).
        engine.present_editor(path)

        # Per-node panel widget mirroring: layout + per-widget state.
        check_panel_widgets(engine, tmpdir)

        # Drive a final value and close: the worksheet autosaves, so the
        # last input value is persisted to disk.
        engine.set_input(path, 55.0)
        wait_for(lambda: engine.get_display(path) == "55", timeout=8.0)
        engine.close_worksheet(path)

        with open(path, encoding="utf-8") as fh:
            saved = json.load(fh)
        inp = next(n for n in saved["nodes"] if n["type"] == "PnPanelInput")
        if abs(inp["properties"].get("value", 0.0) - 55.0) > 1e-9:
            fail("CloseWorksheet did not autosave the last input value 55; "
                 f"file has {inp['properties'].get('value')!r}")
    finally:
        engine.stop()

    print("PASS: the engine ran the worksheet headless (RunWorksheet), "
          "mirrored the Panel Input to the Panel Display via GetDisplayValue "
          "and the ValueChanged signal, drove it by value (SetInput) and by "
          "a mouse event (SendEvent), opened it for editing (PresentEditor), "
          "autosaved the last value on CloseWorksheet, and mirrored each "
          "band-snapped Countdown/LED node to the applet (GetLayout membership "
          "and order, WidgetChanged per-node state, layout-less fallback).")


if __name__ == "__main__":
    run_test()
