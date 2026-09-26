# Sinks

Sinks are the terminal display/render/act nodes of a pipnode flow. Each is a
`GObject` subclass of `PnNode` in the `Sinks` category. A sink **consumes** the
`PnMessage` that arrives on its input port (`has_input = TRUE`) and turns it
into something the user sees, hears, or that the desktop acts on — a plotted
curve, a card, a table, a desktop notification, spoken speech, a sound clip, a
panel readout. Most have no output (`has_output = FALSE`), so the message
journey ends here; the exceptions (Chat, Text View) carry an output port too
and are noted below.

A sink reads whatever member of the schemaless `data.*` bag fits its job:
text displays read `data.output`; numeric plots read `data.value` (or a
configurable JSON path); Table reads each configured `Title:path`; Table View
reads a structured `data.table`; Debug Print can serialise the entire
envelope+bag as JSON. The exact key each one reads is documented per node.

Display sinks paint with cairo/Pango (PLplot for the 2D plot family, **MathGL for Graph's 3D stacked-Z views**, and pure cairo for Oscilloscope and Figure) in a
companion `pn-<name>-gui.c` "gui tier" that is installed onto the class only in
the editor build — the headless core never pulls GTK. Several plot/card sinks
share a common 280×173 footprint (a 40 px header, a 4 px gap, then a body)
deliberately so a row of mixed Graph / XY Graph / Plot / Weather Report /
Sun Path / Table / Table View / Text View nodes lines up cleanly on the canvas.
**Chat is not one of them** — its body is 220 px (`PN_CHAT_BODY_HEIGHT`,
`lib/pn-chat.c:40`) — and Oscilloscope (260×254) and Figure (280×254) have
footprints of their own.

**LED** is a Sinks-category node too, but it is documented with the indicator
family in [`gui-displays-gauges.md`](gui-displays-gauges.md).

---

## Debug Print

**Purpose** — Print every received message to a chosen destination in a chosen
format. The flow-inspection tool of first resort. `lib/pn-debug.c:400`
(`pn_debug_receive`).

**When to use** — To see what is actually on a wire. Pick **Debug Print** (not
Text View) when you want the *envelope* — topic, id, created, source, the whole
`data` bag as JSON — rather than a rendered `data.output` string, and when you
want the output in the collapsible debug pane / status bar / stdout / stderr
rather than painted on the canvas.

**Ports** — input only; no output. `lib/pn-debug.c:508`+.

**Settings**
- `target` (enum, default **Standard Error**) — where rendered text goes:
  *Debug View* (the collapsible pane from View→Debug View), *Status Bar* (main
  window footer), *Standard Output*, *Standard Error*. `lib/pn-debug.c:532`.
- `format` (enum, default **JSON**) — how each message renders:
  *text* = just `data.output`; *oneliner* = compact human trace
  `[name] from= topic= id= created= data={…}`; *JSON* = one pretty JSON object
  wrapping type/from/from_id/topic/id/created/data. `lib/pn-debug.c:539`,
  formatters at `lib/pn-debug.c:329` (oneliner) / `:359` (JSON) / `:375` (text).
  The JSON and oneliner formats **collapse `$pnvector` markers** into a bounded
  sample like `"[0, 1, 2, …] (256 values)"` rather than dumping the buffer
  (`marker_sample_string`, `lib/pn-debug.c:137`; `humanize_vectors`, `:161`).

**Renders / acts** — Formats the message per `format` then writes it: stdout via
`g_print`, stderr via `g_printerr`, or emits the `status-message` /
`debug-message` signal the worksheet forwards to the footer / debug pane.
`lib/pn-debug.c:400`+.

**Gotchas** — *Debug View target needs the pane wired*: the `debug-message`
signal only reaches the collapsible pane when the worksheet/main window is
listening, i.e. the Debug View is open and connected — the pane stays empty
otherwise (see the project memory on "Debug View needs the Debug node to target
it"). The inherited `topic` settings row is hidden. text-format on a message
with no string `output` emits a blank line.

---

## Logger

**Purpose** — Appends every message that reaches it to a log file, with **internal** size-based rotation — no external `logrotate` binary is ever invoked. (`lib/pn-logger.c`)

**When to use** — Keeping a durable record of a flow: an audit trail, an overnight capture you will grep in the morning, evidence that a sensor really did go quiet at 03:00. Vs **Debug Print**, which is a transient on-canvas pane; vs **Pipe Writer**, which hands bytes to another process rather than to a rotated file.

**Tier** — **No gui tier at all**: there is no `pn-logger-gui.c`, so the node behaves identically under `pipnode-run` and in the editor.

**Ports** — one unnamed input, no output (`pn-logger.c:545`).

**Settings** (`pn-logger.c:564`+) — a **single page, no tabs** (`:548`).
- `file-path` (string, default `""`, `PN_EDITOR_FILE`) — `~` is expanded via `pn_path_expand` (`:438`). **Empty disables logging entirely.** Parent directories must already exist.
- `format` (enum `PnLoggerFormat`, default **`Lines`**; nicks `Lines` / `JSON`, `:79`).
- `logrotate` (bool, default **TRUE**) — rotate internally at the size cap.
- `max-size-mb` (int, **1–G_MAXINT, default 10**).
- `max-files` (int, **0–1000, default 5**) — `0` means no archives at all, just a `g_unlink` (`:293`).
- `flush` (bool, default **FALSE**) — fsync-per-line rather than buffered.

`max-size-mb` and `max-files` are greyed out when `logrotate` is off, via `pn_settings_schema_enable_when_truthy(…, "logrotate")`. Unlike Plot and Oscilloscope, the inherited `topic` row is **not** hidden.

**Reads** — In `Lines` format, `data.output` (missing or non-string ⇒ an empty payload, `read_output`, `:116`); and `data.success`, falling back to `data.value > 0.5`, defaulting to SUCCESS when neither is present (`:137`). In `JSON` format the whole envelope is serialised as one line: `type`, `from`, `from_id`, `topic`, `id`, `created`, `data` (`message_to_json_object`, `:166`). **Writes nothing** — pure sink.

**Line format** — `"%Y-%m-%dT%H:%M:%S SUCCESS|FAILURE <payload>\n"`, in **local** time (`:354`).

**Gotchas**
- A fresh Logger is **red with `❗` until `file-path` is set**: `constructed()` calls `pn_node_set_has_error (node, TRUE)` (`:641`), and clearing the path re-reddens it (`:443`).
- The stream is held open across messages, and `cur_size` is seeded from `g_stat`, so a restart rotates against the file's real size rather than from zero.
- An open failure is warned **once per path** via `g_warning` (`:263`) — invisible from a desktop launcher, and there is no output port to report on, so a bad path silently drops everything. Check the node body for red.
- Rotation happens *after* the write, so the live file can overshoot the cap by one line.
- Help page `data/help/PnLogger.html`. No example worksheet ships with it.

---

## Graph

**Purpose** — Summarise one numeric value plucked from each message and plot it
over a rolling time window (time-series) or as a value histogram
(distribution). `lib/pn-graph.c:728` (`pn_graph_receive`). Plotting is done
with **PLplot** for the 2D views and **MathGL** for the 3D stacked-Z views, both
in `lib/pn-graph-gui.c`.

**When to use** — When the X axis should be the message's *arrival time* (or a
value-frequency distribution). Use **Graph** for "this number over time"; use
**XY Graph** instead when each message carries an explicit (x, y) pair.

**Ports** — input only; no output. `lib/pn-graph.c:1674`.

**Settings** (two dialog tabs: *Appearance* | *Data*, `lib/pn-graph.c:1838`)
- `key` (string, default `data/value`) — `/`-separated JSON path to the numeric
  Y value; numbers, decimal strings, and `0x…` hex strings are accepted, others
  dropped. `lib/pn-graph.c:1678`.
- `resolution` (enum, default **1 minute**) — total time span: 1 min / 15 min /
  1 hour / 1 day / 1 week. `lib/pn-graph.c:1685`.
- `x-buckets` (uint 2–200, default 200) — number of time buckets the window is
  split into (time-series only). `lib/pn-graph.c:1694`.
- `data-view` (enum, default **Time series**) — Time series vs Distribution.
  `lib/pn-graph.c:1707`.
- `draw-style` (enum, default **Lines**) — Points / Lines / Bars / Error bars.
  `lib/pn-graph.c:1721`.
- `line-color` / `line-width` (1–8, default 2) / `axis-color` /
  `background-color` (default white) / `show-grid` (default off).
- `log-y` (default off, distribution only) / `y-from-zero` (default off,
  time-series linear only). `lib/pn-graph.c:1771` / `:1780`.
- `save-data` (bool, default **off**) — persist the collected data into the
  worksheet file and restore it on load. Bounded, in-window only: per series the
  time buckets (at most `x-buckets`, so ≤ 200; they drive the time-series view)
  and the newest 512 raw samples (they drive the distribution view). The store
  itself rides on a hidden read/write string property `saved-data` (JSON v2:
  top-level `saved` wall clock + `bin_us`; per series a topic, flat
  `samples` `[age_ms, value, …]` and flat `bins`
  `[age_in_buckets, count, sum, sum_sq, min, max, …]`; v1 stores without `bins`
  still load, buckets refolded from the samples), which is not a dialog row — it
  exists only so the generic property serialiser carries it. Data that aged past
  the `resolution` window while the file sat on disk is dropped on load.
- Legacy write-only `mode` enum migrates old saves to data-view+draw-style.
  `lib/pn-graph.c:1157`.

**Renders / acts** — On each message resolves `key` to a finite double and
folds it into per-topic rings (a 200-bucket aggregate ring for time-series, a
2048 raw-sample ring for distribution); the PLplot painter walks those rings
every frame. Repaints are throttled to 10 Hz and a refresh tick keeps the time
axis scrolling between messages. `lib/pn-graph.c:634`.

**Gotchas** — *Multi-topic auto-3D*: each distinct `msg.topic` becomes its own
series; a 2nd topic flips both views into a stacked-Z 3D projection (series
hues walk the golden-angle wheel from `line-color`), and a multi-series plot
draws a **colour-key legend labelled with the feeding node's name** — the
message's `from` label, captured per series at `lib/pn-graph.c:758` and drawn in
`lib/pn-graph-gui.c`. Capped at 12 topics — a
13th is dropped silently. The PLplot stream is owned lazily by the gui tier;
the core never links PLplot.

---

## XY Graph

**Purpose** — Scatter-plot an explicit (x, y) pair read from each message —
the Y from `key`, the X from `x-key`. `lib/pn-xy-graph.c:437`
(`pn_xy_graph_receive`). Plotting with **PLplot** (`lib/pn-xy-graph-gui.c`).

**When to use** — When both coordinates come from the message, not the clock —
e.g. plotting one measured quantity against another (a parabola demo, a
sensor-vs-sensor correlation). Contrast **Graph**, whose X axis is arrival
time.

**Ports** — input only; no output. `lib/pn-xy-graph.c:1027`.

**Settings** (two tabs: *Appearance* | *Data*, `lib/pn-xy-graph.c:1137`)
- `x-key` (string, default `data/x`) and `key` (Y, default `data/value`) —
  JSON paths; both must resolve to finite numbers or the whole sample is
  dropped. `lib/pn-xy-graph.c:762` / `:755`.
- `max-points` (uint 2–2048, default 2048) — most-recent samples kept/plotted
  per series. `lib/pn-xy-graph.c:770`.
- `draw-style` (enum, default **Points**) — Points / Lines. `lib/pn-xy-graph.c:777`.
- `line-color` / `line-width` (1–8, default 2) / `axis-color` /
  `background-color` (default white) / `show-grid` (default off).
- `x-from-zero` / `y-from-zero` (both default off) — anchor an axis at 0
  instead of tight auto-fit. `lib/pn-xy-graph.c:819` / `:826`.
- `save-data` (bool, default **off**) — persist the plotted points into the
  worksheet file and restore them on load, through the same hidden `saved-data`
  string property the Graph node uses (JSON: per series a topic plus a flat
  `[x, y, …]` array). Capped at `max-points`, and never more than 512 per
  series. No time axis, so nothing ages out on load.

**Renders / acts** — Resolves x and y, appends to the per-topic ring, repaints
(10 Hz throttle); the painter reads the last `max-points` in chronological
order and auto-ranges both axes. `lib/pn-xy-graph.c:459`.

**Gotchas** — *Multiple series, 2D overlay* (not 3D like Graph): each
`msg.topic` overlays on the same axes in its own golden-angle hue, capped at 12
topics (13th dropped). No time window or resolution — the X axis is a value, so
there is nothing time-based to configure.

---

## Plot

**Purpose** — The **vector-fed sibling of Graph** (TODO #48.2): one message carrying a `$pnvector` on a bag member is distributed across M consecutive X-buckets, each aggregated to count / mean / sd / min / max and drawn as a point or an error bar. The X axis is the **bucket index, not time**. (`lib/pn-plot.c` + `lib/pn-plot-gui.c`)

**When to use** — When the whole curve arrives in one message — a **Ramp** sweep run through a Calculator, a batch of samples, a computed series — rather than accumulating a point per message. Because it fills instantly and deterministically from a single message, it is also the testable one: no waiting for a window to fill. Vs **Graph**: Graph's X is arrival time and it accumulates across messages; Plot redraws the whole picture from each message. Vs **Oscilloscope**: Plot aggregates buckets with spread; Oscilloscope traces Y against X faithfully.

**Ports** — input only. Footprint 280×173 body / 217 total — the Graph family's (`pn-plot.c:51`).

**Settings** (`pn-plot.c:618`+) — two tabs *Appearance* | *Data*, mirroring Graph (`:713`); the inherited `topic` row is hidden.
- `value-key` (string, default **`"value"`**) — ⚠ a **flat top-level member name**, read with `pn_message_get_member`, **not** Graph's `/`-separated path. This is the single most likely thing to trip up a reader arriving from the Graph entry.
- `x-buckets` (uint, **2–200** = `PN_GRAPH_MAX_BINS`, default **16** = `PN_PLOT_DEF_BINS`, `pn-plot.c:65`).
- `data-view` (enum `PnGraphView`, default `Time series`; nicks `Time series` / `Distribution`).
- `draw-style` (enum `PnGraphStyle`, default **`Error bars`** — note this differs from Graph's `Lines`).
- `line-color` (dark blue 30/60/140), `line-width` (uint 1–8, default 2), `axis-color` (dark grey 70³), `background-color` (white), `show-grid` (FALSE), `log-y` (FALSE), `y-from-zero` (FALSE).

**No `save-data` / `saved-data`** — unlike Graph, Plot persists nothing.

**Reads** — exactly one member, which must resolve through `pn_message_resolve_vector` (`pn-plot.c:288`); a **scalar is silently ignored**. Writes nothing.

**Tier split** — core owns the type, properties, `receive`, `rebucket` (`:203`), the repaint throttle and the read seam `pn_plot_get_paint_state()` / `_peek_series()` / `_get_n_bins()`, filling PnGraph's own `PnGraphBin` / `PnGraphSample` structs so the painter and the headless test share one plain-data view. The gui tier owns the PLplot+cairo painter, a lazily-boxed per-instance PLplot stream, and reuses `pn_graph_draw_error_bars_2d` / `_series_2d`. Installed by `pn_plot_gui_install()` (`lib/pn-plot-gui.c:279`) from `lib/pn-gui.c:88`.

**Gotchas**
- **Each message replaces the whole plot** — `rebucket` calls `series_reset` first (`:212`). There is no cross-message history.
- Bucket *b* owns `[b·N/M, (b+1)·N/M)`; with M > N the trailing buckets stay unused and are skipped.
- The vector is adopted **by reference**, so changing `x-buckets` re-bins immediately — but changing `value-key` does not (the next message applies it).
- The Distribution view mirrors only the **leading 2048** elements into the sample ring, stamped `G_MAXINT64` so they never age out.
- Single series only — no multi-topic, no 3D. 10 Hz repaint throttle. No error state.
- Examples `examples/Compute/error-bars.json`, `examples/crypto/uniswap-v2.json`; help `PnPlot.html`; test `tests/unit/test-pn-plot.c`.

---

## Oscilloscope

**Purpose** — A green-phosphor CRT that traces Y against X, where **each of X and Y may independently be a scalar or a `$pnvector`** (TODO #44). Pure cairo, no PLplot. (`lib/pn-oscilloscope.c` + `lib/pn-oscilloscope-gui.c`)

**When to use** — To see a waveform's *shape*: a Lissajous figure (vector X and vector Y), a computed trace, a slow-moving scalar leaving a phosphor streak. Vs **Plot**: no bucketing or error bars — the trace is the data. Vs **Graph**: no time window.

**There are exactly two states**, and they **evict each other**:
- **snapshot** — a vector arrives and *is* the trace;
- **point** — a scalar arrives and is drawn as one dot, with **no history**.

A snapshot clears the point, the accumulated bounds and the afterglow; a point clears both vectors.

**Ports** — input only. Footprint **260 wide × 254** (40 header + 4 gap + 210 screen, `pn-oscilloscope.c:51`) — deliberately *not* the 280×173 plot family.

**Settings** (`pn-oscilloscope.c:2196`+) — **three tabs** Appearance | Data | Scale, plus a hidden `topic` and a hidden `cursors` (`:2324`). Eighteen properties:
- *Data* — `value-key` (`"value"`), `x-key` (`"x"`) — again **flat member names, not paths**; an absent `x-key` plots a vector against its sample index and puts a scalar dot at X = 0. `x-from-zero` (FALSE), `y-from-zero` (FALSE).
- *Appearance* — `screen-color` (0.02, 0.05, 0.03), `trace-color` (0.34, 1.00, 0.45), `grid-color` (0.22, 0.50, 0.30), `show-graticule` (TRUE), `trace-width` (uint 1–8, default 2), `focus` (double **0–1, default 1.0**), `intensity` (double **0–1, default 1.0**) — both clamped in the setter (`:2040`, `:2051`).
- *Scale* — `x-auto` (TRUE), `x-range` (0–G_MAXDOUBLE, default 1.0), `x-offset` (±G_MAXDOUBLE, default 0.0), and the same three for Y.
- *Hidden* — `cursors` (string, default `""`): four `;`-separated tokens `V1;V2;H1;H2` in **data coordinates**, written locale-independently (`:1502`, `:1524`).

**Reads** — `value-key`, then `x-key` if set. The vector branch wins; the scalar branch accepts int64, double **and a numeric string** including `0x` hex (`parse_numeric_string`, `:197`). Neither ⇒ silent no-op. Writes nothing.

**Tier split** — the GTK-free core owns the type, all eighteen properties, `receive`, `set_snapshot` / `push_scalar`, the afterglow ring, `compute_raw_bounds`, the size vfuncs, and — unusually — **the entire maximized control panel's geometry, hit-testing and value maths**: six knobs (`PnOscKnob` X_RANGE / X_OFFSET / Y_RANGE / Y_OFFSET / FOCUS / INTENSITY), two Auto buttons and four cursors, all public in `lib/pn-oscilloscope.h:219`–`423`, so the painter and the worksheet's pointer handlers share one source of truth. Read seam `pn_oscilloscope_get_paint_state()` / `_read_trace()` (which decimates to **per-bucket min/max extrema, never averages**) / `_read_afterglow()`. `pn_oscilloscope_gui_install()` (`-gui.c:1508`) sets `paint_plot`, `paint_plot_corner_radius = 16.0` and `paint_plot_zoom_keep_aspect = TRUE`; called from `lib/pn-gui.c:90`.

**Gotchas**
- **Scalar framing is an accumulating envelope**: bounds only ever grow until the next snapshot, so one past outlier permanently widens the frame.
- Afterglow: 64-point cap, 800 ms persistence, 40 ms fade tick; a repeated identical value leaves no streak; it returns 0 in snapshot mode.
- **Setting any of `x-range` / `x-offset` / `y-range` / `y-offset` silently flips that axis out of auto** (`:2082`) — easy to trip from D-Bus or a loaded file.
- `maximized` is runtime-only, not a property, wired through `PN_IS_OSCILLOSCOPE` checks in `lib/pn-worksheet.c` (the in-card drag interaction). Cursors, by contrast, do persist.
- 10 Hz repaint throttle. No error state, no saved data.
- Examples `examples/Compute/oscilloscope.json`, `lissajous.json` (X *and* Y vectors), `projectile.json`, `examples/crypto/uniswap-v2.json`; help `PnOscilloscope.html`; PNG harness `tools/osc-preview.c`.

---

## Figure

**Purpose** — A **programmable vector-drawing sink**: you type a small drawing program into the node and it paints live in its own card, with each input's last `data.value` bound as a variable. Built (TODO #80) for worksheets that look like the plates in a 19th-century physics book — it draws the *apparatus*, not the curve. (`lib/pn-figure.c` + `lib/pn-figure-gui.c`)

**When to use** — When the picture *is* the explanation: a pendulum whose bob hangs at the angle a Knob says, a compass grid that finds a magnet's neutral points, a schematic whose dimensions follow live readings. Vs Graph/Plot/Oscilloscope, which all draw *data*; Figure draws whatever you tell it to.

**Ports** — **1–8 inputs** via the `inputs` property (`pn_node_set_input_count_property`, `:3633`, which also adds an "Inputs" tab), with `pn_node_set_collate_inputs (TRUE)` (`:3637`) so the program sees **every** input on every repaint, not just the one that fired. **Each input's value binds under that port's display name** — `value1`…`valueN` by default, so renaming a port renames the variable. No output.

**Settings** (`pn-figure.c:3500`+) — tab **Figure** = `program` as `PN_EDITOR_CODE` full width (language `"sh"`; a real `figure.lang` is noted as not built) with `error` as a full-width `PN_EDITOR_LABEL` beneath it; tab **Appearance** = `background-color`, `font-family`, `stretch`. Inherited `topic` hidden (`:3581`).
- `program` (string, **multiline**, default `view 0, 0, 100, 100` / `circle 50, 50, 40` / `text 50, 50, "%.1f", value1` — `PN_FIGURE_DEF_PROGRAM`, `:3002`) so a fresh node is never blank.
- `inputs` (int, **1–8, default 1**).
- `background-color` (boxed, **white**) — fills the letterbox bars too.
- `font-family` (string, `""`), `stretch` (bool, FALSE).
- Animation tab (#82.4): `play-mode` (enum `PnFigurePlayMode`, once / **loop** / ping-pong, saved by nick), `fps` (int 1–60, **25**), `frames` (int 0–10000, **0** = the length comes from the data). `fps` retimes a running film without rewinding it, `play-mode` keeps the current frame, and `frames` rewinds.
- `error` (string, `""`, **`G_PARAM_READABLE` only**, `:3564`) — never serialised.

Size **280 × 254** (40 header + 4 gap + 210 client, `lib/pn-figure.h:769`); `paint_plot_zoom_keep_aspect = TRUE` (`:3498`).

**Reads** — via `pn_expr_bind_collated` (`:3274`, implementation `lib/pn-expr-bind.c:107`): `data.<input-name>` per input (for a single-input node the message's own `data.value` binds under input 0's name), plus every **other** numeric `data.*` member of the arriving message suffixed with its 1-based input number (`data.temp` → `temp1`). Any name the program uses but nothing supplies is **0** — "zero-fill" — so a figure draws fully even unwired. **Writes nothing, emits nothing.**

**The language** — `pn_figure_scan` (strips `#` comments, joins a line ending in a comma onto the next, keeps a piece table so errors cite the right source line) → `pn_figure_split` (`name = expr` is an assignment statement) → `check_verbs` → `pn_figure_check_blocks` → `parse_literals` → `parse_expressions`, all core, recompiled whenever `program` is set (`figure_recompile`, `:3140`). Verbs (`:725`): `view` (**Y points up**); pen state `color fill nofill width dash font align`; geometry `move rmove lineto rline line point circle arc rect poly path`; `text x, y, "fmt", …` (a validated printf subset); and `repeat` / `end`. Every argument is an expression in the shared calculator language (`clamp min max atan2 pow hypot floor …`, `pi`, `e`) — so **`^` is XOR, not power**. Resolution yields a device-unit display list (`PnFigureOp`) that `pn_figure_display_to_string()` dumps, which is what makes the whole thing unit-testable.

**`repeat` (TODO #86)** — `repeat <count>` … `end`. The count is an **expression**; the index is **always `i`**, counting from 0, shadowing any input or assignment of that name (`PN_FIGURE_INDEX_NAME`, `lib/pn-figure.h:676`). **No nesting** — a nested `repeat` is a parse error, and a grid is one loop plus floor/mod arithmetic. `pn_figure_check_blocks()` counts *depth* rather than a flag, so one nesting mistake yields exactly one message. A count of zero, negative, NaN or ∞ leaves **one `OP_SKIP` marker** instead of reddening the node; the cap is `PN_FIGURE_MAX_REPEAT` = **1000** (`lib/pn-figure.h:682`, reason `"too-many"`). Pen state and assignments are **sequential across iterations** — the block is shorthand, not a scope. It has no display-list op of its own, and each iteration's ops carry the **same source line numbers**, which is what makes a loop assertable.

**Gotchas**
- Serialised: `program`, `inputs`, `background-color`, `font-family`, `stretch`, plus the core's input-name map. **Not** serialised: `error` and the latched input snapshot.
- `figure_refresh_error` (`:3103`) sets `pn_node_set_has_error()` and paints the error text **instead of** the figure: a program error draws **nothing at all**, deliberately — half a figure is a worse lie than none. A *skipped* statement, by contrast, never reddens the node.
- **A vector argument is a film frame** (TODO #82.2, `eval_args`): frame *i* draws element *i* of every vector argument, and scalars stay the same in every frame. The program is evaluated with the vectors in place (the store is elementwise) and indexed at the argument, so it is **never re-run per frame**. This applies to pen state and `repeat` counts too. The frame count is the **shortest vector input the program reads** (`pn_figure_frame_count`, #82.1), not the arithmetic's longer-with-tail length. An empty vector, or a frame past a vector's end, is a located red error. **The timer (#82.3)** lives in the core half. It fires at 1000/fps (fps and mode are the #82.4 properties) and repaints unthrottled. It runs **only while something is connected to `repaint-needed`**, so headless it never ticks. New data or a program edit keeps the current frame, and only a change in film **length** rewinds to frame 0, and a paint (`pn_figure_render`) starts a film that was waiting for a watcher. The step rule is the pure `pn_figure_step_frame()` (loop, once, ping-pong). `pn_figure_get_frame` and `pn_figure_is_playing` report the state, and `pn_figure_dump (self, frame, …)` dumps any frame. **`frame` and `t` (#82.5)** are bound as vectors over the film, and only if the program reads them. `frame` = 0…N-1. `t` = k/N in **loop** mode, so a periodic figure wraps with no repeated pose, and k/(N-1) in **once / ping-pong**, so the last frame is exactly 1. A still binds both as 0. An input with the same name wins, and an assignment wins over both. `pn_figure_resolve()` takes a nullable `PnFigureFilm *` {index, frames, mode}. A comparison still collapses a vector to one scalar, so `s > 0` cannot vary per frame.
- Errors are collected and shown as e.g. "3 errors, first on line 7".
- Repaint throttle 100 ms, but a `program` set goes through **unthrottled** (`:3379`) so the editor follows keystrokes.
- `receive` resolves once at the at-rest rect purely so `error` is correct headless (`:3281`); that display list is thrown away.
- **Help page `data/help/PnFigure.html`** (#82.6 / 80.14) is the language specification: grammar, verb tables, binding order, `repeat`, animation and error classes. It links to `PnExpression2.html` for the function list instead of copying it. Tests are in `tests/unit/test-pn-figure.c`. The headless preview harness `tools/figure-preview.c` is not in the build; compile it by hand against `lib/.libs`, and prepend `t = <value>` to a program to render a mid-film frame.
- Plates built on it so far: `examples/displays/pendulum.json` (TODO #85.1), `examples/displays/refraction.json` (#85.9), `examples/displays/bar-magnet.json` (#85.12) and #80.15's two proving sheets `examples/displays/ray-optics.json` (thin lens) and `examples/displays/block-and-tackle.json` (which replaced the dropped lever), plus `examples/displays/tower-crane.json` (TODO #88.1) and `examples/displays/wheatstone-bridge.json` (#88.2, a Pt100 self-balancing bridge), the plates with a **controller in the loop**: knobs → three small calculators (load chart, reach limiter, counterweight) → a **five-input** Figure, one headline value per wire. Feed a multi-input figure headline values, not sibling members: the core latches each input's `data.value`, but siblings (`reach1`, …) are bound only from the message that just arrived and vanish when another input fires. Note the Calculator language has **no `#` comments**; only the Figure strips them. TODO #85 (twelve physics plates) is still open — the rest wait partly on 80.19, since there are **no arrows, angle-mark arcs, rotated labels or hatching yet** (the shipped plates hand-build arrowheads and needles out of `poly`).
- **Sizing rule for a plate (#85.9):** every length scales with the `view` (80.4b), so legibility at rest is the view's width against the font sizes. The node's client area is 280×210, so a 132-unit-wide view gives ~2.1 device px per unit and `font 4.8` ≈ 10 px — about the floor for body text. `refraction.json` is sized this way; `pendulum.json` (152 units) and `bar-magnet.json` (124 units) use smaller type and are harder to read unmaximized. `ray-optics.json` is 172 units wide (≈1.6 px/unit) with body `font 5.4` ≈ 8.8 px — the size the user signed off on; keep the view 4:3 and leave ~3 units under the last text line or it touches the card's lower edge.
- Two idioms those plates lean on, both from 80.10(b) *a non-finite value skips the statement*: a quantity that must **vanish** is left NaN (`asin(n·sin θ)` past the critical angle takes the refracted ray, its arrowhead, its arc and its label with it, with no `if` anywhere), and a floor on such a quantity must use **`clamp`, not `max`** — `clamp` propagates NaN deliberately, `min`/`max` are fmin/fmax and swallow it.

---

## Weather Report

**Purpose** — Render the message a **Weather** source node emits as a compact,
mostly-monochrome weather card (place, conditions glyph, temperature, detail
tiles). `lib/pn-weather-report.c:350` (`pn_weather_report_receive`). Painted
with cairo/Pango in `lib/pn-weather-report-gui.c`.

**When to use** — Downstream of a **Weather** node, for a finished "today's
weather" card. Distinguish from **Sun Path**, which renders an **Astronomical**
node's reading as a 3D sun dome rather than current conditions.

**Ports** — input only; no output. `lib/pn-weather-report.c:649`.

**Settings**
- `show-details` (bool, default TRUE) — draw the humidity/wind/pressure/cloud
  detail tiles. `lib/pn-weather-report.c:659`.
- `temperature-unit` (Celsius/Fahrenheit/Kelvin, default °C),
  `wind-unit` (km/h / m/s / mph / knots, default km/h),
  `pressure-unit` (hPa / kPa / inHg / mmHg, default hPa) — display conversions
  from the metric the Weather node reports. `lib/pn-weather-report.c:666`+.
- `font-color` (near-black), `secondary-font-color` (mid-grey),
  `background-color` (white), `background-color2` (gradient end),
  `background-gradient` (Solid / Vertical / Horizontal / Diagonal, default
  Solid). `lib/pn-weather-report.c:687`+.

**Renders / acts** — Deep-copies the message `data` bag (so the reading
survives past the borrowed message) and repaints. Reads the named members the
Weather node promotes — `city`, `country`, `temperature`, `humidity`,
`wind_speed`, `weather_code`, `description`, `success`, `output` — plus the raw
Open-Meteo passthrough at `data/raw/current` (apparent_temperature, is_day,
cloud_cover, pressure_msl, precipitation, wind_direction_10m); falls back
to `data/raw/weather` for the Bright Sky provider. Two **fallback chains** are
worth knowing: temperature falls back from `data/temperature` to `data/value`
(`lib/pn-weather-report-gui.c:481`), and pressure from `raw/current/pressure_msl`
to `raw/current/surface_pressure` to `raw/weather/pressure_msl` (`:490`).
`lib/pn-weather-report.c:355`. Note the card's clock is **local wall time** —
`raw/current/time` is never read (`lib/pn-weather-report-gui.c:636`).

**Gotchas** — Mirrors current conditions onto the node's own header glyph
(`condition_glyph`, `lib/pn-weather-report.c:329`) so the at-rest node shows the
weather. A 15 s clock timer (`lib/pn-weather-report.c:595`) keeps the on-card
time live independent of the slow weather refresh. `success=false` shows the
failure `output`; before any message a "Waiting for weather" placeholder.

---

## Sun Path

**Purpose** — Draw an **Astronomical** node's reading as a small, rotatable 3D
sky dome: a translucent compass ground disc, a reference house, the Sun's
full-day arc, and a Sun glyph at the live position. `lib/pn-sun-path.c:253`
(`pn_sun_path_receive`). Cairo 3D scene painter in `lib/pn-sun-path-gui.c`.

**When to use** — Downstream of an **Astronomical** source, when you want the
solar geometry of the day (where/when the sun rises over a site) rather than a
weather card. One message is enough: the arc is recomputed locally with NOAA
math from the resolved lat/lon.

**Ports** — input only; no output. Supports **in-card drag-to-rotate** in the
zoom overlay (the `scroll` vfunc is wired in the core, `lib/pn-sun-path.c:580`;
drag handled via the worksheet zoom overlay). `lib/pn-sun-path.c:591`.

**Settings**
- `show-house` (bool, default TRUE) — draw the reference house. `lib/pn-sun-path.c:630`.
- `background-color` / `background-color2` / `background-gradient`
  (Solid/Vertical/Horizontal/Diagonal, default **Vertical**) — the sky fill.
  `lib/pn-sun-path.c:636`+.
- `ground-color` (translucent disc), `path-color` (arc), `sun-color` (glyph),
  `text-color` (compass labels / readout). `lib/pn-sun-path.c:658`+.
- Hidden/serialised-only: `view-yaw`, `view-pitch` (camera, driven by drag),
  `house-heading` (spun by the mouse wheel). `lib/pn-sun-path.c:610`+, hidden in
  the schema at `lib/pn-sun-path.c:599`.

**Renders / acts** — Reads `data.success` (false → "No position" + `data.output`),
`data.latitude`/`longitude` (the place the arc is computed for),
`data.sun_azimuth`/`sun_altitude` (where the glyph rides), `data.sun_up`
(bright vs dimmed glyph), `data.city`/`country` (corner label). On a successful
reading it re-samples the 24 h arc at 5-min steps via
`pn_astronomical_compute`. `lib/pn-sun-path.c:264`.

**Gotchas** — Drag orbits the camera (left/right = yaw, up/down = pitch,
clamped 6°–84°); the mouse wheel spins the house heading; both persist with the
worksheet. A failed lookup drops the old arc so no stale path shows under a "no
position" notice. **A message with `success = TRUE` but no `sun_azimuth` +
`sun_altitude` is dropped entirely** (`lib/pn-sun-path.c:275`) and the card keeps
showing "Waiting for sun position" rather than parking the Sun at (0, 0) — most
often this is a Weather report wired in by mistake. Explicit failures
(`success = FALSE`) *do* pass through, to drive the "No position" notice. Same 280×173 footprint as Weather Report / Graph.

---

## Chat

**Purpose** — Show incoming messages as a scrolling list of chat bubbles with a
canvas-resident entry strip + inline Send button; sending emits a new message
on the output port. `lib/pn-chat.c:358` (`pn_chat_receive`),
`lib/pn-chat.c:526` (`pn_chat_submit`). Cairo/Pango painter in
`lib/pn-chat-gui.c`.

**When to use** — As the human end of a conversational flow — e.g. a Meshtastic
or Ollama feed in, typed replies out. The default `text-path`/`sender-path`
match the Meshtastic envelope so it plugs in unchanged.

**Ports** — input **and** output (`has_output = TRUE`, `lib/pn-chat.c:940`) —
one of the two sinks that also sources. Self-loop messages (source == self) are
suppressed so a wrap-back wire does not double a sent bubble.
`lib/pn-chat.c:374`.

**Settings**
- `text-path` (default `data/output`) — JSON pointer for the bubble text.
- `sender-path` (default `data/from_long_name`) — sender label (hashed into a
  per-sender pastel fill). `lib/pn-chat.c:952`.
- `me-name` (default `Me`) — name stamped under `data/from_long_name` on
  outgoing messages and shown over right-aligned "mine" bubbles. `lib/pn-chat.c:963`.
- `limit` (uint 1–1000, default 200) — bubble history cap. `lib/pn-chat.c:972`.
- `background-color`, `border-color`, `text-color`, `me-color`,
  `input-background-color`, `send-button-color`. `lib/pn-chat.c:980`+.

**Renders / acts** — Each received message resolves `text-path`/`sender-path`
to scalars and pushes one bubble (newest pinned to bottom). Pressing
Enter / clicking Send emits a fresh message: `data/output` = typed text,
`data/success` = **TRUE** (`lib/pn-chat.c:555`), `data/from_long_name` =
`me-name`, topic from the node's PnNode topic template; the same text is pushed
locally as a "mine" bubble. A whitespace-only draft is silently discarded
(`lib/pn-chat.c:538`). `lib/pn-chat.c:237`, `:554`.

**Gotchas** — Fully canvas-resident text input (no real `GtkEntry`): a blinking
caret, UTF-8-aware draft editing, and the worksheet routing keystrokes only
while focused (`lib/pn-chat.c:500`+). Mouse wheel scrolls history in the zoom
overlay. 10 Hz repaint throttle.

---

## Sound

**Purpose** — Play a short audio clip whenever any message arrives. The message
*contents are ignored* — the trigger fact alone fires the sound.
`lib/pn-sound.c:349` (`pn_sound_receive`).

**When to use** — An audible "ping" on an event (alert fired, job done). Use
**Sound** for a fixed clip; **TTS** to speak the message's `data.output` text;
**Notify** for a visual desktop bubble.

**Ports** — input only; no output. `lib/pn-sound.c:484`.

**Settings**
- `sound` (string, default NULL/unconfigured) — a freedesktop sound-theme id
  (e.g. `bell`, resolved under `/usr/share/sounds/freedesktop/stereo/<id>.oga`)
  or an absolute audio-file path. Empty paints the node red with a ❗.
  `lib/pn-sound.c:495`, resolver `lib/pn-sound.c:133`.
- `dead-period` (uint 0–3600 s, default 0) — mandatory silence after each
  playback; messages within it are dropped. `lib/pn-sound.c:502`.

**Renders / acts** — In the **default build** the clip is decoded and streamed
**in process** on a worker thread via libsndfile + libpulse-simple
(`play_in_thread`, `lib/pn-sound.c:121`, kicked off at `:288`). Spawning
`paplay <path>` is only the `#else` fallback, compiled when those two libraries
are not both present (`:304`–`:330`); `HAVE_PN_AUDIO` is defined whenever
`sndfile libpulse-simple` are found (`configure.ac:164`). `paplay` is used
rather than canberra-gtk-play, which would honour the often-off
`gtk-enable-event-sounds`. `pn_sound_backend_description()` (`:335`) reports
which mode is in effect, and the settings dialog shows it.

**Gotchas** — No overlap: a message arriving while a clip is still `playing`,
or within the dead period after one ends, is dropped (`lib/pn-sound.c:205`).
Themed ids need the freedesktop sound theme installed. The node depends on
`paplay` **only in the fallback build** — check
`pn_sound_backend_description()` before blaming a missing PulseAudio utility. The settings dialog offers a Preview button (`pn_sound_preview`,
`lib/pn-sound.c:386`).

---

## Text to Speech

**Purpose** — Speak each incoming `data.output` string aloud by piping it
through a Linux TTS program. `lib/pn-tts.c:743` (`pn_tts_receive`),
`lib/pn-tts.c:516` (`pn_tts_speak`).

**When to use** — When the message's text should be heard, not seen — read out
an LLM reply, a chat message, an alert summary. Contrast **Sound** (fixed clip,
ignores content) and **Notify** (silent visual bubble).

**Ports** — input only; no output. `lib/pn-tts.c:1144`.

**Settings**
- `engine` (string, default = first installed) — one of `piper`, `espeak-ng`,
  `espeak`, `festival`, `flite`; picking an uninstalled one turns the node red
  with a ❗ and a status message. `lib/pn-tts.c:1155`, engine table
  `lib/pn-tts.c:89`.
- `model` (string, dialog label "Voice", default the Lessac en_US piper onnx) —
  a `.onnx` path for piper, an engine voice name otherwise, empty for
  Festival. `lib/pn-tts.c:1165`.
- `language` (string, default **`"en_US"`**) — the locale the Voice list is
  restricted to (e.g. `"hu_HU"`); empty offers every installed voice. It filters
  both the Voice combo and the per-source voice pool
  (`pick_voice_for_message`, `lib/pn-tts.c:676`). **Only piper voices are
  locale-tagged**, so it has no effect on the other engines.
  `lib/pn-tts.c:1178`.
- `speed` (double 0.5–2.0, default 1.0) — mapped to each engine's own rate knob;
  Festival ignores it. `lib/pn-tts.c:1192`.
- `sink` (string, dialog label "Output", default empty=default sink) —
  PulseAudio sink (`paplay -d` for piper, `PULSE_SINK` env for the rest).
  `lib/pn-tts.c:1203`.
- `per-source-voice` (bool, default TRUE) — hash the source-node name to pick a
  voice deterministically per speaker. `lib/pn-tts.c:1220`.
- `max-queue` (int −1..MAXINT, default 16) — backlog cap while speaking: 0 =
  drop-while-busy, −1 = unbounded, N = cap+drop. `lib/pn-tts.c:1236`.
- `last-error` is read-only/transient (not serialised). `lib/pn-tts.c:1214`.

**Renders / acts** — Reads only string `data.output`; missing/non-string is
ignored. Spawns the engine's shell pipeline, feeding the text (with line breaks
flattened to spaces so piper speaks the whole reply) to its stdin; queued
utterances drain in arrival order from `on_speak_done`
(`lib/pn-tts.c:359`, which also drains the next pending utterance).

**Gotchas** — Pure-C subprocess spawn, no helper at runtime; errors surface via
`pn_node_log_*` not stdout (no terminal). Needs at least one TTS program
installed; piper additionally needs `paplay`. The per-source voice override is
captured when the utterance is queued, so renaming the source mid-queue does
not change it.

---

## Notify

**Purpose** — Show a desktop notification bubble whenever a message arrives, via
the freedesktop `org.freedesktop.Notifications` D-Bus service.
`lib/pn-notify.c:283` (`pn_notify_receive`).

**When to use** — A silent, visual desktop alert. Use **Notify** for an on-screen
bubble, **Sound** for an audible ping, **TTS** to speak the content.

**Ports** — input only; no output. `lib/pn-notify.c:464`.

**Settings**
- `summary` (template string, default `${topic}`) — bubble title.
  `lib/pn-notify.c:474`.
- `body` (template string, default `${data/output}`) — bubble body.
  `lib/pn-notify.c:481`. Both expand `${path/to/field}` placeholders against the
  message lookup root (same root PnFormat uses), with document-globals fallback.
  `lib/pn-notify.c:139`.
- `icon` (string, default `dialog-information`) — freedesktop icon name or
  absolute path; empty hides it. `lib/pn-notify.c:488`.
- `app-name` (string, default `pipnode`). `lib/pn-notify.c:495`.
- `urgency` (enum low/normal/critical, default **normal**) — critical typically
  bypasses Do-Not-Disturb. `lib/pn-notify.c:502`.
- `timeout-ms` (int −1..86400000, default −1) — −1 server-decides, 0 never
  expires. `lib/pn-notify.c:509`.
- `replace` (bool, default TRUE) — update the previous bubble in place rather
  than stacking. `lib/pn-notify.c:517`.

**Renders / acts** — Expands the templates, lazily acquires the session bus,
and async-calls `Notify(...)` with the urgency hint; the returned id is reused
as `replaces_id` when `replace` is on. `lib/pn-notify.c:219`.

**Gotchas** — Needs a running notification daemon on a session bus; on a
headless host it logs a warning and drops. On XFCE with Do-Not-Disturb on the
call succeeds (an id is returned) but the bubble is silently suppressed — set
`urgency=critical` or disable DND. Errors surface via `pn_node_log_warning`.

---

## FileViewer

**Purpose** — Display whatever image arrives on its input, painting it edge-to-
edge in a view rectangle below the header; non-image messages fall back to a
filename hint. `lib/pn-file-viewer.c:208` (`pn_file_viewer_receive`). Cairo
preview painter in `lib/pn-file-viewer-gui.c`.

**When to use** — Downstream of a **File Drop** source (or any node emitting a
`PnImageMessage`) to show the picture on the canvas — wiring a File Drop's
output into a File Viewer shows the same image twice, ref-shared, no re-read.

**Ports** — input only; no output. `lib/pn-file-viewer.c:336`.

**Settings**
- `area-color` (boxed colour, default white) — view-area fill (shown only while
  empty). `lib/pn-file-viewer.c:346`.
- `border-color` (default dark grey) — 1 px frame. `lib/pn-file-viewer.c:352`.

**Renders / acts** — If the message is a `PnImageMessage`, refs its `GdkPixbuf`
(gdk-pixbuf is an allowed core dep) and resizes the view area to the image's
aspect ratio at a fixed 200 px width (clamped 80–360 px tall). Any other message
clears the preview and shows the hint from `data.filename`, else "Nothing to
show". `lib/pn-file-viewer.c:217`, size at `:117`.

**Gotchas** — Node height is image-driven, so a repaint re-queries `get_size`.
The pixbuf rides the GTK-free paint-state snapshot as a borrowed pointer; the
cairo painter lives only in the gui tier.

---

## Text View

**Purpose** — Render the latest `data.output` string as a read-only multi-line
monospace text block — the "terminal pane" sink. `lib/pn-text-view.c:184`
(`pn_text_view_receive`). Cairo painter in `lib/pn-text-view-gui.c`.

**When to use** — To show a command's stdout/stderr verbatim on the canvas
(e.g. Shell Command → Text View). Contrast **Debug Print** (envelope as JSON to
a pane/stdout) and **Table View** (structured `data.table`).

**Ports** — input **and** output (`has_output = TRUE`, `lib/pn-text-view.c:411`):
re-emits every received message verbatim so it can sit mid-pipeline as an inline
inspector. `lib/pn-text-view.c:213`.

**Settings**
- `background-color` (default black) / `text-color` (default green) — classic
  green-on-black terminal scheme. `lib/pn-text-view.c:413` / `:419`.
- `font-size` (double 6–48 px, default 12) — monospace size. `lib/pn-text-view.c:425`.

**Renders / acts** — Pulls string `data.output`, splits it on `\n` (dropping a
trailing blank line), replaces the displayed snapshot (latest wins, no
scroll-back history), repaints (10 Hz throttle), then forwards the message.
A message with no string `output` clears the view to its "waiting" state but is
still forwarded. `lib/pn-text-view.c:101`, `:185`.

**Gotchas** — Mouse-wheel scroll moves **three lines per notch** —
`lround(dy * 3.0)` (`lib/pn-text-view.c:269`); a single line is only the
rounding fallback for a very small `dy`. The painter clamps the offset back to
live extents. **Every received message resets `scroll_offset` to 0**
(`lib/pn-text-view.c:193`), so a new arrival snaps the pane back to the top.
Same 280×173 footprint as Table View.

---

## Table

**Purpose** — Display received messages as a scrolling, newest-first table of
rows under a column header. `lib/pn-table.c:354` (`pn_table_receive`). Cairo
painter in `lib/pn-table-gui.c`.

**When to use** — To accumulate a *log* of messages, one row each, with columns
you define by JSON path. Contrast **Table View**, which *replaces* its whole
table from a `data.table` produced by a Table Model filter (Table appends; Table
View snapshots).

**Ports** — input only; no output. `lib/pn-table.c:660`.

**Settings**
- `columns` (string, default `topic:topic,id:id`) — comma-separated
  `Title:path` entries; an entry with no `:` uses the path as its title.
  `lib/pn-table.c:670`, parser at `:149`.
- `limit` (uint 1–1000, default 200) — row cap; oldest drop off the bottom.
  `lib/pn-table.c:680`.
- `background-color` (white), `header-background-color` (light grey, reused at
  30% alpha for stripes), `grid-color`, `text-color`, `header-text-color`,
  `alternate-row-background` (bool, default TRUE). `lib/pn-table.c:687`+.

**Renders / acts** — Per message resolves each column path against the message
lookup root and stringifies the result by JSON type (numbers `%g`/`%PRId64`,
booleans true/false, strings verbatim, missing `—`, objects/arrays as
`{…}`/`[…]`), pushes the row at the head, trims to `limit`, repaints (10 Hz).
`lib/pn-table.c:260`, `:354`.

**Gotchas** — Reads each configured `Title:path`, not a fixed `data.*` key.
Click-to-zoom lifts the table into the shared overlay where the wheel scrolls
rows. Table **does not** reset its scroll offset on a new message —
`pn_table_receive` never touches `scroll_offset` — so a scrolled-back view
stays where you left it. (Table **View** does reset; see its entry.) A JSON
`null` cell renders as the literal string `"null"`, not as an em dash
(`lib/pn-table.c:266`).

---

## Table View

**Purpose** — Render the latest structured table carried on `data.table` of an
incoming message. `lib/pn-table-view.c:298` (`pn_table_view_receive`). Cairo
painter in `lib/pn-table-view-gui.c`.

**When to use** — Downstream of a **Table Model** filter, e.g. Shell Command →
Table Model → Table View to show the last `df -h` run as a column-aligned grid.
Contrast **Table**, which appends one row per message from JSON paths you
configure; Table View takes a ready-made `data.table` and shows it whole.

**Ports** — input only; no output. `lib/pn-table-view.c:617`.

**Settings** — Same visual surface as Table: `background-color`,
`header-background-color` (reused at 30% alpha for stripes), `grid-color`,
`text-color`, `header-text-color`, `alternate-row-background` (default TRUE).
No `columns`/`limit` — the structure comes from the payload.
`lib/pn-table-view.c:626`+.

**Renders / acts** — Reads `data.table` (an object of shape
`{ "header": { "cells": [...] }, "rows": [ { "cells": [...] } ] }`), where each
cell is `{ "text": "…" }` (a bare scalar is stringified directly). **Replaces**
the snapshot every message; a payload with no `data.table` clears to the empty
"waiting for table" state. `lib/pn-table-view.c:209`, `:298`.

**Gotchas** — Replace-not-append (unlike Table). Cell rendering reads only the
`text` member, leaving room for future per-cell decorations. Click-to-zoom +
wheel scroll, same 280×173 footprint as Table. **Every received message resets
the on-canvas scroll offset to 0** (`lib/pn-table-view.c:311`) — this is the
node that does it, unlike Table.

---

## Panel Display

**Purpose** — Surface a worksheet value on an **XFCE panel applet** button. The
display half of the panel I/O contract (its source counterpart is Panel Input).
`lib/pn-panel-display.c:114` (`pn_panel_display_receive`). Renders with the
default node painter — no gui companion.

**When to use** — Only in a worksheet driven by the background panel *engine*
(`pipnode-editor` as a D-Bus service) whose applet button should reflect a
value. A normal on-canvas worksheet does not need one.

**Ports** — input only; no output. `lib/pn-panel-display.c:166`. (Being
output-less, its inherited `topic` row is hidden.) `lib/pn-panel-display.c:170`.

**Settings** — None (no GObject properties beyond the base node). The only
public accessor is `pn_panel_display_dup_text`. `lib/pn-panel-display.c:219`.

**Renders / acts** — Derives a short display string in preference order —
`data.text` (string) → `data.value` (boolean as on/off, ints/reals
locale-independently, string verbatim) → message `topic` → empty — and, only
when it changed, stores it, repaints, and emits the `value-changed` signal the
panel engine forwards to the applet over D-Bus (a numeric value renders on the
applet's tiny seven-segment LED readout as `ddd hh:mm:ss`).
`lib/pn-panel-display.c:67`, `:114`, signal at `:182`.

**Gotchas** — *This is for the panel applet, not the canvas* — the visible
output lives on the XFCE panel, mirrored via D-Bus, not in a card on the
worksheet. Repeated identical values are suppressed to keep the panel D-Bus
traffic quiet. An applet that should show a value must contain exactly one
Panel Display.

---

## Pipe Writer

**Purpose** — Write every received message into a named pipe (FIFO) as one
line for another process to read (`pn_pipe_writer_receive`,
`lib/pn-pipe-writer.c:208`). The sink half of the pipe pair; its source
counterpart is **Pipe Reader**.

**When to use** — To hand flow output to a local script (`while read l; do …;
done < /tmp/x.fifo`) or, in *JSON message* format, to pass whole messages to
another pipnode process's Pipe Reader. Use Logger instead when the lines must
persist on disk.

**Ports** — input only; no output (`lib/pn-pipe-writer.c:383`).

**Settings**
- `pipe-path` (string, file editor, default `""`) — the FIFO; a leading `~`
  is expanded via `pn_path_expand()` (the typed form is saved). Created from an
  idle as soon as the path is set, so an outside reader can open it before the
  first message; an existing non-FIFO is refused (red) and never written.
- `format` (enum `PnPipeFormat`, default **Output text**) — *Output text*:
  `data.output` + `\n` (empty line when absent/non-string). *JSON message*:
  `pn_message_serialize (msg, TRUE)` — compact one-line envelope, vector blobs
  included, so a JSON-mode Pipe Reader rebuilds the same message.

**Renders / acts** — Opens `O_WRONLY|O_NONBLOCK` lazily per message; with no
reader the open fails `ENXIO` and the message is **dropped** (counted by
`pn_pipe_writer_get_dropped`, not an error). Accepted lines queue in a
`GString` and `writer_flush` (`lib/pn-pipe-writer.c:129`) writes what the pipe
takes; on `EAGAIN` a `G_IO_OUT` fd watch drains the rest.

**Gotchas** — Never blocks and never raises SIGPIPE: writes go through
`pn_pipe_write` (`lib/pn-pipe-common.c:107`), which blocks SIGPIPE on the
thread and eats a pending one, so a vanished reader is just `EPIPE` → close +
discard the queue (a half-written line never reaches the next reader). Before
each message an idle writer polls for `POLLERR` so a reader that left and
came back gets the message. Queue capped at 1 MiB (`PN_PIPE_MAX_BUFFER`); past
that whole messages are dropped. Multi-line `output` text becomes several
lines at the reader — use JSON format to keep it whole. Starts red (no path).
`fa-sign-out` icon (FontAwesome 4.7).
