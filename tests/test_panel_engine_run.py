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
        return self.bus.signal_subscribe(
            self.bus_name, ENGINE_IFACE, "ValueChanged", self.obj_path,
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
          "and autosaved the last value on CloseWorksheet.")


if __name__ == "__main__":
    run_test()
