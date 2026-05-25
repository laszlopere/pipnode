#!/usr/bin/env python3
"""Functional test: a node resolves a host-provisioned profile at run time
(plugin ABI v5), headless, under pipnode-run.

Loads the in-tree ``vaultecho`` sample plugin (tests/plugins/vaultecho),
which declares a ``vaultecho`` profile type and a PnVaultEcho node that
stamps the resolved ``greeting`` and the resolved ``secret``'s length onto
each message.  A temp ``credentials.json`` provides a profile; the node's
own ``profile`` reference is left empty, so it must follow the type's
*primary*.  Then the same run is repeated with a
``PIPNODE_PROFILE_<ID>_<FIELD>`` environment override to prove the
precedence (env > file).

Run directly:

    python3 tests/test_credentials_profile.py
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
PLUGIN_DIR  = os.environ.get("PIPNODE_VAULTECHO_DIR",
                             os.path.join(ROOT, "tests", "plugins",
                                          "vaultecho", ".libs"))


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def build_flow_json() -> dict:
    """AutoInjector -> Vault Echo -> Debug.  The Vault Echo node leaves its
    ``profile`` reference empty, so it resolves the type's primary."""
    return {
        "format": "pipnode", "version": 1,
        "nodes": [
            {"type": "PnAutoInjector", "name": "Source",
             "position": {"x": 80.0, "y": 100.0},
             "properties": {"period": 1, "value": 1.0, "output": "tick"}},
            {"type": "PnVaultEcho", "name": "VE",
             "position": {"x": 280.0, "y": 100.0}, "properties": {}},
            {"type": "PnDebug", "name": "Sink",
             "position": {"x": 480.0, "y": 100.0},
             "properties": {"target": "Standard Output", "format": "JSON"}},
        ],
        "connections": [
            {"source": "Source", "target": "VE"},
            {"source": "VE", "target": "Sink"},
        ],
    }


def write_vault(path: str) -> None:
    vault = {
        "version": 1,
        "defaults": {"vaultecho": "ve"},
        "profiles": {
            "ve": {
                "type": "vaultecho", "name": "VE",
                "fields": {"greeting": "howdy", "secret": "s3cr3t"},
            }
        },
    }
    with open(path, "w", encoding="utf-8") as f:
        json.dump(vault, f, indent=2)
    os.chmod(path, 0o600)


def find_json_blocks(blob: str) -> list:
    blocks, decoder, i = [], json.JSONDecoder(), 0
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


def run_flow(flow_path: str, extra_env: dict) -> dict:
    """Run the flow once, return the first message emitted by the VE node."""
    env = os.environ.copy()
    env["PIPNODE_PLUGIN_PATH"] = PLUGIN_DIR
    env.update(extra_env)

    prefix = []
    if subprocess.run(["which", "stdbuf"], capture_output=True).returncode == 0:
        prefix = ["stdbuf", "-oL", "-eL"]

    proc = subprocess.run(prefix + [PIPNODE_RUN, "--timeout", "3", flow_path],
                          env=env, capture_output=True, timeout=15)
    if proc.returncode != 0:
        fail(f"pipnode-run rc={proc.returncode}\n"
             f"stderr:\n{proc.stderr.decode(errors='replace')}")

    blob = proc.stdout.decode("utf-8", errors="replace")
    passed = [b for b in find_json_blocks(blob) if b.get("from") == "VE"]
    if not passed:
        fail("no message emitted by the Vault Echo node — the plugin failed "
             f"to load or resolve.\nstdout:\n{blob}\n"
             f"stderr:\n{proc.stderr.decode(errors='replace')}")
    return passed[0].get("data", {})


def run_test() -> None:
    if not (os.path.isfile(PIPNODE_RUN) and os.access(PIPNODE_RUN, os.X_OK)):
        fail(f"pipnode-run not found/executable: {PIPNODE_RUN}")
    if not os.path.isdir(PLUGIN_DIR):
        fail(f"vaultecho plugin dir missing (build it first): {PLUGIN_DIR}")

    with tempfile.TemporaryDirectory(prefix="pipnode-cred-test-") as tmp:
        flow_path  = os.path.join(tmp, "flow.json")
        vault_path = os.path.join(tmp, "credentials.json")
        with open(flow_path, "w", encoding="utf-8") as f:
            json.dump(build_flow_json(), f, indent=2)
        write_vault(vault_path)

        cred_env = {"PIPNODE_CREDENTIALS_FILE": vault_path}

        # (1) Resolve from the file via the primary profile.
        data = run_flow(flow_path, cred_env)
        if data.get("greeting") != "howdy":
            fail(f"greeting was {data.get('greeting')!r}, expected 'howdy' "
                 "(profile string field did not resolve from the file)")
        if data.get("secret_len") != len("s3cr3t"):
            fail(f"secret_len was {data.get('secret_len')!r}, expected "
                 f"{len('s3cr3t')} (secret did not resolve from the file)")

        # (2) Environment override must beat the stored secret.
        env2 = dict(cred_env)
        env2["PIPNODE_PROFILE_VE_SECRET"] = "xy"
        data = run_flow(flow_path, env2)
        if data.get("secret_len") != 2:
            fail(f"secret_len was {data.get('secret_len')!r} under an env "
                 "override, expected 2 (env override did not take precedence)")
        if data.get("greeting") != "howdy":
            fail("greeting changed under a secret-only env override; "
                 "resolution leaked across fields")

    print("PASS: pipnode-run loaded the vaultecho ABI-v5 plugin and the node "
          "resolved its profile from the file (via the primary) and honoured "
          "the PIPNODE_PROFILE_* environment override.")


if __name__ == "__main__":
    run_test()
