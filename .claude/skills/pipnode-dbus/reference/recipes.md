# Pipnode D-Bus — copy-paste recipes

Two ways to drive a running editor: raw `gdbus` from the shell, or the
reference Python client `tests/pndbus.py` (PyGObject). Prefer the Python client
for anything multi-step — it handles UUIDs, error wrapping, and signals for you.

Throughout: `NAME` is the instance's bus name, `OBJ` its object path (the name
with dots→slashes and a leading slash).

## Find a target / launch one

```sh
# What pipnode names are on the session bus right now?
gdbus call --session --dest org.freedesktop.DBus \
  --object-path /org/freedesktop/DBus \
  --method org.freedesktop.DBus.ListNames \
  | tr ',' '\n' | grep -i pipnode
```

Bare `org.pipas.pipnode` (no suffix) = the **panel engine**, not an authoring
instance — skip it. An authoring instance is launched explicitly:

```sh
pipnode-editor --dbus-name=org.pipas.pipnode.work    # → /org/pipas/pipnode/work
```

```sh
# Version handshake (works before any document exists)
gdbus call --session --dest org.pipas.pipnode.work \
  --object-path /org/pipas/pipnode/work \
  --method org.pipas.pipnode.Editor.GetApiVersion
```

## gdbus — build a tiny flow and read it back

```sh
DEST=org.pipas.pipnode.work
OBJ=/org/pipas/pipnode/work

# Fresh document
gdbus call --session --dest $DEST --object-path $OBJ \
  --method org.pipas.pipnode.Editor.New

# Add a Knob, capture its UUID
UUID=$(gdbus call --session --dest $DEST --object-path $OBJ \
  --method org.pipas.pipnode.Worksheet.AddNodeReturningUuid \
  "PnKnob" 100.0 100.0)
echo "added $UUID"

# Discover the palette (includes loaded plugins)
gdbus call --session --dest $DEST --object-path $OBJ \
  --method org.pipas.pipnode.Worksheet.ListNodeTypes

# Set a property
gdbus call --session --dest $DEST --object-path $OBJ \
  --method org.pipas.pipnode.Worksheet.SetNodePropertyByUuid \
  "$UUID" "label" "Volume"

# Save (current path; SaveAs to set one)
gdbus call --session --dest $DEST --object-path $OBJ \
  --method org.pipas.pipnode.Editor.SaveAs "/tmp/demo.pnflow"
```

Note `gdbus call` prints the reply as a wrapped tuple, e.g.
`('a1b2c3-…',)` — strip the tuple/quotes when capturing a UUID for reuse.

## Python — the reference client (preferred)

```python
from pndbus import PipnodeEditor      # tests/pndbus.py

# A: own throwaway instance (unique --dbus-name, torn down on exit)
with PipnodeEditor.launch() as ed:
    ed.new_document()
    ed.subscribe()                              # start collecting live signals

    knob  = ed.add_node("PnKnob",  100, 100)
    topic = ed.add_node("PnTopic", 360, 100)
    debug = ed.add_node("PnDebug", 620, 100)

    ed.set_node_properties(topic, {"topic": "demo/volume"})
    wire = ed.connect(topic, debug)             # topic -> debug, input 0

    # exercise the flow without a Debug wire
    ed.inject_message(topic, {"value": 0.5})
    print(ed.get_last_output_message(topic))    # -> emitted envelope dict

    ed.pump()                                   # drain queued signals
    assert ed.seen("NodeAdded")
    assert ed.seen("WireAdded")

    ed.save_as("/tmp/demo.pnflow")
```

```python
# B: attach to an ALREADY-RUNNING named instance (e.g. the user's editor)
ed = PipnodeEditor("org.pipas.pipnode.work")
ed.connect_bus()
ed.subscribe()
for n in ed.list_nodes():
    print(n["uuid"], n["class_name"], n["name"])
```

The client has a typed wrapper for essentially every documented method —
`add_node`, `move_node`, `rename_node`, `delete_node`, `set_node_property`,
`set_node_properties`, `list_node_properties`, `connect`, `disconnect`,
`list_wires`, `get_node_wires`, `list_node_types`, `get_node_type_info`,
`select_nodes`, `get_selection`, `focus_node`, `center_on`, `fit_to_content`,
`inject_message`, `get_last_output_message`, `list_sheets`, `add_sheet`,
`select_sheet`, `list_globals`, `set_global`, `get_document_json`,
`set_worksheet_json`, `validate_json`, `save`, `save_as`, `current_path`,
`is_modified`, … (named after the method). **Prefer these.**

The raw layer is `ws(method, params, reply_type)` / `ed(method, params,
reply_type)` — `params` is a `GLib.Variant`, the return is a `Variant` you
`.unpack()` (pass `reply_type` like `"(s)"` to get one back; omit for void).
Remote errors raise `PipnodeError` with `.short` = the bare code:

```python
from gi.repository import GLib
ed.ws("FitToContent")                                   # void Worksheet call
ed.ws("CenterOn", GLib.Variant("(s)", (uuid,)))         # one-arg, void
try:
    ed.connect("uuidA", "uuidA")                        # self-loop
except PipnodeError as e:
    assert e.short == "IllegalConnection"
```

## Common editing recipes

```python
# Inspect a node's full property schema before editing (list of dicts)
for p in ed.list_node_properties(uuid):
    print(p["name"], p["value_type"], "=", p["current"],
          "constraints:", p["constraints"], "w" if p["writable"] else "ro")

# Atomic multi-property edit (all-or-nothing)
ed.set_node_properties(uuid, {"topic": "home/temp", "qos": "1"})

# Whole-sheet round-trip — cheaper than many per-node calls
doc = ed.get_worksheet_json()           # canonical JSON string
# ... transform doc ...
ok, err = ed.validate_json(doc)         # check before applying
if ok:
    ed.set_worksheet_json(doc)          # parse-checked again before it applies

# Sheets
ed.add_sheet("Logic")                   # -> actual name
ed.select_sheet("Logic")

# Document globals (${name} variables; types: boolean|integer|double|string)
ed.set_global("threshold", "double", "21.5")

# Show the user what you changed
ed.select_nodes([knob, topic])          # -> match count
ed.focus_node(topic)
ed.fit_to_content()

# Persist — explicitly
if ed.current_path():
    ed.save()
else:
    ed.save_as("/path/to/file.pnflow")
```

## Reacting to live changes

```python
ed.subscribe()
# ... user or another client edits the graph ...
ed.pump(timeout=1.0)
for evt in ed.events:                   # each carries (signal_name, args)
    print(evt)
# or test for a specific one:
if ed.seen("NodePropertyChanged", lambda a: a[0] == uuid):
    ...
```

## One-line smoke test of the client itself

```sh
python3 tests/pndbus.py
```

## Gotchas recap

- Address nodes by **UUID**, never index. Wire UUIDs are session-only.
- `SetNodeProperties` is atomic; `SetWorksheetJson`/`SetDocumentJson` are
  parse-checked before applying; `ValidateJson` never mutates.
- Booleans on `data.value`: `0.0`/`1.0`, "on" = `value > 0.5`.
- `.Worksheet` calls need an active flow sheet (panel-editor tab → `NoActiveSheet`).
- Saving is explicit. Non-read calls light the "⚡ automation" badge and show in
  undo / the modified flag.
