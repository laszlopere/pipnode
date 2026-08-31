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

Display sinks paint with cairo/Pango (and PLplot for the plot family) in a
companion `pn-<name>-gui.c` "gui tier" that is installed onto the class only in
the editor build — the headless core never pulls GTK. Several plot/card sinks
share a common 280×173 footprint (a 40 px header, a 4 px gap, then a body)
deliberately so a row of mixed Graph / XY Graph / Weather Report / Sun Path /
Chat / Table / Table View / Text View nodes lines up cleanly on the canvas.

---

## Debug Print

**Purpose** — Print every received message to a chosen destination in a chosen
format. The flow-inspection tool of first resort. `lib/pn-debug.c:289`
(`pn_debug_receive`).

**When to use** — To see what is actually on a wire. Pick **Debug Print** (not
Text View) when you want the *envelope* — topic, id, created, source, the whole
`data` bag as JSON — rather than a rendered `data.output` string, and when you
want the output in the collapsible debug pane / status bar / stdout / stderr
rather than painted on the canvas.

**Ports** — input only; no output. `lib/pn-debug.c:411`.

**Settings**
- `target` (enum, default **Standard Error**) — where rendered text goes:
  *Debug View* (the collapsible pane from View→Debug View), *Status Bar* (main
  window footer), *Standard Output*, *Standard Error*. `lib/pn-debug.c:421`.
- `format` (enum, default **JSON**) — how each message renders:
  *text* = just `data.output`; *oneliner* = compact human trace
  `[name] from= topic= id= created= data={…}`; *JSON* = one pretty JSON object
  wrapping type/from/from_id/topic/id/created/data. `lib/pn-debug.c:428`,
  formatters at `lib/pn-debug.c:220` / `:248` / `:264`.

**Renders / acts** — Formats the message per `format` then writes it: stdout via
`g_print`, stderr via `g_printerr`, or emits the `status-message` /
`debug-message` signal the worksheet forwards to the footer / debug pane.
`lib/pn-debug.c:311`.

**Gotchas** — *Debug View target needs the pane wired*: the `debug-message`
signal only reaches the collapsible pane when the worksheet/main window is
listening, i.e. the Debug View is open and connected — the pane stays empty
otherwise (see the project memory on "Debug View needs the Debug node to target
it"). The inherited `topic` settings row is hidden. text-format on a message
with no string `output` emits a blank line.

---

## Graph

**Purpose** — Summarise one numeric value plucked from each message and plot it
over a rolling time window (time-series) or as a value histogram
(distribution). `lib/pn-graph.c:599` (`pn_graph_receive`). Plotting is done
with **PLplot** in `lib/pn-graph-gui.c`.

**When to use** — When the X axis should be the message's *arrival time* (or a
value-frequency distribution). Use **Graph** for "this number over time"; use
**XY Graph** instead when each message carries an explicit (x, y) pair.

**Ports** — input only; no output. `lib/pn-graph.c:1033`.

**Settings** (two dialog tabs: *Appearance* | *Data*, `lib/pn-graph.c:1172`)
- `key` (string, default `data/value`) — `/`-separated JSON path to the numeric
  Y value; numbers, decimal strings, and `0x…` hex strings are accepted, others
  dropped. `lib/pn-graph.c:1036`.
- `resolution` (enum, default **1 minute**) — total time span: 1 min / 15 min /
  1 hour / 1 day / 1 week. `lib/pn-graph.c:1043`.
- `x-buckets` (uint 2–200, default 200) — number of time buckets the window is
  split into (time-series only). `lib/pn-graph.c:1052`.
- `data-view` (enum, default **Time series**) — Time series vs Distribution.
  `lib/pn-graph.c:1065`.
- `draw-style` (enum, default **Lines**) — Points / Lines / Bars / Error bars.
  `lib/pn-graph.c:1079`.
- `line-color` / `line-width` (1–8, default 2) / `axis-color` /
  `background-color` (default white) / `show-grid` (default off).
- `log-y` (default off, distribution only) / `y-from-zero` (default off,
  time-series linear only). `lib/pn-graph.c:1129` / `:1138`.
- `save-data` (bool, default **off**) — persist the collected samples into the
  worksheet file and restore them on load. Bounded: in-window samples only, at
  most 512 per series. The store itself rides on a hidden read/write string
  property `saved-data` (JSON: per series a topic plus flat
  `[age_ms, value, …]` pairs relative to the save's wall clock), which is not a
  dialog row — it exists only so the generic property serialiser carries it.
  Samples that aged past the `resolution` window while the file sat on disk are
  dropped on load.
- Legacy write-only `mode` enum migrates old saves to data-view+draw-style.
  `lib/pn-graph.c:1157`.

**Renders / acts** — On each message resolves `key` to a finite double and
folds it into per-topic rings (a 200-bucket aggregate ring for time-series, a
2048 raw-sample ring for distribution); the PLplot painter walks those rings
every frame. Repaints are throttled to 10 Hz and a refresh tick keeps the time
axis scrolling between messages. `lib/pn-graph.c:634`.

**Gotchas** — *Multi-topic auto-3D*: each distinct `msg.topic` becomes its own
series; a 2nd topic flips both views into a stacked-Z 3D projection (series
hues walk the golden-angle wheel from `line-color`). Capped at 12 topics — a
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

**Ports** — input only; no output. `lib/pn-xy-graph.c:752`.

**Settings** (two tabs: *Appearance* | *Data*, `lib/pn-xy-graph.c:835`)
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
cloud_cover, pressure_msl, precipitation, wind_direction_10m, time); falls back
to `data/raw/weather` for the Bright Sky provider. `lib/pn-weather-report.c:355`.

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
position" notice. Same 280×173 footprint as Weather Report / Graph.

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
`data/from_long_name` = `me-name`, topic from the node's PnNode topic template;
the same text is pushed locally as a "mine" bubble. `lib/pn-chat.c:237`,
`:554`.

**Gotchas** — Fully canvas-resident text input (no real `GtkEntry`): a blinking
caret, UTF-8-aware draft editing, and the worksheet routing keystrokes only
while focused (`lib/pn-chat.c:500`+). Mouse wheel scrolls history in the zoom
overlay. 10 Hz repaint throttle.

---

## Sound

**Purpose** — Play a short audio clip whenever any message arrives. The message
*contents are ignored* — the trigger fact alone fires the sound.
`lib/pn-sound.c:194` (`pn_sound_receive`).

**When to use** — An audible "ping" on an event (alert fired, job done). Use
**Sound** for a fixed clip; **TTS** to speak the message's `data.output` text;
**Notify** for a visual desktop bubble.

**Ports** — input only; no output. `lib/pn-sound.c:330`.

**Settings**
- `sound` (string, default NULL/unconfigured) — a freedesktop sound-theme id
  (e.g. `bell`, resolved under `/usr/share/sounds/freedesktop/stereo/<id>.oga`)
  or an absolute audio-file path. Empty paints the node red with a ❗.
  `lib/pn-sound.c:340`, resolver `lib/pn-sound.c:133`.
- `dead-period` (uint 0–3600 s, default 0) — mandatory silence after each
  playback; messages within it are dropped. `lib/pn-sound.c:347`.

**Renders / acts** — Spawns `paplay <path>` asynchronously (not
canberra-gtk-play, which would honour the often-off `gtk-enable-event-sounds`).
`lib/pn-sound.c:155`.

**Gotchas** — No overlap: a message arriving while a clip is still `playing`,
or within the dead period after one ends, is dropped (`lib/pn-sound.c:205`).
Depends on `paplay` (PulseAudio); themed ids need the freedesktop sound theme
installed. The settings dialog offers a Preview button (`pn_sound_preview`,
`lib/pn-sound.c:386`).

---

## Text to Speech

**Purpose** — Speak each incoming `data.output` string aloud by piping it
through a Linux TTS program. `lib/pn-tts.c:540` (`pn_tts_receive`),
`lib/pn-tts.c:362` (`pn_tts_speak`).

**When to use** — When the message's text should be heard, not seen — read out
an LLM reply, a chat message, an alert summary. Contrast **Sound** (fixed clip,
ignores content) and **Notify** (silent visual bubble).

**Ports** — input only; no output. `lib/pn-tts.c:850`.

**Settings**
- `engine` (string, default = first installed) — one of `piper`, `espeak-ng`,
  `espeak`, `festival`, `flite`; picking an uninstalled one turns the node red
  with a ❗ and a status message. `lib/pn-tts.c:860`, engine table
  `lib/pn-tts.c:89`.
- `model` (string, dialog label "Voice", default the Lessac en_US piper onnx) —
  a `.onnx` path for piper, an engine voice name otherwise, empty for
  Festival. `lib/pn-tts.c:870`.
- `speed` (double 0.5–2.0, default 1.0) — mapped to each engine's own rate knob;
  Festival ignores it. `lib/pn-tts.c:883`.
- `sink` (string, dialog label "Output", default empty=default sink) —
  PulseAudio sink (`paplay -d` for piper, `PULSE_SINK` env for the rest).
  `lib/pn-tts.c:894`.
- `per-source-voice` (bool, default TRUE) — hash the source-node name to pick a
  voice deterministically per speaker. `lib/pn-tts.c:911`.
- `max-queue` (int −1..MAXINT, default 16) — backlog cap while speaking: 0 =
  drop-while-busy, −1 = unbounded, N = cap+drop. `lib/pn-tts.c:927`.
- `last-error` is read-only/transient (not serialised). `lib/pn-tts.c:905`.

**Renders / acts** — Reads only string `data.output`; missing/non-string is
ignored. Spawns the engine's shell pipeline, feeding the text (with line breaks
flattened to spaces so piper speaks the whole reply) to its stdin; queued
utterances drain in arrival order from `on_speak_done`. `lib/pn-tts.c:550`,
`:315`.

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

**Gotchas** — Mouse-wheel scroll moves through long output one line at a time;
the painter clamps the offset back to live extents. Same 280×173 footprint as
Table View.

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
rows; the on-canvas scroll offset resets when a new message lands.

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
wheel scroll, same 280×173 footprint as Table.

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
