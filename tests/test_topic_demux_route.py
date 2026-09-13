#!/usr/bin/env python3
"""Functional test: AutoRandom x3 → Topic Demux → one branch per source.

Headless end-to-end check of the Topic Demux node (TODO #67) through
``pipnode-run``, modelled on the network of
``examples/controls-and-logic/topic-demux.json`` but built inline, so a
later edit of the example cannot change what is tested.

Three AutoRandom sources tick into one Topic Demux whose three outputs
carry the sources' default topics (``/pnode/AutoRandom/<name>``).  Where
the example ends each output in a Numeric display, this test ends each
in a Set node that stamps ``data.branch`` with the output's number and
feeds a shared Standard Output Debug node, so the JSON on stdout tells
which output every message left by.  A fourth source, whose topic no
output lists, is wired into the demux too.

Asserted:

  * every output carries at least one message;
  * output k carries only messages from source k (``from`` + ``topic``);
  * nothing from the unlisted source reaches any output.

Run directly:

    python3 tests/test_topic_demux_route.py

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

SOURCES = ["AutoRandom 1", "AutoRandom 2", "AutoRandom 3"]
STRAY   = "AutoRandom Stray"
RUN_SECONDS = 4


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def topic_of(name: str) -> str:
    # The PnNode "topic" template defaults to /pnode/${nodeclass}/${nodename}.
    return f"/pnode/AutoRandom/{name}"


def auto_random(name: str, y: float) -> dict:
    return {
        "type":       "PnAutoRandom",
        "name":       name,
        "position":   {"x": 40.0, "y": y},
        "properties": {
            "period":       1,
            "output":       "AutoRandom sample.",
            "min":          0.0,
            "max":          1.0,
            "distribution": "uniform",
            "success":      True,
        },
    }


def build_network() -> dict:
    nodes = [auto_random(name, 260.0 + 60.0 * i)
             for i, name in enumerate(SOURCES + [STRAY])]

    nodes.append({
        "type":       "PnTopicDemux",
        "name":       "Topic Demux",
        "position":   {"x": 320.0, "y": 260.0},
        "properties": {
            "outputs": len(SOURCES),
            "topics":  json.dumps([topic_of(n) for n in SOURCES]),
        },
    })

    connections = [{"source": n, "target": "Topic Demux"}
                   for n in SOURCES + [STRAY]]

    for k in range(len(SOURCES)):
        tag = f"Branch {k + 1}"
        nodes.append({
            "type":       "PnSet",
            "name":       tag,
            "position":   {"x": 560.0, "y": 180.0 + 120.0 * k},
            "properties": {
                "props": json.dumps([{"path": "branch", "literal": k + 1}]),
            },
        })
        connections.append({"source": "Topic Demux", "target": tag,
                            "source_output": k})
        connections.append({"source": tag, "target": "Sink"})

    nodes.append({
        "type":       "PnDebug",
        "name":       "Sink",
        "position":   {"x": 800.0, "y": 260.0},
        "properties": {"target": "Standard Output", "format": "JSON"},
    })

    return {
        "format":      "pipnode",
        "version":     1,
        "nodes":       nodes,
        "connections": connections,
    }


def find_json_blocks(blob: str) -> list:
    """Every top-level JSON object in @blob (Debug pretty-prints them)."""
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


def main() -> None:
    if not os.path.exists(PIPNODE_RUN):
        fail(f"pipnode-run not found at {PIPNODE_RUN}")

    with tempfile.TemporaryDirectory(prefix="pipnode-test-") as tmp:
        path = os.path.join(tmp, "topic-demux.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(build_network(), fh, indent=2)

        proc = subprocess.run(
                [PIPNODE_RUN, "--timeout", str(RUN_SECONDS), path],
                capture_output=True, text=True, timeout=RUN_SECONDS + 10)

    if proc.returncode != 0:
        fail(f"pipnode-run exited {proc.returncode}; stderr:\n{proc.stderr}")
    if "unknown node type" in proc.stderr.lower():
        fail(f"pipnode-run does not know a node type; stderr:\n{proc.stderr}")

    blocks = find_json_blocks(proc.stdout)
    if not blocks:
        fail(f"no Debug output on stdout; stderr:\n{proc.stderr}")

    per_branch = {k + 1: [] for k in range(len(SOURCES))}
    for b in blocks:
        data   = b.get("data") if isinstance(b.get("data"), dict) else {}
        branch = data.get("branch")
        if b.get("from") == STRAY or b.get("topic") == topic_of(STRAY):
            fail(f"a message from the unlisted source got through: {b!r}")
        if branch not in per_branch:
            fail(f"message without a valid data.branch reached the sink: "
                 f"{b!r}")
        per_branch[branch].append(b)

    for k, name in enumerate(SOURCES, start=1):
        got = per_branch[k]
        if not got:
            fail(f"output {k} carried no messages in {RUN_SECONDS}s")
        for b in got:
            if b.get("from") != name or b.get("topic") != topic_of(name):
                fail(f"output {k} carried from={b.get('from')!r} "
                     f"topic={b.get('topic')!r}, expected only {name!r}")

    counts = ", ".join(f"out{k}={len(v)}" for k, v in per_branch.items())
    print(f"PASS: Topic Demux routes each source to its own output "
          f"and drops the unlisted topic ({counts}).")


if __name__ == "__main__":
    main()
