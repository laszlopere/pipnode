#!/usr/bin/env python3
"""Functional test: a legacy MQTT node with inline credentials imports them
into a host vault profile on first open (plugin ABI v5 back-compat).

Loads a worksheet whose PnMqtt node still carries inline ``username`` /
``password`` (the pre-v5 shape) and no ``broker-profile``.  The node's
one-shot migration (scheduled in ``constructed()``) must, once the run's
main loop turns, create an ``mqtt-broker`` profile in the vault holding
those values and point the node at it — so an old file keeps working and
its plaintext secret moves into the 0600 store.

The broker URL is deliberately unreachable; we are testing the import, not
a connection.  Runs headless under pipnode-run.

Run directly:

    python3 tests/test_credentials_migration.py
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
PLUGIN_DIR  = os.environ.get("PIPNODE_NETWORK_DIR",
                             os.path.join(ROOT, "plugins", "network", ".libs"))

LEGACY_USER = "legacyuser"
LEGACY_PASS = "legacypass"


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def build_flow_json() -> dict:
    """A single MQTT Source carrying the pre-v5 inline credentials, pointed
    at an unreachable broker (the import does not need a live connection)."""
    return {
        "format": "pipnode", "version": 1,
        "nodes": [
            {"type": "PnMqtt", "name": "Broker",
             "position": {"x": 80.0, "y": 100.0},
             "properties": {
                 "url": "tcp://127.0.0.1:1",
                 "username": LEGACY_USER,
                 "password": LEGACY_PASS,
                 "topic": "test/#",
             }},
        ],
        "connections": [],
    }


def run_test() -> None:
    if not (os.path.isfile(PIPNODE_RUN) and os.access(PIPNODE_RUN, os.X_OK)):
        fail(f"pipnode-run not found/executable: {PIPNODE_RUN}")
    if not os.path.isdir(PLUGIN_DIR):
        fail(f"network plugin dir missing (build it first): {PLUGIN_DIR}")

    with tempfile.TemporaryDirectory(prefix="pipnode-mig-test-") as tmp:
        flow_path  = os.path.join(tmp, "flow.json")
        vault_path = os.path.join(tmp, "credentials.json")
        with open(flow_path, "w", encoding="utf-8") as f:
            json.dump(build_flow_json(), f, indent=2)

        env = os.environ.copy()
        env["PIPNODE_PLUGIN_PATH"]      = PLUGIN_DIR
        env["PIPNODE_CREDENTIALS_FILE"] = vault_path

        # --timeout 2 lets the run's main loop turn so the migration idle
        # scheduled in the node's constructed() fires and writes the vault.
        proc = subprocess.run([PIPNODE_RUN, "--timeout", "2", flow_path],
                              env=env, capture_output=True, timeout=15)
        if proc.returncode != 0:
            fail(f"pipnode-run rc={proc.returncode}\n"
                 f"stderr:\n{proc.stderr.decode(errors='replace')}")

        if not os.path.isfile(vault_path):
            fail("no credentials file was written — the legacy import did "
                 "not run")

        # The file must be owner-only.
        mode = os.stat(vault_path).st_mode & 0o777
        if mode != 0o600:
            fail(f"credentials file mode is {oct(mode)}, expected 0o600")

        with open(vault_path, encoding="utf-8") as f:
            vault = json.load(f)

    profiles = vault.get("profiles", {})
    brokers  = [p for p in profiles.values()
                if p.get("type") == "mqtt-broker"]
    if not brokers:
        fail(f"no mqtt-broker profile was imported; vault was:\n"
             f"{json.dumps(vault, indent=2)}")

    fields = brokers[0].get("fields", {})
    if fields.get("username") != LEGACY_USER:
        fail(f"imported username was {fields.get('username')!r}, "
             f"expected {LEGACY_USER!r}")
    if fields.get("password") != LEGACY_PASS:
        fail(f"imported password was {fields.get('password')!r}, "
             f"expected {LEGACY_PASS!r}")

    # And it should have become the primary, so empty-ref nodes follow it.
    if vault.get("defaults", {}).get("mqtt-broker") not in profiles:
        fail("imported profile was not registered as the mqtt-broker primary")

    print("PASS: a legacy MQTT node's inline credentials were imported into "
          "an mqtt-broker vault profile (0600), set as the primary, headless "
          "under pipnode-run.")


if __name__ == "__main__":
    run_test()
