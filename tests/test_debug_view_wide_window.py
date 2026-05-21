#!/usr/bin/env python3
"""Functional test: View → Debug View must reveal the debug pane
even when the user has widened the main window before toggling.

Regression guard for the case "open pipnode at the default 1024×720
size, drag the window much wider, then activate Debug View — the
pane must still appear with a non-zero on-screen width".  The fix
sits in src/pn-window.c around init_debug_pane_position(): when
pack2 of the editor #GtkPaned transitions from hidden to visible,
the paned's first size-allocate clamps the divider to the right
edge (because pack1 had absorbed every pixel while pack2 was
hidden), and a g_idle_add deferred set_position() then re-claims a
quarter of the width for the pane.

This test drives the scenario through two channels rather than
through synthesised X events:

  * D-Bus, for the things that need to round-trip through GTK's own
    layout cycle so the AT-SPI cache cannot lie about them — the
    window resize (gtk_window_resize) and the debug-pane allocation
    read-back (gtk_widget_get_allocation on PnWindow's debug_pane).
    A prior xdotool-based attempt at this test silently passed
    because the AT-SPI tree continued to report the pre-resize
    extents while the underlying GTK allocation was correct.

  * AT-SPI, for activating the View → Debug View check menu item —
    the same path the existing test_debug_view_toggle.py uses, so
    we exercise the toggle handler exactly as a real click does.

The test is intentionally a single self-contained script so it can
be run directly:

    python3 tests/test_debug_view_wide_window.py

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at src/pipnode-editor.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import uuid

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi, Gio, GLib  # noqa: E402


ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE = os.environ.get("PIPNODE", os.path.join(ROOT, "src", "pipnode-editor"))

# Randomised per test run so several pipnode instances can share a
# session bus; passed to the spawned binary via --dbus-name.
DBUS_NAME      = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
DBUS_OBJ_PATH  = "/" + DBUS_NAME.replace(".", "/")
DBUS_INTERFACE = "org.pipas.pipnode.Worksheet"

# How wide we open the window before toggling Debug View.  Picked to
# clearly exceed the 1024 px default — the pre-fix bug shape was
# that pack2 ended up at zero width once pack1 had absorbed the
# whole 1500+ px difference.
WIDE_WINDOW_W = 2400
WIDE_WINDOW_H = 800

# The pane is considered "really visible" only above this on-screen
# extent.  Below ~100 px the user effectively sees nothing useful.
MIN_VISIBLE_W = 100
MIN_VISIBLE_H = 50

# How wide the pane must be to prove the idle-deferred set_position()
# in init_debug_pane_position actually ran.  The pane has a
# 280 px size_request (gtk_widget_set_size_request in
# create_debug_pane), which a #GtkPaned with no explicit position
# would honour as the natural width of pack2 — so a pane of exactly
# 280 px after widening to 2400 px is the tell-tale sign that the
# fix did NOT run.  With the fix, pack2 gets WIDE_WINDOW_W / 4
# (≈ 600 px), so anything well above 280 px confirms the idle fired.
EXPECTED_MIN_W_FROM_FIX = 400


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


# ----------------------------------------------------------------------
# AT-SPI helpers
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
                if app.get_process_id() == pid and app.get_child_count() > 0:
                    return app
            except Exception:
                continue
        time.sleep(0.1)
    return None


def find_first(root, predicate, timeout: float = 10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for node in walk(root):
            try:
                if predicate(node):
                    return node
            except Exception:
                continue
        time.sleep(0.1)
    return None


def is_debug_view_menu_item(n) -> bool:
    try:
        if n.get_role_name() != "check menu item":
            return False
    except Exception:
        return False
    try:
        return "Debug View" in (n.get_name() or "")
    except Exception:
        return False


def click_via_action(node) -> bool:
    try:
        if node.get_n_actions() <= 0:
            return False
        return bool(node.do_action(0))
    except Exception:
        return False


# ----------------------------------------------------------------------
# D-Bus helpers
# ----------------------------------------------------------------------

def dbus_proxy() -> Gio.DBusProxy:
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    return Gio.DBusProxy.new_sync(
        bus, Gio.DBusProxyFlags.NONE, None,
        DBUS_NAME, DBUS_OBJ_PATH, DBUS_INTERFACE, None)


def wait_for_bus_name(timeout: float = 10.0) -> bool:
    """Poll the session bus until our well-known name appears."""
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        v = bus.call_sync(
            "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "NameHasOwner",
            GLib.Variant("(s)", (DBUS_NAME,)),
            GLib.VariantType("(b)"),
            Gio.DBusCallFlags.NONE, 5000, None)
        if v.get_child_value(0).get_boolean():
            return True
        time.sleep(0.1)
    return False


def resize_window(proxy: Gio.DBusProxy, w: int, h: int) -> None:
    proxy.call_sync(
        "ResizeWindow", GLib.Variant("(uu)", (w, h)),
        Gio.DBusCallFlags.NONE, 5000, None)


def debug_pane_allocation(proxy: Gio.DBusProxy):
    v = proxy.call_sync(
        "GetDebugPaneAllocation", None,
        Gio.DBusCallFlags.NONE, 5000, None)
    return (v.get_child_value(0).get_int32(),
            v.get_child_value(1).get_int32())


# ----------------------------------------------------------------------
# Test
# ----------------------------------------------------------------------

def wait_visible(proxy, timeout: float = 5.0):
    deadline = time.monotonic() + timeout
    w, h = 0, 0
    while time.monotonic() < deadline:
        w, h = debug_pane_allocation(proxy)
        if w >= MIN_VISIBLE_W and h >= MIN_VISIBLE_H:
            return w, h
        time.sleep(0.1)
    return w, h


def wait_hidden(proxy, timeout: float = 5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        w, h = debug_pane_allocation(proxy)
        if w == 0 and h == 0:
            return
        time.sleep(0.1)


def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    env = os.environ.copy()
    # Isolate the config dir so persisted UI geometry (debug-view open
    # state / width, window size) from a previous run or another test
    # cannot perturb the cold-reveal sizing this test asserts.
    env["XDG_CONFIG_HOME"] = tempfile.mkdtemp(prefix="pipnode-test-cfg-")
    env.pop("NO_AT_BRIDGE", None)
    parts = [m for m in env.get("GTK_MODULES", "").split(":") if m]
    for m in ("gail", "atk-bridge"):
        if m not in parts:
            parts.append(m)
    env["GTK_MODULES"] = ":".join(parts)

    proc = subprocess.Popen([PIPNODE, f"--dbus-name={DBUS_NAME}"], env=env)
    try:
        app = find_app_by_pid(proc.pid, timeout=15.0)
        if app is None:
            fail("pipnode never appeared on the AT-SPI bus "
                 "(check DISPLAY and at-spi2-registryd)")

        if not wait_for_bus_name(timeout=10.0):
            fail(f"{DBUS_NAME} did not appear on the session bus")

        proxy = dbus_proxy()

        # Pre-toggle: pane is hidden, allocation reads as zero.
        pre_w, pre_h = debug_pane_allocation(proxy)
        if pre_w != 0 or pre_h != 0:
            fail(f"debug pane is unexpectedly already allocated "
                 f"({pre_w}x{pre_h}) before View → Debug View")

        # Widen the window through GTK so the size-allocate cycle
        # actually runs (xdotool windowsize / wmctrl don't always
        # propagate into GtkPaned).
        resize_window(proxy, WIDE_WINDOW_W, WIDE_WINDOW_H)

        # Let GTK process the ConfigureNotify and re-allocate the
        # paned.  The window manager round-trip plus the layout pass
        # need a couple of main-loop iterations; 600 ms is generous
        # but cheap.
        time.sleep(0.6)

        # Find and activate the menu item the same way the basic
        # debug-view-toggle test does.
        item = find_first(app, is_debug_view_menu_item, timeout=10.0)
        if item is None:
            fail("could not find the 'Debug View' check menu item")

        # ---- Scenario 1: cold reveal at wide window size ----
        if not click_via_action(item):
            fail("could not invoke the 'click' action on Debug View")

        w, h = wait_visible(proxy)
        if w < MIN_VISIBLE_W or h < MIN_VISIBLE_H:
            fail(f"[cold-reveal] after widening to "
                 f"{WIDE_WINDOW_W}x{WIDE_WINDOW_H} and toggling Debug "
                 f"View, the debug pane allocation is {w}x{h} — below "
                 f"the {MIN_VISIBLE_W}x{MIN_VISIBLE_H} visibility "
                 "threshold (pack1 absorbed every pixel before pack2 "
                 "became visible)")
        if w < EXPECTED_MIN_W_FROM_FIX:
            fail(f"[cold-reveal] debug pane is only {w} px wide on a "
                 f"{WIDE_WINDOW_W} px window — looks like the deferred "
                 "set_position() never ran (pane is sitting at its "
                 "280 px size_request floor instead of "
                 f"{WIDE_WINDOW_W // 4} px)")

        # ---- Scenario 2: the user-reported regression ----
        # Toggle off (now at default-ish size), widen further, toggle
        # back on — pre-fix this leaves the divider at the right edge
        # of the new wider paned and pack2 is allocated 280 px at
        # INT_MIN coordinates, invisible to the user.
        if not click_via_action(item):
            fail("could not deactivate Debug View for scenario 2")
        wait_hidden(proxy)

        # Pretend the user is dragging the window even wider after
        # closing the pane.  The pre-fix code would not re-run the
        # idle on the next reveal.
        resize_window(proxy, WIDE_WINDOW_W + 600, WIDE_WINDOW_H)
        time.sleep(0.6)

        if not click_via_action(item):
            fail("could not re-activate Debug View for scenario 2")

        w2, h2 = wait_visible(proxy)
        if w2 < MIN_VISIBLE_W or h2 < MIN_VISIBLE_H:
            fail(f"[reveal-after-resize] after toggle-off, resize, "
                 f"toggle-on the debug pane allocation is {w2}x{h2} — "
                 "below the visibility threshold.  This is the user-"
                 "reported regression where GtkPaned's divider drifts "
                 "to the right edge of the (now wider) paned while "
                 "pack2 is hidden, then sticks there on the next "
                 "toggle-on, leaving pack2 allocated at INT_MIN "
                 "coordinates")

        print(f"PASS: debug pane is showing in both the cold-reveal "
              f"({w}x{h}) and reveal-after-resize ({w2}x{h2}) "
              "scenarios on a wide window.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
