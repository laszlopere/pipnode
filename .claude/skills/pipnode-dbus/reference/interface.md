# Pipnode D-Bus interface — full reference

Distilled from the repo's `DBUS-API.md` (the canonical transport contract) for
fast lookup. If this and `DBUS-API.md` ever disagree, `DBUS-API.md` and live
introspection win:

```sh
gdbus introspect --session --dest <NAME> --object-path <OBJ>
```

Bus name ↔ object path mirror each other (`org.pipas.pipnode.work` →
`/org/pipas/pipnode/work`). Both interfaces below live on the same object.

---

## Versioning

| Member | Signature | Meaning |
|--------|-----------|---------|
| `Editor.GetApiVersion` | `() → (u major, u minor)` | API version. The one method that works before a document exists. |
| `Editor.Version` (property) | `u` read-only | Major version. |

Current **1.2**. Major bumps on a breaking change; minor on back-compatible
additions. History: `1.0` addressed/typed/discoverable core; `1.1` whole-document
ops (Phase D); `1.2` interactivity — selection, view, message inject/readback,
live signals (Phase E).

## Error contract

Domain `org.pipas.pipnode.Worksheet.Error.<Code>` (both interfaces raise from
it). Match on the trailing code. In PyGObject:
`Gio.DBusError.get_remote_error(err)`; the reference client wraps it as
`PipnodeError` whose `.short` is the bare code.

| Code | Raised when |
|------|-------------|
| `NoActiveSheet` | A `.Worksheet` call but no active worksheet (e.g. panel-editor tab selected). |
| `NoActiveWindow` | An `.Editor` call but no editor window at all. |
| `NodeNotFound` | A node UUID does not resolve on the active sheet. |
| `WireNotFound` | A wire UUID does not resolve. |
| `UnknownNodeType` | `AddNode`/`GetNodeTypeInfo` type not in the factory. |
| `UnknownProperty` | Property name does not exist on the node. |
| `BadPropertyValue` | Value unparseable for the type, or an arg out of range (input index, sheet count, malformed message JSON, …). |
| `IllegalConnection` | Illegal wire: self-loop, no-output source, no-input target, input index out of range, duplicate. |
| `SheetNotFound` | Sheet name does not exist. |
| `GlobalNotFound` | Document-global name does not exist. |
| `Failed` | Generic I/O / load failure (`Open`/`Save`/`SetDocumentJson`). |

---

## `org.pipas.pipnode.Worksheet` — the active sheet

Address nodes by **UUID** (stable across save/load). Wire UUIDs are
session-only (not serialized).

### Reading the graph

| Method | Signature |
|--------|-----------|
| `GetNodeCount` | `() → (u)` |
| `GetWireCount` | `() → (u)` |
| `GetNodeByUuid` | `(s uuid) → (s class_name, s name, s icon, d x, d y, b has_input, b has_output)` |
| `GetNodesWithUuids` | `() → (a(sssddbbs))` rows `{class_name, name, icon, x, y, has_input, has_output, uuid}` |
| `GetNodeGeometry` | `(s uuid) → (d x, d y, d w, d h)` full worksheet-space box |

### Node lifecycle

| Method | Signature | Notes |
|--------|-----------|-------|
| `AddNodeReturningUuid` | `(s type, d x, d y) → (s uuid)` | `type` is the GType name, e.g. `PnKnob`. |
| `DeleteNode` | `(s uuid)` | Also drops every wire touching the node. |
| `MoveNode` | `(s uuid, d x, d y)` | |
| `RenameNode` | `(s uuid, s name)` | |
| `SetNodeInputCount` | `(s uuid, i count)` | Multi-input nodes (Calculator, …). `count < 1` → `BadPropertyValue`. |
| `SetNodeInputName` | `(s uuid, i index, s name)` | `index` outside `[0,count)` → `BadPropertyValue`. |

### Properties

| Method | Signature | Notes |
|--------|-----------|-------|
| `GetNodePropertyByUuid` | `(s uuid, s prop) → (s value)` | Text-rendered (enums as value-nick). |
| `SetNodePropertyByUuid` | `(s uuid, s prop, s value) → (b ok)` | |
| `SetNodeProperties` | `(s uuid, a{ss} props)` | **Atomic** — all validated first; one bad pair leaves node untouched. |
| `ListNodeProperties` | `(s uuid) → (a(sssssb))` rows `{name, value_type, current, default, constraints, writable}` | `constraints`: `nick1\|nick2\|…` (enum), `min..max` (bounded numeric), else `""`. `writable` true only for non-construct-only writable props. |

### Wiring (port-aware)

| Method | Signature | Notes |
|--------|-----------|-------|
| `Connect` | `(s source, s target, i target_input) → (s wire_uuid)` | Illegal wires → `IllegalConnection`; unknown node → `NodeNotFound`. |
| `Disconnect` | `(s wire_uuid)` | Unknown → `WireNotFound`. |
| `ListWires` | `() → (a(ssis))` rows `{source_uuid, target_uuid, target_input, wire_uuid}` | Dangling endpoint reads as empty UUID. |
| `GetNodeWires` | `(s uuid) → (a(ssis))` | Same shape, restricted to wires touching `uuid`. |

### Discovery (runtime-accurate, includes loaded plugins)

| Method | Signature | Notes |
|--------|-----------|-------|
| `ListNodeTypes` | `() → (a(sssbbs))` rows `{type_name, class_name, category, has_input, has_output, plugin_name}` | Core nodes report the `"Internal"` plugin sentinel. |
| `GetNodeTypeInfo` | `(s type) → (s class_name, s category, b has_input, b has_output, s plugin_name, s icon, s color, s help_page)` | `help_page` = `<type_name>.html`. `UnknownNodeType` on miss. |
| `GetDeviceProviders` | `() → (as ids)` | Registered device-provider ids. |

### Selection and view

| Method | Signature | Notes |
|--------|-----------|-------|
| `SelectNodes` | `(as uuids) → (u matched)` | Replaces selection with on-sheet matches; skips unknown/off-sheet; emits `SelectionChanged` once. |
| `GetSelection` | `() → (as uuids)` | |
| `ClearSelection` | `()` | |
| `FocusNode` | `(s uuid)` | Recenter + focus pulse. `NodeNotFound` on miss. |
| `CenterOn` | `(s uuid)` | Quiet recenter. |
| `FitToContent` | `()` | Zoom/scroll the whole sheet into view. |
| `GetWorksheetScroll` | `() → (d hval, d vval, d hpage, d vpage, d hupper, d vupper)` | |
| `SetWorksheetScroll` | `(d h, d v)` | |

### Message injection and readback

| Method | Signature | Notes |
|--------|-----------|-------|
| `InjectMessage` | `(s uuid, s json)` | Delivers to input 0 as if a wire carried it. Malformed/non-object JSON → `BadPropertyValue`. |
| `InjectMessageOnInput` | `(s uuid, i input, s json)` | On a specific input; out of range → `BadPropertyValue`. |
| `GetLastOutputMessage` | `(s uuid) → (s json)` | Last emitted message as compact envelope; `""` before any emission. |

### Legacy / test-only (ignore in new clients)

`GetNode`, `GetNodes`, `GetWire`, `GetWires`, `GetNodeUuid`,
`GetNodeProperty`/`SetNodeProperty` (by index), `AddNode` (index),
`ConnectNodes`/`ConnectNodesByUuid` (port-blind bool), `Clear`,
`LoadFromFile`/`SaveToFile`; debug-pane and dialog-driving helpers
(`InjectDebugMessage`, `GetDebugRowCount`, `GetDebugRowFromId`,
`ClickDebugFromButton`, `GetFocusPulseUuid`, `GrabWorksheetFocus`,
`ResizeWindow`, `GetDebugPaneAllocation`, `OpenNodeDialog(ByUuid)`,
`CloseNodeDialog`, `GetDialogPageTitles`, `SelectDialogPage`,
`GetDialogEditorText`, `GetDialogEditorSensitive`, `SetDialogEditorText`).

---

## `org.pipas.pipnode.Editor` — the document

### Whole-document / active-sheet JSON

Far cheaper than dozens of per-node round-trips; round-trips the on-disk format.

| Method | Signature | Notes |
|--------|-----------|-------|
| `GetDocumentJson` | `() → (s json)` | Whole file. Non-destructive (modified flag untouched). |
| `SetDocumentJson` | `(s json)` | Replaces whole document. Load failure → `Failed`. |
| `GetWorksheetJson` | `() → (s json)` | Active sheet's nodes only. |
| `SetWorksheetJson` | `(s json)` | Replaces active sheet's nodes (parse-checked first). |
| `ValidateJson` | `(s json) → (b ok, s error)` | Parses without applying — no mutation. |

### Sheets

| Method | Signature | Notes |
|--------|-----------|-------|
| `ListSheets` | `() → (as)` | Tab order. |
| `GetActiveSheet` | `() → (s name)` | |
| `SelectSheet` | `(s name)` | Unknown → `SheetNotFound`. |
| `AddSheet` | `(s name) → (s actual)` | Empty name → auto `Sheet N`; duplicate → `BadPropertyValue`. |
| `RemoveSheet` | `(s name)` | Unknown → `SheetNotFound`; last sheet → `BadPropertyValue`. |
| `RenameSheet` | `(s from, s to)` | Unknown `from` → `SheetNotFound`. |

### File lifecycle (routes through the menu-action code, minus dialogs)

| Method | Signature | Notes |
|--------|-----------|-------|
| `New` | `()` | Fresh empty document. |
| `Open` | `(s path)` | Load a file. |
| `Save` | `()` | Save to current path; no path yet → `Failed`. |
| `SaveAs` | `(s path)` | Save to `path` and adopt it. |
| `GetCurrentPath` | `() → (s path)` | `""` if never saved. |
| `IsModified` | `() → (b modified)` | The flow's modified flag. |

### Document globals (`${…}` variables; Document Settings dialog edits these)

| Method | Signature | Notes |
|--------|-----------|-------|
| `ListGlobals` | `() → (a(sss))` rows `{name, type, value}` | |
| `GetGlobal` | `(s name) → (s type, s value)` | Unknown → `GlobalNotFound`. |
| `SetGlobal` | `(s name, s type, s value)` | Creates/overwrites. Bad type nick / unparseable number → `BadPropertyValue`. |
| `RemoveGlobal` | `(s name)` | Unknown → `GlobalNotFound`. |

`type` ∈ `boolean | integer | double | string` (the four scalar nicks the
on-disk format records). Numbers parsed/rendered locale-independently.

---

## Signals — observe changes live

Bridged from the model's GObject notifications, so they fire for changes from
D-Bus **or** the user's own hands in the GUI.

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
| `MessageEmitted` | `(s node_uuid, s topic, s json)` — debug tap on every emission |

### On `org.pipas.pipnode.Editor`

| Signal | Args |
|--------|------|
| `SheetChanged` | `(s name)` |
| `DocumentModified` | `(b modified)` |

---

## Data formats

### Message JSON envelope (for `InjectMessage`)

Either form is accepted:

- **Full envelope** — object whose `data` member is an object, optional `topic`:
  ```json
  { "topic": "demo/volume", "data": { "value": 0.5, "label": "hi" } }
  ```
- **Bare data bag** — any other object, folded wholesale into the data bag:
  ```json
  { "value": 0.5, "label": "hi" }
  ```

An injected message stands in for a wire delivery, so it has **no source**
(`from_id` empty when read back). `GetLastOutputMessage` / `MessageEmitted`
render the compact envelope:

```json
{ "topic": "demo/volume", "from_id": "…", "data": { "value": 0.5 } }
```

Standard data-bag members are documented in the HTML user manual
(`data/help-index.html`) — that is the message contract; this file is the
transport contract.

### Booleans

Encoded on `data.value` as `0.0` (false/off) / `1.0` (true/on); detect "on"
with `value > 0.5`.
