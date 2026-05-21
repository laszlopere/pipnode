#!/usr/bin/env python3
"""Functional test: AutoInjector → Filter → Debug, JSON to stdout.

Spawns a fresh pipnode binary, then replays a three-node chain
(``AutoInjector → Filter → Debug``) through the
``org.pipas.pipnode.Worksheet`` D-Bus interface, varying the
AutoInjector payload and the Filter rule across a handful of
scenarios.  For every scenario the test verifies that the messages
appearing on the child process's *stdout* are exactly the ones the
configured rule should let through — pass scenarios expect at least
one pretty-printed JSON object whose ``data`` round-trips the
AutoInjector settings, block scenarios expect none.

Six scenarios cover every operator family the rules editor exposes:

  * ``success == true``        passes a true-success injector
  * ``success == false``       blocks the same injector
  * ``value > 10``             passes value=42.0
  * ``value > 100``            blocks value=5.0
  * ``output contains "pulse"`` passes "heartbeat pulse"
  * ``[]`` (empty rule list)   passes everything (documented identity)

Run directly:

    python3 tests/test_auto_injector_filter_debug.py

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import uuid

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402


ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE = os.environ.get("PIPNODE", os.path.join(ROOT, "src", "pipnode-editor"))

# Randomised per test run so several pipnode instances can share a
# session bus; passed to the spawned binary via --dbus-name.  The
# object path follows GApplication's '.' → '/' convention, while the
# interface name stays at the value hard-coded in the introspection
# XML.
BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"


# ----------------------------------------------------------------------
# Scenario table
# ----------------------------------------------------------------------
#
# Each row drives one Clear + LoadFromFile cycle.  ``injector`` carries
# the value / success / output the AutoInjector should embed in every
# tick; ``rules`` is the JSON-array literal handed to PnFilter's
# ``rules`` property (a string at the GObject layer); ``sink`` is the
# Debug node's display name, which doubles as the JSON top-level
# ``"node"`` field we attribute output to.
#
# ``expect_pass=True`` means at least one JSON object should land on
# stdout for that sink within the scenario's window.  ``False`` means
# none should — the rule must drop everything.

SCENARIOS = [
    {
        "name":        "bool-pass",
        "sink":        "sink-bool-pass",
        "injector":    {"value": 42.0, "success": True,
                        "output": "heartbeat pulse"},
        "rules":       [{"path": "success", "op": "==", "literal": True}],
        "expect_pass": True,
    },
    {
        "name":        "bool-block",
        "sink":        "sink-bool-block",
        "injector":    {"value": 42.0, "success": True,
                        "output": "heartbeat pulse"},
        "rules":       [{"path": "success", "op": "==", "literal": False}],
        "expect_pass": False,
    },
    {
        "name":        "num-pass",
        "sink":        "sink-num-pass",
        "injector":    {"value": 42.0, "success": True,
                        "output": "heartbeat pulse"},
        "rules":       [{"path": "value", "op": ">", "literal": 10}],
        "expect_pass": True,
    },
    {
        "name":        "num-block",
        "sink":        "sink-num-block",
        "injector":    {"value": 5.0, "success": True,
                        "output": "heartbeat pulse"},
        "rules":       [{"path": "value", "op": ">", "literal": 100}],
        "expect_pass": False,
    },
    {
        "name":        "str-pass",
        "sink":        "sink-str-pass",
        "injector":    {"value": 42.0, "success": True,
                        "output": "heartbeat pulse"},
        "rules":       [{"path": "output", "op": "contains",
                         "literal": "pulse"}],
        "expect_pass": True,
    },
    {
        "name":        "empty-rules",
        "sink":        "sink-empty",
        "injector":    {"value": 42.0, "success": True,
                        "output": "heartbeat pulse"},
        "rules":       [],
        "expect_pass": True,
    },
]


# ----------------------------------------------------------------------
# Helpers (lifted from test_clock_debug_format.py)
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


class StreamReader(threading.Thread):
    """Drain a byte stream into a thread-safe buffer."""

    def __init__(self, stream, label: str):
        super().__init__(name=f"reader-{label}", daemon=True)
        self._stream = stream
        self._buf    = bytearray()
        self._lock   = threading.Lock()
        self.label   = label

    def run(self):
        try:
            for chunk in iter(lambda: self._stream.read1(4096), b""):
                with self._lock:
                    self._buf.extend(chunk)
        except Exception:
            pass

    def text(self) -> str:
        with self._lock:
            return self._buf.decode("utf-8", errors="replace")


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
    owner_v = bus.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner",
        GLib.Variant("(s)", (name,)),
        GLib.VariantType.new("(s)"),
        Gio.DBusCallFlags.NONE, 1000, None,
    )
    owner = owner_v.unpack()[0]

    pid_v = bus.call_sync(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixProcessID",
        GLib.Variant("(s)", (owner,)),
        GLib.VariantType.new("(u)"),
        Gio.DBusCallFlags.NONE, 1000, None,
    )
    return int(pid_v.unpack()[0])


def call_load_from_file(bus, path: str) -> None:
    bus.call_sync(
        BUS_NAME, OBJECT_PATH, WS_IFACE, "LoadFromFile",
        GLib.Variant("(s)", (path,)),
        None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def call_clear(bus) -> None:
    bus.call_sync(
        BUS_NAME, OBJECT_PATH, WS_IFACE, "Clear",
        None, None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def find_json_blocks(blob: str) -> list:
    """Pull every top-level pretty-printed JSON object out of @blob.

    PnDebug's JSON output places the source node identity in the
    ``"from"`` field; there is no ``"node"`` field on the envelope,
    so callers that need to attribute output filter on
    ``obj.get("from")`` themselves.  Each scenario in this test
    feeds its own buffer slice (between Clear calls), so the slice
    is already scoped to the current scenario."""
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


# ----------------------------------------------------------------------
# Network construction
# ----------------------------------------------------------------------

def build_network_json(scenario: dict) -> dict:
    """Build the per-scenario flow JSON: AutoInjector → Filter → Debug."""
    inj = scenario["injector"]

    nodes = [
        {
            "type": "PnAutoInjector",
            "name": "Source",
            "position": {"x":  80.0, "y": 100.0},
            "properties": {
                "period":  1,
                "value":   inj["value"],
                "success": inj["success"],
                "output":  inj["output"],
            },
        },
        {
            "type": "PnFilter",
            "name": "Gate",
            "position": {"x": 280.0, "y": 100.0},
            "properties": {
                # PnFilter persists its rule list as a JSON-encoded
                # *string*; json.dumps inside the outer json.dump call
                # produces exactly the form examples/network_test.json
                # round-trips through.
                "rules": json.dumps(scenario["rules"]),
            },
        },
        {
            "type": "PnDebug",
            "name": scenario["sink"],
            "position": {"x": 480.0, "y": 100.0},
            "properties": {
                "target": "stdout",
                "format": "JSON",
            },
        },
    ]

    connections = [
        {"source": "Source",         "target": "Gate"},
        {"source": "Gate",           "target": scenario["sink"]},
    ]

    return {
        "format":      "pipnode",
        "version":     1,
        "nodes":       nodes,
        "connections": connections,
    }


# ----------------------------------------------------------------------
# Per-scenario assertions
# ----------------------------------------------------------------------

def validate_scenario(blocks: list, scenario: dict) -> None:
    name = scenario["name"]

    # Restrict to messages from the AutoInjector we just configured;
    # nothing else should reach the Debug node, but filtering on
    # "from" makes the assertions self-checking against any stray
    # output that might land in this slice from elsewhere.
    blocks = [b for b in blocks if b.get("from") == "Source"]

    if scenario["expect_pass"]:
        if not blocks:
            fail(f"[{name}] expected at least one JSON message on stdout, "
                 f"got none — filter dropped a message it should have "
                 f"forwarded")

        # Every forwarded message must round-trip the AutoInjector's
        # configured payload onto data.value / data.success /
        # data.output.  Catch any future change that silently drops
        # one of the three from the emitted message.
        first = blocks[0]
        data  = first.get("data")
        if not isinstance(data, dict):
            fail(f"[{name}] JSON 'data' is not an object: {data!r}")

        inj = scenario["injector"]
        if data.get("value") != inj["value"]:
            fail(f"[{name}] data.value is {data.get('value')!r}, "
                 f"expected {inj['value']!r}")
        if data.get("success") is not inj["success"]:
            fail(f"[{name}] data.success is {data.get('success')!r}, "
                 f"expected {inj['success']!r}")
        if data.get("output") != inj["output"]:
            fail(f"[{name}] data.output is {data.get('output')!r}, "
                 f"expected {inj['output']!r}")

        # Topics now come from the per-node PnNode "topic" template,
        # which resolves to "/pnode/${nodeclass}/${nodename}" by
        # default — for an AutoInjector named "Source", that means
        # "/pnode/AutoInjector/Source".
        expected_topic = "/pnode/AutoInjector/Source"
        if first.get("topic") != expected_topic:
            fail(f"[{name}] topic is {first.get('topic')!r}, "
                 f"expected {expected_topic!r}")
    else:
        if blocks:
            fail(f"[{name}] expected zero JSON messages on stdout, "
                 f"got {len(blocks)} — filter forwarded a message it "
                 f"should have blocked")


# ----------------------------------------------------------------------
# Test driver
# ----------------------------------------------------------------------

def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    # BUS_NAME is randomised per invocation, so a collision is
    # effectively impossible — but if it does happen, bail out loud
    # rather than sharing a name.
    try:
        existing_pid = get_name_owner_pid(bus, BUS_NAME)
        fail(f"another process (pid {existing_pid}) already owns "
             f"{BUS_NAME!r} on the session bus; cannot run this test")
    except GLib.Error:
        pass  # expected: nobody owns the name yet

    with tempfile.TemporaryDirectory(prefix="pipnode-test-") as tmp:
        env = os.environ.copy()
        env["NO_AT_BRIDGE"] = "1"

        # Force line buffering so each tick lands in our reader's
        # buffer as it happens (g_print goes through glibc stdio,
        # which block-buffers stdout when it is not a tty).
        cmd = [PIPNODE, f"--dbus-name={BUS_NAME}"]
        if subprocess.run(["which", "stdbuf"],
                          capture_output=True).returncode == 0:
            cmd = ["stdbuf", "-oL", "-eL"] + cmd

        proc = subprocess.Popen(
            cmd, env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        out_reader = StreamReader(proc.stdout, "stdout")
        err_reader = StreamReader(proc.stderr, "stderr")
        out_reader.start()
        err_reader.start()

        try:
            if not wait_for_bus_name(bus, BUS_NAME, timeout=15.0):
                if proc.poll() is not None:
                    fail(f"pipnode exited before claiming {BUS_NAME!r} "
                         f"(rc={proc.returncode}); stderr was:\n"
                         f"{err_reader.text()}")
                fail(f"pipnode never registered {BUS_NAME!r} on the "
                     f"session bus within 15s")

            owner_pid = get_name_owner_pid(bus, BUS_NAME)
            if owner_pid != proc.pid:
                fail(f"{BUS_NAME!r} is owned by pid {owner_pid}, "
                     f"not our spawned pipnode (pid {proc.pid})")

            for scenario in SCENARIOS:
                # Wipe the previous chain.  No-op on the first
                # iteration (worksheet starts empty).
                try:
                    call_clear(bus)
                except GLib.Error as e:
                    fail(f"[{scenario['name']}] Clear failed: {e.message}")

                # Snapshot the buffer offset so this scenario only
                # sees its own output, not earlier scenarios'.
                offset = len(out_reader.text())

                net_path = os.path.join(tmp, f"{scenario['name']}.json")
                with open(net_path, "w", encoding="utf-8") as f:
                    json.dump(build_network_json(scenario), f, indent=2)

                try:
                    call_load_from_file(bus, net_path)
                except GLib.Error as e:
                    fail(f"[{scenario['name']}] LoadFromFile failed: "
                         f"{e.message}")

                # AutoInjector period is 1 s, so 2.5 s gives at least
                # two ticks per scenario — enough that an intermittent
                # drop of the very first tick cannot mask a pass case.
                time.sleep(2.5)

                # Stop ticking before parsing so a tick can't land
                # mid-decode of the JSON we are reading; then a short
                # grace for any g_print already in flight.
                try:
                    call_clear(bus)
                except GLib.Error:
                    pass
                time.sleep(0.3)

                blob   = out_reader.text()[offset:]
                blocks = find_json_blocks(blob)

                validate_scenario(blocks, scenario)

            print("PASS: AutoInjector → Filter → Debug routes the "
                  "configured payload through every rule shape.")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    run_test()
