#!/usr/bin/env python3
"""Functional test: a complex node settings dialog builds and binds.

Drives a freshly launched pipnode editor through the
``org.pipas.pipnode.Worksheet`` D-Bus interface and exercises the
#PnDial settings dialog — the most structurally complex dialog in the
tree.  Dial overrides #PnNodeClass.build_class_tabs to split its 21
properties across four themed pages, so opening its dialog produces a
six-tab notebook:

    Class | Node | Data | Scale | Zones | Colours

The test asserts three things, each of which is a load-bearing
behaviour the headless core / GTK split (TODO #23) puts at risk by
moving the dialog-building vfuncs out of the core vtable into the GUI
tier:

  1. STRUCTURE — opening the dialog yields exactly the six expected
     tabs in order, proving build_class_tabs fired and the dialog
     walked the type chain correctly.

  2. NODE -> WIDGET binding — on open, each editor shows the node's
     current value (G_BINDING_SYNC_CREATE pulled it in): the Dial's
     default max-value of 120 must already be in its spinner.

  3. WIDGET -> NODE binding — typing into an editor writes straight
     through to the node property (bidirectional binding), for both a
     string entry (label) on the Data tab and a numeric spinner
     (min-value) on the Scale tab, i.e. across two different pages.

A negative check (a bogus property name is not addressable, and a
closed dialog reports no tabs) guards against the editor-locator
silently matching everything.

After the dialog checks, Phase B exercises live message flow: it adds a
#PnAutoRandom node over D-Bus, configures its range (≠ the default so a
match is unambiguous) and period, wires its output to the Dial's input,
and polls the Dial's read-only `value` property until a random reading
lands inside the configured range — proving AddNode, SetNodeProperty,
ConnectNodes, and end-to-end wire routing all work.

Run directly:

    python3 tests/test_node_dialog_dial.py

Watch it happen (slow & visible) — pauses between every step, walks
each tab, and types into the editors so you can follow along on screen:

    python3 tests/test_node_dialog_dial.py --slow
    python3 tests/test_node_dialog_dial.py --slow=2.5   # 2.5s per step
    PIPNODE_TEST_SLOW=1.5 python3 tests/test_node_dialog_dial.py

The default (no flag) runs as fast as possible with no pauses, which is
what `make check` uses.

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at ``src/pipnode-editor``.
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

# Randomised per run so several pipnode instances can share a session
# bus; passed to the spawned binary via --dbus-name.
BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"

# The six tabs a PnDial dialog must show, in order.  "Class" is the
# read-only identity tab; "Node" is the base PnNode property tab; the
# remaining four come from PnDial's build_class_tabs override.
EXPECTED_TABS = ["Class", "Node", "Data", "Scale", "Zones", "Colours"]

# Dial property defaults that the dialog must surface on open (see the
# g_param_spec_* defaults in lib/pn-dial.c).
DIAL_DEFAULT_MAX = 120.0

# Range we configure on the wired-in random source.  Deliberately
# different from PnAutoRandom's [0, 1] default so a Dial reading inside
# it proves both that SetNodeProperty took effect AND that the wire
# carried the value end to end.
RAND_MIN = 10.0
RAND_MAX = 90.0


# Seconds to pause between visible steps; 0 means "run flat out" (the
# mode `make check` uses).  Set by parse_slow() from --slow / the
# PIPNODE_TEST_SLOW env var.
SLOW = 0.0


def parse_slow(argv: list[str]) -> float:
    """Resolve the per-step pause from argv and the environment.

    --slow            -> 1.5s   (a comfortable watching pace)
    --slow=2.5        -> 2.5s
    PIPNODE_TEST_SLOW -> that many seconds (overridden by --slow)
    nothing           -> 0      (fast, no pauses)
    """
    for arg in argv[1:]:
        if arg == "--slow":
            return 1.5
        if arg.startswith("--slow="):
            try:
                return max(0.0, float(arg.split("=", 1)[1]))
            except ValueError:
                fail(f"bad --slow value: {arg!r}")
    env = os.environ.get("PIPNODE_TEST_SLOW")
    if env:
        try:
            return max(0.0, float(env))
        except ValueError:
            fail(f"bad PIPNODE_TEST_SLOW value: {env!r}")
    return 0.0


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def say(msg: str) -> None:
    """Narrate a step.  In slow mode the narration is what the watcher
    reads to follow along with what is happening on screen."""
    if SLOW > 0:
        print(f"  · {msg}", flush=True)


def beat(scale: float = 1.0) -> None:
    """Pause for SLOW*scale seconds so a watcher can take in the last
    step.  A no-op in fast mode."""
    if SLOW > 0:
        time.sleep(SLOW * scale)


def build_worksheet_json() -> dict:
    """A single PnDial node with default configuration."""
    return {
        "format":  "pipnode",
        "version": 1,
        "nodes": [
            {
                "type":       "PnDial",
                "name":       "Gauge",
                # On the right, just past the generator (which is 140px
                # wide and sits at x=80) — the sink the wire flows into.
                "position":   {"x": 260.0, "y": 120.0},
                "properties": {},
            },
        ],
        "connections": [],
    }


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
    """Invoke @method on the worksheet interface synchronously."""
    return bus.call_sync(
        BUS_NAME, OBJECT_PATH, WS_IFACE, method,
        params,
        GLib.VariantType.new(reply_type) if reply_type else None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def set_editor(bus, prop: str, text: str) -> bool:
    """Drive the editor for @prop.  In slow mode this types the value in
    one character at a time so the watcher sees the field fill up; in
    fast mode it sets the whole string in a single call.  Returns the
    final SetDialogEditorText result."""
    if SLOW > 0:
        ok = True
        for i in range(1, len(text) + 1):
            ok = call(bus, "SetDialogEditorText",
                      GLib.Variant("(ss)", (prop, text[:i])), "(b)").unpack()[0]
            time.sleep(min(0.12, SLOW / 4.0))
        return ok
    return call(bus, "SetDialogEditorText",
                GLib.Variant("(ss)", (prop, text)), "(b)").unpack()[0]


def show_tab(bus, titles: list[str], name: str) -> None:
    """Switch the dialog to the tab titled @name so the next edit is
    visible on screen.  No-op in fast mode (the edit still works; the
    page just isn't brought forward)."""
    if SLOW <= 0:
        return
    if name in titles:
        call(bus, "SelectDialogPage",
             GLib.Variant("(u)", (titles.index(name),)), "(b)")


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

        say("editor launched and on the session bus")
        beat()

        # Load the one-Dial worksheet.
        with tempfile.NamedTemporaryFile(
                mode="w", suffix=".json", delete=False) as fp:
            json.dump(build_worksheet_json(), fp)
            tmp_path = fp.name
        try:
            call(bus, "LoadFromFile",
                 GLib.Variant("(s)", (tmp_path,)), None)
        finally:
            os.unlink(tmp_path)

        n = call(bus, "GetNodeCount", None, "(u)").unpack()[0]
        if n != 1:
            fail(f"expected 1 node after LoadFromFile, got {n}")

        cls = call(bus, "GetNode",
                   GLib.Variant("(u)", (0,)), "(sssddbb)").unpack()[0]
        if cls != "Dial":
            fail(f"expected node 0 class 'Dial', got {cls!r}")

        say("loaded a worksheet with one Dial node")
        beat()

        # Opening a dialog for an out-of-range node must fail cleanly.
        if call(bus, "OpenNodeDialog",
                GLib.Variant("(u)", (99,)), "(b)").unpack()[0]:
            fail("OpenNodeDialog(99) returned True for a missing node")

        # --- Open the real dialog ---------------------------------
        say("opening the Dial settings dialog…")
        if not call(bus, "OpenNodeDialog",
                    GLib.Variant("(u)", (0,)), "(b)").unpack()[0]:
            fail("OpenNodeDialog(0) returned False — the Dial dialog "
                 "could not be built")
        beat()

        # 1. STRUCTURE: the six themed tabs, in order.
        titles = list(call(bus, "GetDialogPageTitles", None, "(as)").unpack()[0])
        if titles != EXPECTED_TABS:
            fail(f"dialog tabs are {titles!r}, expected {EXPECTED_TABS!r}; "
                 "build_class_tabs did not contribute the expected pages")

        # Walk every tab so the watcher sees the full structure.
        for i, title in enumerate(titles):
            show_tab(bus, titles, title)
            say(f"tab {i + 1}/{len(titles)}: {title}")
            beat()

        # 2. NODE -> WIDGET: the max-value spinner shows the node's
        #    default (120) pulled in by G_BINDING_SYNC_CREATE.  Read the
        #    node side too and confirm both ends agree.
        show_tab(bus, titles, "Scale")
        say("checking the Scale tab's max-value spinner shows the "
            "node default (120)")
        widget_max = call(bus, "GetDialogEditorText",
                          GLib.Variant("(s)", ("max-value",)), "(s)").unpack()[0]
        if widget_max == "":
            fail("no editor found for 'max-value' — the Scale tab's "
                 "spinner was not built or not addressable")
        node_max = call(bus, "GetNodeProperty",
                        GLib.Variant("(us)", (0, "max-value")), "(s)").unpack()[0]
        if abs(float(widget_max) - DIAL_DEFAULT_MAX) > 1e-9:
            fail(f"max-value editor shows {widget_max!r}, expected "
                 f"{DIAL_DEFAULT_MAX}; node->widget sync did not run")
        if abs(float(node_max) - float(widget_max)) > 1e-9:
            fail(f"max-value: widget {widget_max!r} != node {node_max!r}")
        beat()

        # 3a. WIDGET -> NODE, string entry on the Data tab.
        show_tab(bus, titles, "Data")
        say("typing 'Boiler' into the Data tab's label entry")
        if not set_editor(bus, "label", "Boiler"):
            fail("SetDialogEditorText('label', ...) returned False — the "
                 "Data tab's label entry was not found")
        node_label = call(bus, "GetNodeProperty",
                          GLib.Variant("(us)", (0, "label")), "(s)").unpack()[0]
        if node_label != "Boiler":
            fail(f"after typing into the label entry the node's label is "
                 f"{node_label!r}, expected 'Boiler'; widget->node binding "
                 "did not propagate")
        echo_label = call(bus, "GetDialogEditorText",
                          GLib.Variant("(s)", ("label",)), "(s)").unpack()[0]
        if echo_label != "Boiler":
            fail(f"label editor reads back {echo_label!r}, expected 'Boiler'")
        say("→ node's label property is now 'Boiler'")
        beat()

        # 3b. WIDGET -> NODE, numeric spinner on the Scale tab.
        show_tab(bus, titles, "Scale")
        say("setting the Scale tab's min-value spinner to 5")
        if not set_editor(bus, "min-value", "5"):
            fail("SetDialogEditorText('min-value', ...) returned False — the "
                 "Scale tab's spinner was not found")
        node_min = call(bus, "GetNodeProperty",
                        GLib.Variant("(us)", (0, "min-value")), "(s)").unpack()[0]
        if abs(float(node_min) - 5.0) > 1e-9:
            fail(f"after setting the min-value spinner the node's min-value "
                 f"is {node_min!r}, expected 5; widget->node binding did not "
                 "propagate")
        say("→ node's min-value property is now 5")
        beat()

        # Negative check: a property with no editor must be unaddressable
        # in BOTH directions — guards against the locator matching the
        # first widget it sees.
        if call(bus, "GetDialogEditorText",
                GLib.Variant("(s)", ("no-such-prop",)), "(s)").unpack()[0] != "":
            fail("GetDialogEditorText('no-such-prop') returned a value; the "
                 "editor locator is matching widgets it should not")
        if call(bus, "SetDialogEditorText",
                GLib.Variant("(ss)", ("no-such-prop", "x")), "(b)").unpack()[0]:
            fail("SetDialogEditorText('no-such-prop') returned True for a "
                 "property with no editor")

        # --- Close and confirm teardown ---------------------------
        say("closing the dialog")
        beat()
        if not call(bus, "CloseNodeDialog", None, "(b)").unpack()[0]:
            fail("CloseNodeDialog returned False while a dialog was open")
        leftover = list(call(bus, "GetDialogPageTitles",
                             None, "(as)").unpack()[0])
        if leftover:
            fail(f"after CloseNodeDialog the dialog still reports tabs: "
                 f"{leftover!r}")
        say("dialog checks done")
        beat()

        # ==============================================================
        # Phase B: wire a live random source into the Dial and watch the
        # random values flow through and land on the needle.
        # ==============================================================
        say("adding a random-number generator node on the left")
        # On the left, close to the Dial — the source feeding it, so the
        # short wire reads left-to-right and the layout stays compact.
        rand_idx = call(bus, "AddNode",
                        GLib.Variant("(sdd)", ("PnAutoRandom", 80.0, 120.0)),
                        "(i)").unpack()[0]
        if rand_idx < 0:
            fail("AddNode('PnAutoRandom') failed")

        node_count = call(bus, "GetNodeCount", None, "(u)").unpack()[0]
        if node_count != 2:
            fail(f"expected 2 nodes after AddNode, got {node_count}")
        rand_cls = call(bus, "GetNode",
                        GLib.Variant("(u)", (rand_idx,)),
                        "(sssddbb)").unpack()[0]
        if rand_cls != "AutoRandom":
            fail(f"added node class is {rand_cls!r}, expected 'AutoRandom'")
        beat()

        # Configure the source: a known range (≠ the [0,1] default) and a
        # 1-second period.  "min"/"max" are AutoRandom's own properties;
        # "period" is inherited from PnAutoTrigger — exercising
        # SetNodeProperty on an inherited property too.
        say(f"setting the generator's range to "
            f"[{RAND_MIN:g}, {RAND_MAX:g}] and period 1s")
        for prop, val in (("min", f"{RAND_MIN:g}"),
                          ("max", f"{RAND_MAX:g}"),
                          ("period", "1")):
            if not call(bus, "SetNodeProperty",
                        GLib.Variant("(uss)", (rand_idx, prop, val)),
                        "(b)").unpack()[0]:
                fail(f"SetNodeProperty({prop!r}) returned False")
        got_min = call(bus, "GetNodeProperty",
                       GLib.Variant("(us)", (rand_idx, "min")), "(s)").unpack()[0]
        got_max = call(bus, "GetNodeProperty",
                       GLib.Variant("(us)", (rand_idx, "max")), "(s)").unpack()[0]
        if abs(float(got_min) - RAND_MIN) > 1e-9 or \
           abs(float(got_max) - RAND_MAX) > 1e-9:
            fail(f"generator range did not stick: min={got_min!r}, "
                 f"max={got_max!r}")
        beat()

        # Wire generator output -> Dial input.
        say("wiring the generator's output into the Dial's input")
        if not call(bus, "ConnectNodes",
                    GLib.Variant("(uu)", (rand_idx, 0)), "(b)").unpack()[0]:
            fail("ConnectNodes(random -> dial) returned False")

        wire_count = call(bus, "GetWireCount", None, "(u)").unpack()[0]
        if wire_count != 1:
            fail(f"expected 1 wire after ConnectNodes, got {wire_count}")
        src_i, dst_i = call(bus, "GetWire",
                            GLib.Variant("(u)", (0,)), "(ii)").unpack()
        if (src_i, dst_i) != (rand_idx, 0):
            fail(f"wire connects {src_i}->{dst_i}, expected {rand_idx}->0")
        beat()

        # Watch the random values arrive at the Dial.  The first tick
        # fires within ~1s; poll the Dial's read-only 'value' until it
        # lands inside the configured range.  In slow mode we collect a
        # few distinct readings so the watcher sees the needle move.
        say("waiting for random values to reach the Dial…")
        want = 3 if SLOW > 0 else 1
        deadline = time.monotonic() + 8.0
        readings: list[float] = []
        while time.monotonic() < deadline:
            fv = float(call(bus, "GetNodeProperty",
                            GLib.Variant("(us)", (0, "value")), "(s)").unpack()[0])
            if RAND_MIN - 1e-9 <= fv <= RAND_MAX + 1e-9 and fv not in readings:
                readings.append(fv)
                say(f"  Dial value = {fv:.3f}")
                if len(readings) >= want:
                    break
            time.sleep(0.25 if SLOW > 0 else 0.05)

        if not readings:
            fail(f"the Dial never received a value inside "
                 f"[{RAND_MIN:g}, {RAND_MAX:g}] within 8s; the wire did not "
                 "carry messages from the generator to the Dial")
        beat(2.0)

        print("PASS: the PnDial dialog builds its six themed tabs and binds "
              "its editors, AND a newly-added AutoRandom node wired into the "
              f"Dial feeds live random values onto the needle (saw "
              f"{readings[0]:.3f} in [{RAND_MIN:g}, {RAND_MAX:g}]).")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    SLOW = parse_slow(sys.argv)
    if SLOW > 0:
        print(f"[slow mode: {SLOW:g}s per step — watch the editor window]",
              flush=True)
    run_test()
