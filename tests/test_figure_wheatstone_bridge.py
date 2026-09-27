#!/usr/bin/env python3
"""Functional test: a complex Figure sheet, played live (TODO #93).

The sheet is the self-balancing Wheatstone bridge -- the richest of the
Figure examples: a five-input PnFigure (T, R1, Rx, R3, Tm) whose program
runs to some 150 statements, fed by a deterministic chain:

  Knob T  -> Pt100 sensor  Rx = 100 (1 + 0.00385 T)
          -> Servo         R3 = clamp(R1 Rx / 100, 0, 400)
          -> Reading       Tm = (R3 100 / R1 / 100 - 1) / 0.00385
  Knob R1 -> Servo, Reading and the Figure.

The test opens a temporary copy of tests/data/figure-wheatstone-bridge.json
(itself a copy of examples/displays/wheatstone-bridge.json, so editing the
example never breaks the test) in a private editor, never saves, and plays
it the way the user's hand does: SetControlValue turns the two knobs
(#92), GetLastOutputMessage reads back what the three calculators emitted,
and the Figure's `error` property must stay empty at every operating
point.  A broken `program` set over D-Bus must name its line and column
and raise has-error; restoring it must clear both.

LIMIT: D-Bus cannot see the Figure's display list, so the drawing itself
(the needle, the masked red needle at balance, the "off the end of R3"
branch) is not asserted here -- tests/unit/test-pn-figure.c owns that.
What this test proves is that the whole sheet runs: every input reaches
the Figure and its program evaluates without error at each point.

Run directly:

    python3 tests/test_figure_wheatstone_bridge.py

Set $PIPNODE to override the path to the binary; defaults to the in-tree
build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from pndbus import PipnodeEditor  # noqa: E402

SHEET = os.path.join(HERE, "data", "figure-wheatstone-bridge.json")

KNOB_T   = "2f640d41-3c43-4c8a-9527-285d0cd999d7"
KNOB_R1  = "c03b5bd7-1242-4d10-9229-a357b37494a7"
PT100    = "cc3e0642-8ba4-4f37-a9c1-7f8c8feadad5"
SERVO    = "86acb657-4dd5-4c5c-86fb-a60c66bf310a"
READING  = "fbb4eedf-e60d-4b7c-bdc6-97ae1c56c477"
FIGURE   = "7ace5cd2-c870-4149-8060-a86279a40910"

EPS = 1e-6


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def data_of(ed: PipnodeEditor, uuid: str, what: str) -> dict:
    out = ed.get_last_output_message(uuid)
    if out is None:
        fail(f"{what} has emitted nothing")
    return out["data"]


def expect(ed: PipnodeEditor, uuid: str, what: str, member: str,
           want: float) -> None:
    data = data_of(ed, uuid, what)
    if not data.get("success", False):
        fail(f"{what} failed: {data.get('output')!r}")
    got = data.get(member)
    if got is None or abs(got - want) > EPS:
        fail(f"{what}: {member} = {got!r}, want {want}")
    if abs(data["value"] - want) > EPS:
        fail(f"{what}: value = {data['value']!r}, want {want} "
             f"(its last assignment, {member})")


def expect_figure_clean(ed: PipnodeEditor, where: str) -> None:
    ed.pump(0.3)          # let the figure repaint on the new inputs
    err = ed.get_node_property(FIGURE, "error")
    if err:
        fail(f"{where}: the figure reports an error: {err!r}")
    if ed.get_node_property(FIGURE, "has-error") != "false":
        fail(f"{where}: the figure has has-error set with no error text")


def operating_point(ed: PipnodeEditor, t: float, r1: float, rx: float,
                    r3: float, tm: float, where: str) -> None:
    ed.set_control_value(KNOB_T, t)
    ed.set_control_value(KNOB_R1, r1)
    ed.pump(0.3)
    expect(ed, PT100,   f"{where}: Pt100 sensor", "Rx", rx)
    expect(ed, SERVO,   f"{where}: Servo",        "R3", r3)
    expect(ed, READING, f"{where}: Reading",      "Tm", tm)
    expect_figure_clean(ed, where)


def run_test() -> None:
    tmpdir = tempfile.mkdtemp(prefix="pn-figure-test-")
    path = os.path.join(tmpdir, "wheatstone-bridge.json")
    shutil.copyfile(SHEET, path)
    try:
        with PipnodeEditor.launch() as ed:
            ed.open(path)
            ed.subscribe()
            ed.pump(1.5)      # the knobs' startup announces (g_idle)

            # --- the state the sheet loads in: T 20, R1 100 --------------
            if ed.get_node(FIGURE)["class_name"] != "Figure":
                fail("the sheet did not load its Figure node")
            expect(ed, PT100,   "initial: Pt100 sensor", "Rx", 107.7)
            expect(ed, SERVO,   "initial: Servo",        "R3", 107.7)
            expect(ed, READING, "initial: Reading",      "Tm", 20.0)
            expect_figure_clean(ed, "initial")

            # --- operating points ----------------------------------------
            # Balanced at 0 degC: Rx = R2 = R3 = 100, the galvanometer at
            # zero and the red needle masked (80.10b).
            operating_point(ed, 0.0, 100.0, 100.0, 100.0, 0.0, "balanced")
            # The low end of both knobs: R3 = 50 * 80.75 / 100.
            operating_point(ed, -50.0, 50.0, 80.75, 40.375, -50.0,
                            "low end")
            # Off the end of the rheostat: R1 Rx / R2 = 546.5 > 400, so the
            # servo stops at 400 and the reading sticks at the highest
            # temperature this range can show, (400/200 - 1) / 0.00385.
            operating_point(ed, 450.0, 200.0, 273.25, 400.0,
                            (400.0 / 200.0 - 1.0) / 0.00385,
                            "off the end of R3")

            # --- negative path: a broken program -------------------------
            program = ed.get_node_property(FIGURE, "program")
            lines = program.split("\n")
            try:
                n = next(i for i, l in enumerate(lines)
                         if l.startswith("Ig "))
            except StopIteration:
                fail("the figure's program has no `Ig = ...` line")
            broken = lines[:]
            broken[n] = lines[n].replace("+", "$", 1)
            col = broken[n].index("$") + 1
            ed.set_node_properties(FIGURE, {"program": "\n".join(broken)})
            ed.pump(0.3)
            err = ed.get_node_property(FIGURE, "error")
            if f"line {n + 1}, column {col}" not in err:
                fail(f"broken program: error {err!r} does not name "
                     f"line {n + 1}, column {col}")
            if ed.get_node_property(FIGURE, "has-error") != "true":
                fail("broken program: has-error is not set")

            ed.set_node_properties(FIGURE, {"program": program})
            expect_figure_clean(ed, "restored program")
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)

    print("PASS: Figure Wheatstone bridge (TODO #93) — knobs drive the "
          "sensor/servo/reading chain exactly at four operating points, "
          "the figure evaluates cleanly, and a broken program is "
          "reported by line and column and cleared on restore")


if __name__ == "__main__":
    run_test()
