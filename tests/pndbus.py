#!/usr/bin/env python3
"""Thin Python client for the pipnode editor's D-Bus automation interface.

This is the reference client for the org.pipas.pipnode.Worksheet and
org.pipas.pipnode.Editor interfaces described in ``DBUS-API.md`` (TODO
#40.15).  It is two things at once:

  * the helper the functional tests (tests/test_dbus_*.py) drive the editor
    through — launch an instance under a private ``--dbus-name``, author a
    graph, observe live signals, and tear it down; and

  * the worked example an automation client (e.g. Claude Code) can copy: it
    speaks nothing but the documented introspection contract, so reading it
    top-to-bottom shows exactly which method names, signatures, and error
    names the interface exposes.

It deliberately has no dependencies beyond PyGObject (``gi``), which the
test suite already requires.

Typical use::

    from pndbus import PipnodeEditor

    with PipnodeEditor.launch() as ed:          # own throwaway instance
        ed.new_document()
        a = ed.add_node("PnKnob",  100, 100)
        b = ed.add_node("PnDebug", 360, 100)
        wire = ed.connect(a, b)                  # a -> b, input 0
        ed.set_node_properties(a, {"label": "Volume"})
        ed.inject_message(a, {"value": 0.5})
        print(ed.get_last_output_message(a))

To talk to an *already running* editor instead of launching one, construct
the client directly with the bus name it registered under::

    ed = PipnodeEditor("org.pipas.pipnode")     # the default name
    ed.connect_bus()

See ``DBUS-API.md`` for the full method/signal/error catalogue.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import time
import uuid as _uuid

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402


# Interface names and the object path are derived from the bus name exactly
# as src/pn-application.c does: the object path is the bus name with dots
# turned into slashes, prefixed with a slash.
WORKSHEET_IFACE = "org.pipas.pipnode.Worksheet"
EDITOR_IFACE    = "org.pipas.pipnode.Editor"

# Stable error-name prefixes (see DBUS-API.md §Errors).  Both interfaces
# raise from the same org.pipas.pipnode.Worksheet.Error.* domain.
ERROR_PREFIX = "org.pipas.pipnode.Worksheet.Error."


def _default_binary() -> str:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.environ.get("PIPNODE", os.path.join(root, "src", "pipnode-editor"))


def object_path_for(bus_name: str) -> str:
    """The object path the editor serves @bus_name on."""
    return "/" + bus_name.replace(".", "/")


class PipnodeError(Exception):
    """A remote D-Bus error from the automation interface.

    ``name`` is the stable wire error name (e.g.
    ``org.pipas.pipnode.Worksheet.Error.NodeNotFound``); ``short`` is the
    trailing component (``NodeNotFound``) for convenient comparison.
    """

    def __init__(self, name: str, message: str):
        super().__init__(f"{name}: {message}")
        self.name = name
        self.short = name[len(ERROR_PREFIX):] if name.startswith(ERROR_PREFIX) \
            else name
        self.remote_message = message


class PipnodeEditor:
    """A connection to one pipnode editor instance over D-Bus."""

    # ---- construction / lifecycle ------------------------------------

    def __init__(self, bus_name: str = "org.pipas.pipnode",
                 *, bus: Gio.DBusConnection | None = None):
        self.bus_name = bus_name
        self.object_path = object_path_for(bus_name)
        self._bus = bus
        self._proc: subprocess.Popen | None = None
        self._sub_ids: list[int] = []
        #: collected signals as ``(signal_name, unpacked_args_tuple)``.
        self.events: list[tuple[str, tuple]] = []

    @classmethod
    def launch(cls, *, binary: str | None = None,
               extra_args: list[str] | None = None,
               timeout: float = 15.0) -> "PipnodeEditor":
        """Spawn a private editor instance under a unique --dbus-name.

        Returns a connected client; use it as a context manager (or call
        :meth:`close`) so the process is terminated when you are done.
        """
        binary = binary or _default_binary()
        if not (os.path.isfile(binary) and os.access(binary, os.X_OK)):
            raise RuntimeError(f"pipnode binary not found / not executable: "
                               f"{binary}")
        if not os.environ.get("DISPLAY") and \
           not os.environ.get("WAYLAND_DISPLAY"):
            raise RuntimeError("no DISPLAY / WAYLAND_DISPLAY in the "
                               "environment (the editor needs a display)")

        bus_name = f"org.pipas.pipnode.c{os.getpid()}_{_uuid.uuid4().hex[:8]}"
        argv = [binary, f"--dbus-name={bus_name}", *(extra_args or [])]
        proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)

        client = cls(bus_name)
        client._proc = proc
        try:
            client.connect_bus()
            if not client.wait_for_name(timeout):
                raise RuntimeError("editor never registered its D-Bus name")
        except Exception:
            client.close()
            raise
        return client

    def connect_bus(self) -> "PipnodeEditor":
        """Open the session-bus connection if not already supplied."""
        if self._bus is None:
            self._bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        return self

    def wait_for_name(self, timeout: float = 15.0) -> bool:
        """Block until the editor owns its bus name, or @timeout elapses."""
        assert self._bus is not None
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                self._bus.call_sync(
                    "org.freedesktop.DBus", "/org/freedesktop/DBus",
                    "org.freedesktop.DBus", "GetNameOwner",
                    GLib.Variant("(s)", (self.bus_name,)),
                    GLib.VariantType.new("(s)"),
                    Gio.DBusCallFlags.NONE, 1000, None)
                return True
            except GLib.Error:
                if self._proc is not None and self._proc.poll() is not None:
                    return False
                time.sleep(0.1)
        return False

    def close(self) -> None:
        """Drop subscriptions and terminate a launched instance."""
        if self._bus is not None:
            for sid in self._sub_ids:
                self._bus.signal_unsubscribe(sid)
        self._sub_ids.clear()
        if self._proc is not None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self._proc.kill()
            self._proc = None

    def __enter__(self) -> "PipnodeEditor":
        return self

    def __exit__(self, *_exc) -> None:
        self.close()

    # ---- raw call layer ----------------------------------------------

    def call(self, iface: str, method: str,
             params: GLib.Variant | None = None,
             reply_type: str | None = None,
             timeout_ms: int = 5000):
        """Invoke @method on @iface and return the reply Variant (or None).

        Remote errors are re-raised as :class:`PipnodeError` carrying the
        stable error name.
        """
        assert self._bus is not None, "call connect_bus() / launch() first"
        try:
            return self._bus.call_sync(
                self.bus_name, self.object_path, iface, method, params,
                GLib.VariantType.new(reply_type) if reply_type else None,
                Gio.DBusCallFlags.NONE, timeout_ms, None)
        except GLib.Error as e:
            name = Gio.DBusError.get_remote_error(e) or ""
            if name:
                Gio.DBusError.strip_remote_error(e)
                raise PipnodeError(name, e.message) from None
            raise

    def ws(self, method, params=None, reply=None, **kw):
        """Call a method on org.pipas.pipnode.Worksheet."""
        return self.call(WORKSHEET_IFACE, method, params, reply, **kw)

    def ed(self, method, params=None, reply=None, **kw):
        """Call a method on org.pipas.pipnode.Editor."""
        return self.call(EDITOR_IFACE, method, params, reply, **kw)

    # ---- signals ------------------------------------------------------

    def subscribe(self, handler=None) -> None:
        """Subscribe to every Worksheet + Editor signal on this object.

        With no @handler, signals are appended to :attr:`events` as
        ``(name, args)`` tuples (use :meth:`pump` then inspect / :meth:`seen`).
        With a @handler, it is called as ``handler(name, args)``.
        """
        assert self._bus is not None

        def _on(_c, _s, _p, _iface, name, params):
            args = params.unpack()
            if handler is not None:
                handler(name, args)
            else:
                self.events.append((name, args))

        for iface in (WORKSHEET_IFACE, EDITOR_IFACE):
            self._sub_ids.append(self._bus.signal_subscribe(
                self.bus_name, iface, None, self.object_path, None,
                Gio.DBusSignalFlags.NONE, _on))

    def pump(self, timeout: float = 0.6) -> None:
        """Iterate the default main context so queued signals are delivered."""
        ctx = GLib.MainContext.default()
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while ctx.pending():
                ctx.iteration(False)
            time.sleep(0.02)

    def seen(self, name: str, pred=None) -> bool:
        """Whether a collected signal @name (matching @pred) has arrived."""
        return any(n == name and (pred is None or pred(a))
                   for n, a in self.events)

    def clear_events(self) -> None:
        self.events.clear()

    # ---- convenience: version / document (Editor) --------------------

    def get_api_version(self) -> tuple[int, int]:
        return tuple(self.ed("GetApiVersion", None, "(uu)").unpack())

    def new_document(self) -> None:
        self.ed("New")

    def open(self, path: str) -> None:
        self.ed("Open", GLib.Variant("(s)", (path,)))

    def save(self) -> None:
        self.ed("Save")

    def save_as(self, path: str) -> None:
        self.ed("SaveAs", GLib.Variant("(s)", (path,)))

    def current_path(self) -> str:
        return self.ed("GetCurrentPath", None, "(s)").unpack()[0]

    def is_modified(self) -> bool:
        return self.ed("IsModified", None, "(b)").unpack()[0]

    def get_document_json(self) -> str:
        return self.ed("GetDocumentJson", None, "(s)").unpack()[0]

    def set_document_json(self, text: str) -> None:
        self.ed("SetDocumentJson", GLib.Variant("(s)", (text,)))

    def get_worksheet_json(self) -> str:
        return self.ed("GetWorksheetJson", None, "(s)").unpack()[0]

    def set_worksheet_json(self, text: str) -> None:
        self.ed("SetWorksheetJson", GLib.Variant("(s)", (text,)))

    def validate_json(self, text: str) -> tuple[bool, str]:
        return tuple(self.ed("ValidateJson", GLib.Variant("(s)", (text,)),
                             "(bs)").unpack())

    # ---- convenience: sheets (Editor) --------------------------------

    def list_sheets(self) -> list[str]:
        return list(self.ed("ListSheets", None, "(as)").unpack()[0])

    def active_sheet(self) -> str:
        return self.ed("GetActiveSheet", None, "(s)").unpack()[0]

    def add_sheet(self, name: str) -> str:
        return self.ed("AddSheet", GLib.Variant("(s)", (name,)),
                       "(s)").unpack()[0]

    def remove_sheet(self, name: str) -> None:
        self.ed("RemoveSheet", GLib.Variant("(s)", (name,)))

    def rename_sheet(self, frm: str, to: str) -> None:
        self.ed("RenameSheet", GLib.Variant("(ss)", (frm, to)))

    def select_sheet(self, name: str) -> None:
        self.ed("SelectSheet", GLib.Variant("(s)", (name,)))

    # ---- convenience: globals (Editor) -------------------------------

    def list_globals(self) -> list[tuple[str, str, str]]:
        return [tuple(g) for g in
                self.ed("ListGlobals", None, "(a(sss))").unpack()[0]]

    def get_global(self, name: str) -> tuple[str, str]:
        return tuple(self.ed("GetGlobal", GLib.Variant("(s)", (name,)),
                             "(ss)").unpack())

    def set_global(self, name: str, type_nick: str, value: str) -> None:
        self.ed("SetGlobal",
                GLib.Variant("(sss)", (name, type_nick, value)))

    def remove_global(self, name: str) -> None:
        self.ed("RemoveGlobal", GLib.Variant("(s)", (name,)))

    # ---- convenience: nodes (Worksheet) ------------------------------

    def add_node(self, type_name: str, x: float, y: float) -> str:
        """Add a node and return its stable UUID."""
        return self.ws("AddNodeReturningUuid",
                       GLib.Variant("(sdd)", (type_name, x, y)),
                       "(s)").unpack()[0]

    def delete_node(self, uuid: str) -> None:
        self.ws("DeleteNode", GLib.Variant("(s)", (uuid,)))

    def move_node(self, uuid: str, x: float, y: float) -> None:
        self.ws("MoveNode", GLib.Variant("(sdd)", (uuid, x, y)))

    def rename_node(self, uuid: str, name: str) -> None:
        self.ws("RenameNode", GLib.Variant("(ss)", (uuid, name)))

    def set_node_input_count(self, uuid: str, count: int) -> None:
        self.ws("SetNodeInputCount", GLib.Variant("(si)", (uuid, count)))

    def set_node_input_name(self, uuid: str, index: int, name: str) -> None:
        self.ws("SetNodeInputName",
                GLib.Variant("(sis)", (uuid, index, name)))

    def get_node(self, uuid: str) -> dict:
        cn, name, icon, x, y, hi, ho = self.ws(
            "GetNodeByUuid", GLib.Variant("(s)", (uuid,)),
            "(sssddbb)").unpack()
        return {"class_name": cn, "name": name, "icon": icon,
                "x": x, "y": y, "has_input": hi, "has_output": ho}

    def list_nodes(self) -> list[dict]:
        rows = self.ws("GetNodesWithUuids", None, "(a(sssddbbs))").unpack()[0]
        return [{"class_name": cn, "name": nm, "icon": ic, "x": x, "y": y,
                 "has_input": hi, "has_output": ho, "uuid": uu}
                for (cn, nm, ic, x, y, hi, ho, uu) in rows]

    def node_count(self) -> int:
        return self.ws("GetNodeCount", None, "(u)").unpack()[0]

    def get_node_geometry(self, uuid: str) -> tuple[float, float, float, float]:
        return tuple(self.ws("GetNodeGeometry", GLib.Variant("(s)", (uuid,)),
                             "(dddd)").unpack())

    # ---- convenience: properties (Worksheet) -------------------------

    def get_node_property(self, uuid: str, prop: str) -> str:
        return self.ws("GetNodePropertyByUuid",
                       GLib.Variant("(ss)", (uuid, prop)), "(s)").unpack()[0]

    def set_node_property(self, uuid: str, prop: str, value: str) -> bool:
        return self.ws("SetNodePropertyByUuid",
                       GLib.Variant("(sss)", (uuid, prop, value)),
                       "(b)").unpack()[0]

    def set_node_properties(self, uuid: str, props: dict) -> None:
        """Atomically set several properties (all-or-nothing)."""
        self.ws("SetNodeProperties",
                GLib.Variant("(sa{ss})",
                             (uuid, {k: str(v) for k, v in props.items()})))

    def list_node_properties(self, uuid: str) -> list[dict]:
        rows = self.ws("ListNodeProperties", GLib.Variant("(s)", (uuid,)),
                       "(a(sssssb))").unpack()[0]
        return [{"name": n, "value_type": vt, "current": cur, "default": dflt,
                 "constraints": cons, "writable": w}
                for (n, vt, cur, dflt, cons, w) in rows]

    # ---- convenience: wiring (Worksheet) -----------------------------

    def connect(self, source: str, target: str, target_input: int = 0) -> str:
        """Wire source -> target.target_input and return the wire UUID."""
        return self.ws("Connect",
                       GLib.Variant("(ssi)", (source, target, target_input)),
                       "(s)").unpack()[0]

    def disconnect(self, wire_uuid: str) -> None:
        self.ws("Disconnect", GLib.Variant("(s)", (wire_uuid,)))

    def list_wires(self) -> list[dict]:
        rows = self.ws("ListWires", None, "(a(ssis))").unpack()[0]
        return [{"source": s, "target": t, "target_input": i, "uuid": u}
                for (s, t, i, u) in rows]

    def get_node_wires(self, uuid: str) -> list[dict]:
        rows = self.ws("GetNodeWires", GLib.Variant("(s)", (uuid,)),
                       "(a(ssis))").unpack()[0]
        return [{"source": s, "target": t, "target_input": i, "uuid": u}
                for (s, t, i, u) in rows]

    # ---- convenience: discovery (Worksheet) --------------------------

    def list_node_types(self) -> list[dict]:
        rows = self.ws("ListNodeTypes", None, "(a(sssbbs))").unpack()[0]
        return [{"type_name": tn, "class_name": cn, "category": cat,
                 "has_input": hi, "has_output": ho, "plugin_name": pn}
                for (tn, cn, cat, hi, ho, pn) in rows]

    def get_node_type_info(self, type_name: str) -> dict:
        cn, cat, hi, ho, pn, icon, color, help_page = self.ws(
            "GetNodeTypeInfo", GLib.Variant("(s)", (type_name,)),
            "(ssbbssss)").unpack()
        return {"class_name": cn, "category": cat, "has_input": hi,
                "has_output": ho, "plugin_name": pn, "icon": icon,
                "color": color, "help_page": help_page}

    # ---- convenience: selection / view (Worksheet) -------------------

    def select_nodes(self, uuids: list[str]) -> int:
        return self.ws("SelectNodes", GLib.Variant("(as)", (uuids,)),
                       "(u)").unpack()[0]

    def get_selection(self) -> list[str]:
        return list(self.ws("GetSelection", None, "(as)").unpack()[0])

    def clear_selection(self) -> None:
        self.ws("ClearSelection")

    def focus_node(self, uuid: str) -> None:
        self.ws("FocusNode", GLib.Variant("(s)", (uuid,)))

    def center_on(self, uuid: str) -> None:
        self.ws("CenterOn", GLib.Variant("(s)", (uuid,)))

    def fit_to_content(self) -> None:
        self.ws("FitToContent")

    # ---- convenience: message inject / readback (Worksheet) ----------

    def inject_message(self, uuid: str, message: dict) -> None:
        """Deliver @message (a JSON-able dict) to the node's input 0."""
        self.ws("InjectMessage",
                GLib.Variant("(ss)", (uuid, json.dumps(message))))

    def inject_message_on_input(self, uuid: str, inp: int,
                                message: dict) -> None:
        self.ws("InjectMessageOnInput",
                GLib.Variant("(sis)", (uuid, inp, json.dumps(message))))

    def get_last_output_message(self, uuid: str) -> dict | None:
        raw = self.ws("GetLastOutputMessage", GLib.Variant("(s)", (uuid,)),
                      "(s)").unpack()[0]
        return json.loads(raw) if raw else None


# A tiny smoke test when run directly: launch an instance, build a 3-node
# flow, exercise the round-trip, and print a one-line summary.
def _smoke() -> int:
    with PipnodeEditor.launch() as ed:
        ed.new_document()
        major, minor = ed.get_api_version()
        a = ed.add_node("PnKnob", 100, 100)
        b = ed.add_node("PnTopic", 360, 100)
        wire = ed.connect(b, ed.add_node("PnDebug", 620, 100))
        ed.set_node_properties(b, {"topic": "demo/volume"})
        ed.inject_message(b, {"value": 12.5})
        out = ed.get_last_output_message(b)
        ok = out and out.get("data", {}).get("value") == 12.5
        print(f"pndbus smoke: API {major}.{minor}, "
              f"{ed.node_count()} nodes, wire={wire[:8]}…, "
              f"round-trip {'OK' if ok else 'FAILED'}")
        return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(_smoke())
