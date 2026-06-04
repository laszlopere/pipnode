# GUI Displays, Gauges & Indicators

These ten core nodes are **pure sinks**: each consumes a `PnMessage` and renders it as a visual readout (LED panels, clock faces, needle gauges, a status lamp). Every one has `has_input = TRUE` / `has_output = FALSE` and never forwards a message. Each is split into a GTK-free **logic tier** (`lib/pn-<name>.c`: GType, properties, `receive()`, a GTK-free "paint state" read seam) and a **gui tier** (`lib/pn-<name>-gui.c`: cairo/Pango drawing, installed onto the class at editor startup by `pn_<name>_gui_install()`, leaking one class ref so the vfunc slots stay valid). The headless runtime runs the logic tier without GTK. Icons are FontAwesome 4.7 only. Most read `data.value` (the canonical numeric; booleans encoded 0.0/1.0, tested `> 0.5`); the character/text displays (Segment16, Matrix57, Label) read `data.output`. Numeric repaints are suppressed when the *visible* reading wouldn't change, so a hot once-a-second feed drives at most one redraw per second. The amber body colour `{0.92, 0.76, 0.27}` is shared across the metering family (Numeric, Segment16, Matrix57, DigitalClock, AnalogClock, AnalogMeter, Dial, Countdown) so a worksheet's "metering row" reads as one group; LED is steel-blue and Label is teal (the panel-mirrored family).

## Numeric

**Purpose** — Paints a numeric `data.value` on a row of seven-segment LED digits (the generic-number cousin of Countdown). Integer part + optional fixed-point fractional part, with a leading minus-sign cell. No captions. `lib/pn-numeric.c:120` (`receive`).

**When to use** — Show a single live number as a classic red-on-black LED voltmeter. Vs **Segment16/Matrix57**: those render *text* (`data.output`); Numeric renders a *number* (`data.value`) with fixed-width zero-jitter layout, sign cell, and decimal point. Vs **Dial/AnalogMeter**: digital readout instead of a needle gauge. Vs **Countdown/DigitalClock**: free-form number rather than a time breakdown.

**Ports** — input only.

**Settings** (`pn-numeric.c:280`+) — `digits` (uint, 1–12, default **6**) integer cells; an integer part that doesn't fit reads as all dashes (overload). `decimal-places` (uint, 0–6, default **2**); 0 drops the decimal point and fractional block. `background-color` (default near-black `{0.04,0.04,0.05}`). `segment-color` (lit bar + decimal dot, default LED red `{0.92,0.12,0.08}`). `unlit-segment-color` (off-state ghost, dim red `{0.1472,0.0192,0.0128}`). Dialog tabs: Display (digits, decimal-places), Colours.

**Renders** — `data.value` (double or int64; non-finite/non-numeric ignored) → `pn_numeric_set_value`. Painter (`pn-numeric.c` gui) rounds half-away-from-zero to `decimal-places`, lays out `[sign][int digits][.][frac digits]`. Leading integer zeros render as blank cells (`" 1.23"`, not `"001.23"`); last integer cell always lights; sign cell blank when non-negative. Unlit segments paint as ghosts so the panel always reads as an LED face. Overload (int part > 10^digits−1) lights every digit as the centre `g` bar (dashes). Before the first message: blank screen (ghosts only). `paint_plot_corner_radius = 9.0`, `paint_plot_skip_zoom = TRUE`.

**Gotchas** — `has_value` latches on the first numeric message so a fresh node reads blank rather than `0.00`. Repaint only when the stored value differs (rounding-resolution-aware in practice via the painter).

## Segment16

**Purpose** — Paints an incoming string (`data.output`) on a row of sixteen-segment "starburst" LED cells — the alphanumeric cousin of Countdown/Segment readouts. `lib/pn-segment16.c:90` (`receive`).

**When to use** — Display short alphanumeric text in a retro starburst-LED look. Vs **Numeric**: Segment16 takes a string, not a number. Vs **Matrix57**: Segment16 is angular 16-segment LED bars (red-on-black) — Matrix57 is a dot-matrix character LCD (dark dots on greenish-yellow) with multi-line support. Vs **Label**: Segment16 is a stylised segment font with a fixed glyph table (printable ASCII only); Label uses the real desktop UI font.

**Ports** — input only.

**Settings** (`pn-segment16.c:266`+) — `cells` (uint, 1–32, default **8**) cell count; text longer than the row is cropped on the right. `background-color` (near-black). `segment-color` (lit, LED red). `unlit-segment-color` (dim-red ghost). Dialog tabs: Display (cells), Colours.

**Renders** — `data.output` (must be a JSON string; non-string/missing blanks the row) → `pn_segment16_set_text`. Painter walks text and cells together via an ASCII→16-bit glyph mask table (`glyph_masks[128]` in `pn-segment16-gui.c`); lowercase folds to uppercase; non-ASCII / unmapped punctuation lights no segments. Each cell drawn in two passes (dim ghosts first, lit bars on top) so chevron tips cover ghosts at the centre cluster. `paint_plot_corner_radius = 9.0`, `skip_zoom = TRUE`.

**Gotchas** — `set_text` normalises `NULL` and `""` both to the blank state but only repaints when the visible string actually changes (hot-source throttle). `'.'` lights no segments (a real LED period would be a separate dot).

## Matrix57

**Purpose** — Paints an incoming string (`data.output`) on a row of 5×7 dot-matrix LCD cells — the character-LCD (HD44780) cousin of Segment16. Supports one or two rows. `lib/pn-matrix57.c:200` (`receive`).

**When to use** — Display text (optionally two lines, e.g. log tail) in a green character-LCD look. Vs **Segment16**: dot-matrix LCD with distinct upper/lower-case glyphs and 1–2 line support, vs angular red LED. Vs **Label**: fixed bitmap-style 5×7 font and a hardware-LCD aesthetic vs the real UI font.

**Ports** — input only.

**Settings** (`pn-matrix57.c:368`+) — `cells` (uint, 1–40, default **16**, the canonical HD44780 line); cropped on the right. `lines` (int, 1–2, default **1**, spin editor); when more newline-separated lines arrive than configured, the leading ones are dropped (keeps the latest). `frame-color` (plastic bezel, near-black `{0.08,0.08,0.09}`). `background-color` (LCD face, greenish-yellow `{0.72,0.80,0.28}`). `pixel-color` (lit dot, near-black `{0.06,0.08,0.04}`). `unlit-pixel-color` (very faint off-state ghost `{0.66,0.74,0.26}`). Dialog tabs: Display (cells, lines), Colours.

**Renders** — `data.output` string → `pn_matrix57_set_text`. Text is normalised (`matrix57_normalize`): literal `\n`/`\t`/`\\` escapes resolved, `\r` stripped, so a Format-style `"l1\nl2"` breaks onto the next row. Then tailed (`matrix57_tail`) to the last `lines` rows (trailing blank lines dropped first). The painter walks `display_text` over the `cells`-wide row. Increasing `lines` does NOT grow the node — both rows share the external rectangle, dot pitch shrinks to fit.

**Gotchas** — Two cached strings: `output` (raw) drives repaint-suppression; `display_text` (normalised+tailed) is what the painter walks; changing `lines` recomputes `display_text`. `paint_plot_corner_radius = M57_BEZEL_RADIUS`, `skip_zoom = TRUE`.

## DigitalClock

**Purpose** — A black seven-segment LED panel laid out `HOURS:MINUTES:SECONDS` — literally the Countdown node with the days field dropped (same colours/segment sizes, narrower client area). `lib/pn-digital-clock.c:175` (`receive`).

**When to use** — Show a wall-clock time driven by an epoch-seconds feed (canonically the Clock/RTC node). Vs **AnalogClock**: same logic/feed/timezone resolver, but seven-segment digits instead of a round dial with hands. Vs **Countdown**: clock face (modulo a day, 24h) vs counting-down DAYS+HH:MM:SS. Vs **Numeric**: time breakdown vs free number.

**Ports** — input only.

**Settings** (`pn-digital-clock.c:380`+) — `show-labels` (bool, default **TRUE**) draw HOURS/MINUTES/SECONDS captions. `timezone` (string, default `"Not Set"` = `PN_TZ_NOT_SET`, combo from shared tz-table); a fixed zone shifts the readout by a fixed UTC offset, `"Not Set"` defers to the message's `timezone` abbreviation member (Clock emits e.g. `"CEST"`), unknown/missing → GMT. `background-color` (near-black). `segment-color` (LED red). `unlit-segment-color` (dim-red ghost). `label-color` (soft off-white `{0.82,0.84,0.86,0.92}`). Dialog tabs: Display (show-labels, timezone), Colours.

**Renders** — `data.value` = a **count of seconds** (double or int64). `display_value()` clamps negative/no-value to 0, applies the active UTC offset, takes mod 86400 (wraps into `[0,86400)`), and splits into H:M:S. Zero or negative or pre-message → all zeroes. Seven-segment cairo in the gui tier.

**Gotchas** — `set_value` snapshots `display_value` before/after and repaints only if the visible digits change → exactly one redraw per second on a 1 Hz feed. A configured zone wins outright but the wire's `timezone` hint is still snapshotted so switching back to "Not Set" picks up the same instant. Panel-applet mirrored (like Countdown/LED/Label) — a DigitalClock on a worksheet's panel band shows a live HH:MM:SS on the desktop panel.

## Label

**Purpose** — Shows an incoming message's `data.output` text on a small styled panel — the text counterpart of DigitalClock. Displays the **last one or two lines** (a live tail of a log / command output). `lib/pn-label.c:185` (`receive`).

**When to use** — Show arbitrary text/strings (status, last log line, command output) in the real desktop UI font. Vs **Segment16/Matrix57**: Label uses the actual UI font (not a segment/dot glyph table), fits the box, and crops over-long lines on the right; no character-restriction. Vs **Numeric/clocks**: free text rather than a parsed number/time.

**Ports** — input only.

**Settings** (`pn-label.c:370`+) — `lines` (int, 1–2, default **1**, spin) trailing lines to show. `alignment` (string, "Left"/"Center"/"Right", default **"Center"**, combo). `text-color` (soft off-white `{0.90,0.92,0.94}`). `background-color` (dark slate `{0.10,0.11,0.13}`; set alpha 0 for a transparent label). Dialog tabs: Text (lines, alignment), Colours. **Font face/size/weight are NOT configurable** — forced to the desktop UI font, fill (100%), Semi-Bold (Pango 600), upright, so the readout always looks like a panel clock.

**Renders** — `data.output` string (number-only or non-string message blanks the label) → `pn_label_set_text`, which normalises (resolve `\n`/`\t`/`\\` escapes, strip `\r`; `NULL`/empty/all-blank → blank) and tails to the last `lines` lines. Fixed footprint (the DigitalClock's size) — never resizes with data; an over-long line is cropped on the right (beginning stays visible). Pango drawing + flexible measuring live in the gui tier.

**Gotchas** — Two cached strings (`output` raw-normalised, `display_text` tailed); repaint suppressed when raw output is unchanged. Panel-applet mirrored — the same text appears on the desktop panel, fitted to the panel row and cropped past a max width.

## AnalogClock

**Purpose** — A round analogue wall-clock face: off-white dial, slim metallic bezel, twelve hour markers, sixty minute pips, three drop-shadowed pivoted hands (hour, minute, optional seconds), and a caption below the pivot. `lib/pn-analog-clock.c:185` (`receive`).

**When to use** — Wall-clock time as a dial-with-hands. Vs **DigitalClock**: identical logic/feed/timezone resolver, but a round face with sweeping hands instead of digits. Vs **Dial/AnalogMeter**: AnalogClock's needle angle is fixed by the time-of-day, not by a min/max value range.

**Ports** — input only.

**Settings** (`pn-analog-clock.c:435`+) — `text` (string, default `""`) caption below the pivot (logo slot). `show-seconds` (bool, default **TRUE**) draw the thin seconds hand. `timezone` (string, default `"Not Set"`, combo) — same resolver as DigitalClock. Colours: `face-color` (white `{0.97,0.97,0.96}`), `marker-color` (black markers+pips), `hour-hand-color`, `minute-hand-color` (both black), `second-hand-color` (bright red `{0.85,0.12,0.10}`), `text-color` (dark `{0.20,0.20,0.22}`). Dialog tabs: Display (text, show-seconds, timezone), Colours.

**Renders** — `data.value` = seconds; `display_value()` clamps negative/no-value to 0, applies UTC offset, mod 86400 → H:M:S. Hands swept as a 24-hour clock (hour hand makes two full turns/day). Zero/negative/pre-message → hands park at 12 o'clock. Square footprint (220×220, matches Dial). cairo face/hands in gui tier.

**Gotchas** — Same repaint-only-on-visible-change + tz-hint snapshot behaviour as DigitalClock. Turning off `show-seconds` gives a quieter face that only repaints once a minute.

## AnalogMeter

**Purpose** — A flat panel-meter sink: square white plastic case, paper-white face, a **diagonal** tick arc (lower-left → top → upper-right) with a long thin needle whose pivot sits in the **lower-right corner**, plus a unit glyph and accuracy-class string — the classic old AC voltmeter / ammeter look. `lib/pn-analog-meter.c` (`receive`, JSON-path value reader, damped-spring needle).

**When to use** — Show a single numeric value as an electrician's panel-meter. Vs **Dial**: both are needle gauges reading a value off a JSON `key` through the same damped-spring integrator. Choose **AnalogMeter** for the square housing / diagonal scale / monochrome unit-glyph look (no zone bands); choose **Dial** for a round bezelled gauge with green/yellow/red zone arcs and a process-control-dashboard look.

**Ports** — input only.

**Settings** (`pn-analog-meter.c:680`+) — Data: `key` (string, JSON path, default **`data/value`**) numeric value the needle points at; `unit` (string, default **`V`**) dominant glyph in the upper-left; `mode` (enum `PnAnalogMeterMode` `AC`/`DC`/`None`, default **AC**) picks the IEC current-type symbol below the unit (AC = sine "~", DC = line+dashes, None = empty); `accuracy-class` (string, default **`2.5`**, cosmetic). Scale: `min-value`/`max-value` (double, default **0** / **300**); `start-angle`/`end-angle` (double deg CW from 12 o'clock, default **−90** / **0** = diagonal sweep around the lower-right pivot); `major-ticks` (uint 2–50, default **4**, an *upper bound*, "nice" {1,2,5}×10ⁿ positions); `minor-ticks-per-major` (uint 1–20, default **10**; 1 suppresses). Colours: `frame-color` (case), `face-color` (near-white), `scale-color`, `needle-color`, `label-color` (all near-black default).

**Renders** — Each message: value plucked via `pn_json_resolve_path(root, key)` coerced to a finite double (int/double/numeric-string accepted). Drives `target_value`; a damped-spring integrator (same tuning as Dial — underdamped, visible overshoot) animates `display_value` toward it over a few hundred ms so the needle reads like a real moving-coil meter. Needle angle interpolates `start-angle`→`end-angle` over `min-value`→`max-value`.

**Gotchas** — Unresolvable `key` or non-finite value: needle stays put. Out-of-range readings **clamp** to the nearest scale endpoint (no swinging past the ticks). Arc radius auto-sized from the actual sweep. Click the face to lift it into the centred zoom overlay.

## Dial

**Purpose** — A round analogue dial: metallic bezel, glassy face, tick marks with engraved digits, a 3D-shaded needle on a metallic centre pivot, and optional green/yellow/red zone arcs. Reads a numeric value off a JSON `key`. `lib/pn-dial.c:375` (`receive`), `:295` (damped-spring needle).

**When to use** — Single numeric value as a process-control gauge with warning zones. See AnalogMeter "When to use" for the Dial-vs-Meter split: Dial = round bezel + colour zone bands + glassy face; Meter = square panel + diagonal scale + monochrome.

**Ports** — input only.

**Settings** (`pn-dial.c:709`+) — Data: `key` (string JSON path, default **`data/value`**). Scale: `min-value`/`max-value` (double, default **0** / **120**); `start-angle`/`end-angle` (double −360..360 deg CW from 12 o'clock, default **−120** / **+120** = classic 240° sweep; full 360° allowed); `major-ticks` (uint 2–50, default **13**, upper bound, "nice" {1,2,5}×10ⁿ positions each carrying a digit label); `minor-ticks-per-major` (uint 1–20, default **5**; 1 suppresses). `label` (string, default `""`) name of the quantity, lower-centre wedge; `unit` (string, default `""`) bigger suffix below it. Zones: `green-start`/`green-end`, `yellow-start`/`yellow-end`, `red-start`/`red-end` (double, value-unit bounds; arc omitted when start ≥ end; defaults split the 0..120 scale 0–60 green / 60–90 yellow / 90–120 red). Colours: `face-color`, `scale-color`, `needle-color`, `label-color`, `green-color`/`yellow-color`/`red-color` (zone strokes). Read-only `value` (double) exposes the latest received value. Dialog grouped into ~four tabs (Data | Scale | Zones | Colours).

**Renders** — `pn_json_resolve_path(root, key)` → finite double → `target_value`; damped-spring integrator (stiffness k, damper) animates the needle (moving-coil feel, no teleport). Needle angle interpolates `start-angle`→`end-angle` over the value range. Zone arcs sit just inside the tick band. The bezel and centre pivot use **fixed** silver/grey metallic gradients (NOT configurable); every *painted* colour (face/ticks/digits/needle/label/zones) is a property.

**Gotchas** — Unresolvable `key` / non-finite: needle holds. Out-of-range: clamps to nearest endpoint. The "nice"-step tick placement makes zone bounds land on real tick angles (a zone "9 to 11" lands exactly at the value-9 and value-11 ticks). Click the face to zoom.

## LED

**Purpose** — A status indicator shaped like a 5 mm through-hole LED in a black panel-mount bracket, drawn inset on the right of the node header. Four behaviour modes. `lib/pn-led.c:230` (`receive`).

**When to use** — Heartbeat / liveness / latched-state lamp at the end of a pipeline. **Flash** after a filter to see messages still flow; a level-driven mode after a Threshold/Comparator to show a latched on/off (Steady = quiet, Blink Slow = heartbeat, Blink Fast = draw the eye to a fault). Vs the numeric/gauge sinks: a single boolean/activity indicator, not a value readout.

**Ports** — input only. (Category is `Sinks`, not `GUI/...`; body colour steel-blue `{0.40,0.55,0.70}`.)

**Settings** (`pn-led.c:430`+) — `mode` (enum `PnLedMode`: Flash / Steady (level) / Blink Slow / Blink Fast, default **Flash**). `color` (lit colour, default bright green `{0.20,0.85,0.30}`; unlit face is always neutral grey). `hold-ms` (uint, floor **100**, default **250**, max 3600000) — minimum lit time after the most-recent message in **Flash mode only**; the timer resets on every message so a steady stream stays lit. The settings dialog greys out `hold-ms` unless mode == Flash (`pn_settings_schema_enable_when_eq`).

**Renders** — **Flash**: every `receive` (re)arms a `hold_ms` off-timer and lights the lamp (activity blink); ignores `data.value`. **Steady / Blink Slow / Blink Fast** are level-driven: they read `data.value` (int/double) and latch **on when `value > 0.5`**, off at/below; a message with no numeric value leaves the latch as-is. Steady holds the level; Blink Slow oscillates ~1 Hz (500 ms half-period), Blink Fast ~4 Hz (125 ms). cairo dome + bracket + glow in the gui tier (`paint_header_overlay`).

**Gotchas** — Boolean encoding is the standard `value > 0.5` midpoint test (matches Threshold/Comparator). At most one timer (off-timer or blink-tick) is ever scheduled. Switching modes resets to a clean dark lamp (cancels timers, drops the latch). 100 ms floor guarantees visibility on a 60 Hz display. Panel-applet mirrored.

## Countdown

**Purpose** — A black seven-segment LED panel laid out `DAYS  HOURS:MINUTES:SECONDS`, the look of a wall-mounted event countdown. Reads `data.value` as **seconds remaining**. `lib/pn-countdown.c:145` (`receive`).

**When to use** — Show time remaining to a deadline. Canonical chain: Clock → Deadline → Countdown (Deadline emits seconds-remaining). Vs **DigitalClock**: Countdown adds a DAYS field and counts *down* a total; DigitalClock is Countdown minus days, showing a 24h wall clock. Vs **Numeric**: a time breakdown rather than a raw number.

**Ports** — input only.

**Settings** (`pn-countdown.c:334`+) — `day-digits` (uint, 1–6, default **3**, counts to 999 days; a larger days count saturates at all nines, no wrap). `show-labels` (bool, default **TRUE**) DAYS/HOURS/MINUTES/SECONDS captions. `background-color` (near-black), `segment-color` (LED red), `unlit-segment-color` (dim-red ghost), `label-color` (soft off-white). Dialog tabs: Display (day-digits, show-labels), Colours.

**Renders** — `data.value` (seconds remaining, double/int64; non-numeric ignored, leaves the reading) → `pn_countdown_set_value`. `display_remaining()` clamps to ≥0 and breaks into days/hours/minutes/seconds. Zero/negative (deadline passed) or pre-message → all zeroes. Seven-segment cairo in the gui tier.

**Gotchas** — `set_value` snapshots `display_remaining` before/after and repaints only when the visible digits change → one redraw per second on a 1 Hz feed. **Controller / panel-mirror**: a Countdown (like LED/Label/DigitalClock) snapped onto a worksheet's panel band is mirrored by the panel editor (`lib/pn-panel-editor.c`) to one live readout per node on the desktop panel — the controller keeps a single live readout per Countdown node across all sheets. The Numeric/Segment16/Matrix57 panels deliberately mirror this node's bezel look, colours and segment proportions so the whole LED family reads identically.
