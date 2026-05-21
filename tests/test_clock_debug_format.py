#!/usr/bin/env python3
"""Functional test: Clock → Debug, with several output settings.

Spawns a fresh pipnode binary, loads a small network through the
``org.pipas.pipnode.Worksheet`` D-Bus interface that the application
exposes for tests, and verifies that the #PnDebug node renders ticks
from the #PnRtc (Clock) node to *stdout* or *stderr* in the expected
shape for each ``(target, format)`` combination.

The network has one Clock fan-ing out to four Debug sinks:

  * stdout · text       → ``The time is HH:MM:SS.``
  * stderr · text       → same string, but on stderr
  * stdout · oneliner   → ``[<name>] from=Ticker topic=tick id=… data=…``
  * stderr · JSON       → multi-line pretty JSON with the expected keys

Run directly:

    python3 tests/test_clock_debug_format.py

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import json
import os
import re
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

# Each test invocation registers under its own well-known name on the
# session bus, so several pipnode instances (and concurrent tests) can
# coexist.  The well-known name is passed to the spawned binary via
# --dbus-name; the GApplication object path is derived from it the
# same way GApplication itself derives it.  The interface name does
# not depend on the application id and stays at the fixed value
# baked into the introspection XML.
BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"


CLOCK_NAME = "Ticker"
# Every node now stamps its envelope with the resolved form of the
# user-configurable PnNode `topic` template, which defaults to
# "/pnode/${nodeclass}/${nodename}".  PnRtc's class label is "Clock"
# and the instance is named CLOCK_NAME, so the topic the Debug node
# prints is the two values joined under /pnode/.
CLOCK_TOPIC = f"/pnode/Clock/{CLOCK_NAME}"
SINKS = [
    {"name": "stdout-text",     "target": "Standard Output", "format": "text"},
    {"name": "stderr-text",     "target": "Standard Error",  "format": "text"},
    {"name": "stdout-oneliner", "target": "Standard Output", "format": "oneliner"},
    {"name": "stderr-json",     "target": "Standard Error",  "format": "JSON"},
]


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def build_network_json() -> dict:
    """Return a flow with one Clock connected to every Debug sink."""
    nodes = [
        {
            "type":       "PnRtc",
            "name":       CLOCK_NAME,
            "position":   {"x": 100.0, "y": 100.0},
            "properties": {"period": 1},
        }
    ]

    for i, sink in enumerate(SINKS):
        nodes.append({
            "type":       "PnDebug",
            "name":       sink["name"],
            "position":   {"x": 400.0, "y": 100.0 + 80.0 * i},
            "properties": {
                "target": sink["target"],
                "format": sink["format"],
            },
        })

    connections = [
        {"source": CLOCK_NAME, "target": sink["name"]} for sink in SINKS
    ]

    return {
        "format":      "pipnode",
        "version":     1,
        "nodes":       nodes,
        "connections": connections,
    }


class StreamReader(threading.Thread):
    """Drain a byte stream into a thread-safe buffer."""

    def __init__(self, stream, label: str):
        super().__init__(name=f"reader-{label}", daemon=True)
        self._stream = stream
        self._buf    = bytearray()
        self._lock   = threading.Lock()
        self.label   = label

    def run(self):
        # ``read1`` returns whatever is immediately available rather
        # than waiting for the buffer to fill up to @size — necessary
        # so each line-buffered tick lands in @_buf as soon as it
        # leaves the child process.
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
    """Block until @name owns a connection on @bus, or @timeout."""
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
    """Return the unix PID currently owning @name on @bus."""
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


# ----------------------------------------------------------------------
# Validation
# ----------------------------------------------------------------------

TIME_RE = re.compile(r"^The time is \d{2}:\d{2}:\d{2}\.$")

ONELINER_RE = re.compile(
    r"^\[(?P<node>[^\]]+)\] "
    r"from=(?P<from>\S+) "
    r"topic=(?P<topic>\S+) "
    r"id=(?P<id>\S+) "
    r"created=(?P<created>\S+) "
    r"data=(?P<data>.+)$"
)


def find_text_lines(blob: str) -> list:
    return [ln for ln in blob.splitlines() if TIME_RE.match(ln)]


def find_oneliner_lines(blob: str, node_name: str) -> list:
    out = []
    for ln in blob.splitlines():
        m = ONELINER_RE.match(ln)
        if not m or m.group("node") != node_name:
            continue
        out.append(m)
    return out


def find_json_blocks(blob: str, from_name: str) -> list:
    """Pull every pretty-printed JSON object whose "from" matches.

    The JSON envelope no longer carries the receiving node's display
    name (e38e38b dropped the "node" member because the right-hand
    debug pane already labels itself with it); the producer's name in
    "from" is what every consumer agrees on now, so we filter on
    that.
    """
    blocks = []
    decoder = json.JSONDecoder()
    i = 0
    while i < len(blob):
        # Pretty JSON output from JsonGenerator always starts with "{".
        brace = blob.find("{", i)
        if brace < 0:
            break
        try:
            obj, end = decoder.raw_decode(blob[brace:])
        except ValueError:
            i = brace + 1
            continue
        if isinstance(obj, dict) and obj.get("from") == from_name:
            blocks.append(obj)
        i = brace + end
    return blocks


def validate_text(stream_blob: str, stream_name: str) -> None:
    lines = find_text_lines(stream_blob)
    if not lines:
        fail(f"{stream_name}: no 'The time is HH:MM:SS.' line found "
             f"(text-format Debug node should print one per tick)")


def validate_oneliner(stream_blob: str, sink_name: str,
                      stream_name: str) -> None:
    matches = find_oneliner_lines(stream_blob, sink_name)
    if not matches:
        fail(f"{stream_name}: no oneliner line tagged '[{sink_name}]' "
             f"was found")
    m = matches[0]
    if m.group("from") != CLOCK_NAME:
        fail(f"{stream_name}: oneliner 'from' is {m.group('from')!r}, "
             f"expected {CLOCK_NAME!r}")
    if m.group("topic") != CLOCK_TOPIC:
        fail(f"{stream_name}: oneliner 'topic' is {m.group('topic')!r}, "
             f"expected {CLOCK_TOPIC!r}")
    if not m.group("id"):
        fail(f"{stream_name}: oneliner has empty id field")
    # The compact JSON in the data= tail must include the Clock's
    # convenience fields.
    data_text = m.group("data")
    try:
        data = json.loads(data_text)
    except ValueError as e:
        fail(f"{stream_name}: oneliner data is not valid JSON ({e}): "
             f"{data_text!r}")
    for key in ("epoch", "year", "month", "dom",
                "hour", "minute", "second", "output", "success"):
        if key not in data:
            fail(f"{stream_name}: oneliner data is missing key {key!r}")


def validate_json(stream_blob: str, sink_name: str,
                  stream_name: str) -> None:
    blocks = find_json_blocks(stream_blob, CLOCK_NAME)
    if not blocks:
        fail(f"{stream_name}: no pretty-JSON object with "
             f"from={CLOCK_NAME!r} found (expected from sink "
             f"{sink_name!r})")
    obj = blocks[0]
    for key in ("from", "topic", "id", "data"):
        if key not in obj:
            fail(f"{stream_name}: JSON object is missing top-level "
                 f"key {key!r} (got {sorted(obj.keys())})")
    if obj["topic"] != CLOCK_TOPIC:
        fail(f"{stream_name}: JSON 'topic' is {obj['topic']!r}, "
             f"expected {CLOCK_TOPIC!r}")
    data = obj.get("data")
    if not isinstance(data, dict):
        fail(f"{stream_name}: JSON 'data' is not an object: {data!r}")
    if data.get("success") is not True:
        fail(f"{stream_name}: JSON data.success is not true "
             f"(got {data.get('success')!r})")


# ----------------------------------------------------------------------
# Test driver
# ----------------------------------------------------------------------

def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    # The bus name is randomised per test invocation (see the
    # BUS_NAME definition near the top of the file), so a collision
    # with an unrelated pipnode is effectively impossible.  Still
    # bail out loudly if it ever does happen — silently sharing a
    # name would make the test pass spuriously.
    try:
        existing_pid = get_name_owner_pid(bus, BUS_NAME)
        fail(f"another process (pid {existing_pid}) already owns "
             f"{BUS_NAME!r} on the session bus; cannot run this test")
    except GLib.Error:
        pass  # expected: nobody owns the name yet

    with tempfile.TemporaryDirectory(prefix="pipnode-test-") as tmp:
        net_path = os.path.join(tmp, "clock_debug.json")
        with open(net_path, "w", encoding="utf-8") as f:
            json.dump(build_network_json(), f, indent=2)

        env = os.environ.copy()
        # Keep the AT-SPI bridge out of the way; this test only
        # talks to pipnode over the GApplication D-Bus interface.
        env["NO_AT_BRIDGE"] = "1"

        # ``g_print`` / ``g_printerr`` go through glibc's stdio, which
        # block-buffers stdout when it is not a tty.  Force line
        # buffering via stdbuf(1) so we can observe each tick as it
        # happens instead of waiting for an ~8 KiB block to flush at
        # process exit.
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
                     f"not our spawned pipnode (pid {proc.pid}) — "
                     f"another instance must have raced us to the bus")

            try:
                call_load_from_file(bus, net_path)
            except GLib.Error as e:
                fail(f"LoadFromFile failed: {e.message}")

            # The Clock period is 1s.  Wait long enough to get at
            # least two ticks per sink under load.
            time.sleep(3.5)

            # Stop ticking before draining the buffers so we do not
            # race a tick into the middle of our parsing.
            try:
                call_clear(bus)
            except GLib.Error:
                pass
            time.sleep(0.3)

            stdout_blob = out_reader.text()
            stderr_blob = err_reader.text()

            # text — both targets must produce the same line.
            validate_text(stdout_blob, "stdout")
            validate_text(stderr_blob, "stderr")

            # oneliner — only on stdout.
            validate_oneliner(stdout_blob, "stdout-oneliner", "stdout")

            # JSON — only on stderr.
            validate_json(stderr_blob, "stderr-json", "stderr")

            # And a quick cross-check: stuff we did *not* configure
            # should not appear on the wrong stream.
            if find_oneliner_lines(stderr_blob, "stdout-oneliner"):
                fail("oneliner output for 'stdout-oneliner' leaked "
                     "onto stderr")
            if find_json_blocks(stdout_blob, CLOCK_NAME):
                fail("pretty-JSON tick output leaked onto stdout — "
                     "only the stderr-json sink should emit JSON")

            print("PASS: Clock → Debug renders the expected shape on "
                  "stdout and stderr for text, oneliner, and JSON "
                  "formats.")
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


if __name__ == "__main__":
    run_test()
