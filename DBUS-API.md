# Pipnode D-Bus Automation API

This document describes the D-Bus interface the pipnode **editor**
(`pipnode-editor`) exposes so that automation — Claude Code, scripts, or any
other program — can author and manipulate worksheets interactively: read the
graph, mutate nodes/wires/sheets, drive a running flow, and observe changes
live.

It is the machine-readable companion to the `pipnode-nodes` skill: the skill
carries the *semantics* of each node (what it does, which message-bag fields
it reads and writes); this interface gives an agent the *hands* to build and
edit the graph that wires those nodes together.

The single source of truth for the exact wire signatures is the
introspection XML compiled into `src/pn-application.c`. Point any D-Bus tool
at a running instance to dump it:

```sh
gdbus introspect --session \
  --dest org.pipas.pipnode \
  --object-path /org/pipas/pipnode
```

A thin, fully-worked **reference client** lives at `tests/pndbus.py`
(PyGObject). It is what the functional tests drive the editor through, and
it is the easiest thing to copy into a new automation client — see
[The reference Python client](#the-reference-python-client) below.

---

## Contents

1. [Connecting: bus name and object path](#connecting-bus-name-and-object-path)
2. [Interfaces at a glance](#interfaces-at-a-glance)
3. [Versioning and feature detection](#versioning-and-feature-detection)
4. [The error contract](#the-error-contract)
5. [`org.pipas.pipnode.Worksheet` — the active sheet](#orgpipaspipnodeworksheet--the-active-sheet)
6. [`org.pipas.pipnode.Editor` — the document](#orgpipaspipnodeeditor--the-document)
7. [Signals — observing changes live](#signals--observing-changes-live)
8. [Data formats](#data-formats)
9. [Worked examples](#worked-examples)
10. [The reference Python client](#the-reference-python-client)
11. [Safety and the trust boundary](#safety-and-the-trust-boundary)

---

## Connecting: bus name and object path

The editor registers on the **session bus**. By default it claims the
well-known name

```
org.pipas.pipnode
```

and serves every automation interface on the object path derived from that
name by replacing the dots with slashes and prefixing a slash:

```
/org/pipas/pipnode
```

A normal editor launch is *non-unique*: it runs as its own process and does
**not** take the shared well-known name (so opening a file never pops a
window in some other instance). To get an addressable instance you launch
the editor with an explicit name:

```sh
pipnode-editor --dbus-name=org.pipas.pipnode.work
```

The object path follows the name, so this instance is reached at
`/org/pipas/pipnode/work`. Use a private `--dbus-name` to run several
instances side by side, or to give an unattended automation run its own
throwaway editor that cannot disturb an interactive one. The reference
client's `PipnodeEditor.launch()` does exactly this — it mints a unique name
per process.

`--dbus-name` must be a valid D-Bus / GApplication application id (reverse-DNS
dotted form). An invalid name aborts startup with an error.

> The background **panel engine** (`pipnode-editor --gapplication-service`,
> used by the XFCE applet) also takes the well-known name and serves a
> *third* interface, `org.pipas.pipnode.Engine`, on the same object. That
> interface is for the panel applet, not for worksheet authoring, and is out
> of scope here.

---

## Interfaces at a glance

Two automation interfaces co-exist on the same object:

| Interface | Scope |
|-----------|-------|
| `org.pipas.pipnode.Worksheet` | The **active sheet's** graph: nodes, wires, properties, discovery, selection/view, message injection, and the live graph signals. |
| `org.pipas.pipnode.Editor`    | The **document**: the API version handshake, whole-file/active-sheet JSON, sheets, file lifecycle (New/Open/Save), document globals, and the document-level signals. |

The split mirrors how an agent works: most calls are graph edits on the
active sheet (`.Worksheet`); the document-wide operations (which sheet is
active, save/load, file globals) are on `.Editor`.

---

## Versioning and feature detection

`.Editor` carries the version handshake. Always feature-detect rather than
assuming a version:

| Member | Signature | Meaning |
|--------|-----------|---------|
| `GetApiVersion` | `() → (u major, u minor)` | The automation API version. |
| `Version` (property) | `u` (read-only) | The major version, for clients that prefer a property read. |

The version is **`1.2`** at the time of writing. The major bumps only on a
breaking change to either interface; the minor bumps on back-compatible
additions. `GetApiVersion` is the one method that works before a document
exists.

History: `1.0` = the addressed/typed/discoverable core; `1.1` added the
whole-document operations (Phase D); `1.2` added interactivity — selection,
view, message inject/readback, and the live signals (Phase E).

---

## The error contract

Every failure is a real D-Bus error, never a silent `false` or empty string.
The errors come from one stable domain; the trailing component is what you
match on:

```
org.pipas.pipnode.Worksheet.Error.<Code>
```

(Both interfaces raise from this same `Worksheet.Error` domain.)

| Code | Raised when |
|------|-------------|
| `NoActiveSheet` | A `.Worksheet` call arrived but there is no active worksheet (e.g. the panel-editor tab is selected). |
| `NoActiveWindow` | An `.Editor` call arrived but there is no editor window at all. |
| `NodeNotFound` | A node UUID does not resolve on the active sheet. |
| `WireNotFound` | A wire UUID does not resolve. |
| `UnknownNodeType` | An `AddNode`/`GetNodeTypeInfo` type name is not in the factory. |
| `UnknownProperty` | A property name does not exist on the node. |
| `BadPropertyValue` | A value could not be parsed for the property's type, or an argument was out of range (input index, sheet count, malformed message JSON, …). |
| `IllegalConnection` | A wire would be illegal: self-loop, source with no output, target with no input, input index out of range, or a duplicate of an existing wire. |
| `SheetNotFound` | A sheet name does not exist. |
| `GlobalNotFound` | A document-global name does not exist. |
| `Failed` | A generic I/O / load failure (e.g. `Open`/`Save`/`SetDocumentJson`). |

In PyGObject, read the name with
`Gio.DBusError.get_remote_error(err)`; the reference client wraps this into a
`PipnodeError` whose `.short` is the bare code (`"NodeNotFound"`).

---

## `org.pipas.pipnode.Worksheet` — the active sheet

Nodes are addressed by their **UUID** — a stable string (`pn_node_get_uuid`)
that survives save/load. *Prefer the UUID forms for everything.* The
integer-index and other test-shaped forms (below) exist only so the older
functional tests keep working; an index shifts the moment a node is deleted,
which makes it useless across a multi-step edit.

### Reading the graph

| Method | Signature |
|--------|-----------|
| `GetNodeCount` | `() → (u count)` |
| `GetWireCount` | `() → (u count)` |
| `GetNodeByUuid` | `(s uuid) → (s class_name, s name, s icon, d x, d y, b has_input, b has_output)` |
| `GetNodesWithUuids` | `() → (a(sssddbbs))` rows of `{class_name, name, icon, x, y, has_input, has_output, uuid}` |
| `GetNodeGeometry` | `(s uuid) → (d x, d y, d w, d h)` — full worksheet-space box |

### Node lifecycle

| Method | Signature | Notes |
|--------|-----------|-------|
| `AddNodeReturningUuid` | `(s type, d x, d y) → (s uuid)` | `type` is the GType name, e.g. `PnKnob`. `UnknownNodeType` if unknown. |
| `DeleteNode` | `(s uuid)` | Also drops every wire touching the node. |
| `MoveNode` | `(s uuid, d x, d y)` | |
| `RenameNode` | `(s uuid, s name)` | |
| `SetNodeInputCount` | `(s uuid, i count)` | For multi-input nodes (Calculator, etc.). `count < 1` → `BadPropertyValue`. |
| `SetNodeInputName` | `(s uuid, i index, s name)` | `index` outside `[0, count)` → `BadPropertyValue`. |

### Properties

| Method | Signature | Notes |
|--------|-----------|-------|
| `GetNodePropertyByUuid` | `(s uuid, s prop) → (s value)` | Value rendered as text (enums as their value-nick). |
| `SetNodePropertyByUuid` | `(s uuid, s prop, s value) → (b ok)` | |
| `SetNodeProperties` | `(s uuid, a{ss} props)` | **Atomic**: every pair is validated first; one bad pair (`UnknownProperty`/`BadPropertyValue`) leaves the node untouched. |
| `ListNodeProperties` | `(s uuid) → (a(sssssb))` rows of `{name, value_type, current, default, constraints, writable}` | `constraints` is `nick1\|nick2\|…` for enums, `min..max` for bounded numerics, else `""`. `writable` is true only for non-construct-only writable props. |

### Wiring (port-aware)

Wires also have a session UUID, minted on creation. It is **not** serialized
(a wire is fully described on disk by its endpoints + target input, so a
reload mints a fresh one); node UUIDs are the stable cross-save identity.

| Method | Signature | Notes |
|--------|-----------|-------|
| `Connect` | `(s source, s target, i target_input) → (s wire_uuid)` | Wires `source`'s output to `target`'s input `target_input`. Rejections all raise `IllegalConnection`: self-loop, no-output source, no-input target, input out of range, duplicate. Unknown node → `NodeNotFound`. |
| `Disconnect` | `(s wire_uuid)` | Unknown wire → `WireNotFound`. |
| `ListWires` | `() → (a(ssis))` rows of `{source_uuid, target_uuid, target_input, wire_uuid}` | A dangling endpoint reads as an empty UUID. |
| `GetNodeWires` | `(s uuid) → (a(ssis))` | Same row shape, restricted to wires touching `uuid`. |

### Discovery

The runtime, install-accurate mirror of the node palette — including any
loaded plugins.

| Method | Signature | Notes |
|--------|-----------|-------|
| `ListNodeTypes` | `() → (a(sssbbs))` rows of `{type_name, class_name, category, has_input, has_output, plugin_name}` | Core nodes report the `"Internal"` plugin sentinel. |
| `GetNodeTypeInfo` | `(s type) → (s class_name, s category, b has_input, b has_output, s plugin_name, s icon, s color, s help_page)` | `help_page` is `<type_name>.html` — the same anchor the in-app help browser resolves. `UnknownNodeType` on a miss. |
| `GetDeviceProviders` | `() → (as ids)` | The registered device-provider ids. |

### Selection and view

So an agent can *show the user* what it just built or changed.

| Method | Signature | Notes |
|--------|-----------|-------|
| `SelectNodes` | `(as uuids) → (u matched)` | Replaces the selection with the on-sheet nodes matching `uuids`; unknown/off-sheet uuids are skipped; returns the match count and emits `SelectionChanged` once. |
| `GetSelection` | `() → (as uuids)` | |
| `ClearSelection` | `()` | |
| `FocusNode` | `(s uuid)` | Recenter + focus pulse. `NodeNotFound` on a miss. |
| `CenterOn` | `(s uuid)` | Quiet recenter. |
| `FitToContent` | `()` | Zoom/scroll so the whole sheet fits. |
| `GetWorksheetScroll` | `() → (d hval, d vval, d hpage, d vpage, d hupper, d vupper)` | |
| `SetWorksheetScroll` | `(d h, d v)` | |

### Message injection and readback

Exercise a flow you just built — deliver a message to a node and read the
result — without wiring up a Debug node.

| Method | Signature | Notes |
|--------|-----------|-------|
| `InjectMessage` | `(s uuid, s json)` | Delivers a message to the node's input 0 as if a wire carried it. See [Message JSON](#message-json-envelope). Malformed/non-object JSON → `BadPropertyValue`. |
| `InjectMessageOnInput` | `(s uuid, i input, s json)` | As above, on a specific input. `input` out of range → `BadPropertyValue`. |
| `GetLastOutputMessage` | `(s uuid) → (s json)` | The last message the node emitted, as the compact envelope JSON. `""` before any emission. |

### Legacy / test-only forms

These are kept only so the existing functional tests keep working. New
clients should ignore them in favour of the UUID forms above.

`GetNode`, `GetNodes`, `GetWire`, `GetWires`, `GetNodeUuid` (index→uuid),
`GetNodeProperty`/`SetNodeProperty` (by index), `AddNode` (returns an index),
`ConnectNodes`/`ConnectNodesByUuid` (port-blind, returns a bool), `Clear`,
`LoadFromFile`/`SaveToFile`; the debug-pane and dialog-driving helpers
(`InjectDebugMessage`, `GetDebugRowCount`, `GetDebugRowFromId`,
`ClickDebugFromButton`, `GetFocusPulseUuid`, `GrabWorksheetFocus`,
`ResizeWindow`, `GetDebugPaneAllocation`, `OpenNodeDialog(ByUuid)`,
`CloseNodeDialog`, `GetDialogPageTitles`, `SelectDialogPage`,
`GetDialogEditorText`, `GetDialogEditorSensitive`, `SetDialogEditorText`).

---

## `org.pipas.pipnode.Editor` — the document

### Whole-document / active-sheet JSON

Reading or rewriting a whole sheet in one call is dramatically cheaper than
dozens of per-node round-trips, and round-trips the canonical on-disk format.

| Method | Signature | Notes |
|--------|-----------|-------|
| `GetDocumentJson` | `() → (s json)` | The whole file. Non-destructive (the modified flag is untouched). |
| `SetDocumentJson` | `(s json)` | Replaces the whole document. Load failure → `Failed`. |
| `GetWorksheetJson` | `() → (s json)` | Just the active sheet's nodes. |
| `SetWorksheetJson` | `(s json)` | Replaces the active sheet's nodes (parse-checked first, so a malformed payload changes nothing). |
| `ValidateJson` | `(s json) → (b ok, s error)` | Parses without applying — no mutation. |

### Sheets

| Method | Signature | Notes |
|--------|-----------|-------|
| `ListSheets` | `() → (as sheets)` | In tab order. |
| `GetActiveSheet` | `() → (s name)` | |
| `SelectSheet` | `(s name)` | Unknown → `SheetNotFound`. |
| `AddSheet` | `(s name) → (s actual)` | Returns the actual name (auto-generated `Sheet N` if `name` is empty); a duplicate → `BadPropertyValue`. |
| `RemoveSheet` | `(s name)` | Unknown → `SheetNotFound`; removing the last sheet → `BadPropertyValue`. |
| `RenameSheet` | `(s from, s to)` | Unknown `from` → `SheetNotFound`. |

### File lifecycle

These route through the same code the menu actions use, minus the chooser
and confirmation dialogs.

| Method | Signature | Notes |
|--------|-----------|-------|
| `New` | `()` | Fresh empty document. |
| `Open` | `(s path)` | Load a file. |
| `Save` | `()` | Save to the current path; no path yet → `Failed`. |
| `SaveAs` | `(s path)` | Save to `path` and adopt it. |
| `GetCurrentPath` | `() → (s path)` | `""` if never saved. |
| `IsModified` | `() → (b modified)` | The flow's modified flag. |

### Document globals

Typed `name → value` pairs saved into the file — the `${…}` document
variables the Document Settings dialog edits.

| Method | Signature | Notes |
|--------|-----------|-------|
| `ListGlobals` | `() → (a(sss))` rows of `{name, type, value}` | |
| `GetGlobal` | `(s name) → (s type, s value)` | Unknown → `GlobalNotFound`. |
| `SetGlobal` | `(s name, s type, s value)` | Creates or overwrites. Bad type nick / unparseable number → `BadPropertyValue`. |
| `RemoveGlobal` | `(s name)` | Unknown → `GlobalNotFound`. |

The `type` is one of the four scalar nicks the on-disk format records:
`boolean`, `integer`, `double`, `string`. Numbers are parsed/rendered
locale-independently.

---

## Signals — observing changes live

Subscribe and react to edits — the user's or your own — instead of polling.
The signals are bridged from the model's GObject notifications, so they fire
whether a change came over D-Bus or from the user's own hands in the GUI.

### On `org.pipas.pipnode.Worksheet`

| Signal | Args |
|--------|------|
| `NodeAdded` | `(s uuid, s type_name)` |
| `NodeRemoved` | `(s uuid)` |
| `NodeMoved` | `(s uuid, d x, d y)` |
| `NodeRenamed` | `(s uuid, s name)` |
| `NodePropertyChanged` | `(s uuid, s property)` |
| `WireAdded` | `(s wire_uuid, s source_uuid, s target_uuid, i target_input)` |
| `WireRemoved` | `(s wire_uuid)` |
| `SelectionChanged` | `(as uuids)` |
| `MessageEmitted` | `(s node_uuid, s topic, s json)` — a debug tap on every emission |

### On `org.pipas.pipnode.Editor`

| Signal | Args |
|--------|------|
| `SheetChanged` | `(s name)` — the active sheet changed |
| `DocumentModified` | `(b modified)` — the modified flag flipped |

---

## Data formats

### Message JSON envelope

`InjectMessage` accepts either form:

* **Full envelope** — an object whose `data` member is an object, with an
  optional `topic`:

  ```json
  { "topic": "demo/volume", "data": { "value": 0.5, "label": "hi" } }
  ```

* **Bare data bag** — any other object, folded wholesale into the message's
  data bag:

  ```json
  { "value": 0.5, "label": "hi" }
  ```

An injected message stands in for a wire delivery, so it has **no source**
node (`from_id` is empty when read back).

`GetLastOutputMessage` and the `MessageEmitted` signal render the compact
envelope:

```json
{ "topic": "demo/volume", "from_id": "…", "data": { "value": 0.5 } }
```

The full set of standard data-bag members is documented in the HTML user
manual (`data/help-index.html`), not here — this file is the transport
contract, the manual is the message contract.

### Boolean values

Pipnode encodes booleans on `data.value` as `0.0` (false/off) and `1.0`
(true/on); detect "on" with `value > 0.5`.

---

## Worked examples

### From the shell with `gdbus`

Assuming an instance launched as
`pipnode-editor --dbus-name=org.pipas.pipnode.work`:

```sh
DEST=org.pipas.pipnode.work
OBJ=/org/pipas/pipnode/work

# Fresh document, then add a Knob and read it back.
gdbus call --session --dest $DEST --object-path $OBJ \
  --method org.pipas.pipnode.Editor.New

UUID=$(gdbus call --session --dest $DEST --object-path $OBJ \
  --method org.pipas.pipnode.Worksheet.AddNodeReturningUuid \
  "PnKnob" 100.0 100.0)
echo "added $UUID"

# Discover the palette.
gdbus call --session --dest $DEST --object-path $OBJ \
  --method org.pipas.pipnode.Worksheet.ListNodeTypes
```

### From Python with the reference client

```python
from pndbus import PipnodeEditor

with PipnodeEditor.launch() as ed:        # own throwaway instance
    ed.new_document()
    ed.subscribe()                        # collect live signals

    knob  = ed.add_node("PnKnob",  100, 100)
    topic = ed.add_node("PnTopic", 360, 100)
    debug = ed.add_node("PnDebug", 620, 100)

    ed.set_node_properties(topic, {"topic": "demo/volume"})
    wire = ed.connect(topic, debug)       # topic -> debug, input 0

    ed.inject_message(topic, {"value": 0.5})
    print(ed.get_last_output_message(topic))   # -> the emitted envelope

    ed.pump()
    assert ed.seen("NodeAdded")
    assert ed.seen("WireAdded")
```

To talk to an *already running* editor instead of launching one, name it:

```python
ed = PipnodeEditor("org.pipas.pipnode.work")
ed.connect_bus()
ed.subscribe()
print(ed.list_nodes())
```

---

## The reference Python client

`tests/pndbus.py` is a dependency-light (PyGObject only) wrapper around
everything above. It is:

* the helper the functional tests (`tests/test_dbus_*.py`) drive the editor
  through; and
* the worked example to copy into a new automation client — it speaks
  nothing but the documented introspection contract.

Key surface:

* `PipnodeEditor.launch(...)` — spawn a private instance under a unique
  `--dbus-name`, returning a connected client (use as a context manager).
* `PipnodeEditor(bus_name).connect_bus()` — attach to an already-running
  instance.
* `ws(method, params, reply)` / `ed(method, params, reply)` — the raw call
  layer; remote errors become `PipnodeError` (with `.short` = the bare error
  code).
* `subscribe()` / `pump()` / `seen()` / `events` — signal collection.
* Typed convenience wrappers for the whole surface: `add_node`, `connect`,
  `set_node_properties`, `inject_message`, `get_last_output_message`,
  `list_node_types`, `add_sheet`, `set_global`, `save_as`, … (one per
  documented method, named after it).

Run it directly for a one-line smoke test:

```sh
python3 tests/pndbus.py
```

---

## Safety and the trust boundary

These methods mutate the user's open document. The design decision (TODO
#40.16) is:

* **The mutating surface is always-on** — there is no enable flag and no
  per-call confirmation. The trust boundary is the **session bus itself**:
  only processes running as the same user can reach it, which is the same
  boundary that already lets any same-user process read the user's files or
  drive their display. Gating the API behind a flag would not raise that
  boundary; it would only break unattended automation, which is the whole
  point of the interface.

* **Isolation is by instance, not by permission.** For an unattended or
  experimental run that must not touch an interactive editor, launch a
  **throwaway instance** under a private `--dbus-name` (the reference client
  does this automatically). It registers its own name, serves its own
  object, and is torn down when the process exits.

* **Automation is made visible, not silent.** Whenever a non-read call
  drives an editor window, that window lights an **"⚡ automation" badge** in
  its statusbar; the badge fades a few seconds after the automation traffic
  stops. So a user watching an interactive editor can always see when
  something other than their own hands is editing the document. (Pure reads
  — `Get*`, `List*`, `Is*`, `ValidateJson` — do not light it.)

* **Nothing is hidden from undo or save.** Automation edits go through the
  same model paths as user edits, so they appear in the undo history and in
  the modified-flag/title exactly as a manual edit would; the
  `DocumentModified` signal fires the same way.

In short: treat the automation interface as exactly as trusted as any other
program the user runs in their own session. If you need stronger isolation,
give the run its own `--dbus-name` instance.
