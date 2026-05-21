#!/usr/bin/env python3
"""Functional test: View → Debug View must reveal the debug pane.

Drives a freshly launched pipnode through the AT-SPI DBus interface
(no synthesised keyboard input).  The test:

  1. Spawns the pipnode binary with the AT-SPI bridge loaded.
  2. Locates the running application on the accessibility bus.
  3. Walks the widget tree to find the "Debug View" check menu
     item and the right-hand list/tree-shaped widget that should
     host the debug output.
  4. Asserts the menu item is unchecked and the pane is not
     SHOWING at startup.
  5. Activates the menu item via the AT-SPI Action interface
     (i.e. over DBus, not via the keyboard).
  6. Waits for the pane to enter the SHOWING state, and fails
     the test if it never does.

The pane is a #GtkListBox of structured rows (each row is an
expander wrapping a clickable `from`-button); historically it was a
#GtkTreeView, hence the slightly broader role-name predicate below
that accepts either flavour.

The test is intentionally a single self-contained script so it can
be run directly:

    python3 tests/test_debug_view_toggle.py

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at src/pipnode-editor.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time

import gi

gi.require_version("Atspi", "2.0")
from gi.repository import Atspi  # noqa: E402


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE = os.environ.get("PIPNODE", os.path.join(ROOT, "src", "pipnode-editor"))


# ----------------------------------------------------------------------
# AT-SPI helpers
# ----------------------------------------------------------------------

def walk(node):
    """Yield @node and every descendant, depth-first."""
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
    """Wait until the AT-SPI desktop exposes an application owned by @pid."""
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
                if app.get_process_id() == pid:
                    # The app might still be populating its tree; let
                    # it settle for a moment so the menu bar is there.
                    if app.get_child_count() > 0:
                        return app
            except Exception:
                continue
        time.sleep(0.1)
    return None


def state_contains(node, state) -> bool:
    try:
        return node.get_state_set().contains(state)
    except Exception:
        return False


def is_showing(node) -> bool:
    return state_contains(node, Atspi.StateType.SHOWING)


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


def find_first(root, predicate, timeout: float = 5.0):
    """Return the first node under @root matching @predicate, or None."""
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


def find_all(root, predicate):
    return [n for n in walk(root) if predicate(n)]


def is_debug_view_menu_item(n) -> bool:
    if safe_role_name(n) != "check menu item":
        return False
    name = safe_name(n)
    # Mnemonics show up as either "Debug View" or "_Debug View"
    # depending on the AT-SPI bridge stripping the underscore.
    return "Debug View" in name


def is_pane_widget(n) -> bool:
    """Predicate for the debug pane's main scrollable contents.

    Accepts both the historical tree-shaped roles (when the pane was
    a #GtkTreeView) and the list-shaped roles the current
    #GtkListBox-based pane reports — so the test stays robust as the
    inner widget evolves.  The palette on the left exposes a "tree"
    role too; the caller is expected to baseline-diff against the
    initial visible set so that pre-existing widget is excluded.
    """
    role = safe_role_name(n)
    return role in ("table", "tree table", "tree", "list", "list box")


def click_via_action(node) -> bool:
    """Activate @node through the AT-SPI Action interface.

    For GTK menu items "click" is action index 0 — we invoke it
    directly to avoid the deprecated name lookup. Returns True on
    success.
    """
    try:
        if node.get_n_actions() <= 0:
            return False
        return bool(node.do_action(0))
    except Exception:
        return False


# ----------------------------------------------------------------------
# Test
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    env = os.environ.copy()
    # Isolate the config dir so persisted UI geometry (debug-view open
    # state / width, window size) from a previous run or another test
    # cannot change the startup state this test asserts.
    env["XDG_CONFIG_HOME"] = tempfile.mkdtemp(prefix="pipnode-test-cfg-")
    # Make sure the AT-SPI bridge loads inside pipnode so we can
    # reach it over DBus.
    env.pop("NO_AT_BRIDGE", None)
    existing = env.get("GTK_MODULES", "")
    needed = ["gail", "atk-bridge"]
    parts = [m for m in existing.split(":") if m]
    for m in needed:
        if m not in parts:
            parts.append(m)
    env["GTK_MODULES"] = ":".join(parts)

    proc = subprocess.Popen([PIPNODE], env=env)
    try:
        app = find_app_by_pid(proc.pid, timeout=15.0)
        if app is None:
            fail("pipnode never appeared on the AT-SPI bus "
                 "(check DISPLAY and at-spi2-registryd)")

        # Locate the menu item.
        item = find_first(app, is_debug_view_menu_item, timeout=10.0)
        if item is None:
            fail("could not find the 'Debug View' check menu item")

        if state_contains(item, Atspi.StateType.CHECKED):
            fail("Debug View is unexpectedly checked at startup")

        # The palette on the left exposes a "tree" role too; record
        # the baseline of pane-shaped widgets currently SHOWING so we
        # can detect a *new* one becoming visible after the toggle.
        baseline_visible = {id(t): t for t in find_all(app, is_pane_widget)
                            if is_showing(t)}

        # Toggle through the AT-SPI/DBus action interface.
        if not click_via_action(item):
            fail("could not invoke the 'click' action on Debug View")

        # Wait for an additional pane widget to appear in SHOWING state.
        deadline = time.monotonic() + 5.0
        new_showing = []
        while time.monotonic() < deadline:
            current = [t for t in find_all(app, is_pane_widget)
                       if is_showing(t)]
            new_showing = [t for t in current if id(t) not in baseline_visible]
            if new_showing:
                break
            time.sleep(0.1)

        if not new_showing:
            fail("after toggling Debug View, no new pane widget is "
                 "SHOWING — the debug pane was not actually revealed "
                 f"(baseline {len(baseline_visible)} pane(s) still the "
                 "only ones visible)")

        # Even if SHOWING is set, the user only sees the pane when it
        # actually has on-screen extents.  GtkPaned can keep a hidden
        # child sized to zero and still propagate the visibility flag
        # — that case must fail this test.
        too_small = []
        extents = []
        for t in new_showing:
            try:
                ext = t.get_extents(Atspi.CoordType.SCREEN)
                extents.append((t, ext))
                # A real debug pane needs meaningful width — under
                # ~100px the column header is the only thing that
                # fits and the user effectively cannot see content.
                if ext.width < 100 or ext.height < 50:
                    too_small.append((t, ext))
            except Exception:
                pass
        if too_small:
            details = ", ".join(
                f"{safe_role_name(t)} {ext.width}x{ext.height}"
                for t, ext in too_small)
            fail("debug pane is reported as SHOWING but has no visible "
                 f"on-screen area: {details}")

        # Sanity: the menu item should now report checked.
        if not state_contains(item, Atspi.StateType.CHECKED):
            fail("Debug View menu item did not transition to CHECKED")

        sizes = ", ".join(f"{e.width}x{e.height}" for _, e in extents)
        print("PASS: debug pane is showing after toggling View → Debug View "
              f"({len(new_showing)} new pane widget(s) visible on top of "
              f"{len(baseline_visible)} baseline; sizes {sizes}).")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
