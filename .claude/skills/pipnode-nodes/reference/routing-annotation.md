# Routing & Annotation — the nodes about the drawing, not the data

Two small palette groups that have nothing to do with transforming messages, and everything to do with the diagram. **Routing** (Jump In, Jump Out) moves a signal without a wire; **Annotation** (Comment) writes on the canvas without touching the graph. All three are GTK-free core nodes with no `-gui.c` companion, and all three escape the normal node painter in `pn-worksheet.c` — they have no header, no icon panel and no name label, so they read as schematic furniture rather than as boxes.

They are registered next to the sinks in `register_builtins()` (`lib/pn-node-factory.c:380-385`) purely because that is where the palette groups port-less decorations; the category strings are what actually place them.

## Jump In

**Purpose** — The entry end of a *named, wireless* connection: whatever arrives here is delivered to every **Jump Out** carrying the same tag, anywhere in the document. A schematic global label, not a wire. (`lib/pn-jump-in.c`)

**When to use** — When a wire would have to cross most of the worksheet, or when producer and consumer live on **different sheets** — a wire cannot cross sheets at all (`pn_flow_load_from_data` drops cross-sheet wires with a warning, `lib/pn-flow.c:2222`), so a jump pair is the only sanctioned way to route between them. Also for a one-to-many bus (several Jump Outs on one tag) or a many-to-one merge (several Jump Ins on one tag).

**Ports** — 1 input, **no output** (`has_input = TRUE`, `has_output = FALSE`, `pn-jump-in.c:164`). The input port sits at the pennant's tip.

**Settings** — no `PnSettingsSchema`, so the dialog auto-generates the rows.
- `tag` (string, default `""`) — the connection name (`pn-jump-in.c:174`). Rendered on a **Jump In** tab by the generic property-tab builder, alongside the standard **Node** tab.

**Reads / writes** — **neither.** `pn_jump_in_receive` (`pn-jump-in.c:57`) calls `pn_jump_collect_outputs(flow, tag)` and, for each match, `pn_message_clone()` + `pn_jump_out_deliver()`. The data bag passes through byte-for-byte and the envelope (`topic`, `id`, `created`, source) is preserved, because a jump never mints a message. It emits nothing itself.

**Gotchas**
- Its inherited **Topic** row is dead weight — a jump flag never stamps a topic. (Comment hides that row; the jump flags do not.)
- A flag with no owning `PnFlow` is inert, not a crash.
- Disabling a Jump In makes it inert at the generic dispatch level (`pn_node_receive_message_on_input`, `lib/pn-node.c:952`).

## Jump Out

**Purpose** — The exit end of the named connection: whatever reaches any same-tag Jump In is emitted from this flag's output port. (`lib/pn-jump-out.c`)

**When to use** — The receiving half of the pair; drop as many as you like on one tag to fan a signal out across sheets.

**Ports** — **no input**, 1 output (`has_input = FALSE`, `has_output = TRUE`, `pn-jump-out.c:148`). Deliberately **no `receive` vfunc** (`pn-jump-out.c:139`): it is driven only by its partners, never by a wire. The output port sits at `pos->x + width`, which is why `pn_jump_measure()` rounds the width up to a whole grid step (`lib/pn-jump.c:44`).

**Settings** — `tag` (string, default `""`, `pn-jump-out.c:158`); same auto-generated dialog as Jump In.

**Reads / writes** — neither. `pn_jump_out_deliver()` (`pn-jump-out.c:49`) checks `pn_node_get_disabled()` and then `pn_node_emit_message()`; each output wire gets its own deep copy as usual.

**Gotchas** — Disabling a Jump Out silently drops just that branch, leaving the other flags on the tag working. It is skipped twice: once in `pn_jump_collect_outputs`, again in `deliver`.

### How a Jump In finds its Jump Outs

- **No registry, no index.** Matching is a **linear scan of the document's node store** at delivery time: `pn_jump_collect_outputs()` (`lib/pn-jump.c:83`) walks `pn_flow_get_nodes(flow)` and collects every enabled `PnJumpOut` whose tag matches. Flags are a handful per document, so an index would cost more to keep correct than the scan saves.
- **Match rule** — `tag_matches()` (`lib/pn-jump.c:67`): exact, **case-sensitive** `g_strcmp0`, and **an empty tag never matches anything**, not even another empty tag — otherwise every freshly dropped flag would silently join one bus.
- **Scope** — the **whole document, every sheet**. The store is document-wide, not per-sheet.
- **Duplicates are legal in both directions.** N Jump Ins on one tag merge onto the bus; M Jump Outs each get an **independent `pn_message_clone()`** (`pn-jump-in.c:69`), exactly as `PnWire` fan-out does, so one branch mutating its bag cannot contaminate a sibling. Delivery order is document order — the list is built with `g_list_prepend` and then reversed, so it is stable across loads.
- **Missing or typo'd tags** — no exception and no dropped-message warning (invisible under a desktop launcher). Instead the pair uses the generic **has-error** state: `pn_jump_refresh_errors()` (`lib/pn-jump.c:119`) marks a flag in error when its tag is empty, when a Jump In has no Jump Out, or when a Jump Out has no Jump In. Two flags of the *same* direction on one tag do **not** satisfy each other. The worksheet then paints the pennant red with a warning mark. It is **edit-time only** — recomputed when a node is added or removed (`lib/pn-flow.c:1016`) and when a `tag` changes (`lib/pn-flow.c:1096`), never per message.
- **Tag ≠ node name.** The tag lives in its own property precisely so a cosmetic rename cannot silently re-route a signal on a sheet nobody is looking at.
- **Cycles** need no special guard: a Jump Out emits onto wires that dispatch through `pn_node_receive_message_on_input()`, which carries the thread-local depth counter, so a tag loop trips `PN_NODE_MAX_DISPATCH_DEPTH` (256) like any wired feedback path (`lib/pn-node.c:912`).
- **Serialization** — completely ordinary. The flag is a normal node in `nodes[]` with its tag in the generic `properties` bag. **Nothing links the two ends on disk** — the connection is re-derived from the tags at load, which is exactly why a rename-free tag and the has-error sweep matter.
- **Geometry** (`lib/pn-jump.h:67`) — footprint 40 px high (2 grid steps) and ≥60 px wide, width rounded **up** to a whole 20 px step from a nominal 8.5 px/char advance so the port lands on the grid; the *drawn* pennant is 24 px and centred inside the footprint. `get_size` is core/GTK-free so it cannot ask Pango to measure — the painter ellipsizes if the real text overruns.

Both flags share the body colour `{0.65, 0.22, 0.22}` (KiCad global-label red) so a matched pair reads as one connection. Icons: Jump In `fa-sign-in` U+F090, Jump Out `fa-sign-out` U+F08B. Painted by `draw_jump_flag()` (`lib/pn-worksheet.c:932`). Help: `data/help/PnJumpIn.html`, `data/help/PnJumpOut.html`. Tests: `tests/unit/test-pn-jump.c`.

**Worked example** — `examples/controls-and-logic/jumps.json`: a pair on `"Sheet 2"` plus a set on `"Worksheet"`, i.e. the cross-sheet case.

## Comment

**Purpose** — A chromeless free-text annotation box: a resizable rectangle with a frame holding a few paragraphs of wrapped text, used to document a worksheet. (`lib/pn-comment.c`)

**When to use** — Labelling a region of a flow, explaining a wiring trick, leaving a TODO on the canvas. Vs **Label** (`GUI/Displays`), which *is* a message sink that shows incoming text: a Comment is static and never receives anything.

**Ports** — **none at all**: `has_input = FALSE`, `has_output = FALSE` (`pn-comment.c:292`), no `receive` vfunc, no emission. It reads and writes **no** bag members and never fires. It is a node only to inherit selection, dragging, marquee, Delete, copy/paste, per-sheet membership, JSON persistence and undo/redo for free.

**Settings** — declarative `PnSettingsSchema`, two tabs (`pn-comment.c:345`):
- **Text** — `text` (string, default `""`, `PN_EDITOR_MULTILINE`); `text-size` (double, 6.0–96.0, default **13.0** px, `PN_EDITOR_SPIN`).
- **Colours** — `background-color` (PnColor, white by default; **alpha 0 gives a transparent note**), `frame-color` (grey), `text-color` (dark slate).
- `width` (double, 80.0–100000.0, default **200.0**) and `height` (double, 40.0–100000.0, default **120.0**) are properties but **not dialog rows** — edited on the canvas with the resize handles.
- The inherited **`topic`** row is explicitly hidden (`PN_ROW_FLAG_HIDDEN`, `pn-comment.c:358`) since the node never stamps one. The **Node** tab still shows Name.

**Drawing, sizing, editing**
- **Tier split** — the node is fully GTK-free; the cairo/Pango drawing and the resize interaction live in the worksheet, which reads the snapshot struct `PnCommentPaintState` via `pn_comment_get_paint_state()` (`pn-comment.c:76`). The `text` pointer in that snapshot is **borrowed** and valid only until the next property change.
- **Drawn** by `draw_comment_box()` (`lib/pn-worksheet.c:794`), reached by an early escape in `draw_node()`: filled rounded rect (radius 4, padding 8), wrapped text **clipped** inside, plus eight resize handles when selected.
- **Sized** — the footprint *is* `width`×`height`; `get_header_height` returns the full height so the whole box is hit-testable end to end (`pn-comment.c:120`).
- **Resized** — eight 10 px handles (NW, N, NE, E, SE, S, SW, W), only on a *selected* comment; the drag snaps to grid and clamps to the minima, a shrinking left/top edge stopping at the fixed opposite edge (`pn-worksheet.c:6030`). `pn_comment_set_size()` clamps and notifies; the worksheet subscribes to `notify::width`/`notify::height` specifically for comments to re-measure canvas extents.
- **Created** by right-clicking empty canvas → **"Add note"** (`pn-worksheet.c:4772`) — placed top-left at the click point, grid-snapped, assigned the current sheet, then selected so the handles show at once. Also draggable from the palette's **Annotation** group.
- **Edited** by double-clicking the box, or right-click → Configure.

**Serialized?** Yes, like any other node, with no extra code, so it round-trips through save/load and undo/redo. (Colours serialise as 8-bit hex, hence the 1/255 tolerance in `tests/unit/test-pn-comment.c`.)

**Gotchas**
- Overflowing text is **cropped**, not spilled — enlarge the box or shrink the font.
- Only a *selected* comment is resizable; the handles are invisible otherwise.
- It can be "disabled" from the context menu like any node, which greys it and suppresses nothing (there is no behaviour to suppress).
- `width`/`height` are still settable over the JSON/D-Bus property surface even though the dialog omits them.

Icon `fa-sticky-note-o` U+F24A; body grey `{0.62,0.62,0.62}`. Help page `data/help/PnComment.html`.
