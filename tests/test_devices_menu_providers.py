#!/usr/bin/env python3
"""Functional test: the editor's Devices menu is provider-driven.

TODO #34 Phase D replaced the hardcoded "Meshtastic…" entry in
src/pn-devices-menu.c with an enumeration of the #PnDeviceProvider
registry (lib/pn-device-provider.c).  A built-in device kind names
itself once, from pn_gui_install_builtin_nodes() — Meshtastic does so
via pn_mesh_dialog_register_provider() — and the menu is built by
walking pn_device_provider_list().

This test launches a fresh editor over the
``org.pipas.pipnode.Worksheet`` D-Bus interface and calls the
GetDeviceProviders method, which snapshots that same registry the menu
reads.  The Meshtastic provider's presence in the returned list proves:

  1. pn_gui_install_builtin_nodes() ran pn_mesh_dialog_register_provider(),
  2. the provider landed in the registry under the expected id, and
  3. therefore the Devices menu (built from the identical list) carries
     a Meshtastic entry — without pn-devices-menu.c naming it.

Run directly:

    python3 tests/test_devices_menu_providers.py
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

# The built-in Meshtastic provider, registered from
# pn_gui_install_builtin_nodes() via pn_mesh_dialog_register_provider().
EXPECTED_PROVIDER = "meshtastic"


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


def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode editor binary not found or not executable: {PIPNODE}")

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
            fail("pipnode editor never registered its DBus name")

        ids = list(call(bus, "GetDeviceProviders", None, "(as)").unpack()[0])

        if not ids:
            fail("GetDeviceProviders returned an empty list — no device "
                 "provider registered; pn_gui_install_builtin_nodes() did "
                 "not run pn_mesh_dialog_register_provider()")

        if EXPECTED_PROVIDER not in ids:
            fail(f"device providers are {ids!r}; the built-in "
                 f"{EXPECTED_PROVIDER!r} provider is missing — the "
                 f"Devices menu would not carry a Meshtastic entry")

        print("PASS: the editor's Devices menu is provider-driven — the "
              f"{EXPECTED_PROVIDER!r} provider is in the registry the menu "
              f"enumerates (providers: {ids!r}).")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    run_test()
