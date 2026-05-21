#!/usr/bin/env python3
"""Functional test: pipnode-run loads a third-party plugin .so and
instantiates a node from it.

Builds a worksheet that wires AutoInjector → Echo → Debug, where the
Echo node type comes from the in-tree sample plugin under
``tests/plugins/echo``.  The plugin is *not* installed; the test runs
``pipnode-run`` with ``PIPNODE_PLUGIN_PATH`` pointed at the libtool
output directory (``.libs``) so ``pn_node_factory_load_plugins_default``
discovers it at startup.

If the plugin loader, the entry-point lookup, or the worksheet's
JSON-driven instantiation breaks, the test fails: the message that
should make it through the Echo pass-through never reaches Debug, so
nothing lands on stdout.

Run directly:

    PIPNODE_PLUGIN_DIR=tests/plugins/echo/.libs \\
    python3 tests/test_plugin_load.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile


ROOT        = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE_RUN = os.environ.get("PIPNODE_RUN",
                             os.path.join(ROOT, "src", "pipnode-run"))
PLUGIN_DIR  = os.environ.get("PIPNODE_PLUGIN_DIR",
                             os.path.join(ROOT, "tests", "plugins",
                                          "echo", ".libs"))


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def build_flow_json() -> dict:
    """AutoInjector → Echo → Debug.

    AutoInjector ticks once per second emitting a known string;
    PnEcho (from the plugin) forwards it untouched; Debug prints the
    JSON envelope to stdout.  We only need a single tick to land for
    the test to pass."""
    return {
        "format":      "pipnode",
        "version":     1,
        "nodes": [
            {
                "type": "PnAutoInjector",
                "name": "Source",
                "position": {"x":  80.0, "y": 100.0},
                "properties": {
                    "period":  1,
                    "value":   42.0,
                    "success": True,
                    "output":  "via-plugin",
                },
            },
            {
                # The plugin registers the type under its #GType name
                # ("PnEcho") via pn_node_factory_register; the load
                # path resolves the worksheet's "type" string against
                # that registry, so the node is constructed exactly
                # the same way a built-in would be.
                "type": "PnEcho",
                "name": "Pass",
                "position": {"x": 280.0, "y": 100.0},
                "properties": {},
            },
            {
                "type": "PnDebug",
                "name": "Sink",
                "position": {"x": 480.0, "y": 100.0},
                "properties": {
                    "target": "stdout",
                    "format": "JSON",
                },
            },
        ],
        "connections": [
            {"source": "Source", "target": "Pass"},
            {"source": "Pass",   "target": "Sink"},
        ],
    }


def find_json_blocks(blob: str) -> list:
    blocks = []
    decoder = json.JSONDecoder()
    i = 0
    while i < len(blob):
        brace = blob.find("{", i)
        if brace < 0:
            break
        try:
            obj, end = decoder.raw_decode(blob[brace:])
        except ValueError:
            i = brace + 1
            continue
        if isinstance(obj, dict):
            blocks.append(obj)
        i = brace + end
    return blocks


def run_test() -> None:
    if not os.path.isfile(PIPNODE_RUN) or not os.access(PIPNODE_RUN, os.X_OK):
        fail(f"pipnode-run binary not found or not executable: "
             f"{PIPNODE_RUN}")

    if not os.path.isdir(PLUGIN_DIR):
        fail(f"plugin directory does not exist: {PLUGIN_DIR}\n"
             f"(expected the in-tree sample plugin to be built first)")

    # Confirm the .so we expect is actually there before we even spawn
    # the binary — gives a clearer error than an empty stdout would.
    sos = [n for n in os.listdir(PLUGIN_DIR)
           if n.startswith("pn_echo") and (n.endswith(".so")
                                            or n.endswith(".dylib"))]
    if not sos:
        fail(f"no pn_echo.so / .dylib found under {PLUGIN_DIR}; "
             f"contents: {os.listdir(PLUGIN_DIR)}")

    env = os.environ.copy()
    env["PIPNODE_PLUGIN_PATH"] = PLUGIN_DIR
    # Force line buffering so our timeout window catches the first
    # tick — same trick the other functional tests use.
    cmd_prefix = []
    if subprocess.run(["which", "stdbuf"],
                      capture_output=True).returncode == 0:
        cmd_prefix = ["stdbuf", "-oL", "-eL"]

    with tempfile.TemporaryDirectory(prefix="pipnode-plugin-test-") as tmp:
        flow_path = os.path.join(tmp, "flow.json")
        with open(flow_path, "w", encoding="utf-8") as f:
            json.dump(build_flow_json(), f, indent=2)

        # --timeout 3 gives at least two AutoInjector ticks (period=1
        # configured above), which is plenty for the Debug node to
        # print a JSON envelope to stdout.
        cmd = cmd_prefix + [PIPNODE_RUN, "--timeout", "3", flow_path]

        proc = subprocess.run(cmd, env=env, capture_output=True,
                              timeout=15)

    if proc.returncode != 0:
        fail(f"pipnode-run exited rc={proc.returncode}\n"
             f"stdout:\n{proc.stdout.decode(errors='replace')}\n"
             f"stderr:\n{proc.stderr.decode(errors='replace')}")

    blob   = proc.stdout.decode("utf-8", errors="replace")
    blocks = find_json_blocks(blob)

    # Restrict to messages that came through the AutoInjector and
    # passed the Echo node — anything else means the plugin's
    # pass-through is silently dropping data.
    passed = [b for b in blocks if b.get("from") == "Source"]

    if not passed:
        fail("no JSON envelope reached the Debug sink — the Echo "
             "plugin either failed to load or failed to forward the "
             "message.\n"
             f"stdout was:\n{blob}\n"
             f"stderr was:\n"
             f"{proc.stderr.decode(errors='replace')}")

    first = passed[0]
    data  = first.get("data", {})
    if data.get("output") != "via-plugin":
        fail(f"data.output was {data.get('output')!r}, "
             f"expected 'via-plugin' — message corrupted in transit")

    print("PASS: pipnode-run loaded PnEcho from a plugin .so and "
          "the AutoInjector → Echo → Debug chain delivered the "
          "configured payload through the plugin-defined node.")


if __name__ == "__main__":
    run_test()
