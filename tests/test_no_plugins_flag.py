#!/usr/bin/env python3
"""Functional test: ``pipnode-editor --no-plugins`` starts the session
with the built-in nodes only and skips the plugin scan entirely.

The flag is for isolating a misbehaving plugin (or for getting a clean
editor up when a stale/broken plugin would otherwise hang the dlopen
scan).  Because plugin discovery has to finish before the first window
builds its palette — which is ahead of the GApplication option parser —
main() detects the flag with a manual argv scan rather than reading it
back from the parsed options.  This test pins that wiring.

Two editor instances are launched, both with ``PIPNODE_PLUGIN_PATH``
pointed at the in-tree sample plugin under ``tests/plugins/echo``:

  * the control run (no flag) must resolve the plugin-provided
    ``PnEcho`` type — proof the .so loaded and registered its GType;
  * the ``--no-plugins`` run must *not* know ``PnEcho`` (AddNode raises
    a D-Bus error), yet must still create a built-in ``PnDebug`` —
    proof the flag suppressed only plugins, not the built-ins.

Each instance is driven over the ``org.pipas.pipnode.Worksheet`` D-Bus
interface, the same harness test_plugin_gui_companion.py uses.

Run directly:

    PIPNODE_PLUGIN_DIR=tests/plugins/echo/.libs \\
    python3 tests/test_no_plugins_flag.py
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


ROOT       = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE    = os.environ.get("PIPNODE",
                            os.path.join(ROOT, "src", "pipnode-editor"))
PLUGIN_DIR = os.environ.get("PIPNODE_PLUGIN_DIR",
                            os.path.join(ROOT, "tests", "plugins",
                                         "echo", ".libs"))

WS_IFACE = "org.pipas.pipnode.Worksheet"


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


def add_node(bus, bus_name: str, obj_path: str, type_name: str):
    """Return the new node's index, or raise GLib.Error if the editor
    rejects the type (which is what an unloaded plugin type looks like)."""
    reply = bus.call_sync(
        bus_name, obj_path, WS_IFACE, "AddNode",
        GLib.Variant("(sdd)", (type_name, 100.0, 100.0)),
        GLib.VariantType.new("(i)"),
        Gio.DBusCallFlags.NONE, 5000, None,
    )
    return reply.unpack()[0]


class Editor:
    """A pipnode-editor process under its own private D-Bus name, with
    PIPNODE_PLUGIN_PATH pointed at the echo sample plugin."""

    def __init__(self, extra_args: list[str]) -> None:
        self.bus_name = (f"org.pipas.pipnode."
                         f"t{os.getpid()}_{uuid.uuid4().hex[:8]}")
        self.obj_path = "/" + self.bus_name.replace(".", "/")
        env = os.environ.copy()
        env["PIPNODE_PLUGIN_PATH"] = PLUGIN_DIR
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.proc = subprocess.Popen(
            [PIPNODE, f"--dbus-name={self.bus_name}"] + extra_args,
            env=env,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    def wait_ready(self) -> bool:
        return wait_for_bus_name(self.bus, self.bus_name, timeout=15.0)

    def add(self, type_name: str):
        return add_node(self.bus, self.bus_name, self.obj_path, type_name)

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode editor binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    if not os.path.isdir(PLUGIN_DIR):
        fail(f"plugin directory does not exist: {PLUGIN_DIR}\n"
             f"(expected the in-tree sample plugin to be built first)")

    sos = [n for n in os.listdir(PLUGIN_DIR)
           if n.startswith("pn_echo") and "-gui" not in n
           and (n.endswith(".so") or n.endswith(".dylib"))]
    if not sos:
        fail(f"no logic pn_echo.so found under {PLUGIN_DIR}; "
             f"contents: {os.listdir(PLUGIN_DIR)}")

    # --- Control: without the flag, the plugin type must resolve. ---
    control = Editor(extra_args=[])
    try:
        if not control.wait_ready():
            fail("control editor never registered its DBus name")

        try:
            idx = control.add("PnEcho")
        except GLib.Error as e:
            fail("control run (no --no-plugins) could not add 'PnEcho' — "
                 "the sample plugin failed to load even without the flag, "
                 f"so the test cannot tell the flag apart: {e.message}")
        if idx < 0:
            fail("control run returned a negative index for 'PnEcho' — "
                 "the plugin type did not register")
    finally:
        control.stop()

    # --- Flag set: the plugin type must be gone, built-ins must remain. ---
    noplug = Editor(extra_args=["--no-plugins"])
    try:
        if not noplug.wait_ready():
            fail("--no-plugins editor never registered its DBus name")

        plugin_rejected = False
        try:
            noplug.add("PnEcho")
        except GLib.Error:
            # Exactly what we want: the type is unknown because the
            # plugin .so was never scanned or dlopened.
            plugin_rejected = True
        if not plugin_rejected:
            fail("--no-plugins still resolved the plugin type 'PnEcho' — "
                 "the flag did not suppress plugin loading")

        try:
            dbg = noplug.add("PnDebug")
        except GLib.Error as e:
            fail("--no-plugins also lost the built-in 'PnDebug' node — the "
                 f"flag suppressed too much: {e.message}")
        if dbg < 0:
            fail("--no-plugins returned a negative index for the built-in "
                 "'PnDebug' — built-in nodes should be unaffected")
    finally:
        noplug.stop()

    print("PASS: pipnode-editor --no-plugins suppressed the plugin scan "
          "(plugin type 'PnEcho' unknown) while built-in nodes still "
          "registered ('PnDebug' added); the default run loaded the same "
          "plugin fine.")


if __name__ == "__main__":
    run_test()
