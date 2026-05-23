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

- **Phase 5 DONE — the two binaries are wired to the two tiers;
  `pipnode-run` links core only.** Four increments cleared the carried
  debt and made `libpipnode-core` fully self-contained:
  - **5.1 — split Table-View (the missed 24th dual node).** `PnTableView`
    was misclassified in §6 as a "pure widget, not a node," but it is a
    full sink `PnNode` with `receive()` logic, so its `pn_table_view_
    get_type` stayed in the gui lib — one of core's three undefined gui
    symbols. Split it the established Table way: core keeps the GType, the
    five colour properties (migrated `GDK_TYPE_RGBA`→`PN_TYPE_COLOR` — the
    **last node-body `GdkRGBA` pspec in the tree**), `receive` + the JSON
    cell parser, the snapshot, the repaint throttle, `get_size`/
    `get_header_height` and the scroll vfunc; the painter moved to
    `pn-table-view-gui.c` behind the published read seam
    (`PnTableViewRow`, `PnTableViewPaintState`, `pn_table_view_get_paint_
    state`/`peek_header`/`peek_rows` + the scroll get/clamp pair).
  - **5.2 — plugin-policy seam.** The core factory consulted
    `PnPreferences` directly (forward-declared) to skip user-disabled
    plugins — core's other two undefined gui symbols. Replaced with a
    GTK-free hook, `pn_node_factory_set_plugin_filter()`; the **editor**
    installs a `PnPreferences`-backed filter in `src/main.c` before the
    discovery scan, `pipnode-run` installs none (loads every plugin).
    `libpipnode-core.so` now has **zero undefined gui/gtk symbols**.
  - **5.3 — retired the `pn-flow.c` legacy `GdkRGBA` by-name clause**
    (`pn_flow_legacy_rgba_type()` + the two call sites); every colour
    property is `PN_TYPE_COLOR` now, on-disk form unchanged.
  - **5.4 — core-only linkage.** `libpipnode-core` builds with
    `-no-undefined`; `pipnode-run` and the unit tests link core ALONE,
    dropping `libpipnode-gui`, the GTK flags and `-Wl,--no-as-needed`
    (the unit tests keep `GTK_LIBS` only for `test-pn-color`'s
    `gdk_rgba` cross-check, which `--as-needed` drops elsewhere).
  - **Prerequisite fix (committed first): the auto/default property
    editor now builds a `GtkColorButton` for `PN_TYPE_COLOR`.** The
    Phase-1/4 migration moved every colour property to `PN_TYPE_COLOR`,
    but `default_editor_impl` only matched `GDK_TYPE_RGBA` and so silently
    dropped colour rows to the read-only-label fallback (the dial D-Bus
    test only checks string/numeric bindings; the spot-checks covered
    canvas rendering). Added a `PN_TYPE_COLOR` branch binding through a
    `PnColor`↔`GdkRGBA` `GBinding` transform; kept the `GDK_TYPE_RGBA`
    branch for legacy plugins.
  - **Verified:** `objdump -p src/.libs/pipnode-run` `NEEDED` is
    libpipnode-core, libgobject, libglib, libc — **no gtk/gdk/cairo/
    pango/webkit/plplot/gtksourceview and no libpipnode-gui**; core links
    `-no-undefined` clean; 52/52 C unit tests, the D-Bus dial test and the
    plugin-load test PASS; **screenshot-confirmed** the dial Colours tab
    renders GtkColorButtons and a TableView draws ("waiting for table") in
    the editor while loading headless in `pipnode-run`. Phase 5 = commits
    4b9ca1f, a308bfe, e0de30e, f44f688, b1e6512.

- **Phase 4 DONE — the final four dual nodes split; ALL 23 of 23 are now
  core + gui.** Rate, Rewrite, Filedrop and File-Viewer (the special-case
  nodes) followed the established pattern: the GTK-free logic half (GType,
  properties, `receive`/source/emit + parsers + state, `get_size`,
  lifecycle, read seams) stays in `libpipnode-core`; the cairo painter
  and/or settings dialog move to a new gui-tier `pn-<name>-gui.c` exporting
  `pn_<name>_gui_install()`, wired into `pn_gui_install_builtin_nodes()`.
  - **Rate (4.20):** despite the expectation of a cairo+pango readout
    painter, Rate has **no painter and no `paint_*` vfunc** — its only GTK
    was the `build_property_editor` dialog, so it was a clean dialog-only
    cut. The whole dialog (read-only label rows for cached `rate` /
    `last-update`, incl. the `PangoAttrList` mono font, and the
    icon+ticker `PnCurrency` combos with gdk-pixbuf icon loading) → gui.
    The cache-aware periodic fetch + the receive() conversion stay core.
    **No read seam** (dialog binds the properties + the already-public
    `PN_TYPE_CURRENCY` / `pn_currency_get_icon_name`). **No colour pspec**
    — the body colour is a class field, not a property.
  - **Rewrite (4.21):** the JSON-template **GtkSourceView** code editor
    (`build_class_tab` + its text-buffer binding) → gui;
    `<gtksourceview/gtksource.h>` moved with it. **No WebKit was involved**
    (the help browser, not Rewrite, uses webkit). The placeholder
    expansion + receive() rewrite stay core. No read seam (binds the
    `template` property), no colour pspec.
  - **Filedrop (4.22):** the cairo drop-area painter → gui behind a
    `pn_filedrop_get_paint_state` snapshot; **the GTK drag-and-drop was
    NOT in this node** — it already lives in the gui-tier worksheet
    (`pn-worksheet.c`), which calls the public `pn_filedrop_drop_file()`
    (unchanged). The drop-emit logic stays core: it loads the path into a
    `GdkPixbuf` and emits the `PnImageMessage` / `PnMessage` — **gdk-pixbuf
    stays in core** (allowed image-data dep) for both the carrying logic
    and the snapshot pointer; only the cairo rendering went to gui. Both
    colour pspecs migrated `GDK_TYPE_RGBA` → `PN_TYPE_COLOR`.
  - **File-Viewer (4.23):** structurally identical to Filedrop. The cairo
    preview painter → gui behind `pn_file_viewer_get_paint_state`; the
    receive() (ref the incoming `PnImageMessage`'s pixbuf, read the
    filename hint) stays core (gdk-pixbuf in core). Both colour pspecs
    migrated to `PN_TYPE_COLOR`. The painter casts the snapshot's `PnColor`
    fields to `GdkRGBA*` for `gdk_cairo_set_source_rgba` (layout-identical).
  - **Carried debt is now RETIREABLE in Phase 5.** With every dual node
    split: (a) `pipnode-run` can drop `-Wl,--no-as-needed` and link
    core-only; (b) the `pn-flow.c` by-name `GdkRGBA` clause
    (`pn_flow_legacy_rgba_type()` at lines ~42/698/800) can go — a
    tree-wide grep confirms **no core or dual node carries a
    `GDK_TYPE_RGBA` pspec anymore**; every remaining `GDK_TYPE_RGBA` /
    `gdk_rgba_` in `lib/` is either a comment (`pn-color.{c,h}`,
    `pn-flow.c`) or lives in a **gui-tier** file that pn-flow never
    serialises as a node (`pn-table-view.c` — a GtkWidget, not a node;
    `pn-node-dialog.c` — the colour-editor selection; `pn-preferences.c` —
    serialised via its own keyfile path, not pn-flow). (c) The core
    factory's hard-registration of the gui node GTypes is the remaining
    Phase 5 item. **Phase 5 is now unblocked** (not done here).
  - **Verified per node:** whole tree builds clean (exit 0, no new
    warnings), 52/52 C unit tests, the D-Bus dial test PASS, the core
    `.so` `DT_NEEDED` stays GTK-free (now also pulling the allowed
    `libgdk_pixbuf-2.0` once Filedrop/File-Viewer moved into core) and each
    core `pn-<name>.o` carries zero `cairo_`/`gtk_`/`gdk_cairo`/
    `gdk_rgba_`/`pango_`/`gtk_source_`/`webkit_` refs (only the allowed
    `gdk_pixbuf_*`).

- **Phase 4 continues — the nine dialog-only dual nodes split
  (Expression, Expression2, Filter, Meshtastic, Notify, Ollama, Set,
  Sound, Tts; 19 of 23 dual nodes done).** These nodes confine their GTK
  to a settings-dialog vfunc (`build_property_editor` and/or
  `build_class_tab`) with **no cairo painter**, so the cut is purely the
  dialog: the GTK-free logic half (GType, all properties, `receive` +
  parsers + state, lifecycle) stays in `libpipnode-core`; only the dialog
  vfunc(s) + their helper callbacks move to a new gui-tier
  `pn-<name>-gui.c` exporting `pn_<name>_gui_install()`, wired into
  `pn_gui_install_builtin_nodes()`. **None of the nine has a colour
  property**, so there was no `GDK_TYPE_RGBA` → `PnColor` pspec migration
  in this batch.
  - **Expression (4.11) / Expression2 (4.12):** the full-width expression
    editor tab → gui; the editor resolves its pspec via
    `g_object_class_find_property` (no core seam). Evaluation/AST logic
    stays core.
  - **Filter (4.13) / Set (4.17):** the structured per-rule / per-
    assignment row editors (PnFilterRow/PnSetRow + serialise/rebuild) →
    gui; both read/write their single `rules` / `props` JSON string
    property purely via `g_object_get/g_object_set`, so no core seam. The
    rule-match / assignment logic + compiled caches stay core. (Still the
    Phase 7 schema candidates.)
  - **Meshtastic (4.14):** both dialog vfuncs (device/channel combos +
    the runtime status row) → gui; the status row reads busy / device /
    hw-model / last-busy-path / last-error through their read-only GObject
    properties and the dialog-only `format_hw_model()` table (its only
    caller) moved with it. The serial worker / protobuf + the public
    `pn_meshtastic_list_devices/list_channels/get_channel_cache` stay
    core.
  - **Notify (4.15):** the editable icon-name combo (curated freedesktop
    list) → gui, binding the `icon` property directly; the GDBus
    notification path stays core.
  - **Ollama (4.16):** the model combo + Refresh button → gui; its one
    core dependency, the GTK-free `/api/tags` enumeration
    `pn_ollama_list_models`, stays core and is now in the public header
    so the gui can call it (hostname/port/model read via properties).
  - **Sound (4.18):** the system-sound combo + GtkFileChooserButton
    composite editor → gui (reads/writes `sound`, calls the public
    `pn_sound_list_system_sounds` / `pn_sound_preview`). **Carried debt
    note:** removing `<gtk/gtk.h>` from core exposed that the
    GSubprocess playback had been getting `<gio/gio.h>` transitively
    through gtk — added the explicit gio include (gio is already a core
    dep). Same fix applied to Tts.
  - **Tts (4.19):** both dialog vfuncs (engine/voice/sink combos + status
    row + the debounced audio-preview) → gui. Needed the only **read seam
    of this batch**: a GTK-free engine-table accessor set
    (`pn_tts_n_engines` / `pn_tts_engine_id` / `_label` / `_installed` /
    `_label_for_id` / `_list_voices`) plus exposing `pn_tts_speak` in the
    header for the preview, so the dialog enumerates the file-static
    engine table and auditions without touching core internals. The
    already-exported parsers (`pn_tts_derive_voice_label`,
    `pn_tts_voice_index_for`) and all spawn/queue logic stay core; its
    unit test (which drives those parsers) needed no change.
  - **Verified per node:** whole tree builds clean (exit 0, no new
    warnings), 52/52 C unit tests, the D-Bus dial test PASS, the core
    `.so` `DT_NEEDED` stays GTK-free and each core `pn-<name>.o` carries
    zero `cairo_`/`gtk_`/`gdk_rgba_`/`pango_` refs.
  - **Carried debt still stands** (retires as the rest split): the
    `-Wl,--no-as-needed` on `pipnode-run`, the by-name `GdkRGBA` clause in
    `pn-flow.c`, and the `GDK_TYPE_RGBA` pspecs in the **4 remaining dual
    nodes** (Rate, Rewrite, Filedrop, File-Viewer). **Next:** those four,
    then Phase 5 (point `pipnode-run` at core only).

- **Phase 4 continues — Dial, Graph, Table, Chat, Text-View,
  Weather-Report split (10 of 23 dual nodes done).** Six more cairo-drawing
  dual nodes split the established way: the GTK-free logic half (GType,
  properties, `receive` + parsers + accumulation/state, timers/throttles,
  `get_size`, the read-only inspection seams) stays in `libpipnode-core`;
  the cairo/Pango (and, for Graph, PLplot) drawing + any settings dialog
  moves to a new gui-tier `pn-<name>-gui.c` exporting
  `pn_<name>_gui_install()`, wired into `pn_gui_install_builtin_nodes()`.
  - **Dial (4.5):** 7 colour pspecs → `PnColor`; painter + 4-tab dialog →
    gui behind a single `pn_dial_get_paint_state` snapshot (the
    `clock_to_cairo`/`value_to_angle`/tick helpers moved with it). The
    damped-spring needle animation + 30 Hz throttle stay core; the D-Bus
    dial test still PASSes against the gui-installed six-tab dialog.
  - **Graph (4.6):** 3 colour pspecs → `PnColor`; **all PLplot is gui-tier
    only.** The per-topic series ring structs + `PN_GRAPH_MAX_BINS/SAMPLES`
    are published in the header (plain data) so the painter walks the same
    rings; a scalar snapshot + `pn_graph_collect_series_sorted` /
    `pn_graph_resolution_seconds` / `pn_graph_bin_width_us` are the
    data-access seam. The per-instance PLplot stream is now owned entirely
    by the gui tier (lazy `plmkstrm` on first paint, torn down via a
    GObject-data destroy-notify), so the core never calls PLplot. Series
    fan-out, the `PN_GRAPH_MAX_SERIES` cap and `pn_graph_get_series_count`
    stay core.
  - **Table (4.7):** only `pn-table.c` split (model already core, view
    already gui; no held GtkWidget so the cut is clean). 5 colour pspecs →
    `PnColor`; `PnTableColumn`/`PnTableRow` published; row/`#limit`
    accumulation + `pn_table_get_row_count` + the `scroll` vfunc (just an
    int) stay core; painter → gui, with scroll clamping crossing back via
    `pn_table_clamp_scroll_offset`.
  - **Chat (4.8):** 6 colour pspecs → `PnColor`; self-loop suppression +
    bubble accumulation + focus flag + caret-blink timer + submit +
    `pn_chat_get_bubble_count` stay core. The one genuinely GDK-needing
    piece — `pn_chat_handle_key_press` (reads keyvals) — moved to gui, and
    the draft mutation it does is now GTK-free core primitives
    (`pn_chat_draft_insert/backspace/delete`, `pn_chat_caret_*`) the gui
    handler dispatches into; painter + bubble struct → gui/header.
  - **Text-View (4.9):** 2 colour pspecs → `PnColor`; line-split snapshot
    + passthrough re-emit + `scroll` vfunc stay core; painter → gui, scroll
    clamping via `pn_text_view_clamp_scroll_offset`.
  - **Weather-Report (4.10):** 4 colour pspecs → `PnColor`; the unit +
    gradient enum typedefs published in the header (GType registrations
    stay core); `receive` + data snapshot + header-glyph mirroring +
    `condition_glyph` + clock tick stay core; the whole card painter + its
    unit converters / JSON readers (duplicated as painter-local statics) →
    gui behind `pn_weather_report_get_paint_state` + `pn_weather_report_peek_data`.
  - **Verified per node:** whole tree builds clean (exit 0, no new
    warnings), 52/52 C unit tests, the D-Bus dial test PASS, the core `.so`
    `DT_NEEDED` stays GTK-free and each core `pn-<name>.o` carries zero
    `cairo_`/`gtk_`/`gdk_rgba_`/`pango_`/`pl` (plplot) refs.
  - **Carried debt still stands** (retires as the rest split): the
    `-Wl,--no-as-needed` on `pipnode-run`, the by-name `GdkRGBA` clause in
    `pn-flow.c`, and the `GDK_TYPE_RGBA` pspecs in the remaining ~13
    dialog-only / display dual nodes. **Next:** the dialog-only dual nodes,
    then Phase 5 (point `pipnode-run` at core only).

- **Phase 4 continues — Switch, Knob, Analog-Meter split (4 of 23 dual
  nodes done, counting LED).** Each followed the LED pattern exactly: the
  GTK-free logic half (GType, properties, `receive`/source-emit +
  startup-announce, lifecycle, `get_size`, hit-tests) stays in
  `libpipnode-core`; the cairo drawing + any settings-dialog override moves
  to a new gui-tier `pn-<name>-gui.c` exporting `pn_<name>_gui_install()`,
  which is added to `pn_gui_install_builtin_nodes()` in `pn-gui.c`.
  - **Switch (4.2):** no colour property, so no pspec migration; the slider
    painter (`pill_path`, `paint_switch`, `paint_header_overlay`) +
    `paint_right_decoration_width` moved to gui, which reads the latch via
    the existing public `pn_switch_get_on()`. The subclass vfuncs
    (`apply_visual_state`, `build_outbound_message`) and the
    startup-announce one-shot stay core.
  - **Knob (4.3):** no colour property; the dial painter (`paint_knob`,
    `paint_header_overlay`), the pointer sweep angles +
    `paint_right_decoration_width` moved to gui. One GTK-free read seam
    `pn_knob_get_value_fraction()` (wrapping the pure `value_fraction`
    helper) lets the painter read the derived pointer position. Wheel-emit
    + startup-announce stay core.
  - **Analog-Meter (4.4):** the biggest split. Five colour properties
    migrated `GDK_TYPE_RGBA` → `PN_TYPE_COLOR` (instance fields, the shared
    `set_color_prop` helper's `gdk_rgba_equal`→`pn_color_equal`, the five
    pspecs, the init defaults, the body-colour cast dropped). The whole
    panel-meter painter (case/face/ticks/needle/glyphs/pivot/labels, the
    `paint_plot` vfunc + `paint_plot_skip_shadow`/`paint_plot_skip_zoom`
    flags) AND the three-tab settings dialog (`build_class_tabs` +
    `build_am_page`) moved to gui. Because the painter touches ~16 private
    fields plus the derived `value_to_angle`, a single snapshot read seam
    `pn_analog_meter_get_paint_state(self, PnAnalogMeterPaintState *out)`
    (struct declared in the public header) replaces the per-field accessor
    approach; the angle helpers (`clock_to_cairo`, `value_to_angle`) moved
    to gui and now read the snapshot. The damped-spring animation + repaint
    throttle stay core.
  - **Verified per node:** whole tree builds clean (exit 0, no new
    warnings), 52/52 C unit tests, the D-Bus dial functional test PASS, the
    core `.so` `DT_NEEDED` stays GTK-free and each core `pn-<name>.o`
    carries zero `cairo_`/`gtk_`/`gdk_rgba_` refs; `nm` shows the logic
    symbols in `libpipnode-core.so` and `pn_<name>_gui_install` in
    `libpipnode-gui.so`.
  - **Carried debt still stands** (retires as the rest split): the
    `-Wl,--no-as-needed` on `pipnode-run`, the by-name `GdkRGBA` clause in
    `pn-flow.c`, and the `GDK_TYPE_RGBA` pspecs in the remaining ~19 dual
    nodes. **Next:** the remaining cairo-drawing dual nodes (Graph, Dial,
    Table, Chat, Text-View, Weather-Report) and the dialog-only ones, then
    Phase 5 (point `pipnode-run` at core only).

- **Phase 4 STARTED — first dual node split: LED (the pattern).** `pn-led.c`
  is now the GTK-free logic half (GType, properties, `receive`, hold-timer,
  `get_size`) and lives in `libpipnode-core`. Its colour property migrated
  `GDK_TYPE_RGBA` → `PN_TYPE_COLOR` (the instance field, get/set, the pspec,
  and the init default), so the node carries no GDK type at all; the
  serializer round-trips it through the existing `PN_TYPE_COLOR` branch
  (`rgb(28,113,216)` in `examples/staircase.json` loads + runs headless
  unchanged). The cairo drawing (`paint_led`, `paint_header_overlay`), the
  reserved-label-margin constant (`paint_right_decoration_width`) and the
  settings-dialog customisation (`build_property_editor` + its hold-field
  sensitivity tracking) moved to a **new gui-tier `pn-led-gui.c`**. Because
  the painter can't see the core file's private instance struct, two
  GTK-free read seams were added to core (`pn_led_get_lit`,
  `pn_led_peek_color`), matching the existing inspection-seam pattern.
  - **The install mechanism (the reusable Phase 4 seam):** `pn-led-gui.c`
    exports `pn_led_gui_install()`, which `g_type_class_ref`s `PN_TYPE_LED`
    and writes the three gui vfunc/data slots onto the class (keeping the
    ref for process lifetime, like the factory). A new gui-tier
    **`pn-gui.c`** holds `pn_gui_install_builtin_nodes()`, which calls each
    split node's `_gui_install()`; the **editor** calls it once in
    `src/main.c` after the factory registers the built-ins. `pipnode-run`
    never calls it, so the LED's logic loads and runs with no GTK and NULL
    paint/dialog slots. This is the in-tree counterpart of Phase 6's
    per-plugin `pn_plugin_gui_init()` companion entry point — each
    remaining dual node adds one `_gui_install()` + one line in `pn-gui.c`.
  - `lib/Makefile.am`: `pn-led.{c,h}` moved to the core source + core
    public-header lists; `pn-led-gui.{c,h}` + `pn-gui.{c,h}` added to the
    gui lists. **Verified:** `nm` shows `pn_led_{new,receive,get_lit,
    peek_color,get_type}` in `libpipnode-core.so` and
    `pn_{led_gui_install,gui_install_builtin_nodes}` in `libpipnode-gui.so`;
    `objdump -p libpipnode-core.so` `DT_NEEDED` stays GTK-free and the core
    `pn-led.o` carries zero `cairo_`/`gtk_`/`gdk_rgba_` refs.
  - Regression net green: whole tree builds (exit 0, no warnings from the
    split), 52/52 C unit tests (incl. `test-pn-led`, whose colour test
    moved `gdk_rgba_*` → `pn_color_*` for the boxed `PN_TYPE_COLOR` value),
    the D-Bus dial functional test PASS, `pipnode-run` runs the LED
    worksheet headless, and the editor draws the LED identically (verified
    by screenshot: icon + panel-mount disc + reserved label margin).
  - **Carried debt still present** (retires as more dual nodes split): the
    `-Wl,--no-as-needed` on `pipnode-run`, the by-name `GdkRGBA` clause in
    `pn-flow.c`, and the ~44 remaining `GDK_TYPE_RGBA` pspecs in the other
    22 dual nodes. **Next: batch the remaining cairo-drawing dual nodes**
    (Switch, Knob, Analog-Meter, then Graph/Dial/Table/Chat/Text-View/
    Weather-Report) and the dialog-only ones, each following the LED
    pattern; then Phase 5 (point `pipnode-run` at core only).

- **Phase 3 increment 3.2 DONE — the build is split into two libraries.**
  `lib/Makefile.am` now builds `libpipnode-core.la` (43 sources, the §6
  core tier) and `libpipnode-gui.la` (31 sources, dual + gui tiers,
  `LIBADD libpipnode-core.la`). Core compiles with **no GTK on the command
  line** — its `CFLAGS` are only `GLIB`/`JSON_GLIB`/`LIBSOUP`/`GMODULE`/
  `GDK_PIXBUF`. **Verified: `objdump -p libpipnode-core.so` `DT_NEEDED` is
  json-glib, soup, gio, gmodule, gobject, glib, m, c — zero gtk/gdk-widget/
  cairo/pango/webkit/plplot/gtksourceview** (gdk-pixbuf is even
  `--as-needed`-dropped, since image-message treats the pixbuf as an opaque
  GObject). The gui lib correctly needs core + gtk + plplot + webkit.
  - `configure.ac`: explicit `PKG_CHECK_MODULES([GLIB], glib/gobject/gio)`
    + `[GDK_PIXBUF]`; generates `pipnode-core.pc` + `pipnode-gui.pc` +
    `pipnode.pc` (now a compat alias requiring `pipnode-gui`, so existing
    GTK plugins keep building; the m4 plugin macro still reads it).
  - All consumers repoint from the gone `libpipnode.la` to
    `libpipnode-gui.la` + explicit `libpipnode-core.la` (src binaries,
    `tests/unit`, the 5 bundled plugins, the echo test plugin).
  - **Intermediate scaffolding:** the core factory still hard-registers the
    GUI-tier node GTypes (and consults `PnPreferences`), so `libpipnode-
    core.so` carries undefined gui symbols. Under Debian/Ubuntu's default
    `--as-needed` that drops gui from the headless `pipnode-run` link, so
    `pipnode-run` + `tests/unit` use `-Wl,--no-as-needed` to keep gui
    linked. This goes away in Phase 4/5/6 (dual logic → core; gui nodes
    self-register; pipnode-run core-only). For now both binaries link both
    libraries and every node still works in both.
  - Whole tree builds (exit 0); 52/52 unit tests + the D-Bus dial test
    green; all 5 plugins build; `pipnode-run` loads + runs a worksheet;
    the three `.pc` files validate. **Next: Phase 4 — split the dual-node
    sources (logic→core, drawing+dialog→gui), starting with the simplest
    (LED) as the pattern.** That also lets the §6 colour-pspec migration
    (`GDK_TYPE_RGBA`→`PN_TYPE_COLOR`) and the by-name clause in pn-flow
    finally retire.

- **Phase 3 STARTED — increment 3.1: core serializer + factory de-GTK'd
  (prereq for the library split).** Splitting `lib/` into a GTK-free core
  needs the two core files that still pulled GTK to stop:
  - `pn-flow.c` (the serializer — must be core) dropped `#include
    <gtk/gtk.h>`. Its two `GDK_TYPE_RGBA` (de)serialise branches — there
    for the dual nodes' not-yet-migrated colour properties — folded into
    the existing `PN_TYPE_COLOR` branches, resolving the legacy boxed type
    by name (`g_type_from_name("GdkRGBA")`) and reading its value through a
    `PnColor *` (layout-identical; `pn_color_*()` byte-compatible with
    `gdk_rgba_*()`). Verified: a Dial `needle-color` round-trips through
    `save`/`load_from_file` to the identical on-disk `"rgb(51,102,153)"`
    and back. This pulls only the *serializer's* slice of Phase 4 forward;
    the bulk pspec migration (`GDK_TYPE_RGBA`→`PN_TYPE_COLOR`, ~45 specs)
    stays in Phase 4, where the allocator-safe `g_object_get` free sites
    move with the dual-node split. The by-name clause becomes dead then.
  - `pn-node-factory.c` (core registry) dropped `#include
    "pn-preferences.h"` (a GTK-pulling GUI header) — it only needed the
    plugin-disabled check; forward-declared the opaque `PnPreferences` +
    its two functions, which resolve from the gui lib at link (both libs
    link into the binaries). Phase 5/6 replaces this with a core plugin-
    policy seam. `pn-node-factory.o` and `pn-flow.o` now carry zero
    `gtk_`/`gdk_rgba_` refs. Tree builds (still single lib), 52/52 unit
    tests + the D-Bus dial test green. **Next: increment 3.2 — the actual
    two-library split (configure.ac core/gui deps, `lib/Makefile.am` two
    libraries, pkg-config split, relink).** D1 refined: `gdk-pixbuf` is an
    allowed core dep (see §6 decision 1).

- **Phase 2 DONE — tier boundary re-measured (see §6 table).** A direct-
  call scan (`gtk_`/`gdk_`/`cairo_`/`pango_`/`webkit_`/`gtk_source_`
  tokens, not transitive includes) cross-checked against each file's flow
  role classified all 74 `lib/pn-*.c` into **43 core · 23 dual · 8 gui** —
  comfortably above TODO #23's pre-Phase-1 ~35 estimate, confirming the
  header de-GTK paid off. Heuristic edge cases resolved by inspection:
  `pn-color.c`'s lone `gdk_` is a comment (core); `pn-flow.c` keeps a
  *temporary* GTK include + `GDK_TYPE_RGBA` serializer branch by design
  (core-destined, cleaned in Phase 4); the 11 "dialog-only" dual nodes
  have their GTK confined to a settings-dialog vfunc (clean Phase-4 cut /
  Phase-7 schema candidates); `pn-knob.c` is dual despite no `receive()`
  (manual source with core emit logic); and `filedrop`/`file-viewer` are
  `PnNode`s, so their GType+property half stays core-loadable (dual, not
  pure gui). Three open decisions recorded for the review gate — the
  notable one: **may core link `gdk-pixbuf`** for `PnImageMessage` (no GTK
  footprint; recommend yes). No code moved; classification only.

- **Phase 1 DONE — the type headers are de-GTK'd (ABI-stable).** The
  proven recipe is now applied to the real tree:
  - New `lib/pn-color.[ch]`: `PnColor` (4×`double` r,g,b,a, same member
    names as `GdkRGBA`) as a boxed `PN_TYPE_COLOR`, with
    `pn_color_parse()` / `pn_color_to_string()` (byte-compatible with
    `gdk_rgba_to_string()` so saved worksheets round-trip unchanged),
    `pn_color_equal()` / `_copy` / `_free`.  Pure gobject — no GDK.
  - `lib/pn-node.h` (the plugin/class ABI) no longer includes
    `<gtk/gtk.h>` / `<gdk/gdk.h>`: it pulls `<glib-object.h>` +
    `pn-color.h`, forward-declares the pointer-only `cairo_t` /
    `GtkWidget` / `GtkWindow` / `GtkNotebook` (struct tags matching GTK's
    so the GUI tier's real headers coexist), and the vtable's by-value
    `GdkRGBA color` + the three `pn_node*_color` signatures are now
    `PnColor`.  **`pn-node.c` compiles GTK-free.**
  - Base colour surface migrated: `pn-node.c` (storage/property/accessors
    → `PnColor`, `PROP_COLOR` spec → `PN_TYPE_COLOR`, `gdk_rgba_equal` →
    `pn_color_equal`), `pn-node-factory.[ch]`, and the `pn-http`/`pn-ws`
    base classes (dropped their `<gdk/gdk.h>`, `normal_color` → PnColor).
  - `pn-flow.c` serializer gained a `PN_TYPE_COLOR` branch
    (`pn_color_to_string`/`parse`) for the base node colour; it keeps the
    `GDK_TYPE_RGBA` branch (and a temporary GTK include) for the
    dual-nodes' own colour properties, which move to PnColor in Phase 4.
  - ~110 logic/image/plugin files: mechanical `GdkRGBA` → `PnColor`
    rename (they only ever used it as the class body colour).  The
    dual/GUI "boundary" files (chat, dial, graph, table(-view),
    text-view, analog-meter, led, filedrop, file-viewer, weather-report,
    node-dialog, worksheet) keep `GdkRGBA` internally and cast
    `PnColor* <-> GdkRGBA*` at the base-colour touchpoints; the dual
    nodes that lost their transitive GTK (drawing via cairo / dialog
    widgets) gained an explicit `#include <gtk/gtk.h>` (correct for the
    still-monolithic Phase 1 lib; Phase 4 splits them properly).
  - New permanent guard `tests/unit/test-pn-color.c`: compile-time
    `G_STATIC_ASSERT`s that `PnColor` is layout-identical to `GdkRGBA`,
    plus parser/serializer **cross-checks against gdk_rgba_parse/
    to_string** (the risk-register mitigation) and the lenient-superset
    forms.  `./run-unit-tests.sh` green at **52/52**; full tree builds
    with **zero warnings**; `tests/test_node_dialog_dial.py` green.  No
    behaviour change.  Phase 2 (re-measure the tier boundary) is next.

- **IO-node parsers extracted + tested (TODO #24, Phase 0 fill-out).**
  The pure parse/aggregate logic of the auto-trigger IO nodes — whose
  receive/trigger logic is core-bound but was untestable headless
  because `trigger()` does real disk/network I/O — is now exposed
  (non-static) and unit-tested on canned fixtures, per the
  `pn-disk-io`/`pn-net-io` precedent:
  - **Temp** — `pn_temp_parse_remote_lines` (one
    `name|label|millidegrees` line each → readings) +
    `pn_temp_aggregate` (AVERAGE/MAXIMUM collapse, min/max/avg, hottest
    label); `TempReading` → `PnTempReading` in the header.
  - **Http** — `pn_http_split_body_and_status` (curl body + status via
    sentinel) was already exported; added its test.
  - **Weather** — extracted `pn_weather_parse_current` (Open-Meteo
    `current` reading + provider error `reason` out of a parsed object)
    from inside `emit_message` (behaviour-preserving) and exported
    `pn_weather_code_description` (WMO code → label).
  - **Tts** — `pn_tts_derive_voice_label` (piper model filename →
    label; refactored to take an engine id, not the private struct) +
    `pn_tts_voice_index_for` (deterministic sender → voice slot).
  - **Net-IO** — already done (`pn_net_io_parse_net_dev` exported +
    tested); the per-second delta/rate stays in the trigger (entangled
    with the monotonic clock + prev-sample state), not a clean pure cut.

  Standardised on **declaring the exported parsers in the node headers**
  (the TODO's wording; net-io/disk-io historically forward-declared in
  the test). Four new binaries (`test-pn-{temp,http,weather,tts}`);
  `./run-unit-tests.sh` green (51/51). This closes the Phase 0
  characterization net — the header de-GTK surgery (Phase 1) can now
  proceed against a behaviour baseline.

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

### Phase 1 — De-GTK the type headers (ABI-stable) — **DONE** (see §0)

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

### Phase 2 — Re-measure the tier boundary — **DONE** (see §0 + §6 table)

**Objective:** with `pn-node.h` no longer dragging GTK in transitively,
get the *true* list of GTK-free `.c` files (the pre-Phase-1 token scan
over-counts; TODO #23 estimated ~35 of 71 are genuinely clean).

**Result:** **43 core · 23 dual · 8 gui** of 74 `lib/pn-*.c`; full table +
three open decisions in §6. Awaiting the review gate before Phase 3.

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

### Phase 3 — Split the build into two libraries — **DONE** (see §0)

**Objective:** produce `libpipnode-core.la` (GObject + json-glib + soup)
and `libpipnode-gui.la` (links core + the GTK stack).

**Result:** done in two increments — 3.1 freed the core serializer/factory
from GTK; 3.2 built the two libraries. `libpipnode-core.so`'s `DT_NEEDED`
is GTK-free (verified by `objdump`). The `--disable-gui` server build (the
optional bullet in step 1) is deferred to Phase 5, when `pipnode-run` goes
core-only and the core↔gui symbol debt is gone; doing it now would be
moot while the dual logic still lives in the gui tier.

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

### Phase 4 — Split the dual-nature node sources (D3) — **DONE** (see §0)

All 23 of 23 dual-nature nodes are split (logic → core, drawing + dialog
→ gui). The carried debt (the `-Wl,--no-as-needed` on `pipnode-run`, the
`pn-flow.c` by-name `GdkRGBA` clause, and the core factory's gui-GType
hard-registration) is now retireable in Phase 5 — see §0.

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

### Phase 5 — Wire the two binaries to the two tiers — **DONE** (see §0)

**Objective:** `pipnode-run` links core only; `pipnode-editor` links core
+ gui.

**Result:** done in four increments (5.1–5.4, see §0). The central work
was making `libpipnode-core` self-contained: splitting the missed 24th
dual node (Table-View), replacing the factory's direct `PnPreferences`
calls with a core-side plugin-policy hook, and retiring the `pn-flow`
legacy `GdkRGBA` clause — clearing core's three undefined gui symbols.
Then `pipnode-run` + the unit tests dropped to core-only linkage and core
gained `-no-undefined`. `src/pn-run.c` was already GTK-free (confirmed).

**Steps (as executed)**
1. `src/Makefile.am`: `pipnode_run_LDADD` → core only (+ `GLIB_LIBS`),
   `pipnode_run_CFLAGS` `GTK_CFLAGS`→`GLIB_CFLAGS`, dropped
   `-Wl,--no-as-needed`. `pipnode_editor` unchanged (core + gui).
2. `lib/Makefile.am`: core builds `-no-undefined`; moved
   `pn-table-view.{c,h}` to core, added `pn-table-view-gui.{c,h}` to gui.
3. `tests/unit/Makefile.am`: core-only linkage, dropped gui +
   `-Wl,--no-as-needed`.

**Verification:** `objdump -p src/.libs/pipnode-run` `NEEDED` carries no
`libgtk`/`libgui`; it runs a worksheet headless; the editor is unchanged
for the user (screenshot-confirmed). 52/52 unit tests + dial + plugin-load
PASS.

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

## 6. Appendix — file tier classification (Phase 2 result)

Method: direct-call scan over `lib/pn-*.c` for `gtk_`/`gdk_`/`cairo_`/
`pango_`/`webkit_`/`gtk_source_` tokens (not transitive includes), cross-
checked against each file's flow role (`receive()` / `emit` / source). The
post-Phase-1 count is **43 core · 23 dual · 8 gui** of 74 files — above the
TODO #23 pre-Phase-1 estimate of ~35 clean, confirming the header de-GTK
landed.

Tier meanings (per D1/D3): **core** = compiles into `libpipnode-core` with
zero GTK; **dual** = a `PnNode` whose logic/`receive`/source half is core
and whose cairo drawing + settings dialog move to the gui tier (split in
Phase 4 / schema'd in Phase 7); **gui** = editor infrastructure or a pure
widget with no `PnNode` flow role — moves wholesale to `libpipnode-gui`.

### Core (43) — move wholesale to `libpipnode-core`

Infra: `pn-node.c`, `pn-node-factory.c`, `pn-node-store.c`, `pn-message.c`,
`pn-image-message.c` ⚠, `pn-wire.c`, `pn-wire-store.c`, `pn-connections.c`,
`pn-edge.c`, `pn-var-store.c`, `pn-load.c`, `pn-color.c`, `pn-flow.c` ⚠.

Logic nodes / helpers: `pn-ambient.c`, `pn-auto-injector.c`,
`pn-auto-random.c`, `pn-auto-trigger.c`, `pn-comparator.c`, `pn-cpu.c`,
`pn-debug.c`, `pn-dedup.c`, `pn-delay.c`, `pn-disk-io.c`, `pn-expr-parser.c`,
`pn-format.c`, `pn-http.c`, `pn-inject.c`, `pn-jmespath.c`, `pn-json-path.c`,
`pn-memory.c`, `pn-net-io.c`, `pn-query.c`, `pn-rtc.c`, `pn-staircase.c`,
`pn-stats.c`, `pn-subst.c`, `pn-table-model.c`, `pn-temp.c`,
`pn-threshold.c`, `pn-throttle.c`, `pn-watchdog.c`, `pn-weather.c`,
`pn-ws.c`.

⚠ `pn-color.c` — its only `gdk_` token is a comment; truly GTK-free.
⚠ `pn-flow.c` — carries a **temporary** `#include <gtk/gtk.h>` + a
`GDK_TYPE_RGBA` serializer branch for the dual nodes' own colour props;
both vanish in Phase 4 when those props become `PnColor`. Core-destined.
⚠ `pn-image-message.c` — code is GTK-free but `pn-image-message.h` carries
a `GdkPixbuf *` (pulls `gdk-pixbuf-2.0`, which is **not** GTK but is a
graphics dep). **OPEN DECISION** (see below).

### Dual (23) — split per Phase 4: logic→core, drawing+dialog→gui

Cairo-drawing nodes (10): `pn-analog-meter.c`, `pn-chat.c`, `pn-dial.c`,
`pn-graph.c`, `pn-knob.c` †, `pn-led.c`, `pn-switch.c`, `pn-table.c`,
`pn-text-view.c`, `pn-weather-report.c`.

Dialog-only nodes (11) — GTK confined to a settings-dialog vfunc, so a
clean cut (and prime candidates for the Phase 7 schema): `pn-expression.c`,
`pn-expression2.c`, `pn-filter.c`, `pn-meshtastic.c`, `pn-notify.c`,
`pn-ollama.c`, `pn-rate.c` ‡, `pn-rewrite.c` §, `pn-set.c`, `pn-sound.c`,
`pn-tts.c`.

Display-only nodes (2): `pn-filedrop.c` (source — emits a dropped file/
image msg), `pn-file-viewer.c` (sink — receives & previews). Both are
`PnNode`s, so the **core half is just GType + property storage** (+ the
viewer's trivial `receive`) to keep worksheets loadable headless; drawing +
drag-drop go to gui.

† `pn-knob.c` has no `receive()` (it's a manual source) but holds core
emit/startup-announce logic — dual, not gui.
‡ `pn-rate.c` also uses pango for its readout text.
§ `pn-rewrite.c`'s `webkit_`/`gtk_source_` tokens are its dialog code
editor — dialog-tier, splits cleanly.

### GUI (8) — move wholesale to `libpipnode-gui` (no `PnNode` flow role)

Editor infra: `pn-node-dialog.c` (the auto-dialog framework; Phase 7's
schema renderer lands here), `pn-document-settings-dialog.c`,
`pn-preferences.c`, `pn-preferences-dialog.c`, `pn-palette.c`,
`pn-help-browser.c` (webkit), `pn-worksheet.c` (the canvas renderer).

> **Correction (Phase 5):** `pn-table-view.c` was listed here as a "pure
> widget, not a node" — that was wrong. `PnTableView` is a full sink
> `PnNode` with `receive()` logic (it renders the latest received
> `data.table`), so it is the **24th dual node** and was split in Phase
> 5.1 (logic → core, cairo painter → `pn-table-view-gui.c`). The true
> final tally is **44 core · 24 dual · 7 gui**.

### Open decisions for the Phase 2 review gate

1. **`gdk-pixbuf` in core? — DECIDED: YES (option a).** `PnImageMessage`
   carries a `GdkPixbuf *`. `gdk-pixbuf-2.0` pulls no GTK/GDK widget code
   but is still a graphics library, so strict D1 ("zero graphics") was
   ambiguous. Resolved 2026-05-23 (Laszlo): **core may link `gdk-pixbuf`**
   — it's how image messages flow between logic nodes headlessly, and it
   has no toolkit footprint. D1 is hereby refined to "no GTK/GDK widget
   toolkit, cairo, pango, webkit, gtksourceview, plplot"; `gdk-pixbuf` is
   an allowed core image-data dep. The core dep set is therefore: glib,
   gobject, json-glib, libsoup, **gdk-pixbuf-2.0**. (Rejected: (b) opaque
   pixel ref — needless churn; (c) move to gui — loses headless image
   flow.)
2. **Display-only dual nodes** (`filedrop`, `file-viewer`): confirm the
   agreed shape is "core registers the GType + props so headless load
   succeeds; all visuals in gui." (Assumed above.)
3. **`pn-flow.c` temp GTK**: tracked — removed in Phase 4 with the dual
   nodes' colour-prop migration to `PnColor`. No action in Phase 3.
