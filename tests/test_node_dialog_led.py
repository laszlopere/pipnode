#!/usr/bin/env python3
"""Functional test for the PnLed settings dialog's conditional row.

Drives the editor over the ``org.pipas.pipnode.Worksheet`` D-Bus
interface and verifies the Phase-7.5 declarative settings schema: the
``hold-ms`` row is sensitive only while ``mode`` is Flash, and tracks
the mode live.

Before Phase 7.5 this greying was hand-wired in pn-led-gui.c's
build_property_editor; it is now a GTK-free ``enable_when_eq`` in the
core pn-led.c PnSettingsSchema, rendered by the GUI tier.  This test
guards that the renderer reproduces the exact behaviour:

  1. STRUCTURE: the dialog shows Class | Node | Led (the auto property
     tab survives — the schema declares no named tab, it only customises
     one row).
  2. INITIAL: with the default mode (Flash) the ``hold-ms`` spinner is
     sensitive.
  3. LIVE: setting ``mode`` to Steady greys ``hold-ms`` out; setting it
     back to Flash re-enables it — proving the notify::mode tracking the
     schema installs fires on every change.

Exits 0 on success, non-zero on the first failure.
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

# Class | Node | Led: the schema declares no named tab, so the auto
# "Led" property tab stays; the schema only greys the hold-ms row.
EXPECTED_TABS = ["Class", "Node", "Led"]

# PnLedMode enum nicks (lib/pn-led.c).
MODE_FLASH      = "Flash"
MODE_STEADY     = "Steady (level)"
MODE_BLINK_SLOW = "Blink Slow"
MODE_BLINK_FAST = "Blink Fast"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def say(msg: str) -> None:
    print(msg)


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
        BUS_NAME, OBJECT_PATH, WS_IFACE, method, params,
        GLib.VariantType.new(reply_type) if reply_type else None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def build_worksheet_json() -> dict:
    return {
        "format":  "pipnode",
        "version": 1,
        "nodes": [
            {
                "type":       "PnLed",
                "name":       "Lamp",
                "position":   {"x": 120.0, "y": 120.0},
                "properties": {},
            },
        ],
        "connections": [],
    }


def hold_sensitive(bus) -> bool:
    return call(bus, "GetDialogEditorSensitive",
                GLib.Variant("(s)", ("hold-ms",)), "(b)").unpack()[0]


def set_mode(bus, nick: str) -> None:
    ok = call(bus, "SetNodeProperty",
              GLib.Variant("(uss)", (0, "mode", nick)), "(b)").unpack()[0]
    if not ok:
        fail(f"SetNodeProperty(mode={nick!r}) returned False")
    # Let the notify::mode handler run on the main loop.
    ctx = GLib.MainContext.default()
    deadline = time.monotonic() + 1.0
    while time.monotonic() < deadline:
        while ctx.pending():
            ctx.iteration(False)
        time.sleep(0.02)


def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")
    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    proc = subprocess.Popen(
        [PIPNODE, f"--dbus-name={BUS_NAME}"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    try:
        if not wait_for_bus_name(bus, BUS_NAME, timeout=15.0):
            fail("pipnode never registered its DBus name")

        with tempfile.NamedTemporaryFile(
                mode="w", suffix=".json", delete=False) as fp:
            json.dump(build_worksheet_json(), fp)
            tmp_path = fp.name
        try:
            call(bus, "LoadFromFile", GLib.Variant("(s)", (tmp_path,)), None)
        finally:
            os.unlink(tmp_path)

        if call(bus, "GetNodeCount", None, "(u)").unpack()[0] != 1:
            fail("expected exactly one node after LoadFromFile")

        if not call(bus, "OpenNodeDialog",
                    GLib.Variant("(u)", (0,)), "(b)").unpack()[0]:
            fail("OpenNodeDialog(0) returned False — Led dialog not built")

        titles = list(call(bus, "GetDialogPageTitles", None, "(as)").unpack()[0])
        if titles != EXPECTED_TABS:
            fail(f"dialog tabs are {titles!r}, expected {EXPECTED_TABS!r}")
        say(f"tabs OK: {titles}")

        # Default mode is Flash -> hold-ms editable.
        if not hold_sensitive(bus):
            fail("hold-ms should be sensitive at the default Flash mode")
        say("initial: hold-ms sensitive at mode=Flash")

        set_mode(bus, MODE_STEADY)
        if hold_sensitive(bus):
            fail("hold-ms should be greyed out at mode=Steady (level)")
        say("after mode=Steady: hold-ms greyed out")

        set_mode(bus, MODE_FLASH)
        if not hold_sensitive(bus):
            fail("hold-ms should be re-enabled after switching back to Flash")
        say("after mode=Flash: hold-ms sensitive again")

        # Blink Slow / Blink Fast share Steady's level-driven semantics
        # -- the hold-ms field is meaningless there too, so the same
        # enable_when_eq("mode", "Flash") rule should grey it out.
        for nick in (MODE_BLINK_SLOW, MODE_BLINK_FAST):
            set_mode(bus, nick)
            if hold_sensitive(bus):
                fail(f"hold-ms should be greyed out at mode={nick}")
            say(f"after mode={nick}: hold-ms greyed out")

        set_mode(bus, MODE_FLASH)
        if not hold_sensitive(bus):
            fail("hold-ms should be re-enabled after a Blink->Flash switch")

        print("PASS: PnLed hold-ms sensitivity tracks mode via the schema "
              "enable-when across Flash / Steady / Blink Slow / Blink Fast, "
              "and the dialog shows Class | Node | Led.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except Exception:
            proc.kill()


if __name__ == "__main__":
    run_test()
