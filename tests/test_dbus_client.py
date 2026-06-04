#!/usr/bin/env python3
"""Functional test: the reference Python client (TODO #40.15).

Phase F of the D-Bus automation interface ships a thin reference client
(``tests/pndbus.py``) and the contract doc (``DBUS-API.md``).  This test
drives the *whole* documented surface through that client in one end-to-end
session — version handshake, document JSON round-trip, sheets, globals, node
lifecycle, single + batch properties, port-aware wiring, discovery,
selection/view, message inject + readback, and the live signals — so the
client doubles as executable documentation and as broad coverage that every
method/signal still answers.

Per-feature edge cases (atomicity, every error name, each rejection path)
live in the focused ``test_dbus_*.py`` siblings; this one proves the happy
path of the surface as a client actually uses it, and that the helper itself
is correct.

Run directly:

    python3 tests/test_dbus_client.py

Set $PIPNODE to override the path to the binary; defaults to the in-tree
build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pndbus import PipnodeEditor, PipnodeError  # noqa: E402


def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


def expect_error(short: str, fn, *args, **kw) -> None:
    """Assert that calling fn(*args) raises PipnodeError with .short == short."""
    try:
        fn(*args, **kw)
    except PipnodeError as e:
        if e.short != short:
            fail(f"expected error {short!r}, got {e.short!r}")
        return
    fail(f"expected error {short!r} but the call succeeded")


def run_test() -> None:
    with PipnodeEditor.launch() as ed:
        ed.new_document()
        ed.subscribe()              # collect every signal into ed.events

        # --- version handshake --------------------------------------------
        major, minor = ed.get_api_version()
        if (major, minor) < (1, 2):
            fail(f"unexpected API version {major}.{minor} (want >= 1.2)")

        # --- discovery: the live palette ----------------------------------
        types = ed.list_node_types()
        by_name = {t["type_name"] for t in types}
        for need in ("PnKnob", "PnTopic", "PnDebug"):
            if need not in by_name:
                fail(f"ListNodeTypes is missing core type {need}")
        info = ed.get_node_type_info("PnKnob")
        if info["help_page"] != "PnKnob.html":
            fail(f"GetNodeTypeInfo help_page wrong: {info['help_page']!r}")
        expect_error("UnknownNodeType", ed.get_node_type_info, "PnNope")

        # --- node lifecycle -----------------------------------------------
        ed.clear_events()
        knob  = ed.add_node("PnKnob",  100.0, 100.0)
        topic = ed.add_node("PnTopic", 360.0, 100.0)
        debug = ed.add_node("PnDebug", 620.0, 100.0)
        if ed.node_count() != 3:
            fail(f"expected 3 nodes, got {ed.node_count()}")

        ed.move_node(knob, 120.0, 220.0)
        ed.rename_node(knob, "Volume")
        n = ed.get_node(knob)
        if n["name"] != "Volume" or abs(n["x"] - 120.0) > 0.01:
            fail(f"move/rename did not stick: {n!r}")

        gx, gy, gw, gh = ed.get_node_geometry(knob)
        if gw <= 0 or gh <= 0:
            fail(f"GetNodeGeometry gave non-positive size {gw}x{gh}")
        expect_error("NodeNotFound", ed.get_node, "no-such-uuid")

        # --- discovery: property schema -----------------------------------
        props = {p["name"]: p for p in ed.list_node_properties(topic)}
        if "topic" not in props:
            fail("ListNodeProperties did not surface PnTopic.topic")
        if not props["topic"]["writable"]:
            fail("PnTopic.topic should be writable")

        # --- properties: single + atomic batch ----------------------------
        ed.set_node_property(topic, "topic", "demo/level")
        if ed.get_node_property(topic, "topic") != "demo/level":
            fail("single SetNodeProperty/Get round-trip failed")

        ed.set_node_properties(topic, {"topic": "demo/volume"})
        if ed.get_node_property(topic, "topic") != "demo/volume":
            fail("batch SetNodeProperties did not apply")
        # Atomic: a bad pair leaves the earlier good value untouched.
        expect_error("UnknownProperty", ed.set_node_properties, topic,
                     {"topic": "rolled-back", "nope": "x"})
        if ed.get_node_property(topic, "topic") != "demo/volume":
            fail("SetNodeProperties was not atomic on a bad pair")

        # --- multi-input wiring -------------------------------------------
        ed.set_node_input_count(debug, 1)        # PnDebug single input
        wire = ed.connect(topic, debug, 0)
        wires = ed.list_wires()
        if not any(w["uuid"] == wire and w["source"] == topic
                   and w["target"] == debug for w in wires):
            fail(f"ListWires missing the topic->debug wire: {wires!r}")
        if not ed.get_node_wires(topic):
            fail("GetNodeWires(topic) returned nothing")
        expect_error("IllegalConnection", ed.connect, debug, debug, 0)

        # --- selection & view ---------------------------------------------
        matched = ed.select_nodes([knob, topic, "bogus"])
        if matched != 2:
            fail(f"SelectNodes matched {matched}, expected 2")
        if set(ed.get_selection()) != {knob, topic}:
            fail("GetSelection disagreed with SelectNodes")
        ed.clear_selection()
        if ed.get_selection():
            fail("ClearSelection left a non-empty selection")
        ed.focus_node(knob)
        ed.center_on(debug)
        ed.fit_to_content()

        # --- message inject + readback ------------------------------------
        if ed.get_last_output_message(topic) is not None:
            fail("GetLastOutputMessage should be None before any emission")
        ed.inject_message(topic, {"value": 42.5, "label": "hi"})
        out = ed.get_last_output_message(topic)
        if out["data"]["value"] != 42.5 or out["data"]["label"] != "hi":
            fail(f"inject/readback round-trip lost data: {out!r}")
        expect_error("BadPropertyValue", ed.inject_message_on_input,
                     topic, 9, {"value": 1})

        # --- whole-document JSON round-trip -------------------------------
        doc = ed.get_document_json()
        ok, err = ed.validate_json(doc)
        if not ok:
            fail(f"ValidateJson rejected our own document: {err}")
        ed.set_document_json(doc)               # must not raise
        if ed.node_count() != 3:
            fail("document round-trip changed the node count")

        ws_json = ed.get_worksheet_json()
        ed.set_worksheet_json(ws_json)          # active-sheet round-trip

        # --- sheets -------------------------------------------------------
        start_sheets = ed.list_sheets()
        logic = ed.add_sheet("Logic")
        if logic not in ed.list_sheets():
            fail("AddSheet did not appear in ListSheets")
        ed.select_sheet(logic)
        if ed.active_sheet() != logic:
            fail("SelectSheet/GetActiveSheet disagree")
        ed.rename_sheet(logic, "Rules")
        ed.select_sheet(start_sheets[0])
        ed.remove_sheet("Rules")
        if "Rules" in ed.list_sheets():
            fail("RemoveSheet left the sheet behind")
        expect_error("SheetNotFound", ed.select_sheet, "ghost")

        # --- globals ------------------------------------------------------
        ed.set_global("threshold", "double", "3.5")
        if ed.get_global("threshold") != ("double", "3.5"):
            fail("global double round-trip failed")
        ed.set_global("name", "string", "alpha")
        names = {g[0] for g in ed.list_globals()}
        if not {"threshold", "name"} <= names:
            fail(f"ListGlobals missing entries: {names!r}")
        ed.remove_global("threshold")
        expect_error("GlobalNotFound", ed.get_global, "threshold")

        # --- lifecycle: Save / Open via the client ------------------------
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "client.pnflow")
            ed.save_as(path)
            if ed.current_path() != path:
                fail("GetCurrentPath did not follow SaveAs")
            if ed.is_modified():
                fail("document still modified right after SaveAs")
            ed.new_document()
            ed.open(path)
            if ed.node_count() != 3:
                fail("reopened document lost its nodes")

        # --- live signals -------------------------------------------------
        # By now the bridge has emitted across the whole session; confirm a
        # representative spread arrived (the client collected them).
        ed.pump()
        for sig in ("NodeAdded", "WireAdded", "NodeMoved", "NodeRenamed",
                    "SelectionChanged", "MessageEmitted", "SheetChanged",
                    "DocumentModified"):
            if not ed.seen(sig):
                fail(f"expected D-Bus signal {sig!r} but it never arrived "
                     f"(saw: {sorted({n for n, _ in ed.events})})")

    print("PASS: reference Python client (TODO #40.15) — the full automation "
          "surface round-trips through pndbus.py end to end")


if __name__ == "__main__":
    run_test()
