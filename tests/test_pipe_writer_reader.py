#!/usr/bin/env python3
"""Functional test: AutoInjector → Pipe Writer → FIFO → Pipe Reader → Debug.

Headless end-to-end check of the named pipe node pair through
``pipnode-run``: a Pipe Writer and a Pipe Reader share one FIFO, so the
writer's lines are exactly what the reader parses, and one run checks
both nodes.

The worksheet holds two independent pairs, one per format, each on its
own FIFO in a temporary directory:

    AutoInjector "JSON Source" → Pipe Writer (JSON message) → json.fifo
    json.fifo → Pipe Reader "JSON Reader" (JSON message) → Debug

    AutoInjector "Text Source" → Pipe Writer (Output text) → text.fifo
    text.fifo → Pipe Reader "Text Reader" (Output text)  → Debug

Neither FIFO exists beforehand; the nodes create them.

Asserted:

  * both FIFOs were created as named pipes;
  * the JSON pair carries the whole envelope: the injector's topic and
    data.value / data.success / data.output arrive unchanged, with
    ``from`` rewritten to the reader;
  * the text pair carries only the output line: data.output is the
    injector's output, data.value is the number that line spells, and
    the topic is the reader's own;
  * no reader emitted a failure message.

Run directly:

    python3 tests/test_pipe_writer_reader.py

Set ``$PIPNODE_RUN`` to override the binary path; defaults to the
in-tree build at ``src/pipnode-run``.
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
import sys
import tempfile


ROOT        = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE_RUN = os.environ.get(
        "PIPNODE_RUN", os.path.join(ROOT, "src", "pipnode-run"))

RUN_SECONDS = 4

JSON_SOURCE = "JSON Source"
JSON_WRITER = "JSON Writer"
JSON_READER = "JSON Reader"
TEXT_SOURCE = "Text Source"
TEXT_WRITER = "Text Writer"
TEXT_READER = "Text Reader"

JSON_PAYLOAD = {"value": 7.0, "success": True, "output": "hello-from-pipe"}
# A line that spells a number, so the reader's value parsing is checked.
TEXT_PAYLOAD = {"value": 1.0, "success": True, "output": "42.5"}


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def injector(name: str, payload: dict, y: float) -> dict:
    return {
        "type":       "PnAutoInjector",
        "name":       name,
        "position":   {"x": 40.0, "y": y},
        "properties": {
            "period":  1,
            "value":   payload["value"],
            "success": payload["success"],
            "output":  payload["output"],
        },
    }


def pipe_node(kind: str, name: str, path: str, fmt: str,
              x: float, y: float) -> dict:
    return {
        "type":       kind,
        "name":       name,
        "position":   {"x": x, "y": y},
        "properties": {"pipe-path": path, "format": fmt},
    }


def build_network(json_fifo: str, text_fifo: str) -> dict:
    nodes = [
        injector(JSON_SOURCE, JSON_PAYLOAD, 100.0),
        pipe_node("PnPipeWriter", JSON_WRITER, json_fifo, "JSON message",
                  280.0, 100.0),
        pipe_node("PnPipeReader", JSON_READER, json_fifo, "JSON message",
                  520.0, 100.0),
        injector(TEXT_SOURCE, TEXT_PAYLOAD, 260.0),
        pipe_node("PnPipeWriter", TEXT_WRITER, text_fifo, "Output text",
                  280.0, 260.0),
        pipe_node("PnPipeReader", TEXT_READER, text_fifo, "Output text",
                  520.0, 260.0),
        {
            "type":       "PnDebug",
            "name":       "Sink",
            "position":   {"x": 760.0, "y": 180.0},
            "properties": {"target": "Standard Output", "format": "JSON"},
        },
    ]
    connections = [
        {"source": JSON_SOURCE, "target": JSON_WRITER},
        {"source": TEXT_SOURCE, "target": TEXT_WRITER},
        {"source": JSON_READER, "target": "Sink"},
        {"source": TEXT_READER, "target": "Sink"},
    ]
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


def check_data(label: str, block: dict, expected: dict) -> None:
    data = block.get("data")
    if not isinstance(data, dict):
        fail(f"{label}: 'data' is not an object: {block!r}")
    for key, want in expected.items():
        got = data.get(key)
        if got != want or type(got) is not type(want):
            fail(f"{label}: data.{key} is {got!r}, expected {want!r}")


def main() -> None:
    if not os.path.exists(PIPNODE_RUN):
        fail(f"pipnode-run not found at {PIPNODE_RUN}")

    with tempfile.TemporaryDirectory(prefix="pipnode-pipe-test-") as tmp:
        json_fifo = os.path.join(tmp, "json.fifo")
        text_fifo = os.path.join(tmp, "text.fifo")
        path      = os.path.join(tmp, "pipes.json")
        with open(path, "w", encoding="utf-8") as fh:
            json.dump(build_network(json_fifo, text_fifo), fh, indent=2)

        proc = subprocess.run(
                [PIPNODE_RUN, "--timeout", str(RUN_SECONDS), path],
                capture_output=True, text=True, timeout=RUN_SECONDS + 10)

        for fifo in (json_fifo, text_fifo):
            try:
                mode = os.stat(fifo).st_mode
            except FileNotFoundError:
                fail(f"{fifo} was never created; stderr:\n{proc.stderr}")
            if not stat.S_ISFIFO(mode):
                fail(f"{fifo} exists but is not a named pipe")

    if proc.returncode != 0:
        fail(f"pipnode-run exited {proc.returncode}; stderr:\n{proc.stderr}")
    if "unknown node type" in proc.stderr.lower():
        fail(f"pipnode-run does not know a node type; stderr:\n{proc.stderr}")

    blocks = find_json_blocks(proc.stdout)
    if not blocks:
        fail(f"no Debug output on stdout; stderr:\n{proc.stderr}")

    for b in blocks:
        data = b.get("data") if isinstance(b.get("data"), dict) else {}
        if data.get("success") is False:
            fail(f"a reader reported a failure: {b!r}")

    from_json = [b for b in blocks if b.get("from") == JSON_READER]
    from_text = [b for b in blocks if b.get("from") == TEXT_READER]
    stray     = [b for b in blocks
                 if b.get("from") not in (JSON_READER, TEXT_READER)]
    if stray:
        fail(f"unexpected message reached the sink: {stray[0]!r}")

    if not from_json:
        fail(f"no message came through the JSON pipe in {RUN_SECONDS}s; "
             f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")
    if not from_text:
        fail(f"no message came through the text pipe in {RUN_SECONDS}s; "
             f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}")

    # The JSON format carries the envelope, topic included.
    json_topic = f"/pnode/AutoInjector/{JSON_SOURCE}"
    for b in from_json:
        if b.get("topic") != json_topic:
            fail(f"JSON pipe: topic is {b.get('topic')!r}, "
                 f"expected {json_topic!r}")
        check_data("JSON pipe", b, JSON_PAYLOAD)

    # The text format carries only the line; the reader supplies the rest.
    text_topic = f"/pnode/Pipe Reader/{TEXT_READER}"
    for b in from_text:
        if b.get("topic") != text_topic:
            fail(f"text pipe: topic is {b.get('topic')!r}, "
                 f"expected {text_topic!r}")
        check_data("text pipe", b, {
            "value":   float(TEXT_PAYLOAD["output"]),
            "success": True,
            "output":  TEXT_PAYLOAD["output"],
        })

    print(f"PASS: Pipe Writer → FIFO → Pipe Reader round-trips messages "
          f"in both formats (json={len(from_json)}, text={len(from_text)}).")


if __name__ == "__main__":
    main()
