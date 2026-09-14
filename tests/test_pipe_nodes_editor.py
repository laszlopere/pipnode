#!/usr/bin/env python3
"""Functional test: Pipe Reader / Pipe Writer in the editor (TODO #68.6).

Drives a private ``pipnode-editor`` over D-Bus through the same paths a
user takes, with this script standing in for the outside program on the
far end of each named pipe:

  1. PALETTE: both types are registered, Pipe Reader under Sources and
     Pipe Writer under Sinks, each with its help page.
  2. ERROR STATE: a freshly dropped node is in the error state (painted
     red) because no pipe is set.
  3. SETTINGS DIALOG: the node's dialog has a file-path editor for
     "pipe-path" (empty) and a format combo showing "Output text".
     Typing a path into the dialog creates the FIFO and clears the
     error state.
  4. READER: a line written into the FIFO from outside (``echo hi >
     fifo``) comes out of the node as data.output; after switching the
     dialog's combo to "JSON message" a one-line envelope comes out
     with its topic and data intact.
  5. WRITER: with an outside reader on the FIFO (``cat fifo``), a
     message injected into the node arrives as its output line; after
     switching the combo to "JSON message" the whole envelope arrives.

Exits 0 on success, non-zero on the first failure.
"""

from __future__ import annotations

import json
import os
import select
import stat
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from gi.repository import GLib  # noqa: E402

from pndbus import PipnodeEditor  # noqa: E402


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def say(msg: str) -> None:
    print(msg)


def wait_until(pred, timeout: float = 3.0, step: float = 0.05):
    """Poll @pred until it returns a truthy value or @timeout elapses."""
    deadline = time.monotonic() + timeout
    while True:
        got = pred()
        if got or time.monotonic() >= deadline:
            return got
        time.sleep(step)


# ----------------------------------------------------------------------
# Dialog helpers (the test-only Worksheet dialog methods)
# ----------------------------------------------------------------------

def open_dialog(ed: PipnodeEditor, uuid: str) -> None:
    if not ed.ws("OpenNodeDialogByUuid", GLib.Variant("(s)", (uuid,)),
                 "(b)").unpack()[0]:
        fail(f"OpenNodeDialogByUuid({uuid}) returned False")


def editor_text(ed: PipnodeEditor, prop: str) -> str:
    return ed.ws("GetDialogEditorText", GLib.Variant("(s)", (prop,)),
                 "(s)").unpack()[0]


def editor_sensitive(ed: PipnodeEditor, prop: str) -> bool:
    return ed.ws("GetDialogEditorSensitive", GLib.Variant("(s)", (prop,)),
                 "(b)").unpack()[0]


def set_editor_text(ed: PipnodeEditor, prop: str, text: str) -> None:
    if not ed.ws("SetDialogEditorText", GLib.Variant("(ss)", (prop, text)),
                 "(b)").unpack()[0]:
        fail(f"SetDialogEditorText({prop!r}, {text!r}) returned False")


def has_error(ed: PipnodeEditor, uuid: str) -> bool:
    return ed.get_node_property(uuid, "has-error") == "true"


def check_dialog(ed: PipnodeEditor, uuid: str, label: str) -> None:
    """The settings editors exist, are live, and show the defaults."""
    open_dialog(ed, uuid)
    titles = list(ed.ws("GetDialogPageTitles", None, "(as)").unpack()[0])
    expected = ["Class", "Node", label.replace(" ", "")]
    if titles != expected:
        fail(f"{label}: dialog tabs are {titles!r}, expected {expected!r}")
    say(f"{label}: dialog tabs {titles}")

    for prop in ("pipe-path", "format"):
        if not editor_sensitive(ed, prop):
            fail(f"{label}: the {prop!r} editor is greyed out")

    if editor_text(ed, "pipe-path") != "":
        fail(f"{label}: pipe-path editor shows "
             f"{editor_text(ed, 'pipe-path')!r}, expected empty")
    if editor_text(ed, "format") != "Output text":
        fail(f"{label}: format combo shows {editor_text(ed, 'format')!r}, "
             f"expected 'Output text'")


def set_path_in_dialog(ed: PipnodeEditor, uuid: str, path: str,
                       label: str) -> None:
    set_editor_text(ed, "pipe-path", path)
    if ed.get_node_property(uuid, "pipe-path") != path:
        fail(f"{label}: typing into the dialog did not set pipe-path")

    if not wait_until(lambda: os.path.exists(path)):
        fail(f"{label}: {path} was not created")
    if not stat.S_ISFIFO(os.stat(path).st_mode):
        fail(f"{label}: {path} is not a named pipe")
    if not wait_until(lambda: not has_error(ed, uuid)):
        fail(f"{label}: still in the error state after the path was set")
    say(f"{label}: path set in the dialog, FIFO created, error cleared")


def set_format_in_dialog(ed: PipnodeEditor, uuid: str, label: str) -> None:
    set_editor_text(ed, "format", "JSON message")
    if ed.get_node_property(uuid, "format") != "JSON message":
        fail(f"{label}: the combo did not switch format, node has "
             f"{ed.get_node_property(uuid, 'format')!r}")


# ----------------------------------------------------------------------
# Test steps
# ----------------------------------------------------------------------

def check_palette(ed: PipnodeEditor) -> None:
    types = {t["type_name"]: t for t in ed.list_node_types()}
    for type_name, category, help_page in (
            ("PnPipeReader", "Sources", "PnPipeReader.html"),
            ("PnPipeWriter", "Sinks",   "PnPipeWriter.html")):
        if type_name not in types:
            fail(f"{type_name} is not in the palette")
        if types[type_name]["category"] != category:
            fail(f"{type_name} is under {types[type_name]['category']!r}, "
                 f"expected {category!r}")
        info = ed.get_node_type_info(type_name)
        if info["help_page"] != help_page:
            fail(f"{type_name} help page is {info['help_page']!r}")
    say("palette: Pipe Reader under Sources, Pipe Writer under Sinks")


def last_output(ed: PipnodeEditor, uuid: str):
    msg = ed.get_last_output_message(uuid)
    return msg if msg else None


def check_reader(ed: PipnodeEditor, fifo: str) -> None:
    label = "Pipe Reader"
    uuid  = ed.add_node("PnPipeReader", 120.0, 120.0)

    if not has_error(ed, uuid):
        fail(f"{label}: a new node without a pipe should be in error")
    say(f"{label}: red until a path is set")

    check_dialog(ed, uuid, label)
    set_path_in_dialog(ed, uuid, fifo, label)

    # echo hi > fifo
    fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
    try:
        os.write(fd, b"hi\n")
        msg = wait_until(lambda: last_output(ed, uuid))
        if msg is None:
            fail(f"{label}: nothing came out after writing 'hi' to the FIFO")
        data = msg.get("data", {})
        if data.get("output") != "hi" or data.get("success") is not True:
            fail(f"{label}: 'hi' line came out as {msg!r}")
        say(f"{label}: 'echo hi > fifo' emitted output 'hi'")

        set_format_in_dialog(ed, uuid, label)
        envelope = {"topic": "/test/pipe",
                    "data": {"value": 3.5, "success": True,
                             "output": "from-json"}}
        os.write(fd, (json.dumps(envelope) + "\n").encode())
        msg = wait_until(
                lambda: (m := last_output(ed, uuid)) is not None
                and m.get("data", {}).get("output") == "from-json" and m)
        if not msg:
            fail(f"{label}: the JSON line never came out; last message "
                 f"{last_output(ed, uuid)!r}")
        if msg.get("topic") != "/test/pipe" or \
           msg["data"].get("value") != 3.5:
            fail(f"{label}: JSON envelope came out as {msg!r}")
        say(f"{label}: a JSON line came out with its topic and data")
    finally:
        os.close(fd)
        ed.ws("CloseNodeDialog", None, "(b)")


def read_line(fd: int, timeout: float = 3.0) -> str | None:
    """One newline-terminated line from the non-blocking @fd, or None."""
    buf      = b""
    deadline = time.monotonic() + timeout
    while b"\n" not in buf:
        left = deadline - time.monotonic()
        if left <= 0:
            return None
        ready, _, _ = select.select([fd], [], [], left)
        if ready:
            chunk = os.read(fd, 65536)
            if chunk:
                buf += chunk
    return buf.split(b"\n", 1)[0].decode()


def check_writer(ed: PipnodeEditor, fifo: str) -> None:
    label = "Pipe Writer"
    uuid  = ed.add_node("PnPipeWriter", 420.0, 120.0)

    if not has_error(ed, uuid):
        fail(f"{label}: a new node without a pipe should be in error")
    say(f"{label}: red until a path is set")

    check_dialog(ed, uuid, label)
    set_path_in_dialog(ed, uuid, fifo, label)

    # cat fifo
    fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
    try:
        ed.inject_message(uuid, {"topic": "/test/writer",
                                 "data": {"value": 1.0, "success": True,
                                          "output": "hello-cat"}})
        line = read_line(fd)
        if line != "hello-cat":
            fail(f"{label}: 'cat fifo' read {line!r}, expected 'hello-cat'")
        say(f"{label}: 'cat fifo' shows the injected output line")

        set_format_in_dialog(ed, uuid, label)
        ed.inject_message(uuid, {"topic": "/test/writer",
                                 "data": {"value": 2.5, "success": True,
                                          "output": "as-json"}})
        line = read_line(fd)
        if line is None:
            fail(f"{label}: no JSON line reached the FIFO")
        try:
            got = json.loads(line)
        except ValueError:
            fail(f"{label}: JSON line is not JSON: {line!r}")
        data = got.get("data", {})
        if got.get("topic") != "/test/writer" or \
           data.get("output") != "as-json" or data.get("value") != 2.5:
            fail(f"{label}: JSON line is {line!r}")
        say(f"{label}: 'cat fifo' shows the whole envelope as one JSON line")
    finally:
        os.close(fd)
        ed.ws("CloseNodeDialog", None, "(b)")


def main() -> None:
    os.environ.setdefault("NO_AT_BRIDGE", "1")

    with tempfile.TemporaryDirectory(prefix="pipnode-pipe-ed-") as tmp, \
         PipnodeEditor.launch() as ed:
        ed.new_document()
        check_palette(ed)
        check_reader(ed, os.path.join(tmp, "reader.fifo"))
        check_writer(ed, os.path.join(tmp, "writer.fifo"))

    print("PASS: Pipe Reader and Pipe Writer are in the palette, red until "
          "a path is set, configurable through their dialogs, and trade "
          "lines with an outside program in both formats.")


if __name__ == "__main__":
    main()
