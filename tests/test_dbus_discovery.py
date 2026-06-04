#!/usr/bin/env python3
"""Functional test: node-type catalog + property schema over D-Bus (TODO #40.7/40.8).

Phase C of the automation interface is *discovery* — letting an agent learn
what it can build and configure without reading the C source.  Three methods
land on org.pipas.pipnode.Worksheet:

  ListNodeTypes()        -> a(sssbbs) {type_name,class_name,category,
                                       has_input,has_output,plugin_name}
  GetNodeTypeInfo(type)  -> (ssbbssss) {class_name,category,has_input,
                                        has_output,plugin_name,icon,color,
                                        help_page}
  ListNodeProperties(uuid) -> a(sssssb) {name,value_type,current,default,
                                         constraints,writable}

This test drives them through a freshly launched editor and asserts:

  1. CATALOG — ListNodeTypes enumerates the install palette; a known core
     type (PnDial) is present with the expected class/category and
     has_input/has_output, and core types carry the "Internal" plugin_name.

  2. TYPE INFO — GetNodeTypeInfo(PnDial) returns the same identity plus a
     non-empty icon, a colour string, and the per-class help-page basename
     "PnDial.html"; an unknown type raises UnknownNodeType.

  3. PROPERTY SCHEMA — ListNodeProperties on a live PnDial reports its
     properties; 'label' is a writable gchararray, a numeric bound carries
     a "min..max" constraint, the current value reflects a prior
     SetNodeProperty, and a bad UUID raises NodeNotFound.

Run directly:

    python3 tests/test_dbus_discovery.py

Set $PIPNODE to override the path to the binary; defaults to the in-tree
build at ``src/pipnode-editor``.
"""

from __future__ import annotations

import os
import subprocess
import sys
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

ERR   = "org.pipas.pipnode.Worksheet.Error."
BOGUS = "00000000-0000-0000-0000-000000000000"


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


def call(bus, method: str, params, reply_type: str | None):
    return bus.call_sync(
        BUS_NAME, OBJECT_PATH, WS_IFACE, method,
        params,
        GLib.VariantType.new(reply_type) if reply_type else None,
        Gio.DBusCallFlags.NONE, 5000, None,
    )


def expect(bus, method: str, params, reply_type: str | None,
           want: str) -> None:
    try:
        call(bus, method, params, reply_type)
    except GLib.Error as e:
        got = Gio.DBusError.get_remote_error(e)
        if got != want:
            fail(f"{method}: expected error {want!r}, got {got!r} "
                 f"(message: {e.message!r})")
        return
    fail(f"{method}: expected error {want!r} but the call succeeded")


def add_node(bus, type_name: str, x: float, y: float) -> str:
    return call(bus, "AddNodeReturningUuid",
                GLib.Variant("(sdd)", (type_name, x, y)), "(s)").unpack()[0]


def set_prop(bus, node_uuid: str, prop: str, value: str) -> None:
    call(bus, "SetNodePropertyByUuid",
         GLib.Variant("(sss)", (node_uuid, prop, value)), "(b)")


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
    try:
        if not wait_for_bus_name(bus, BUS_NAME, timeout=15.0):
            fail("pipnode never registered its DBus name")

        call(bus, "Clear", None, None)

        # --- 1. CATALOG ----------------------------------------------
        types = call(bus, "ListNodeTypes", None, "(a(sssbbs))").unpack()[0]
        if not types:
            fail("ListNodeTypes returned an empty catalog")
        by_name = {t[0]: t for t in types}
        if "PnDial" not in by_name:
            fail("ListNodeTypes is missing the core PnDial type "
                 f"(saw {sorted(by_name)[:8]}...)")
        tn, cls, cat, hin, hout, plg = by_name["PnDial"]
        if not cls:
            fail(f"PnDial has an empty class_name in the catalog")
        if not cat:
            fail(f"PnDial has an empty category in the catalog")
        if not hin:
            fail("PnDial should have an input in the catalog")
        # Core nodes carry the factory's "Internal" sentinel; a plugin node
        # would report its plugin name here instead.
        if plg != "Internal":
            fail(f"core PnDial should carry plugin_name 'Internal', got {plg!r}")

        # --- 2. TYPE INFO --------------------------------------------
        info = call(bus, "GetNodeTypeInfo",
                    GLib.Variant("(s)", ("PnDial",)),
                    "(ssbbssss)").unpack()
        i_cls, i_cat, i_hin, i_hout, i_plg, i_icon, i_color, i_help = info
        if i_cls != cls or i_cat != cat or i_hin != hin or i_hout != hout:
            fail("GetNodeTypeInfo identity disagrees with the catalog row "
                 f"({info!r} vs {by_name['PnDial']!r})")
        if not i_icon:
            fail("GetNodeTypeInfo returned an empty icon for PnDial")
        if not i_color:
            fail("GetNodeTypeInfo returned an empty colour for PnDial")
        if i_help != "PnDial.html":
            fail(f"GetNodeTypeInfo help_page = {i_help!r}, want 'PnDial.html'")

        expect(bus, "GetNodeTypeInfo",
               GLib.Variant("(s)", ("PnNoSuchType",)), "(ssbbssss)",
               ERR + "UnknownNodeType")

        # --- 3. PROPERTY SCHEMA --------------------------------------
        dial = add_node(bus, "PnDial", 200.0, 160.0)
        set_prop(bus, dial, "label", "Boiler")
        set_prop(bus, dial, "min-value", "10")
        set_prop(bus, dial, "max-value", "90")

        props = call(bus, "ListNodeProperties",
                     GLib.Variant("(s)", (dial,)),
                     "(a(sssssb))").unpack()[0]
        by_prop = {p[0]: p for p in props}

        if "label" not in by_prop:
            fail(f"ListNodeProperties is missing 'label' "
                 f"(saw {sorted(by_prop)})")
        name, vtype, cur, dflt, con, wr = by_prop["label"]
        if vtype != "gchararray":
            fail(f"'label' value_type = {vtype!r}, want 'gchararray'")
        if not wr:
            fail("'label' should be writable")
        if cur != "Boiler":
            fail(f"'label' current = {cur!r}, want 'Boiler' "
                 "(set via SetNodeProperty)")

        # A numeric property must surface a min..max constraint.
        if "min-value" not in by_prop:
            fail("ListNodeProperties is missing 'min-value'")
        _, mv_type, mv_cur, _, mv_con, _ = by_prop["min-value"]
        if ".." not in mv_con:
            fail(f"'min-value' constraints = {mv_con!r}, want a 'min..max' range")
        if mv_cur != "10":
            fail(f"'min-value' current = {mv_cur!r}, want '10'")

        expect(bus, "ListNodeProperties",
               GLib.Variant("(s)", (BOGUS,)), "(a(sssssb))",
               ERR + "NodeNotFound")

        print("PASS: ListNodeTypes enumerates the install palette (PnDial "
              "present, core plugin_name 'Internal'); GetNodeTypeInfo adds icon / "
              "colour / 'PnDial.html' help page and raises UnknownNodeType for "
              "a bogus type; ListNodeProperties reports a live node's writable "
              "'label' (gchararray) with its SetNodeProperty value and a "
              "min..max numeric constraint, and raises NodeNotFound for a bad "
              "UUID.")
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()


if __name__ == "__main__":
    run_test()
