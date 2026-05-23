#!/usr/bin/env python3
"""Functional test: the editor loads a plugin's companion GUI module.

This is the editor-side half of the Phase 6 (TODO #23) verification —
the headless half is tests/test_plugin_load.py, which proves the same
plugin's GTK-free *logic* .so loads and runs under pipnode-run.

The sample plugin under tests/plugins/echo is built as a two-tier pair:

    pn_echo.so       logic half, links libpipnode-core only (no GTK),
                     exports pn_plugin_init, registers the PnEcho type;
    pn_echo-gui.so   companion GUI module, links the GTK tier, exports
                     pn_plugin_gui_init, and installs the Echo node's
                     settings-dialog vfuncs (a custom "device" combo via
                     build_property_editor and an extra "Stats" page via
                     build_extra_pages) onto the already-registered class.

The editor (src/pipnode-editor) loads the logic .so via the factory's
plugin scan and then, in pn_gui_load_plugin_companions(), finds the
sibling pn_echo-gui.so and calls its pn_plugin_gui_init().  pipnode-run
never does this, so the companion's GTK code only ever runs in the editor.

The test drives a freshly launched editor over the
``org.pipas.pipnode.Worksheet`` D-Bus interface (the same harness the
Dial dialog test uses), adds a PnEcho node, opens its settings dialog,
and asserts the dialog carries the **"Stats"** page.  That page exists
only if the companion's build_extra_pages override was installed — so a
"Stats" tab is the clean positive signal that:

  1. the companion .so was discovered next to the logic .so,
  2. pn_plugin_gui_init() was found, ABI-checked and called, and
  3. the vfunc slots it wrote landed on the right class.

If the companion failed to load, the Echo dialog would show only the
auto-generated "Class" + "Node" tabs and the assertion fails.

Run directly:

    PIPNODE_PLUGIN_DIR=tests/plugins/echo/.libs \\
    python3 tests/test_plugin_gui_companion.py
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
                            os.path.join(ROOT, "tests", "plugins",
                                         "echo", ".libs"))

BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"

# The companion installs build_extra_pages, which appends this page; its
# presence in the Echo dialog is what proves pn_plugin_gui_init() ran.
COMPANION_TAB = "Stats"


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
             f"(expected the in-tree sample plugin to be built first)")

    # Both halves must be present — that is the whole point of the split.
    logic = [n for n in os.listdir(PLUGIN_DIR)
             if n.startswith("pn_echo") and "-gui" not in n
             and (n.endswith(".so") or n.endswith(".dylib"))]
    companion = [n for n in os.listdir(PLUGIN_DIR)
                 if n.startswith("pn_echo-gui")
                 and (n.endswith(".so") or n.endswith(".dylib"))]
    if not logic:
        fail(f"no logic pn_echo.so found under {PLUGIN_DIR}; "
             f"contents: {os.listdir(PLUGIN_DIR)}")
    if not companion:
        fail(f"no companion pn_echo-gui.so found under {PLUGIN_DIR}; "
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

        # Add a node of the plugin-provided type.  Resolving "PnEcho"
        # proves the logic .so loaded and registered the GType.
        idx = call(bus, "AddNode",
                   GLib.Variant("(sdd)", ("PnEcho", 120.0, 120.0)),
                   "(i)").unpack()[0]
        if idx < 0:
            fail("AddNode('PnEcho') failed — the logic plugin did not "
                 "register its node type in the editor")

        cls = call(bus, "GetNode",
                   GLib.Variant("(u)", (idx,)), "(sssddbb)").unpack()[0]
        if cls != "Echo":
            fail(f"expected node {idx} class 'Echo', got {cls!r}")

        # Open the Echo node's settings dialog and read its tab titles.
        if not call(bus, "OpenNodeDialog",
                    GLib.Variant("(u)", (idx,)), "(b)").unpack()[0]:
            fail("OpenNodeDialog returned False — the Echo dialog could "
                 "not be built")

        titles = list(call(bus, "GetDialogPageTitles",
                           None, "(as)").unpack()[0])

        if COMPANION_TAB not in titles:
            fail(f"the Echo dialog tabs are {titles!r}; the companion-"
                 f"installed {COMPANION_TAB!r} page is missing — "
                 f"pn_plugin_gui_init() did not run, so the companion "
                 f"GUI module was not loaded by the editor")

        call(bus, "CloseNodeDialog", None, "(b)")

        print("PASS: the editor loaded the Echo plugin's companion GUI "
              f"module (pn_echo-gui.so) — its dialog carries the "
              f"{COMPANION_TAB!r} page installed by pn_plugin_gui_init(), "
              "while the GTK-free logic half loads under pipnode-run "
              "(test_plugin_load.py).")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


if __name__ == "__main__":
    run_test()
