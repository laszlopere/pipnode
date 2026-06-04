#!/usr/bin/env python3
"""Functional test: the org.pipas.pipnode.Editor interface (TODO #40.3).

Phase A of the editor automation API splits the surface into two
sibling D-Bus interfaces on the SAME object path:

  * org.pipas.pipnode.Worksheet — active-sheet graph operations
  * org.pipas.pipnode.Editor    — document / window / sheet lifecycle
                                  and the API version handshake

The lifecycle methods proper arrive in 40.9-40.11; what lands now is the
split itself plus a version a client reads to feature-detect a modern
(UUID-aware, error-contract-bearing) build before it makes any call.

This test asserts:

  1. CO-EXISTENCE — both interfaces are exported on the one object path
     (introspection lists both), and a Worksheet call and an Editor call
     each succeed against that same path.

  2. VERSION METHOD — Editor.GetApiVersion returns the expected
     (major, minor).

  3. VERSION PROPERTY — the Editor.Version property (read via the
     standard org.freedesktop.DBus.Properties.Get) returns the major
     version, agreeing with GetApiVersion.

Run directly:

    python3 tests/test_dbus_editor_interface.py

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at ``src/pipnode-editor``.
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
ED_IFACE    = "org.pipas.pipnode.Editor"

# The version this build of the interface is expected to advertise; keep
# in step with PN_AUTOMATION_API_VERSION_* in src/pn-application.c.
EXPECT_MAJOR = 1
EXPECT_MINOR = 1


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

        # --- 1. CO-EXISTENCE -----------------------------------------
        # Introspection of the single object path must advertise both
        # automation interfaces.
        xml = call(bus, "org.freedesktop.DBus.Introspectable", "Introspect",
                   None, "(s)").unpack()[0]
        for iface in (WS_IFACE, ED_IFACE):
            if f'interface name="{iface}"' not in xml and \
               f"interface name='{iface}'" not in xml:
                fail(f"introspection of {OBJECT_PATH} does not list {iface}; "
                     f"the two interfaces do not co-exist on one object")

        # A Worksheet call against the shared path still works…
        n = call(bus, WS_IFACE, "GetNodeCount", None, "(u)").unpack()[0]
        if n != 0:
            fail(f"fresh editor reports {n} nodes, expected 0 "
                 "(Worksheet interface broken by the split?)")

        # --- 2. VERSION METHOD ---------------------------------------
        major, minor = call(bus, ED_IFACE, "GetApiVersion",
                            None, "(uu)").unpack()
        if (major, minor) != (EXPECT_MAJOR, EXPECT_MINOR):
            fail(f"GetApiVersion returned {(major, minor)!r}, expected "
                 f"{(EXPECT_MAJOR, EXPECT_MINOR)!r}")

        # --- 3. VERSION PROPERTY -------------------------------------
        # Read it the standard way, through org.freedesktop.DBus.Properties.
        prop = call(bus, "org.freedesktop.DBus.Properties", "Get",
                    GLib.Variant("(ss)", (ED_IFACE, "Version")),
                    "(v)").unpack()[0]
        if prop != EXPECT_MAJOR:
            fail(f"Editor.Version property = {prop!r}, expected {EXPECT_MAJOR} "
                 "(should equal the major version from GetApiVersion)")

        print("PASS: org.pipas.pipnode.Editor co-exists with .Worksheet on the "
              f"shared object path and advertises API version "
              f"{major}.{minor} via both GetApiVersion and the Version "
              "property.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
