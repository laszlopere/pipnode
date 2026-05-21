#!/usr/bin/env python3
"""Functional test: HTTPS Tunnel Sender → (network) → HTTPS Tunnel Receiver, end-to-end.

Spawns *two* pipnode binaries on the session bus — one acting as the
sender, one as the receiver — and verifies that messages produced by
an AutoInjector on the sender land on a Debug sink on the receiver
intact, with their envelope (topic / data) preserved.

Sender flow:

    AutoInjector  →  HTTPS Tunnel Sender  →  https://127.0.0.1:PORT/

Receiver flow:

    HTTPS Tunnel Receiver (port=PORT)  →  Debug (stdout, JSON)

The HTTPS Tunnel Receiver's port is chosen at random from the kernel's ephemeral
range so the test does not collide with another pipnode instance the
developer happens to have open (the default 8443 in particular is what
the GUI uses out of the box).  Both binaries register under their own
randomised D-Bus names — the same scheme the other functional tests use
since TODO #14 — so several copies of this test can also share a
session bus.

The HTTPS Tunnel Receiver keeps its default self-signed certificate
(generated on the fly in `pn_https_tunnel_receiver_init`), and the sender's
HTTPS Tunnel Sender keeps its default `verify-tls=FALSE`, so the pairing works
with zero PKI setup — exactly the out-of-the-box experience documented
in `help/PnHttpsTunnelSender.html`.

Run directly:

    python3 tests/test_https_tunnel.py

Set $PIPNODE to override the path to the binary; defaults to the
in-tree build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import json
import os
import socket
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

# One D-Bus name per pipnode process.  The shared `tNNN_HHHHHHHH` tail
# matches the convention `test_auto_injector_filter_debug.py` uses so
# `dbus-monitor` traces are immediately attributable to this test run.
_RUN_TAG       = f"t{os.getpid()}_{uuid.uuid4().hex[:8]}"
SERVER_BUS     = f"org.pipas.pipnode.{_RUN_TAG}_srv"
CLIENT_BUS     = f"org.pipas.pipnode.{_RUN_TAG}_cli"
SERVER_OBJPATH = "/" + SERVER_BUS.replace(".", "/")
CLIENT_OBJPATH = "/" + CLIENT_BUS.replace(".", "/")
WS_IFACE       = "org.pipas.pipnode.Worksheet"

# Display names baked into the worksheets we load.  Kept as constants
# because both the network builder and the assertions reference them.
SERVER_NAME = "Receiver"
SINK_NAME   = "Sink"
SOURCE_NAME = "Source"
CLIENT_NAME = "Sender"

# The payload the AutoInjector emits.  Picked to be cheap to compare
# verbatim once the message has round-tripped through JSON.
INJECTOR_PAYLOAD = {
    "value":   42.0,
    "success": True,
    "output":  "hello-from-client",
}


# ----------------------------------------------------------------------
# Helpers (same shape as the other tests in this directory)
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def pick_free_port() -> int:
    """Ask the kernel for an unused TCP port on the loopback.

    Bind-then-close races against another process grabbing the same
    port before we hand it to pipnode, but the window is tiny and the
    alternative (hard-coding a port) collides with the GUI defaults.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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


def call_load_from_file(bus, bus_name: str, obj_path: str, path: str) -> None:
    bus.call_sync(
        bus_name, obj_path, WS_IFACE, "LoadFromFile",
        GLib.Variant("(s)", (path,)),
        None,
        Gio.DBusCallFlags.NONE, 10000, None,
    )


def find_json_blocks(blob: str) -> list:
    """Pull every top-level pretty-printed JSON object out of @blob.

    Same scanner as `test_auto_injector_filter_debug.py`: walk forward
    looking for `{`, let `JSONDecoder.raw_decode` consume one object,
    and step past it.  Tolerant of arbitrary text between objects so
    incidental log lines on the same stream do not break parsing.
    """
    blocks  = []
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

def build_server_network(port: int) -> dict:
    """HTTPS Tunnel Receiver → Debug (stdout, JSON)."""
    return {
        "format":  "pipnode",
        "version": 1,
        "nodes": [
            {
                "type":       "PnHttpsTunnelReceiver",
                "name":       SERVER_NAME,
                "position":   {"x": 100.0, "y": 100.0},
                "properties": {
                    "port":      port,
                    # Leave the auth + cert defaults baked into
                    # pn_https_tunnel_receiver_init: empty username/password
                    # disable Basic auth, and cert-path/key-path are
                    # omitted entirely so the XDG-default paths
                    # ($XDG_DATA_HOME/pipnode/default-{cert,key}.pem)
                    # take effect — meaning this test exercises the
                    # production "drop the node, it just works" path,
                    # including the lazy mint-and-persist branch on
                    # the first run.
                    "username":  "",
                    "password":  "",
                },
            },
            {
                "type":       "PnDebug",
                "name":       SINK_NAME,
                "position":   {"x": 400.0, "y": 100.0},
                "properties": {
                    "target": "stdout",
                    "format": "JSON",
                },
            },
        ],
        "connections": [
            {"source": SERVER_NAME, "target": SINK_NAME},
        ],
    }


def build_client_network(port: int) -> dict:
    """AutoInjector → HTTPS Tunnel Sender (targeting the receiver)."""
    return {
        "format":  "pipnode",
        "version": 1,
        "nodes": [
            {
                "type":       "PnAutoInjector",
                "name":       SOURCE_NAME,
                "position":   {"x": 100.0, "y": 100.0},
                "properties": {
                    "period":  1,
                    "value":   INJECTOR_PAYLOAD["value"],
                    "success": INJECTOR_PAYLOAD["success"],
                    "output":  INJECTOR_PAYLOAD["output"],
                },
            },
            {
                "type":       "PnHttpsTunnelSender",
                "name":       CLIENT_NAME,
                "position":   {"x": 400.0, "y": 100.0},
                "properties": {
                    "url":        f"https://127.0.0.1:{port}/",
                    "username":   "",
                    "password":   "",
                    # FALSE is the default in pn_https_tunnel_sender_init, but
                    # spell it out so the test does not silently break
                    # if the default ever changes.
                    "verify-tls": False,
                },
            },
        ],
        "connections": [
            {"source": SOURCE_NAME, "target": CLIENT_NAME},
        ],
    }


# ----------------------------------------------------------------------
# Spawning helpers
# ----------------------------------------------------------------------

def spawn_pipnode(bus_name: str, env: dict):
    """Start a pipnode binary under @bus_name and return (proc, out, err)."""
    cmd = [PIPNODE, f"--dbus-name={bus_name}"]
    if subprocess.run(["which", "stdbuf"],
                      capture_output=True).returncode == 0:
        cmd = ["stdbuf", "-oL", "-eL"] + cmd

    proc = subprocess.Popen(
        cmd, env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )

    out = StreamReader(proc.stdout, f"{bus_name}-stdout")
    err = StreamReader(proc.stderr, f"{bus_name}-stderr")
    out.start()
    err.start()
    return proc, out, err


# ----------------------------------------------------------------------
# Test driver
# ----------------------------------------------------------------------

def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    # Refuse to run if either of our randomised names is already taken
    # — effectively impossible (8-char hex tail) but worth catching
    # loud rather than letting two pipnodes share a name.
    for name in (SERVER_BUS, CLIENT_BUS):
        try:
            owner = get_name_owner_pid(bus, name)
            fail(f"another process (pid {owner}) already owns {name!r} "
                 f"on the session bus; cannot run this test")
        except GLib.Error:
            pass  # expected: nobody owns the name yet

    port = pick_free_port()

    env = os.environ.copy()
    env["NO_AT_BRIDGE"] = "1"

    with tempfile.TemporaryDirectory(prefix="pipnode-https-test-") as tmp:
        srv_proc, srv_out, srv_err = spawn_pipnode(SERVER_BUS, env)
        cli_proc, cli_out, cli_err = spawn_pipnode(CLIENT_BUS, env)

        try:
            for proc, name, err_reader in (
                (srv_proc, SERVER_BUS, srv_err),
                (cli_proc, CLIENT_BUS, cli_err),
            ):
                if not wait_for_bus_name(bus, name, timeout=15.0):
                    if proc.poll() is not None:
                        fail(f"pipnode exited before claiming {name!r} "
                             f"(rc={proc.returncode}); stderr was:\n"
                             f"{err_reader.text()}")
                    fail(f"pipnode never registered {name!r} on the "
                         f"session bus within 15s")

            # Load the server worksheet first so the listener is ready
            # before the client starts ticking.  Loading is synchronous
            # over D-Bus (the LoadFromFile method returns once the new
            # nodes have been instantiated, and PnHttpsTunnelReceiver starts
            # listening synchronously in restart_server()).
            srv_path = os.path.join(tmp, "server.json")
            with open(srv_path, "w", encoding="utf-8") as f:
                json.dump(build_server_network(port), f, indent=2)
            try:
                call_load_from_file(bus, SERVER_BUS, SERVER_OBJPATH, srv_path)
            except GLib.Error as e:
                fail(f"server LoadFromFile failed: {e.message}\n"
                     f"server stderr:\n{srv_err.text()}")

            cli_path = os.path.join(tmp, "client.json")
            with open(cli_path, "w", encoding="utf-8") as f:
                json.dump(build_client_network(port), f, indent=2)
            try:
                call_load_from_file(bus, CLIENT_BUS, CLIENT_OBJPATH, cli_path)
            except GLib.Error as e:
                fail(f"client LoadFromFile failed: {e.message}\n"
                     f"client stderr:\n{cli_err.text()}")

            # AutoInjector period is 1 s; 3.5 s lets the first tick fire,
            # complete a full TLS handshake against the freshly-minted
            # self-signed cert, and reach the server's Debug sink, with
            # enough slack for a second tick if the first one races the
            # TLS warm-up.
            time.sleep(3.5)

            srv_blob   = srv_out.text()
            srv_blocks = find_json_blocks(srv_blob)

            # Every message we care about comes from the HTTPS Tunnel Receiver
            # node (it is the producer on the receiver side, even
            # though the *original* author was the AutoInjector on the
            # sender side).
            ours = [b for b in srv_blocks if b.get("from") == SERVER_NAME]
            if not ours:
                fail(
                    "no JSON messages from the HTTPS Tunnel Receiver reached the "
                    "receiver's Debug sink within 3.5s — the client/"
                    "server round-trip did not complete.\n"
                    f"server stdout:\n{srv_blob}\n"
                    f"server stderr:\n{srv_err.text()}\n"
                    f"client stderr:\n{cli_err.text()}"
                )

            first = ours[0]

            # Topic round-trips: the AutoInjector stamps the resolved
            # form of its PnNode topic template
            # ("/pnode/${nodeclass}/${nodename}") onto every tick, and
            # the HTTPS Tunnel Sender/Receiver pair carries the field
            # through untouched.
            expected_topic = f"/pnode/AutoInjector/{SOURCE_NAME}"
            if first.get("topic") != expected_topic:
                fail(f"topic round-trip failed: got {first.get('topic')!r}, "
                     f"expected {expected_topic!r}")

            data = first.get("data")
            if not isinstance(data, dict):
                fail(f"server-side JSON 'data' is not an object: {data!r}")

            # Payload round-trips field by field.  Catches any future
            # change that silently drops or coerces one of the three.
            if data.get("value") != INJECTOR_PAYLOAD["value"]:
                fail(f"data.value is {data.get('value')!r}, "
                     f"expected {INJECTOR_PAYLOAD['value']!r}")
            if data.get("success") is not INJECTOR_PAYLOAD["success"]:
                fail(f"data.success is {data.get('success')!r}, "
                     f"expected {INJECTOR_PAYLOAD['success']!r}")
            if data.get("output") != INJECTOR_PAYLOAD["output"]:
                fail(f"data.output is {data.get('output')!r}, "
                     f"expected {INJECTOR_PAYLOAD['output']!r}")

            print("PASS: AutoInjector → HTTPS Tunnel Sender → HTTPS Tunnel Receiver → "
                  "Debug round-trips the envelope intact across two "
                  "pipnode processes.")
        finally:
            for proc in (cli_proc, srv_proc):
                proc.terminate()
                try:
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()


if __name__ == "__main__":
    run_test()
