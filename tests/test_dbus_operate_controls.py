#!/usr/bin/env python3
"""Functional test: operating controls over D-Bus (TODO #92).

The Worksheet interface can build a sheet; these three methods PLAY it,
through the same emitting paths the user's hand takes:

  SetControlValue(uuid, value)  Knob: clamp, repaint, emit -- on every
                                call, even at an unchanged value.
  ActivateNode(uuid)            Switch toggles, Inject fires.
  PressKey(uuid, code)          Keypad presses the key with that code;
                                an unknown code -> BadPropertyValue.

Each control is wired to a PnTopic so the test proves the message leaves
the control itself and travels its wire.  A property write must still
NOT emit (the silent contract loads and undo rely on), and a node that
cannot be operated that way answers NotSupported.

Run directly:

    python3 tests/test_dbus_operate_controls.py

Set $PIPNODE to override the path to the binary; defaults to the in-tree
build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pndbus import PipnodeEditor, PipnodeError  # noqa: E402


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def expect_error(short: str, fn, *args) -> None:
    try:
        fn(*args)
    except PipnodeError as e:
        if e.short != short:
            fail(f"{fn.__name__}{args!r}: expected {short}, got {e.short} "
                 f"({e.remote_message})")
        return
    fail(f"{fn.__name__}{args!r}: expected {short} but the call succeeded")


def data_of(ed: PipnodeEditor, uuid: str) -> dict:
    out = ed.get_last_output_message(uuid)
    if out is None:
        fail(f"node {uuid} has emitted nothing")
    return out["data"]


def emissions(ed: PipnodeEditor, uuid: str) -> int:
    ed.pump(0.3)
    return sum(1 for n, a in ed.events
               if n == "MessageEmitted" and a[0] == uuid)


def run_test() -> None:
    with PipnodeEditor.launch() as ed:
        ed.new_document()

        major, minor = ed.get_api_version()
        if (major, minor) < (1, 3):
            fail(f"API version {major}.{minor}, want >= 1.3")

        knob    = ed.add_node("PnKnob",   100, 100)
        k_topic = ed.add_node("PnTopic",  360, 100)
        switch  = ed.add_node("PnSwitch", 100, 220)
        s_topic = ed.add_node("PnTopic",  360, 220)
        inject  = ed.add_node("PnInject", 100, 340)
        i_topic = ed.add_node("PnTopic",  360, 340)
        keypad  = ed.add_node("PnKeypad", 100, 460)
        p_topic = ed.add_node("PnTopic",  360, 460)
        debug   = ed.add_node("PnDebug",  620, 100)

        ed.connect(knob,   k_topic)
        ed.connect(switch, s_topic)
        ed.connect(inject, i_topic)
        ed.connect(keypad, p_topic)

        ed.set_node_properties(knob,   {"max": "10"})
        ed.set_node_properties(inject, {"text": "hello", "value": "3"})

        ed.subscribe()
        ed.pump(1.0)          # let the startup announces fire first

        # --- Knob: SetControlValue ---------------------------------------
        ed.clear_events()
        ed.set_control_value(knob, 2.5)
        if data_of(ed, knob)["value"] != 2.5:
            fail(f"knob emitted {data_of(ed, knob)!r}, want value 2.5")
        if data_of(ed, k_topic)["value"] != 2.5:
            fail("the knob's message did not reach the wired Topic")
        if float(ed.get_node_property(knob, "value")) != 2.5:
            fail("knob's value property did not follow the turn")

        ed.set_control_value(knob, 99.0)            # clamps to max
        if data_of(ed, k_topic)["value"] != 10.0:
            fail(f"out-of-range value not clamped: {data_of(ed, k_topic)!r}")

        ed.set_control_value(knob, 10.0)            # unchanged: still emits
        if emissions(ed, knob) != 3:
            fail(f"want 3 knob emissions (one per call), "
                 f"got {emissions(ed, knob)}")

        expect_error("BadPropertyValue", ed.set_control_value,
                     knob, float("nan"))

        # --- the silent contract: a property write does NOT emit ---------
        ed.clear_events()
        ed.set_node_properties(knob, {"value": "4"})
        if emissions(ed, knob) != 0:
            fail("a property write on the knob emitted a message")
        if data_of(ed, k_topic)["value"] != 10.0:
            fail("a property write reached the knob's consumer")

        # --- Switch: ActivateNode toggles ---------------------------------
        before = data_of(ed, s_topic)["value"] if \
            ed.get_last_output_message(s_topic) else 0.0
        ed.activate_node(switch)
        first = data_of(ed, s_topic)["value"]
        if first == before or first not in (0.0, 1.0):
            fail(f"switch did not toggle: {before} -> {first}")
        ed.activate_node(switch)
        if data_of(ed, s_topic)["value"] != before:
            fail("second activation did not toggle the switch back")

        # --- Inject: ActivateNode fires -----------------------------------
        ed.activate_node(inject)
        got = data_of(ed, i_topic)
        if got.get("output") != "hello" or got.get("value") != 3.0:
            fail(f"inject fired the wrong message: {got!r}")

        # --- Keypad: PressKey ---------------------------------------------
        ed.press_key(keypad, "7")
        got = data_of(ed, p_topic)
        if got.get("key") != "7" or got.get("value") != 7.0:
            fail(f"keypad pressed the wrong key: {got!r}")
        expect_error("BadPropertyValue", ed.press_key, keypad, "no-such-key")

        # --- errors ---------------------------------------------------------
        for fn, args in ((ed.set_control_value, (debug, 1.0)),
                         (ed.set_control_value, (switch, 1.0)),
                         (ed.activate_node,     (debug,)),
                         (ed.activate_node,     (knob,)),
                         (ed.press_key,         (debug, "7"))):
            expect_error("NotSupported", fn, *args)

        bogus = "00000000-0000-0000-0000-000000000000"
        expect_error("NodeNotFound", ed.set_control_value, bogus, 1.0)
        expect_error("NodeNotFound", ed.activate_node, bogus)
        expect_error("NodeNotFound", ed.press_key, bogus, "7")

    print("PASS: operate controls over D-Bus (TODO #92) — knob, switch, "
          "inject and keypad emit through their wires; property writes "
          "stay silent")


if __name__ == "__main__":
    run_test()
