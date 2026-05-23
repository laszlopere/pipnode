#!/usr/bin/env python3
"""Functional test: the editor loads the *bundled* shell plugin's
companion GUI module (TODO #23, Phase 8.3).

The shell plugin is the first bundled two-tier plugin:

    pipnode_shell.so       logic half, links libpipnode-core only (no GTK),
                           exports pn_plugin_init, registers every shell
                           #GType (incl. PnTmuxMonitor);
    pipnode_shell-gui.so   companion GUI module, links the GTK tier,
                           exports pn_plugin_gui_init, and installs the
                           Tmux Monitor node's settings-dialog vfuncs
                           (a live session combobox + a deferred-commit
                           host entry + a runtime status row) onto the
                           already-registered class.

This is the editor-side check; the headless half is covered by running
the logic .so under pipnode-run (it loads with no GTK and the node runs).

The test launches the editor over the ``org.pipas.pipnode.Worksheet``
D-Bus interface, adds a PnTmuxMonitor node, opens its settings dialog,
and probes for the **last-error status row** the companion's
build_class_tab installs.  That row is the read-only display for the
non-writable ``last-error`` property; the auto-generated dialog skips
non-writable properties, so a ``pn-prop-last-error`` widget exists *only*
when the companion's build_class_tab ran.  GetDialogEditorSensitive raises
a D-Bus error when no such widget exists, so a clean return is the
positive signal that:

  1. the companion pipnode_shell-gui.so was discovered next to the logic
     .so, 2. pn_plugin_gui_init() was found, ABI-checked and called, and
  3. the vfunc slots it wrote landed on the PnTmuxMonitor class.

If the companion failed to load, the dialog would carry only the
auto-generated property tabs (no status row) and the probe fails.

Run directly:

    PIPNODE_PLUGIN_DIR=plugins/shell/.libs \\
    python3 tests/test_node_dialog_shell_companion.py
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
PLUGIN_DIR = os.environ.get("PIPNODE_PLUGIN_DIR",
                            os.path.join(ROOT, "plugins", "shell", ".libs"))

BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"

# The companion's build_class_tab tags its read-only status row with this
# property handle; the auto dialog never builds an editor for a
# non-writable property, so finding it proves the companion ran.
COMPANION_PROP = "last-error"


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

    if not os.path.isdir(PLUGIN_DIR):
        fail(f"plugin directory does not exist: {PLUGIN_DIR}\n"
             f"(build the bundled shell plugin first)")

    # Both halves must be present — that is the whole point of the split.
    logic = [n for n in os.listdir(PLUGIN_DIR)
             if n.startswith("pipnode_shell") and "-gui" not in n
             and (n.endswith(".so") or n.endswith(".dylib"))]
    companion = [n for n in os.listdir(PLUGIN_DIR)
                 if n.startswith("pipnode_shell-gui")
                 and (n.endswith(".so") or n.endswith(".dylib"))]
    if not logic:
        fail(f"no logic pipnode_shell.so under {PLUGIN_DIR}; "
             f"contents: {os.listdir(PLUGIN_DIR)}")
    if not companion:
        fail(f"no companion pipnode_shell-gui.so under {PLUGIN_DIR}; "
             f"contents: {os.listdir(PLUGIN_DIR)}")

    env = os.environ.copy()
    env["PIPNODE_PLUGIN_PATH"] = PLUGIN_DIR

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    proc = subprocess.Popen(
        [PIPNODE, f"--dbus-name={BUS_NAME}"],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        if not wait_for_bus_name(bus, BUS_NAME, timeout=15.0):
            fail("pipnode editor never registered its DBus name")

        # Resolving "PnTmuxMonitor" proves the logic .so loaded and
        # registered the GType.
        idx = call(bus, "AddNode",
                   GLib.Variant("(sdd)", ("PnTmuxMonitor", 120.0, 120.0)),
                   "(i)").unpack()[0]
        if idx < 0:
            fail("AddNode('PnTmuxMonitor') failed — the shell logic plugin "
                 "did not register its node type in the editor")

        if not call(bus, "OpenNodeDialog",
                    GLib.Variant("(u)", (idx,)), "(b)").unpack()[0]:
            fail("OpenNodeDialog returned False — the Tmux Monitor dialog "
                 "could not be built")

        # The positive companion-loaded signal: the status row tagged
        # pn-prop-last-error exists only when build_class_tab ran.
        try:
            call(bus, "GetDialogEditorSensitive",
                 GLib.Variant("(s)", (COMPANION_PROP,)), "(b)")
        except GLib.Error as exc:
            fail(f"the Tmux Monitor dialog has no {COMPANION_PROP!r} status "
                 f"row ({exc.message}); the companion's build_class_tab did "
                 f"not run, so pipnode_shell-gui.so was not loaded by the "
                 f"editor")

        call(bus, "CloseNodeDialog", None, "(b)")

        print("PASS: the editor loaded the bundled shell plugin's companion "
              "GUI module (pipnode_shell-gui.so) — the Tmux Monitor dialog "
              "carries the status row installed by pn_plugin_gui_init(), "
              "while the GTK-free logic half loads under pipnode-run.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    run_test()
