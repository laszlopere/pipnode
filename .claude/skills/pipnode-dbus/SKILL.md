---
name: pipnode-dbus
description: How to drive a RUNNING pipnode-editor over its session-bus D-Bus automation API — author and edit worksheets live (add/move/wire/configure nodes, edit sheets and document globals, inject test messages, observe changes) and save them. Load this whenever a task means editing or inspecting a worksheet in a live editor instead of editing the on-disk JSON by hand, or when you see org.pipas.pipnode.Worksheet / .Editor, --dbus-name, pndbus.py, or "edit the running worksheet".
---

# Driving a running pipnode-editor over D-Bus

`pipnode-editor` exposes a session-bus D-Bus API so an agent (you) can author
and edit worksheets **in a running editor** and save them — read the graph,
add/move/wire/configure nodes, manage sheets and document globals, inject test
messages, observe live changes, and Save/Save As. This is the *hands* for the
graph; the **`pipnode-nodes`** skill carries the *semantics* of each node (what
it does, which message-bag fields it reads/writes). Load both for authoring.

**Canonical source of truth in the repo** (read these first if anything here is
ambiguous; do not let this skill drift from them):

- `DBUS-API.md` — the full transport contract (every method/signal signature).
- `tests/pndbus.py` — the reference PyGObject client; the functional tests
  (`tests/test_dbus_*.py`) drive the editor through it. Copy from it.
- Live introspection is authoritative over any doc:
  ```sh
  gdbus introspect --session --dest <NAME> --object-path <OBJ>
  ```

Full method/signal/error tables are in
[`reference/interface.md`](reference/interface.md); copy-paste workflows in
[`reference/recipes.md`](reference/recipes.md).

## Step 0 — find (or launch) an addressable instance

The bus name and object path mirror each other: name `org.pipas.pipnode.work`
→ object path `/org/pipas/pipnode/work`.

A **normal** editor launch is *non-unique*: it runs as its own process and does
**not** claim the well-known `org.pipas.pipnode` name (so opening a file never
pops a window in another instance). Two consequences:

1. The well-known `org.pipas.pipnode` name, when present on the bus, is the
   **panel engine** (`--gapplication-service`, the XFCE applet backend). It
   serves a *different* interface (`org.pipas.pipnode.Engine`) and is **not**
   for worksheet authoring — do not target it.
2. To get an addressable editor you (or the user) must launch it with an
   explicit name:
   ```sh
   pipnode-editor --dbus-name=org.pipas.pipnode.work
   ```

List what is reachable, then pick the right target:

```sh
gdbus call --session --dest org.freedesktop.DBus \
  --object-path /org/freedesktop/DBus \
  --method org.freedesktop.DBus.ListNames \
  | tr ',' '\n' | grep -i pipnode
```

- **Editing the user's open document?** The user must be running an editor
  under a private `--dbus-name`. If only bare `org.pipas.pipnode` is present,
  there is no authoring instance — ask the user to relaunch with
  `--dbus-name=...`, or launch a throwaway one yourself (next bullet).
- **Unattended / experimental run?** Launch your *own* throwaway instance under
  a private name so you cannot disturb an interactive editor. In Python,
  `PipnodeEditor.launch()` mints a unique name per process and tears it down on
  exit. Per the user's testing convention, prefer leaving an isolated
  `--dbus-name` instance running and telling the user what to look at rather
  than synthetically driving their interactive window.

`--dbus-name` must be a valid reverse-DNS D-Bus / GApplication id; an invalid
one aborts startup.

## The two interfaces

Both live on the same object path; pick by scope:

| Interface | Scope |
|-----------|-------|
| `org.pipas.pipnode.Worksheet` | The **active sheet**: nodes, wires, properties, discovery, selection/view, message inject/readback, live graph signals. |
| `org.pipas.pipnode.Editor` | The **document**: API-version handshake, whole-file / active-sheet JSON, sheets, file lifecycle (New/Open/Save/SaveAs), document globals, document signals. |

Always feature-detect: `Editor.GetApiVersion() → (u major, u minor)` (the one
method that works before a document exists). Current: **1.2**.

## The core authoring loop

1. **Connect** to the right instance (Step 0).
2. **Discover** the palette — `Worksheet.ListNodeTypes` (runtime-accurate,
   includes loaded plugins) and `Worksheet.GetNodeTypeInfo(type)` for a node's
   category/ports/icon/help page. Type names are GType names, e.g. `PnKnob`,
   `PnTopic`, `PnDebug`.
3. **Inspect** — `Worksheet.GetNodesWithUuids`, `ListWires`, and
   `ListNodeProperties(uuid)` (gives value-type, current, default, constraints,
   writable per property). For a whole-sheet view in one call, prefer
   `Editor.GetWorksheetJson` / `GetDocumentJson` over many round-trips.
4. **Mutate** (see rules below) — add/move/rename/delete nodes, set properties,
   `Connect`/`Disconnect` wires, manage sheets/globals.
5. **Verify** — read back, or `InjectMessage` + `GetLastOutputMessage` to
   exercise the flow without wiring a Debug node.
6. **Show the user** — `SelectNodes` / `FocusNode` / `FitToContent` so what you
   built is visible.
7. **Save** — `Editor.Save` (current path; errors `Failed` if never saved) or
   `Editor.SaveAs(path)`. Check `IsModified` / `GetCurrentPath` first. **Saving
   is never implicit — do it explicitly when the user wants it persisted.**

## Rules that bite

- **Address nodes by UUID, always.** `pn_node_get_uuid` is stable across
  save/load. Integer-index forms exist only for legacy tests and shift the
  moment a node is deleted — useless across a multi-step edit. Use
  `AddNodeReturningUuid`, `GetNodeByUuid`, `*ByUuid`, `Connect` (UUID-based).
- **Wire UUIDs are session-only** (not serialized — a reload mints fresh ones).
  Node UUIDs are the stable cross-save identity.
- **Batch property edits atomically** with `SetNodeProperties(uuid, a{ss})`:
  every pair is validated first, so one bad pair leaves the node untouched.
- **Booleans ride on `data.value`** as `0.0` (off) / `1.0` (on); test "on" with
  `value > 0.5`. Same encoding when injecting/reading messages.
- **Connections are port-aware.** `Connect(source, target, target_input)`.
  Illegal wires (self-loop, no-output source, no-input target, input index out
  of range, duplicate) all raise `IllegalConnection`.
- **Every failure is a real D-Bus error**, never a silent false. Domain
  `org.pipas.pipnode.Worksheet.Error.<Code>` (e.g. `NodeNotFound`,
  `UnknownNodeType`, `UnknownProperty`, `BadPropertyValue`, `IllegalConnection`,
  `SheetNotFound`, `GlobalNotFound`, `NoActiveSheet`, `Failed`). Match on the
  trailing code. Full list in `reference/interface.md`.
- **`.Worksheet` calls need an active worksheet.** If the panel-editor tab is
  selected (not a flow sheet) they raise `NoActiveSheet`.

## Safety / trust model (TODO #40.16)

- The mutating surface is **always-on** — no enable flag, no per-call confirm.
  The trust boundary is the **session bus itself** (same-user processes only).
- **Isolation is by instance**: for runs that must not touch an interactive
  editor, use a throwaway `--dbus-name` instance.
- Automation is **visible**: any non-read call lights an "⚡ automation" badge
  in the target window's statusbar (pure `Get*`/`List*`/`Is*`/`ValidateJson`
  reads do not). Edits go through the same model paths as user edits, so they
  appear in **undo** and flip the **modified flag** normally.
- Treat the API as exactly as trusted as any program the user runs in their own
  session; reach for a private `--dbus-name` when you need stronger isolation.
