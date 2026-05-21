#!/usr/bin/env python3
"""Functional test: Edit → Cut / Copy / Paste round-trips through GTK
clipboard.

The test drives a freshly launched pipnode entirely through the
*real* user-facing channels:

  * activates **Edit → Paste / Copy / Cut** menu items via the
    AT-SPI Action interface (no synthesised key events);
  * uses a #GtkClipboard owned by *this* Python process to seed
    the system clipboard, and to read pipnode's clipboard back
    after Copy/Cut.

It asserts the full clipboard contract:

  1. Paste an external pipnode-clipboard JSON payload — the
     worksheet ends up with the expected nodes & wire.
  2. The pasted nodes auto-select; activating Copy hands pipnode
     the clipboard ownership and the text it now serves parses
     as a "pipnode-clipboard" payload with the same shape.
  3. Activating Cut both re-publishes the payload to the
     clipboard *and* removes the selection from the worksheet.
  4. A second Paste (this time from pipnode's own clipboard)
     restores the nodes.

Run directly:

    python3 tests/test_clipboard_ops.py

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import uuid

import gi

gi.require_version("Gtk",   "3.0")
gi.require_version("Gdk",   "3.0")
gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, Gdk, Gio, GLib, Gtk  # noqa: E402


ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE = os.environ.get("PIPNODE", os.path.join(ROOT, "src", "pipnode-editor"))

# Randomised per test run so several pipnode instances can share a
# session bus.  The well-known name is passed to the spawned binary
# via --dbus-name and the matching object path is derived the same
# way GApplication does (each '.' becomes '/', leading '/' prepended).
# The interface name is fixed in the introspection XML and does not
# depend on the application id.
BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"


# Two-node payload used to seed the clipboard.  The names and
# positions are deliberately distinctive so we can tell, after a
# round-trip, that the right blob made it into the clipboard.
SEED_PAYLOAD = {
    "format":  "pipnode-clipboard",
    "version": 1,
    "nodes": [
        {
            "type":       "PnRtc",
            "name":       "Tick-A",
            "position":   {"x": 80.0,  "y": 60.0},
            "properties": {"period": 1},
        },
        {
            "type":       "PnDebug",
            "name":       "Sink-B",
            "position":   {"x": 320.0, "y": 60.0},
            "properties": {"target": "Standard Error", "format": "text"},
        },
    ],
    "connections": [
        {"source": "Tick-A", "target": "Sink-B"},
    ],
}


# ----------------------------------------------------------------------
# Generic helpers
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def pump(seconds: float) -> None:
    """Pump GLib/Gtk events for @seconds.

    The test process owns the clipboard while seeding the payload —
    pipnode's async ``gtk_clipboard_request_text`` cannot complete
    unless we serve the X11 ``SelectionRequest`` event during that
    window.
    """
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        ctx = GLib.MainContext.default()
        while ctx.iteration(False):
            pass
        time.sleep(0.01)


# ----------------------------------------------------------------------
# AT-SPI helpers (mirrors test_debug_view_toggle.py)
# ----------------------------------------------------------------------

def walk(node):
    yield node
    try:
        n = node.get_child_count()
    except Exception:
        return
    for i in range(n):
        try:
            child = node.get_child_at_index(i)
        except Exception:
            continue
        if child is not None:
            yield from walk(child)


def find_app_by_pid(pid: int, timeout: float = 15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        desktop = Atspi.get_desktop(0)
        for i in range(desktop.get_child_count()):
            try:
                app = desktop.get_child_at_index(i)
            except Exception:
                continue
            if app is None:
                continue
            try:
                if (app.get_process_id() == pid and
                        app.get_child_count() > 0):
                    return app
            except Exception:
                continue
        time.sleep(0.1)
    return None


def safe_role_name(node) -> str:
    try:
        return node.get_role_name() or ""
    except Exception:
        return ""


def safe_name(node) -> str:
    try:
        return node.get_name() or ""
    except Exception:
        return ""


def find_menu_item(root, label: str, timeout: float = 5.0):
    """Find an Edit-menu item whose visible name equals @label
    (mnemonics stripped)."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for n in walk(root):
            if safe_role_name(n) != "menu item":
                continue
            name = safe_name(n).replace("_", "")
            if name == label:
                return n
        time.sleep(0.1)
    return None


def click_via_action(node) -> bool:
    try:
        if node.get_n_actions() <= 0:
            return False
        return bool(node.do_action(0))
    except Exception:
        return False


# ----------------------------------------------------------------------
# DBus convenience wrappers (worksheet state)
# ----------------------------------------------------------------------

def call_uint(bus, method: str) -> int:
    v = bus.call_sync(
        BUS_NAME, OBJECT_PATH, WS_IFACE, method,
        None, GLib.VariantType.new("(u)"),
        Gio.DBusCallFlags.NONE, 5000, None,
    )
    return int(v.unpack()[0])


def get_node_count(bus) -> int:
    return call_uint(bus, "GetNodeCount")


def get_wire_count(bus) -> int:
    return call_uint(bus, "GetWireCount")


def get_node_names(bus) -> list:
    v = bus.call_sync(
        BUS_NAME, OBJECT_PATH, WS_IFACE, "GetNodes",
        None, GLib.VariantType.new("(a(sssddbb))"),
        Gio.DBusCallFlags.NONE, 5000, None,
    )
    return [entry[1] for entry in v.unpack()[0]]


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


def get_name_owner_pid(bus, name: str) -> int:
    owner = bus.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        GLib.Variant("(s)", (name,)),
        GLib.VariantType.new("(s)"),
        Gio.DBusCallFlags.NONE, 1000, None,
    ).unpack()[0]
    pid = bus.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixProcessID",
        GLib.Variant("(s)", (owner,)),
        GLib.VariantType.new("(u)"),
        Gio.DBusCallFlags.NONE, 1000, None,
    ).unpack()[0]
    return int(pid)


# ----------------------------------------------------------------------
# Wait for a worksheet state change
# ----------------------------------------------------------------------

def wait_until(predicate, timeout: float, pump_seconds: float = 0.1):
    """Pump events and poll @predicate() until it returns truthy or
    @timeout elapses.  Returns the truthy value, or %None on timeout."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        v = predicate()
        if v:
            return v
        pump(pump_seconds)
    return None


# ----------------------------------------------------------------------
# Test
# ----------------------------------------------------------------------

def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    # The bus name is randomised per test invocation, so a collision
    # with an unrelated pipnode is effectively impossible — but if
    # one ever happens we want a loud failure rather than a quiet
    # name-share.
    try:
        existing_pid = get_name_owner_pid(bus, BUS_NAME)
        fail(f"another process (pid {existing_pid}) already owns "
             f"{BUS_NAME!r} on the session bus; cannot run this test")
    except GLib.Error:
        pass

    # The Atspi+Gtk imports above already opened a display
    # connection; explicitly initialise Gtk so its main context is
    # ready for the X clipboard exchanges that follow.
    Gtk.init([])
    display   = Gdk.Display.get_default()
    if display is None:
        fail("no Gdk display available")
    clipboard = Gtk.Clipboard.get_for_display (display,
                                               Gdk.SELECTION_CLIPBOARD)

    env = os.environ.copy()
    # Make sure the AT-SPI bridge loads inside pipnode.
    env.pop("NO_AT_BRIDGE", None)
    parts = [m for m in env.get("GTK_MODULES", "").split(":") if m]
    for m in ("gail", "atk-bridge"):
        if m not in parts:
            parts.append(m)
    env["GTK_MODULES"] = ":".join(parts)

    proc = subprocess.Popen([PIPNODE, f"--dbus-name={BUS_NAME}"], env=env)
    try:
        if not wait_for_bus_name(bus, BUS_NAME, timeout=15.0):
            if proc.poll() is not None:
                fail(f"pipnode exited before claiming {BUS_NAME!r} "
                     f"(rc={proc.returncode})")
            fail(f"pipnode never registered {BUS_NAME!r} on the "
                 f"session bus within 15s")

        owner_pid = get_name_owner_pid(bus, BUS_NAME)
        if owner_pid != proc.pid:
            fail(f"{BUS_NAME!r} is owned by pid {owner_pid}, not "
                 f"our spawned pipnode (pid {proc.pid})")

        app = find_app_by_pid(proc.pid, timeout=15.0)
        if app is None:
            fail("pipnode never appeared on the AT-SPI bus")

        cut_item   = find_menu_item(app, "Cut")
        copy_item  = find_menu_item(app, "Copy")
        paste_item = find_menu_item(app, "Paste")
        if cut_item is None or copy_item is None or paste_item is None:
            fail("could not find one of the Edit menu items "
                 f"(cut={cut_item}, copy={copy_item}, paste={paste_item})")

        # Sanity: a freshly opened pipnode has nothing selected, so
        # Cut/Copy must start out insensitive.  (Paste stays
        # always-on per the menu wiring.)
        try:
            ss = cut_item.get_state_set()
            if ss.contains(Atspi.StateType.SENSITIVE):
                fail("Cut menu item is sensitive at startup, but "
                     "the empty worksheet has nothing selected")
        except Exception:
            pass

        # ---- 1. Seed the X clipboard from this Python process. -----
        seed_text = json.dumps(SEED_PAYLOAD, indent=2)
        clipboard.set_text(seed_text, -1)
        # Briefly pump so the X SetSelectionOwner call lands before
        # pipnode's request goes out.
        pump(0.1)

        if get_node_count(bus) != 0:
            fail("worksheet was not empty at the start of the test")

        # ---- 2. Paste -- pipnode pulls from our clipboard. ---------
        if not click_via_action(paste_item):
            fail("could not invoke Paste via AT-SPI Action")

        # The Paste path is async (gtk_clipboard_request_text); pump
        # the main loop while waiting for the worksheet to grow.
        if not wait_until(lambda: get_node_count(bus) == 2, timeout=5.0):
            fail("after Paste, GetNodeCount never reached 2 "
                 f"(have {get_node_count(bus)})")

        if get_wire_count(bus) != 1:
            fail(f"after Paste, GetWireCount is {get_wire_count(bus)}, "
                 f"expected 1")

        # The pasted node names should derive from the seed names
        # (no collision with anything pre-existing → kept verbatim).
        names = set(get_node_names(bus))
        if names != {"Tick-A", "Sink-B"}:
            fail(f"after Paste, node names are {sorted(names)}, "
                 f"expected ['Sink-B', 'Tick-A']")

        # Cut/Copy must now be sensitive: paste auto-selects the new
        # nodes.
        if not wait_until(
                lambda: cut_item.get_state_set().contains(
                        Atspi.StateType.SENSITIVE),
                timeout=2.0):
            fail("after Paste, Cut menu item never became sensitive "
                 "— the pasted nodes were not auto-selected")

        # ---- 3. Copy -- pipnode owns the clipboard now. ------------
        if not click_via_action(copy_item):
            fail("could not invoke Copy via AT-SPI Action")
        pump(0.3)

        text = clipboard.wait_for_text()
        if text is None:
            fail("clipboard.wait_for_text() returned None after Copy")
        try:
            payload = json.loads(text)
        except ValueError as e:
            fail(f"clipboard contents after Copy are not valid JSON: "
                 f"{e}; text was: {text!r}")
        if payload.get("format") != "pipnode-clipboard":
            fail(f"clipboard 'format' after Copy is "
                 f"{payload.get('format')!r}, expected "
                 f"'pipnode-clipboard'")
        nodes_out = payload.get("nodes") or []
        wires_out = payload.get("connections") or []
        names_out = sorted(n.get("name", "") for n in nodes_out)
        if names_out != ["Sink-B", "Tick-A"]:
            fail(f"clipboard nodes after Copy are {names_out}, "
                 f"expected ['Sink-B', 'Tick-A']")
        if len(wires_out) != 1:
            fail(f"clipboard connections after Copy: {wires_out!r} "
                 f"(expected exactly one)")
        wire = wires_out[0]
        # Wires now reference endpoints by UUID; map the ids back to
        # node names via the same payload's `nodes` array.
        uuid_to_name = {n.get("uuid"): n.get("name") for n in nodes_out}
        wire_src = uuid_to_name.get(wire.get("source_id"))
        wire_dst = uuid_to_name.get(wire.get("target_id"))
        if (wire_src, wire_dst) != ("Tick-A", "Sink-B"):
            fail(f"clipboard wire after Copy is {wire!r} "
                 f"(resolved to {wire_src} → {wire_dst}), expected "
                 f"Tick-A → Sink-B")

        # ---- 4. Cut -- removes nodes; clipboard still serves them. -
        if not click_via_action(cut_item):
            fail("could not invoke Cut via AT-SPI Action")

        if not wait_until(lambda: get_node_count(bus) == 0, timeout=5.0):
            fail("after Cut, GetNodeCount never reached 0 "
                 f"(have {get_node_count(bus)})")
        if get_wire_count(bus) != 0:
            fail(f"after Cut, GetWireCount is {get_wire_count(bus)}, "
                 f"expected 0")

        text2 = clipboard.wait_for_text()
        if text2 is None:
            fail("clipboard.wait_for_text() returned None after Cut")
        try:
            payload2 = json.loads(text2)
        except ValueError as e:
            fail(f"clipboard contents after Cut are not valid JSON: "
                 f"{e}; text was: {text2!r}")
        names_cut = sorted(n.get("name", "") for n in payload2.get("nodes", []))
        if names_cut != ["Sink-B", "Tick-A"]:
            fail(f"clipboard nodes after Cut are {names_cut}, "
                 f"expected ['Sink-B', 'Tick-A']")

        # Cut/Copy must have gone insensitive again — selection is
        # empty after a cut.
        if not wait_until(
                lambda: not cut_item.get_state_set().contains(
                        Atspi.StateType.SENSITIVE),
                timeout=2.0):
            fail("after Cut, Cut menu item is still sensitive — "
                 "the worksheet selection was not cleared")

        # ---- 5. Paste again, this time from pipnode's own clipboard.
        if not click_via_action(paste_item):
            fail("could not invoke second Paste via AT-SPI Action")
        if not wait_until(lambda: get_node_count(bus) == 2, timeout=5.0):
            fail("after re-Paste, GetNodeCount never reached 2 "
                 f"(have {get_node_count(bus)})")
        if get_wire_count(bus) != 1:
            fail(f"after re-Paste, GetWireCount is "
                 f"{get_wire_count(bus)}, expected 1")

        print("PASS: clipboard ops — Paste seeds nodes, Copy publishes "
              "them, Cut removes the selection while keeping the "
              "clipboard payload, and a second Paste round-trips them "
              "back into the worksheet.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
