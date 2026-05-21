#!/usr/bin/env python3
"""Functional test: multi-worksheet JSON files round-trip through
``pipnode-run`` with the expected runtime semantics.

The test exercises the headless runner instead of the GUI because the
runtime is sheet-agnostic by design: every node on every sheet runs in
the same shared message-dispatch loop, and ``pipnode-run`` carries no
GTK dependency so the test is fast and stable.

Three things are verified end-to-end:

  1. A multi-sheet file (``"sheets": [...]`` member alongside the
     existing flat ``"nodes"`` / ``"connections"`` lists) loads
     without error and runs every sheet's nodes — a Debug Print
     placed on a *non-active* sheet still emits to stdout exactly as
     one on the active sheet would.

  2. A wire whose endpoints sit on different sheets is silently
     dropped on load with a ``dropping cross-sheet wire`` warning on
     stderr.  No message forwarding happens across the dropped wire
     (the Debug Print on the other sheet stays quiet).

  3. A file written before this commit (no top-level ``"sheets"``
     member; every node tagged with the legacy "Worksheet" default)
     loads as one sheet, exactly the way it did before — the
     existing single-worksheet behaviour is unchanged.

Run directly::

    python3 tests/test_multi_worksheet.py

Set ``$PIPNODE_RUN`` to override the binary path; defaults to the
in-tree build at ``src/pipnode-run``.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile


ROOT        = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE_RUN = os.environ.get(
        "PIPNODE_RUN", os.path.join(ROOT, "src", "pipnode-run"))


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def run_with_timeout(path: str, seconds: int = 2):
    """Spawn pipnode-run with --timeout=N on @path and capture I/O."""
    proc = subprocess.run(
            [PIPNODE_RUN, "--timeout", str(seconds), path],
            capture_output=True, text=True, timeout=seconds + 5)
    return proc.returncode, proc.stdout, proc.stderr


def write_json(payload: dict) -> str:
    fd, path = tempfile.mkstemp(suffix=".json", prefix="pn-test-")
    with os.fdopen(fd, "w") as fh:
        json.dump(payload, fh, indent=2)
    return path


# ----------------------------------------------------------------------
# Fixtures
# ----------------------------------------------------------------------

def auto_inject(name: str, sheet: str, marker: str,
                x: float = 100.0, y: float = 100.0):
    """AutoInjector that emits ``marker`` as ``data.output`` on every
    tick.  The default 1-second period means a 2-second test run sees
    the message once or twice — fine for greppability."""
    return {
        "type":       "PnAutoInjector",
        "name":       name,
        "worksheet":  sheet,
        "position":   {"x": x, "y": y},
        "properties": {
            "output":  marker,
            "value":   1.0,
            "success": True,
        },
    }


def debug_print(name: str, sheet: str,
                x: float = 400.0, y: float = 100.0):
    """Debug Print sink rendering the upstream ``data.output`` to stdout."""
    return {
        "type":       "PnDebug",
        "name":       name,
        "worksheet":  sheet,
        "position":   {"x": x, "y": y},
        "properties": {
            "target": "stdout",
            "format": "text",
        },
    }


# ----------------------------------------------------------------------
# Test 1 — Multi-sheet file runs every sheet's nodes
# ----------------------------------------------------------------------

def test_multi_sheet_runs_every_sheet():
    nodes = [
        auto_inject("Auto1",  "Sheet1", marker="MARKER1", x=100, y=100),
        debug_print("Debug1", "Sheet1",                   x=400, y=100),
        auto_inject("Auto2",  "Sheet2", marker="MARKER2", x=100, y=300),
        debug_print("Debug2", "Sheet2",                   x=400, y=300),
    ]
    payload = {
        "format":       "pipnode",
        "version":      1,
        "sheets":       ["Sheet1", "Sheet2", "Sheet3"],
        "active_sheet": "Sheet1",
        "nodes":        nodes,
        "connections": [
            {"source": "Auto1", "target": "Debug1"},
            {"source": "Auto2", "target": "Debug2"},
        ],
    }
    path = write_json(payload)
    try:
        rc, out, err = run_with_timeout(path, seconds=2)
    finally:
        os.unlink(path)

    if rc != 0:
        fail(f"pipnode-run exit code {rc}: stderr={err!r}")
    if "MARKER1" not in out:
        fail("active-sheet Debug Print did not emit; "
             f"stdout={out!r} stderr={err!r}")
    if "MARKER2" not in out:
        fail("non-active-sheet Debug Print did not emit; "
             "the runtime should be sheet-agnostic. "
             f"stdout={out!r} stderr={err!r}")
    print("PASS: multi-sheet file runs every sheet's nodes")


# ----------------------------------------------------------------------
# Test 2 — Cross-sheet wires are dropped with a warning
# ----------------------------------------------------------------------

def test_cross_sheet_wire_dropped():
    nodes = [
        auto_inject("Auto1",  "Sheet1", marker="SHOULD-NOT-APPEAR",
                    x=100, y=100),
        debug_print("Debug2", "Sheet2",                     x=400, y=300),
    ]
    payload = {
        "format":      "pipnode",
        "version":     1,
        "sheets":      ["Sheet1", "Sheet2"],
        "nodes":       nodes,
        "connections": [
            {"source": "Auto1", "target": "Debug2"},
        ],
    }
    path = write_json(payload)
    try:
        rc, out, err = run_with_timeout(path, seconds=2)
    finally:
        os.unlink(path)

    if rc != 0:
        fail(f"pipnode-run exit code {rc}: stderr={err!r}")
    if "dropping cross-sheet wire" not in err:
        fail("expected 'dropping cross-sheet wire' warning on stderr; "
             f"got stderr={err!r}")
    if "SHOULD-NOT-APPEAR" in out:
        fail("cross-sheet wire was not actually dropped — message "
             f"propagated through to the other sheet. stdout={out!r}")
    print("PASS: cross-sheet wires are dropped with a warning")


# ----------------------------------------------------------------------
# Test 3 — Legacy file (no "sheets" member) still loads
# ----------------------------------------------------------------------

def test_legacy_file_loads_unchanged():
    nodes = [
        # Note: no per-node "worksheet" field — the loader fills in
        # the "Worksheet" default for older saves.
        {
            "type":     "PnAutoInjector",
            "name":     "Auto1",
            "position": {"x": 100.0, "y": 100.0},
            "properties": {
                "output":  "LEGACY-MARKER",
                "value":   1.0,
                "success": True,
            },
        },
        {
            "type":     "PnDebug",
            "name":     "Debug1",
            "position": {"x": 400.0, "y": 100.0},
            "properties": {
                "target": "stdout",
                "format": "text",
            },
        },
    ]
    payload = {
        "format":      "pipnode",
        "version":     1,
        "nodes":       nodes,
        "connections": [
            {"source": "Auto1", "target": "Debug1"},
        ],
    }
    path = write_json(payload)
    try:
        rc, out, err = run_with_timeout(path, seconds=2)
    finally:
        os.unlink(path)

    if rc != 0:
        fail(f"pipnode-run exit code {rc}: stderr={err!r}")
    if "LEGACY-MARKER" not in out:
        fail("legacy single-sheet file failed to run end-to-end; "
             f"stdout={out!r} stderr={err!r}")
    if "dropping cross-sheet wire" in err:
        fail("legacy file should not produce cross-sheet warnings; "
             f"stderr={err!r}")
    print("PASS: legacy single-sheet file loads unchanged")


def main():
    if not os.path.exists(PIPNODE_RUN):
        fail(f"pipnode-run not found at {PIPNODE_RUN}")

    test_multi_sheet_runs_every_sheet()
    test_cross_sheet_wire_dropped()
    test_legacy_file_loads_unchanged()

    print("OK: multi-worksheet end-to-end behaviour verified")


if __name__ == "__main__":
    main()
