#!/usr/bin/env python3
"""Functional test: whole-document operations over D-Bus (TODO #40.9-40.11).

Phase D of the automation interface adds the *cheap bulk* surface to
org.pipas.pipnode.Editor — reading/rewriting an entire sheet or document
in one call, driving the sheet list and file lifecycle, and editing the
document globals an agent wires ${...} variables from.  This test drives
all three sub-items through a freshly launched editor:

  40.9  JSON round-trip
        GetDocumentJson()/SetDocumentJson(s), GetWorksheetJson()/
        SetWorksheetJson(s) (active sheet), ValidateJson(s)->(ok,error).

  40.10 Sheets + document lifecycle
        ListSheets/GetActiveSheet/SelectSheet/AddSheet/RemoveSheet/
        RenameSheet and New/Open/Save/SaveAs/GetCurrentPath/IsModified.

  40.11 Document globals
        ListGlobals/GetGlobal/SetGlobal(name,type,value)/RemoveGlobal.

Run directly:

    python3 tests/test_dbus_document.py

Set $PIPNODE to override the path to the binary; defaults to the in-tree
build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import uuid

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402


ROOT    = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIPNODE = os.environ.get("PIPNODE", os.path.join(ROOT, "src", "pipnode-editor"))

BUS_NAME    = f"org.pipas.pipnode.t{os.getpid()}_{uuid.uuid4().hex[:8]}"
OBJECT_PATH = "/" + BUS_NAME.replace(".", "/")
WS_IFACE    = "org.pipas.pipnode.Worksheet"
ED_IFACE    = "org.pipas.pipnode.Editor"

ERR = "org.pipas.pipnode.Worksheet.Error."


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------

def fail(msg: str) -> None:
    print(f"FAIL: {msg}", file=sys.stderr)
    sys.exit(1)


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


def call(bus, iface: str, method: str, params, reply_type: str | None):
    return bus.call_sync(
        BUS_NAME, OBJECT_PATH, iface, method,
        params,
        GLib.VariantType.new(reply_type) if reply_type else None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def expect(bus, iface: str, method: str, params, reply_type: str | None,
           want: str) -> None:
    try:
        call(bus, iface, method, params, reply_type)
    except GLib.Error as e:
        got = Gio.DBusError.get_remote_error(e)
        if got != want:
            fail(f"{method}: expected error {want!r}, got {got!r} "
                 f"(message: {e.message!r})")
        return
    fail(f"{method}: expected error {want!r} but the call succeeded")


def ws(bus, method, params=None, reply="(s)"):
    return call(bus, WS_IFACE, method, params, reply)


def ed(bus, method, params=None, reply=None):
    return call(bus, ED_IFACE, method, params, reply)


def add_node(bus, type_name: str, x: float, y: float) -> str:
    return ws(bus, "AddNodeReturningUuid",
              GLib.Variant("(sdd)", (type_name, x, y)), "(s)").unpack()[0]


def node_count(bus) -> int:
    return ws(bus, "GetNodeCount", None, "(u)").unpack()[0]


def sheets(bus) -> list[str]:
    return ed(bus, "ListSheets", None, "(as)").unpack()[0]


def active_sheet(bus) -> str:
    return ed(bus, "GetActiveSheet", None, "(s)").unpack()[0]


# ----------------------------------------------------------------------
# Test
# ----------------------------------------------------------------------

def run_test() -> None:
    if not os.path.isfile(PIPNODE) or not os.access(PIPNODE, os.X_OK):
        fail(f"pipnode binary not found or not executable: {PIPNODE}")

    if not os.environ.get("DISPLAY") and not os.environ.get("WAYLAND_DISPLAY"):
        fail("no DISPLAY/WAYLAND_DISPLAY in the environment")

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)

    proc = subprocess.Popen(
        [PIPNODE, f"--dbus-name={BUS_NAME}"],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    tmpdir = tempfile.mkdtemp(prefix="pn-doc-test-")
    try:
        if not wait_for_bus_name(bus, BUS_NAME, timeout=15.0):
            fail("pipnode never registered its DBus name")

        ed(bus, "New")  # clean slate, no current path

        # --- 1. JSON round-trip (40.9) -------------------------------
        a = add_node(bus, "PnKnob", 100.0, 100.0)
        b = add_node(bus, "PnDebug", 360.0, 100.0)
        ws(bus, "Connect", GLib.Variant("(ssi)", (a, b, 0)), "(s)")
        if node_count(bus) != 2:
            fail(f"expected 2 nodes after building, got {node_count(bus)}")

        doc = ed(bus, "GetDocumentJson", None, "(s)").unpack()[0]
        parsed = json.loads(doc)
        if parsed.get("format") != "pipnode":
            fail(f"GetDocumentJson format tag = {parsed.get('format')!r}, "
                 "want 'pipnode'")
        if len(parsed.get("nodes", [])) != 2:
            fail("GetDocumentJson did not carry both nodes")
        if len(parsed.get("connections", [])) != 1:
            fail("GetDocumentJson did not carry the connection")

        # Sheet-scoped read: the active sheet carries the same two nodes.
        sheet_json = ed(bus, "GetWorksheetJson", None, "(s)").unpack()[0]
        sparsed = json.loads(sheet_json)
        if len(sparsed.get("nodes", [])) != 2:
            fail("GetWorksheetJson did not carry the active sheet's nodes")

        # ValidateJson: the document we just read validates; garbage does not.
        ok, _ = ed(bus, "ValidateJson",
                   GLib.Variant("(s)", (doc,)), "(bs)").unpack()
        if not ok:
            fail("ValidateJson rejected a document GetDocumentJson produced")
        bad_ok, bad_msg = ed(bus, "ValidateJson",
                             GLib.Variant("(s)", ("{ not json",)),
                             "(bs)").unpack()
        if bad_ok:
            fail("ValidateJson accepted malformed JSON")
        if not bad_msg:
            fail("ValidateJson reported no error message for bad JSON")
        # Validation must not have touched the live document.
        if node_count(bus) != 2:
            fail("ValidateJson mutated the live document")

        # SetWorksheetJson replaces the active sheet's content atomically.
        replacement = {
            "format": "pipnode-clipboard",
            "version": parsed.get("version", 1),
            "nodes": [sparsed["nodes"][0]],   # keep just one node
            "connections": [],
        }
        ed(bus, "SetWorksheetJson",
           GLib.Variant("(s)", (json.dumps(replacement),)))
        if node_count(bus) != 1:
            fail(f"SetWorksheetJson should have left 1 node, got "
                 f"{node_count(bus)}")

        # A malformed SetWorksheetJson must be rejected and change nothing.
        expect(bus, ED_IFACE, "SetWorksheetJson",
               GLib.Variant("(s)", ("{ broken",)), None, ERR + "Failed")
        if node_count(bus) != 1:
            fail("a rejected SetWorksheetJson still mutated the sheet")

        # SetDocumentJson swaps the whole document back to the 2-node graph.
        ed(bus, "SetDocumentJson", GLib.Variant("(s)", (doc,)))
        if node_count(bus) != 2:
            fail(f"SetDocumentJson should have restored 2 nodes, got "
                 f"{node_count(bus)}")
        expect(bus, ED_IFACE, "SetDocumentJson",
               GLib.Variant("(s)", ("nonsense",)), None, ERR + "Failed")

        # --- 2. Sheets + lifecycle (40.10) ---------------------------
        base = sheets(bus)
        if active_sheet(bus) not in base:
            fail("GetActiveSheet returned a sheet not in ListSheets")

        actual = ed(bus, "AddSheet",
                    GLib.Variant("(s)", ("Logic",)), "(s)").unpack()[0]
        if actual != "Logic":
            fail(f"AddSheet returned {actual!r}, want 'Logic'")
        if "Logic" not in sheets(bus):
            fail("AddSheet did not append the new sheet")
        # Duplicate name rejected.
        expect(bus, ED_IFACE, "AddSheet",
               GLib.Variant("(s)", ("Logic",)), "(s)", ERR + "BadPropertyValue")

        ed(bus, "SelectSheet", GLib.Variant("(s)", ("Logic",)))
        if active_sheet(bus) != "Logic":
            fail("SelectSheet did not change the active sheet")
        expect(bus, ED_IFACE, "SelectSheet",
               GLib.Variant("(s)", ("Nope",)), None, ERR + "SheetNotFound")

        ed(bus, "RenameSheet", GLib.Variant("(ss)", ("Logic", "Rules")))
        if "Rules" not in sheets(bus) or "Logic" in sheets(bus):
            fail("RenameSheet did not rename the sheet")
        expect(bus, ED_IFACE, "RenameSheet",
               GLib.Variant("(ss)", ("Ghost", "X")), None, ERR + "SheetNotFound")

        ed(bus, "RemoveSheet", GLib.Variant("(s)", ("Rules",)))
        if "Rules" in sheets(bus):
            fail("RemoveSheet did not drop the sheet")
        expect(bus, ED_IFACE, "RemoveSheet",
               GLib.Variant("(s)", ("Ghost",)), None, ERR + "SheetNotFound")
        # Cannot remove the last remaining sheet.
        only = active_sheet(bus)
        expect(bus, ED_IFACE, "RemoveSheet",
               GLib.Variant("(s)", (only,)), None, ERR + "BadPropertyValue")

        # IsModified / Save / SaveAs / Open / GetCurrentPath.
        if not ed(bus, "IsModified", None, "(b)").unpack()[0]:
            fail("document should read modified after all those edits")
        # No path yet -> Save fails, SaveAs adopts a path.
        expect(bus, ED_IFACE, "Save", None, None, ERR + "Failed")
        path = os.path.join(tmpdir, "doc.json")
        ed(bus, "SaveAs", GLib.Variant("(s)", (path,)))
        if not os.path.isfile(path):
            fail("SaveAs did not write the file")
        if ed(bus, "GetCurrentPath", None, "(s)").unpack()[0] != path:
            fail("GetCurrentPath did not report the SaveAs path")
        if ed(bus, "IsModified", None, "(b)").unpack()[0]:
            fail("document should read unmodified right after SaveAs")
        # Now plain Save works (path is known).
        add_node(bus, "PnLabel", 500.0, 300.0)
        ed(bus, "Save")
        if ed(bus, "IsModified", None, "(b)").unpack()[0]:
            fail("document should read unmodified right after Save")

        # New clears everything and forgets the path.
        ed(bus, "New")
        if ed(bus, "GetCurrentPath", None, "(s)").unpack()[0] != "":
            fail("New did not forget the current path")
        # Open reloads the saved file (3 nodes: knob, debug, label).
        ed(bus, "Open", GLib.Variant("(s)", (path,)))
        if node_count(bus) != 3:
            fail(f"Open should have reloaded 3 nodes, got {node_count(bus)}")
        if ed(bus, "GetCurrentPath", None, "(s)").unpack()[0] != path:
            fail("Open did not adopt the file path")
        expect(bus, ED_IFACE, "Open",
               GLib.Variant("(s)", (os.path.join(tmpdir, "missing.json"),)),
               None, ERR + "Failed")

        # --- 3. Document globals (40.11) -----------------------------
        if ed(bus, "ListGlobals", None, "(a(sss))").unpack()[0]:
            fail("a fresh document should have no globals")

        ed(bus, "SetGlobal", GLib.Variant("(sss)", ("host", "string", "pi")))
        ed(bus, "SetGlobal", GLib.Variant("(sss)", ("port", "integer", "1883")))
        ed(bus, "SetGlobal", GLib.Variant("(sss)", ("gain", "double", "1.5")))
        ed(bus, "SetGlobal", GLib.Variant("(sss)", ("on", "boolean", "true")))

        glist = {g[0]: (g[1], g[2])
                 for g in ed(bus, "ListGlobals", None, "(a(sss))").unpack()[0]}
        if glist.get("host") != ("string", "pi"):
            fail(f"global 'host' = {glist.get('host')!r}, want ('string','pi')")
        if glist.get("port") != ("integer", "1883"):
            fail(f"global 'port' = {glist.get('port')!r}")
        if glist.get("gain") != ("double", "1.5"):
            fail(f"global 'gain' = {glist.get('gain')!r}")
        if glist.get("on") != ("boolean", "true"):
            fail(f"global 'on' = {glist.get('on')!r}")

        gtype, gval = ed(bus, "GetGlobal",
                         GLib.Variant("(s)", ("port",)), "(ss)").unpack()
        if (gtype, gval) != ("integer", "1883"):
            fail(f"GetGlobal('port') = {(gtype, gval)!r}")

        # Overwrite changes the value (and type) in place.
        ed(bus, "SetGlobal", GLib.Variant("(sss)", ("port", "integer", "8883")))
        _, gval = ed(bus, "GetGlobal",
                     GLib.Variant("(s)", ("port",)), "(ss)").unpack()
        if gval != "8883":
            fail(f"SetGlobal overwrite left value {gval!r}, want '8883'")

        # Error contract: unknown name, bad type nick, unparseable number.
        expect(bus, ED_IFACE, "GetGlobal",
               GLib.Variant("(s)", ("nope",)), "(ss)", ERR + "GlobalNotFound")
        expect(bus, ED_IFACE, "RemoveGlobal",
               GLib.Variant("(s)", ("nope",)), None, ERR + "GlobalNotFound")
        expect(bus, ED_IFACE, "SetGlobal",
               GLib.Variant("(sss)", ("x", "frobnicate", "1")), None,
               ERR + "BadPropertyValue")
        expect(bus, ED_IFACE, "SetGlobal",
               GLib.Variant("(sss)", ("x", "integer", "not-a-number")), None,
               ERR + "BadPropertyValue")

        ed(bus, "RemoveGlobal", GLib.Variant("(s)", ("host",)))
        if "host" in {g[0] for g in
                      ed(bus, "ListGlobals", None, "(a(sss))").unpack()[0]}:
            fail("RemoveGlobal did not drop the global")

        # Globals round-trip through GetDocumentJson/SetDocumentJson.
        doc2 = ed(bus, "GetDocumentJson", None, "(s)").unpack()[0]
        if '"port"' not in doc2 or "8883" not in doc2:
            fail("globals did not survive into GetDocumentJson")

        print("OK: Phase D document operations "
              "(JSON round-trip, sheets/lifecycle, globals) "
              "behave as specified")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        try:
            for f in os.listdir(tmpdir):
                os.unlink(os.path.join(tmpdir, f))
            os.rmdir(tmpdir)
        except OSError:
            pass


if __name__ == "__main__":
    run_test()
