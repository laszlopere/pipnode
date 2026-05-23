# Headless Core / GTK GUI Split — Implementation Plan

Status: **in progress** · Owner: Laszlo · Tracks: **TODO #23** (and #24)

This document is the step-by-step plan for splitting pipnode into a
GTK-free headless core and a separate GTK GUI tier, so plugins and
worksheets can run on servers without pulling the GTK toolkit, while
still letting plugins ship rich settings dialogs (and, later, full GUI)
when the editor is present.

Follow the phases in order. Each phase has an **objective**, concrete
**steps**, a **verification** gate, and a **rollback** note. Do not start
a phase until the previous phase's verification passes.

---

## 0. Progress so far

What has been done toward this plan, newest first:

- **Dual-node logic characterized (Phase 0 fill-out).** Added headless
  unit tests for the four dual-nature nodes that previously had none —
  `test-pn-dial.c`, `test-pn-graph.c`, `test-pn-table.c`,
  `test-pn-chat.c` — locking down the `receive()` contract that Phase 4
  will move into the core tier: sink-ness (none forward), key/path
  resolution and gating (empty key / missing / unparseable value is a
  safe no-op), per-topic series fan-out and its `PN_GRAPH_MAX_SERIES`
  cap, row/bubble accumulation and `#limit` trimming, the Dial's raw
  (un-clamped) read-only `value`, and Chat's self-loop suppression +
  focus flag. Because the binned/buffered state is private, three tiny
  read-only inspection seams were added (`pn_table_get_row_count`,
  `pn_graph_get_series_count`, `pn_chat_get_bubble_count`) — additive,
  GTK-free, logic-tier accessors that survive the split. All four wired
  into `tests/unit/Makefile.am`; `./run-unit-tests.sh` green (47/47).
  Remaining Phase 0 gap: the TODO #24 IO-parser extraction (Temp/Http/
  Tts/Weather/Net-IO) and the `pipnode-run`-level golden tests.

- **Phase 1 viability proven (de-GTK header, ABI-stable).** A throwaway
  proof-of-concept confirmed both TODO #23 build-time blockers are
  solvable without breaking the plugin ABI:
  - a de-GTK'd copy of `pn-node.h` — `<gtk/gtk.h>`/`<gdk/gdk.h>` replaced
    by `<glib-object.h>` plus opaque forward declarations of `cairo_t`,
    `GtkWidget`, `GtkWindow`, `GtkNotebook` (all pointer-only in the
    vtable), and the by-value `GdkRGBA color` replaced by a `PnColor`
    struct — **compiles against `gobject-2.0` alone**, with no GTK on the
    compiler command line; and
  - `G_STATIC_ASSERT`s showed **`PnColor` is byte-identical to `GdkRGBA`**
    (same size and field offsets), and the forward declarations coexist
    with the real `<gtk/gtk.h>` in one translation unit — so the GUI tier
    can cast `PnColor* ↔ GdkRGBA*` for free and an already-compiled
    plugin's `color` slot still reads correctly.

  The proof lived in `tests/degtk-proof/` (a standalone `run.sh` driving
  two compile checks). It was a scratch artifact, **not** wired into the
  build or committed; it has been removed now that its conclusions are
  recorded here and in Phase 1's recipe below. The actual `pn-node.h`
  surgery is still TODO (gated on Phase 0).

- **Regression net started (Phase 0 groundwork).** Added
  `tests/test_node_dialog_dial.py`, a functional test that drives the
  editor over D-Bus to open the `PnDial` settings dialog (asserting its
  six `build_class_tabs` pages and its editor↔property bindings) and then
  wires a live `PnAutoRandom` source into the Dial and checks values flow
  onto the needle. This exercises **both** sides the split touches: the
  GUI-tier dialog vfuncs and the core wire-routing engine — so it will
  catch regressions while the split proceeds. Committed; see that file
  and the D-Bus methods added to `src/pn-application.c`. (The broader
  characterization net of Phase 0 is still to be filled out.)

---

## 1. Decisions already settled

These were decided up front and the whole plan depends on them. If any
changes, re-read the affected phases.

| # | Decision | Choice |
|---|----------|--------|
| D1 | Core dependency strictness | **Strict — zero graphics.** Core links only glib/gobject/json-glib/libsoup. No gtk, gdk, cairo, pango, webkit, gtksourceview, plplot. All drawing moves to the GUI tier. |
| D2 | Plugin GUI mechanism | **Declarative settings schema (default path) + companion `-gui.so` (escape hatch).** Most plugins ship one GTK-free `.so` and *describe* their settings as data; bespoke UI ships a second `.so` the server never installs. |
| D3 | Dual-nature visual nodes (Rate, LED, Knob, Dial, Analog-Meter, Graph, Switch, Table, Chat, …) | **Logic stays headless-loadable.** Each `.c` is split: `receive()`/logic into core, cairo drawing + dialog into the GUI tier. |
| D4 | Bundled plugin re-tiering (network / shell / tasmota → core-only) | **In scope**, done in Phase 8 once the mechanism exists. |

---

## 2. Target architecture

```
                         glib / gobject / json-glib / libsoup
                                        │
                        ┌───────────────┴───────────────┐
                        │        libpipnode-core         │   ← servers install only this
                        │  PnNode (GTK-free vtable),     │
                        │  PnColor, factory, flow,       │
                        │  message, ~all receive() logic,│
                        │  settings-SCHEMA descriptors   │
                        └───────────────┬───────────────┘
                                        │ (links core)
                        ┌───────────────┴───────────────┐
                        │        libpipnode-gui          │   ← workstation / editor only
                        │  worksheet renderer, palette,  │
                        │  node dialog, schema→GtkWidget │
                        │  renderer, all cairo drawing,  │
                        │  webkit help, gtksourceview    │
                        └───────────────┬───────────────┘
        pipnode-run ────────┘ (core only)│
        pipnode-editor ──────────────────┘ (core + gui)

Plugin shapes:
  • logic-only plugin:        myplugin.so            (links core; schema describes its UI)
  • plugin + bespoke GUI:     myplugin.so  +  myplugin-gui.so  (gui .so links gui tier)
```

Key invariant: **the plugin ABI stays stable.** The class vtable keeps
its current slots; GTK/cairo types in vfunc signatures become opaque
forward declarations, and the one by-value `GdkRGBA` becomes the
layout-identical `PnColor`. A plugin compiled against the old header
keeps loading. (Proven — see §4, Phase 1.)

---

## 3. Global invariants / rules of engagement

- **Tests before refactor** (project rule). No tier move or header
  surgery lands before Phase 0's characterization net is green.
- **One ABI, additive only.** No vfunc slot is removed or reordered.
  New capability (schema, companion init) is added, never substituted.
- **Work on `master`**, commit in small reviewable steps; push `origin`
  every step, `github` for substantive milestones.
- After each phase, `./run-unit-tests.sh` must pass **and** the new
  characterization tests must pass.

---

## 4. Phases

### Phase 0 — Characterization test net (PREREQUISITE)

**Objective:** lock down current headless behavior so the refactor is
provably behavior-preserving. This is the TODO #23 / #24 prerequisite.

**Steps**
1. Inventory what `pipnode-run` does headless today: load a worksheet
   JSON, run the flow, observe emitted messages. Capture golden outputs.
2. Add `pipnode-run`-level characterization tests (load → run → assert
   emitted messages / side effects) for a representative spread of
   nodes, especially the dual-nature ones (Rate, LED, Knob, Dial, Graph,
   Switch, Table) whose logic must survive the split.
3. Export the pure parsers/aggregators still buried as `static` helpers
   in the auto-trigger IO nodes (Temp, Http, Tts, Weather, Net-IO) —
   follow the `pn-disk-io` / `pn-net-io` precedent already in the tree —
   and add unit tests driven by `pn_auto_trigger_run_once_sync()` (the
   seam added in TODO #24).
4. Wire all new tests into `tests/unit/` + `run-unit-tests.sh`.

**Verification:** `./run-unit-tests.sh` green; golden outputs committed.

**Rollback:** N/A (additive tests only).

---

### Phase 1 — De-GTK the type headers (ABI-stable)

**Objective:** remove `#include <gtk/gtk.h>` from the headers that define
the plugin/class ABI, so a pure-logic translation unit compiles against
GObject alone. **This was already proven viable** (see §0): a de-GTK'd
`pn-node.h` compiled against `gobject-2.0` alone, and `PnColor` was shown
byte-identical to `GdkRGBA`. What remains is to apply that recipe to the
real header and migrate the call sites.

**Steps**
1. Apply the proven recipe to `lib/pn-node.h`:
   - drop `<gtk/gtk.h>` / `<gdk/gdk.h>`, include `<glib-object.h>`;
   - forward-declare the pointer-only types: `cairo_t`, `GtkWidget`,
     `GtkWindow`, `GtkNotebook`;
   - add `PnColor` (4×`double` r,g,b,a — same member names as GdkRGBA so
     `color->red` style accesses don't churn) with `pn_color_parse()` /
     `pn_color_to_string()` / `_copy` / `_free` / `_get_type`;
   - swap the vtable `GdkRGBA color` → `PnColor color` and the three
     `pn_node*_get/set_color` signatures.
2. Implement `PnColor` in `lib/pn-color.c` (boxed GType + a CSS-ish
   parser to replace `gdk_rgba_parse`; the GUI tier may delegate to
   `gdk_rgba_parse` via the layout cast, but core must not).
3. Migrate every `GdkRGBA` call site in the tree to `PnColor`. At the
   cairo/gdk drawing boundary in the (future) GUI tier, cast
   `PnColor* ↔ GdkRGBA*` (same layout — proven).
4. Repeat the forward-declare treatment for the other ABI-relevant
   headers that currently pull GTK but only need pointers:
   `pn-node-dialog-helpers.h` keeps GTK (GUI-tier helper) — leave it;
   audit `pn-http.h` / `pn-ws.h` (`<gdk/gdk.h>`) and
   `pn-image-message.h` (`gdk-pixbuf`) — these likely belong to GUI tier
   or need a core-safe type.
5. Run LSP diagnostics; fix every fallout site.

**Verification:** whole tree still builds (monolithic lib for now);
`./run-unit-tests.sh` + Phase 0 tests green. No behavior change.

**Rollback:** revert the header + `pn-color.c`; `PnColor` is isolated.

**Note:** when the real `pn-node.h` is de-GTK'd, consider re-creating the
two compile checks the §0 proof used (header-compiles-without-GTK, and
`PnColor`/`GdkRGBA` layout `G_STATIC_ASSERT`s) as a small permanent guard
wired into `run-unit-tests.sh`, so the property can't silently regress.

---

### Phase 2 — Re-measure the tier boundary

**Objective:** with `pn-node.h` no longer dragging GTK in transitively,
get the *true* list of GTK-free `.c` files (the pre-Phase-1 token scan
over-counts; TODO #23 estimated ~35 of 71 are genuinely clean).

**Steps**
1. After Phase 1, scan each `lib/pn-*.c` for *direct* `gtk_`/`gdk_`/
   `cairo_`/`pango_`/`webkit_` calls (not transitive includes).
2. Classify every file into: **core** (no GTK calls), **gui** (GTK only,
   no `receive()` logic worth running headless), **dual** (both — must be
   split per D3).
3. Record the classification table in this document (append to §6).

**Verification:** classification reviewed and agreed before any file
moves.

---

### Phase 3 — Split the build into two libraries

**Objective:** produce `libpipnode-core.la` (GObject + json-glib + soup)
and `libpipnode-gui.la` (links core + the GTK stack).

**Steps**
1. `configure.ac`: keep GTK/webkit/sourceview/plplot detection but make
   the GUI bits feed only the gui library; core gets a minimal dep set.
   Consider a `--disable-gui` configure switch that builds core only
   (the server build).
2. `lib/Makefile.am`: replace the single `lib_LTLIBRARIES = libpipnode.la`
   with two libraries and two source lists (from Phase 2's table). Core
   sources compile with **no** `GTK_CFLAGS`; gui sources keep them.
   `libpipnode_gui_la_LIBADD` includes `libpipnode-core.la`.
3. Provide two `pkg-config` files: `pipnode-core.pc`, `pipnode-gui.pc`
   (split from `pipnode.pc.in`). A core-only install ships only the
   former.
4. Public header install: core headers (`pn-node.h`, `pn-color.h`,
   `pn-plugin.h`, factory, flow, message, schema) go to the core dev
   package; GUI headers to the gui package.

**Verification:** `./configure --disable-gui` builds core + `pipnode-run`
with no GTK linkage (`ldd src/pipnode-run` shows no `libgtk`); full build
still produces a working editor. Phase 0 tests green in both modes.

**Rollback:** the source lists are the only structural change; revert
`Makefile.am` / `configure.ac` to the single-library form.

---

### Phase 4 — Split the dual-nature node sources (D3)

**Objective:** each dual node's `receive()`/logic lives in core; its
cairo drawing + dialog hooks live in the gui tier — without breaking the
single-GType-per-node model.

**Steps** (per node, e.g. `pn-graph.c`)
1. Move logic (properties, `receive`, parsers, state) into the core
   `.c`; it registers the `GType` and leaves drawing/dialog vfunc slots
   `NULL`.
2. Move cairo painters (`paint_plot`, `paint_header_overlay`, …) and any
   custom dialog (`build_class_tab`, …) into a gui-tier `.c` that, at GUI
   init, looks up the type and installs those vfunc slots on the class
   (`g_type_class_ref` + assign — same mechanism the companion plugin
   loader will use, Phase 6).
3. Confirm the node's logic now compiles into core with no GTK.

**Verification:** Phase 0 characterization tests for each split node
still pass (logic unchanged); editor still draws the node identically
(manual `/verify` or screenshot diff).

**Rollback:** per-node; revert the individual split.

> Order suggestion: do the simplest dual node first (e.g. LED) end-to-end
> as the pattern, review, then batch the rest.

---

### Phase 5 — Wire the two binaries to the two tiers

**Objective:** `pipnode-run` links core only; `pipnode-editor` links core
+ gui.

**Steps**
1. `src/Makefile.am`: `pipnode_run_LDADD` → core only, drop `GTK_CFLAGS`
   from `pipnode_run_CFLAGS`. `pipnode_editor` → core + gui.
2. Audit `src/pn-run.c` for any stray GTK include (TODO #23 says it's
   already clean — confirm).

**Verification:** `ldd src/pipnode-run | grep -i gtk` is empty; it runs a
worksheet headless. `pipnode-editor` unchanged for the user.

---

### Phase 6 — Plugin ABI: companion GUI module (D2 escape hatch)

**Objective:** let a plugin ship logic in `myplugin.so` (core) and
bespoke GUI in `myplugin-gui.so` (gui tier), with the server installing
only the former.

**Steps**
1. Define a second entry point in `pn-plugin.h`, e.g.
   `pn_plugin_gui_init(PnNodeFactory*)` (or a GUI-augmentation callback),
   exported only by `-gui.so`. Bump `PN_PLUGIN_ABI_VERSION` and document
   the new contract in `PLUGINS` §15 / a new §.
2. Two loaders:
   - the **core** loader (used by `pipnode-run` and the editor) loads
     `*.so` and calls `pn_plugin_init` exactly as today;
   - the **gui** loader (editor only) additionally looks for the sibling
     `*-gui.so`, loads it, and calls `pn_plugin_gui_init`, which installs
     drawing/dialog vfunc slots onto the already-registered classes
     (same `g_type_class_ref` + assign mechanism as Phase 4).
3. The headless runtime never looks for `-gui.so`; a missing companion is
   not an error.
4. Add a reference plugin under `tests/plugins/` that demonstrates the
   split (logic `.so` + companion `-gui.so`).

**Verification:** the reference plugin's logic loads and runs under
`pipnode-run` with no GTK present; the editor additionally loads its
companion and shows the custom dialog. ABI version mismatch surfaces a
clear `GError` (existing behavior).

---

### Phase 7 — Declarative settings schema (D2 default path)

**Objective:** the common dialog customisations (custom editor per
property, list/table editors, multi-tab grouping) become **data** held in
core, so most plugins need no GTK at all. Only bespoke UI falls through
to the Phase 6 companion.

**Steps**
1. Design a core, GTK-free schema describing settings UI: per-property
   editor kind (entry / spin + range / combo + choices / file path /
   multiline / list-of-records / read-only label), tab grouping, and the
   existing hints (`multiline`, `hostname_hint`). This generalises the
   qdata hints already on param specs (`pn_param_spec_set_multiline`,
   `pn_param_spec_set_hostname_hint`).
2. Let a node/plugin attach a schema in `_class_init` (pure data, no
   widgets).
3. In the gui tier, write a **schema → GtkWidget renderer** that the node
   dialog uses; it reproduces today's auto-generated tab plus the
   declarative customisations. Keep the imperative `build_*` vfuncs as
   the escape hatch for what the schema can't express.
4. Port the in-tree structured editors (`PnFilter`, `PnSet` rule lists,
   `PnDial`'s 4-tab grouping) to the schema where they fit; leave
   genuinely bespoke ones on the imperative hooks.

**Verification:** the ported nodes' dialogs look/behave the same in the
editor; their classes carry no GTK and load under `pipnode-run`.

---

### Phase 8 — Re-tier bundled plugins + docs + packaging (D4)

**Objective:** move network / shell / tasmota to core-only where they no
longer need GTK; finalise documentation and packaging.

**Steps**
1. Re-tier `plugins/network`, `plugins/shell`, `plugins/tasmota` to
   core-only (logic `.so`, schema for any settings UI). `plugins/image`
   and `plugins/sound-effects` keep a companion `-gui.so` if they draw.
2. Update `PLUGINS`: the two-tier model, `pn_plugin_gui_init`, the schema
   API, and a "how to ship a server-installable plugin" checklist.
3. Update `README.md` / `INSTALL` with the server (`--disable-gui`) build
   and the `pipnode-core` vs `pipnode-gui` packages.
4. Provide distro packaging guidance: `pipnode-core` (no GTK depends),
   `pipnode-gui` (depends on core + GTK), `pipnode-plugin-*` split into
   core/gui sub-packages where applicable.

**Verification:** a clean container with **no GTK installed** can
`--disable-gui` build, install core + bundled core plugins, and run a
worksheet via `pipnode-run`.

---

### Phase 9 (future) — Full-GUI nodes

Out of scope for the split itself, but the architecture supports it: a
node that presents its *own* window (not just a settings dialog) does so
from its companion `-gui.so`, which links the gui tier and owns the
GtkWindow. The headless logic half still runs on servers. Revisit
out-of-process / web GUI only if remote dashboards become a requirement.

---

## 5. Risk register

| Risk | Mitigation |
|------|-----------|
| Behavior drift during dual-node splits | Phase 0 golden tests gate every split (Phase 4). |
| ABI break for existing plugins | Forward-decl + `PnColor` layout match proven (Phase 1); ABI is additive only. |
| `PnColor` parser diverges from `gdk_rgba_parse` | Unit-test `pn_color_parse` against the GTK parser's results for the colour strings the tree actually uses. |
| Hidden transitive GTK use in "core" files | Phase 2 re-measurement uses direct-call scan; Phase 3 verification asserts `ldd` has no `libgtk`. |
| Schema can't express some dialog | That's expected — it falls through to the imperative `build_*` companion hooks; schema is the 80% path, not the only path. |

## 6. Appendix — file tier classification

_(populate in Phase 2 after the header de-GTK re-measurement)_

| File | Tier (core / gui / dual) | Notes |
|------|--------------------------|-------|
| _TBD_ | | |
