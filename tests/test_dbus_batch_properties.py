#!/usr/bin/env python3
"""Functional test: atomic batch property set over D-Bus (TODO #40.6).

The by-UUID single-property getter/setter landed with #40.1; #40.6 adds
the one piece still missing for configuring a node in one round-trip:

  SetNodeProperties(uuid, a{ss})   applied all-or-nothing

This test drives it through a freshly launched editor and asserts:

  1. BATCH WRITE — a single SetNodeProperties call sets several properties
     of mixed type (string / double / uint) at once, and each reads back
     through GetNodePropertyByUuid.

  2. ATOMIC on UnknownProperty — a batch whose last entry names a property
     that does not exist fails with UnknownProperty AND leaves every other
     property in the batch unchanged (nothing was half-applied).

  3. ATOMIC on BadPropertyValue — a batch with an unparseable value (a
     boxed colour property the string codec cannot handle) fails with
     BadPropertyValue and again changes nothing.

  4. ERRORS — an unknown node UUID raises NodeNotFound.

Run directly:

    python3 tests/test_dbus_batch_properties.py

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


def get_prop(bus, node_uuid: str, prop: str) -> str:
    return call(bus, "GetNodePropertyByUuid",
                GLib.Variant("(ss)", (node_uuid, prop)), "(s)").unpack()[0]


def set_prop(bus, node_uuid: str, prop: str, value: str) -> None:
    call(bus, "SetNodePropertyByUuid",
         GLib.Variant("(sss)", (node_uuid, prop, value)), "(b)")


def set_props(bus, node_uuid: str, props: dict) -> None:
    call(bus, "SetNodeProperties",
         GLib.Variant("(sa{ss})", (node_uuid, props)), None)


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
        dial = add_node(bus, "PnDial", 200.0, 160.0)

        # --- 1. BATCH WRITE (mixed types in one call) ----------------
        set_props(bus, dial, {
            "label":       "Boiler",
            "unit":        "degC",
            "min-value":   "10",
            "max-value":   "90",
            "major-ticks": "5",
        })
        for prop, want in (("label", "Boiler"), ("unit", "degC"),
                           ("min-value", "10"), ("max-value", "90"),
                           ("major-ticks", "5")):
            got = get_prop(bus, dial, prop)
            if got != want:
                fail(f"after batch set, {prop} = {got!r}, want {want!r}")

        # --- 2. ATOMIC on UnknownProperty ----------------------------
        # Establish a known baseline, then try a batch that would change
        # 'label' but also names a property that does not exist.
        set_prop(bus, dial, "label", "Baseline")
        expect(bus, "SetNodeProperties",
               GLib.Variant("(sa{ss})", (dial, {
                   "label":          "ShouldNotStick",
                   "no-such-prop":   "x",
               })), None,
               ERR + "UnknownProperty")
        if get_prop(bus, dial, "label") != "Baseline":
            fail("UnknownProperty batch was partially applied: 'label' "
                 f"changed to {get_prop(bus, dial, 'label')!r}")

        # --- 3. ATOMIC on BadPropertyValue ---------------------------
        # 'face-color' is a boxed GdkRGBA the string codec cannot parse,
        # so the whole batch must fail and 'unit' must stay put.
        expect(bus, "SetNodeProperties",
               GLib.Variant("(sa{ss})", (dial, {
                   "unit":       "ShouldNotStick",
                   "face-color": "not-a-color",
               })), None,
               ERR + "BadPropertyValue")
        if get_prop(bus, dial, "unit") != "degC":
            fail("BadPropertyValue batch was partially applied: 'unit' "
                 f"changed to {get_prop(bus, dial, 'unit')!r}")

        # --- 4. ERRORS -----------------------------------------------
        expect(bus, "SetNodeProperties",
               GLib.Variant("(sa{ss})", (BOGUS, {"label": "x"})), None,
               ERR + "NodeNotFound")

        print("PASS: SetNodeProperties writes a mixed-type batch in one call "
              "and is atomic — an unknown property or unparseable value fails "
              "the whole batch (UnknownProperty / BadPropertyValue) and leaves "
              "the node untouched; a bad UUID is NodeNotFound.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
