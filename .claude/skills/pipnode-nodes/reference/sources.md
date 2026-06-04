# Sources

Source nodes sit at the head of a flow: they EMIT `PnMessage`s onto an output
port and (almost always) have no input. Each message carries the envelope
(`topic`/`id`/`created`) plus a schemaless `data.*` bag; the three mandatory
bag members are `data.value` (canonical numeric reading, booleans encoded as
`0.0`/`1.0`), `data.output` (human-readable summary string) and
`data.success` (boolean). Sources split into three flavours by *what makes
them fire*: **periodic** ones subclass `PnAutoTrigger` (a background worker
thread ticks every `period` seconds — Clock, AutoInjector, AutoRandom,
Astronomical); **manual** ones fire on a user gesture (Injector click, Knob
wheel, Switch click, FileDrop drop, Panel Input applet click); and the manual
latch/value sources (Knob, Switch, Panel Input) additionally **announce once**
shortly after load via a one-shot `g_idle` scheduled in `constructed()`, so
downstream nodes learn their state without a first gesture.

`PnAutoTrigger` (`lib/pn-auto-trigger.c`) gives its subclasses two extra
properties for free: `period` (uint seconds, range 1–3600, default 1) and
`autostart` (boolean, CONSTRUCT_ONLY, default TRUE — tests pass FALSE for a
quiescent instance). The subclass overrides the `trigger` vfunc, builds the
message on the worker thread, and hands it to
`pn_auto_trigger_emit_on_main()` so signal listeners (which may touch GTK) run
on the main thread. `pn_auto_trigger_kick()` forces an immediate one-shot tick
(Astronomical uses it on property change).

## Clock

**Purpose** — Real-time-clock heartbeat. Emits one message per `period`
seconds carrying the current local time, decomposed into the data bag
(`lib/pn-rtc.c:39`).

**When to use** — As a tick generator / heartbeat to drive downstream work on
a schedule, or to stamp the current time into a flow. Differs from
AutoInjector (which emits a *fixed configured* payload) by emitting the *live*
clock decomposition each tick.

**Ports** — `has_input = FALSE`, `has_output = TRUE` (`pn-rtc.c:100`).

**Settings** — Only the inherited `period` (uint seconds, 1–3600, default 1).
No own properties; the settings dialog is built generically from the
`PnAutoTrigger` `period` pspec.

**Emits** — On every worker tick (`pn-rtc.c:40`). Topic: default (NULL).
Writes: `value` (Unix epoch as double, canonical), `epoch` (int64, full-width
instant), `year`, `month`, `dom`, `hour`, `minute`, `second` (ints),
`timezone` (DST-aware local abbreviation, e.g. "CET"/"CEST"), `success`
(always TRUE), `output` (spoken sentence "The time is HH:MM:SS.").

**Gotchas** — `value` is the epoch seconds (not a 0/1 flag); `epoch` keeps the
same instant at full int64 width. The amber colour + `fa-clock-o` icon are set
in both `class_init` and `init`.

## Injector

**Purpose** — Manual one-shot source. Emits a single configured message when
the user clicks its fire button on the worksheet (`pn_inject_fire`,
`lib/pn-inject.c:298`).

**When to use** — To kick a flow by hand — testing, triggering an action,
injecting a fixed payload on demand. The auto-firing cousin is AutoInjector
(no button, ticks on a timer); use Injector when you want a manual one-shot.

**Ports** — `has_input = FALSE`, `has_output = TRUE` (`pn-inject.c:227`).

**Settings** (`g_object_class_install_properties`, `pn-inject.c:230`):
- `text` (string, default "Injector activated.") — payload emitted as
  `data.output`. Empty/NULL puts the node in the red "configuration required"
  state and firing becomes a no-op (`apply_visual_state`, `pn-inject.c:63`).
- `value` (double, default 0.0) — number emitted as `data.value`.
- `success` (boolean, default TRUE) — emitted as `data.success`.
- `button-icon` (string, default NULL/empty) — freedesktop icon name shown on
  the worksheet fire button; empty keeps the legacy compact play-triangle tab,
  a name swaps in a wider themed-icon button. Picked via the Settings dialog's
  icon combo in the `pn-inject-gui.c` companion.

**Emits** — On `pn_inject_fire()` (user click). Topic: default. Writes
`output` (text), `value`, `success`. Refuses to emit while `text` is
empty/NULL.

**Gotchas** — Body flips teal `fa-paper-plane` ↔ red ❗ on the
configured/unconfigured boundary; the palette icon is pinned to the normal
glyph regardless of state. No startup-announce — Injector only ever fires on
an explicit click.

## FileDrop

**Purpose** — Desktop drop target. Paints a drop-area rectangle below its
header; dropping a file emits one message describing it. Image drops decode
to a `GdkPixbuf`, resize the area to the image's aspect ratio, show a preview,
and emit a `PnImageMessage` that carries the decoded image by pointer
(`pn_filedrop_drop_file`, `lib/pn-filedrop.c:206`).

**When to use** — To pull a file (especially an image) into a flow by
drag-and-drop from the desktop/file manager, e.g. feeding an image-processing
graph. The actual GTK drag-and-drop is in `pn-worksheet.c`, which calls the
public `pn_filedrop_drop_file()`.

**Ports** — `has_input = FALSE`, `has_output = TRUE` (`pn-filedrop.c:397`).

**Settings** (`pn-filedrop.c:400`) — appearance only, both `PnColor` boxed
properties that just affect painting:
- `area-color` (default white) — fill of the drop-area rectangle.
- `border-color` (default dark grey ~70/255) — the 1 px frame.

**Emits** — On a drop (no timer, no startup-announce). Topic: default.
- *Image drop* → `PnImageMessage` with pixels riding the pixbuf pointer (never
  in the JSON bag). Writes `filename`, `path`, `mimetype`, `image`=TRUE,
  `width`, `height` (ints), `size` (int64, omitted if unreadable), `success`
  =TRUE, `output` = "basename (w×h)".
- *Non-image drop* → plain `PnMessage`: `filename`, `path`, `mimetype`
  (octet-stream fallback), `image`=FALSE, `size` (if readable), `success`
  =TRUE, `output` = basename.

**Gotchas** — "Is it an image?" is decided by *trying* to decode with
gdk-pixbuf (a NULL return falls through to the plain-file path). Node geometry
is dynamic: `get_size`/`get_header_height` derive the drop-area height from the
image aspect ratio, clamped 80–360 px (`pn-filedrop.c:117`). gdk-pixbuf is an
allowed core dep, so this logic tier stays GTK-free; the cairo painter lives in
the `pn-filedrop-gui.c` companion, which reads state through the GTK-free
`pn_filedrop_get_paint_state()` seam (`pn-filedrop.c:166`).

## Switch

**Purpose** — Two-position latching slide switch. Clicking the slider toggles
the latch and emits `data.value` 1.0 (on) / 0.0 (off);
`pn_switch_toggle()` flips and emits in one step
(`lib/pn-switch.c:519`). Unusually for a source it also has an *input* and
acts as a value-latch on inbound traffic.

**When to use** — A manual on/off latch the user reads off the slider, or a
loop-breaking latch in a relay-status feedback chain. Use over Knob when the
value is binary; over Injector when you want a persisted latched state plus
inbound drive.

**Ports** — `has_input = TRUE`, `has_output = TRUE` (`pn-switch.c:435`) — the
only Source here with an input.

**Settings** (`pn-switch.c:438`):
- `on` (boolean, default FALSE) — latched position. Writing it programmatically
  flips the visual state *without* emitting; use `pn_switch_toggle()` to flip
  and emit.

**Emits** — Three triggers, all via the overridable `build_outbound_message`
vfunc (`pn-switch.c:117`):
1. user click / `pn_switch_toggle()`;
2. startup-announce one-shot;
3. passthrough of an inbound message whose `value` crosses the 0.5 midpoint.
Default outbound writes `value` (1.0/0.0) and `success` (mirrors `on`). Topic:
default.

**Gotchas** — `receive()` (`pn-switch.c:182`) is a *latch with edge-trigger*:
an inbound `value` equal to the current latch is **dropped on the floor** (no
forward) to break relay-status feedback loops; an inbound message with no
usable numeric `value` passes through untouched with the latch unchanged.
Midpoint test is `value > 0.5`. Startup-announce fires once via a
`constructed()` `g_idle` (`pn-switch.c:371`), gated at *fire* time by
`announce_on_startup` (default TRUE) — read late because subclass props (e.g.
`PnTasmotaSwitch`'s enforce-on-startup) are applied by the loader after
`constructed()`; outward-effect subclasses clear the flag so opening a
worksheet never actuates hardware. Cairo slider painter lives in
`pn-switch-gui.c`; `pn_switch_hit_slider()` (`pn-switch.c:251`) lets the
worksheet route a slider click.

## Knob

**Purpose** — Rotary knob source. A dial on the node header; spinning the mouse
wheel over it turns the knob and emits `data.value` mapped onto `[min, max]`
(`pn_knob_scroll`, `lib/pn-knob.c:216`).

**When to use** — Hand-dial a continuous value into a flow (a setpoint, a test
input). Use over Switch when the value is continuous rather than binary.

**Ports** — `has_input = FALSE`, `has_output = TRUE` (`pn-knob.c:415`).

**Settings** (`pn-knob.c:418`):
- `min` (double, default 0.0) — value at fully-left.
- `max` (double, default 1.0) — value at fully-right.
- `value` (double, default 0.0) — current position, kept clamped into
  `[min, max]` (tolerates an inverted range). Writing `value` (or `min`/`max`)
  turns the dial *without* emitting.

**Emits** — Only on a wheel notch (`pn_knob_scroll`) and once at startup. Topic:
default. Writes just `value` (the canonical reading). Note it does **not** set
`output` or `success`. One full sweep = 24 wheel detents
(`PN_KNOB_SCROLL_TICKS`), so a notch moves by `(max-min)/24`.

**Gotchas** — Writing the properties never emits — only a wheel rotation (or
the startup shot) does. Startup-announce: one-shot `g_idle` in `constructed()`
(`pn-knob.c:361`) emits the loaded position once after the graph is wired;
`dispose()` pulls the idle if the node dies first. Cairo dial painter in
`pn-knob-gui.c`, reading position via `pn_knob_get_value_fraction()`
(`pn-knob.c:161`); `pn_knob_hit_knob()` is the worksheet hit-test.

## Panel Input

**Purpose** — Source driven by an XFCE panel applet over D-Bus. The background
engine calls `pn_panel_input_send()` with a numeric value, or
`pn_panel_input_send_event()` to report an applet mouse event
(`lib/pn-panel-input.c:233`). The I/O counterpart of Panel Display.

**When to use** — Wire a physical XFCE panel button click into a worksheet (route
on topic, speak the output, toggle a Switch, count clicks…). Use when the
trigger comes from the panel applet rather than the worksheet canvas.

**Ports** — `has_input = FALSE`, `has_output = TRUE` (`pn-panel-input.c:193`).

**Settings** (`pn-panel-input.c:196`):
- `value` (double, default 0.0) — numeric value carried on the next emitted
  message; restored from the worksheet on load, announced once on startup, and
  updated each `pn_panel_input_send()`.

**Emits** — Three paths:
1. *Value send* (`pn_panel_input_send`, `pn-panel-input.c:233`) — topic
   default; writes `value`, `success`=TRUE.
2. *Mouse event* (`pn_panel_input_send_event`, `pn-panel-input.c:265`) — topic
   `"<event>/<button>"` e.g. `click/left`; writes `output` ("Applet <button>
   mouse button <verb>."), `value` = button number (1 left / 2 middle / 3
   right, else `buttonN`), `button` (name), `event` (e.g. "click"), `success`
   =TRUE.
3. *Startup-announce* — emits the loaded `value` once.

**Gotchas** — A mouse event does **not** disturb the configured `value`
property (it ships the button number on that message's `value` only).
Startup-announce one-shot via `constructed()` `g_idle` (`pn-panel-input.c:150`),
same deferred-wiring rationale as Switch/Knob. Renders with the default node
painter — no gui companion. `fa-hand-pointer-o` icon (FontAwesome 4.7).

## AutoInjector

**Purpose** — Auto-firing cousin of Injector: emits the same configured payload
every `period` seconds with no external prodding
(`pn_auto_injector_trigger`, `lib/pn-auto-injector.c:61`).

**When to use** — A steady heartbeat for downstream nodes — a constant Graph
baseline, an "alive" pulse for a Watchdog, or a stub source while a real probe
is built. No fire button; use Injector for a manual one-shot instead.

**Ports** — `has_input = FALSE`, `has_output = TRUE` (`pn-auto-injector.c:211`).

**Settings** (`pn-auto-injector.c:214`) — plus inherited `period`:
- `output` (string, default "AutoInjector activated.") → `data.output`.
- `value` (double, default 0.0) → `data.value`.
- `success` (boolean, default TRUE) → `data.success`.

**Emits** — Every worker tick. Topic: default. Writes `value`, `success`, and
`output` (when non-NULL) — exactly the three canonical members, so it slots
into the same wiring an upstream probe would.

**Gotchas** — Payload fields are read under an instance `mutex` so a
mid-tick property change can't half-apply into an in-flight message (worker
thread vs main-thread setters). Orange body (vs Injector's teal) signals
"this one fires itself"; shares the `fa-paper-plane` glyph.

## AutoRandom

**Purpose** — Auto-triggered sibling of AutoInjector that fills `data.value`
with a freshly *sampled* random number every `period` seconds, over a
configurable range and distribution (`pn_auto_random_trigger`,
`lib/pn-auto-random.c:228`).

**When to use** — Synthetic noisy signal for a Graph, a known-distribution
stress test for a Filter, or a stand-in for a probe whose dynamic range is
already known. Use over AutoInjector when you want a varying rather than fixed
value.

**Ports** — `has_input = FALSE`, `has_output = TRUE` (`pn-auto-random.c:415`).

**Settings** (`pn-auto-random.c:418`) — plus inherited `period`:
- `output` (string, default "AutoRandom sample.") → `data.output`.
- `min` (double, default 0.0) — lower bound, inclusive.
- `max` (double, default 1.0) — upper bound, inclusive. `min > max` is swapped
  silently.
- `distribution` (enum `PnAutoRandomDistribution`, default `uniform`;
  registered GType so the dialog renders a combobox — nicks `uniform` /
  `normal` / `triangular` / `exponential`). normal = Gaussian, mean=midpoint,
  stddev=range/6; triangular = symmetric apex at midpoint; exponential =
  lambda 3/range, biased toward `min`. Every draw is clamped to `[min, max]`.
- `success` (boolean, default TRUE) → `data.success`.

**Emits** — Every worker tick. Topic: default. Writes `value` (sampled,
clamped), `success`, and `output` (when non-NULL).

**Gotchas** — Each instance owns a `GRand` seeded with time **plus its own
address bits** so two AutoRandoms dropped together don't lock-step
(`pn-auto-random.c:471`). RNG is touched only on the (serialised) worker
thread, so it lives outside the payload `mutex`; the other fields are
mutex-guarded as in AutoInjector. Coral/salmon body + `fa-random` glyph
distinguish it from the paper-plane injectors.

## Astronomical

**Purpose** — Emits the Sun's position and the day's solar events for a named
city, recomputed every `period` seconds (default 60). The city is geocoded to
lat/lon once via the shared `pn-geocode` lookup (Open-Meteo, the only network
call, cached); everything else is computed locally from the NOAA
solar-position equations in `pn_astronomical_compute()`
(`lib/pn-astronomical.c:54`).

**When to use** — A daylight signal: wire the boolean Sun up/down into an Edge
detector for sunrise/sunset, or into a Switch/Filter to gate a flow on
daylight. Also exposes altitude, azimuth, day length and event times for
richer scheduling.

**Ports** — `has_input = FALSE`, `has_output = TRUE` (`pn-astronomical.c:595`).

**Settings** (`pn-astronomical.c:598`) — plus inherited `period`:
- `city` (string, default "") — place to compute for; geocoded once through
  Open-Meteo (re-geocodes when changed). Empty city stays quiet (no emission).
- `datum` (enum `PnAstronomicalDatum`, default `PN_ASTRO_DATUM_SUN_IS_UP`;
  registered GType → combobox, nicks are the visible labels and the serialised
  strings). Selects which computed quantity drives `data.value`: "The Sun is
  up" (1.0 up / 0.0 down), "The Sun is down" (inverse), "Sun altitude
  (degrees)", "Sun azimuth (degrees)", "Day length (hours)".

**Emits** — Every worker tick (`pn_astronomical_trigger`,
`pn-astronomical.c:353`). Topic: default. Always writes the full field set
regardless of `datum`: `value` (the chosen datum), `success`, `sun_up`
(bool), `sun_altitude`, `sun_azimuth`, `day_length` (doubles), `solar_noon`
(ISO 8601 string in the location tz), `sunrise`/`sunset` (ISO 8601, omitted in
polar day/night), `latitude`, `longitude` (doubles), `city`, `country`
(strings), `timezone` (IANA name), `output` ("At <place> the Sun is
up/down"). On a geocode failure it emits `city`, `success`=FALSE and an
`output` reason instead.

**Gotchas** — Geocoding runs on the worker thread (`astro_ensure_coords`,
`pn-astronomical.c:263`), cached under `mutex` keyed on `geo_key` (the city it
was resolved from); a failed lookup nulls `geo_key` to force a retry next tick.
Changing `city` or `datum` calls `pn_auto_trigger_kick()` to re-emit promptly
(`pn-astronomical.c:554`). Times are formatted in the location's own IANA time
zone (falls back to UTC if the geocoder didn't supply one); sunrise/sunset are
for the UTC calendar day of the queried instant. `sun_up` uses the −0.833°
refraction-corrected horizon so it agrees with the reported times. `fa-sun-o`
icon.
