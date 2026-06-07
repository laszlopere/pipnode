# Tasmota (bundled plugin)

Nodes from the in-tree **`plugins/tasmota`** plugin (loadable module `pipnode_tasmota.so`, GUI companion `pipnode_tasmota-gui.so`). They talk to [Tasmota](https://tasmota.github.io/) smart plugs/switches/sensors **over MQTT** — none of them open a socket themselves. The pattern is always: an **MQTT Source** subscribes to the broker and fans raw publishes into these nodes; the command-side nodes reshape a message and a downstream **MQTT Sink** publishes it. So a Tasmota node is a *filter/transformer/gauge* on the MQTT stream, not an I/O node.

All 15 are registered in `plugins/tasmota/pn-tasmota-plugin.c:65-79`. They share helpers in `pn-tasmota-common.c`:

- `pn_tasmota_device_from_topic(topic)` — the device name is the **second-to-last** topic segment (`tele/<device>/SENSOR` → `<device>`); needs ≥3 segments or returns NULL (`pn-tasmota-common.c:84`).
- `pn_tasmota_topic_is_family(topic)` — TRUE only for the three roots Tasmota speaks on: `tele/`, `stat/`, `cmnd/`. The discovery-side code gates on this so a foreign app on a shared broker (zigbee2mqtt, Shelly, Node-RED) isn't mistaken for a Tasmota device.
- `pn_tasmota_find_number(payload, key, …)` — finds a numeric key top-level **or** one level down inside each per-sensor child object (`AM2301.Temperature`, `DHT22.Temperature`, …).
- `PnTasmotaStatusKind` GEnum — the 12 `Status N` sections (0 Everything … 11 Device state; `pn-tasmota-common.c:26-64`).

**Categories below:** energy gauges (sinks) · sensor filters · relay/switch control · status & discovery.

The Devices menu also has a **Tasmota Devices** dialog (live MQTT fleet discovery) provided by the GUI companion — that's a dialog, not a worksheet node, so it isn't catalogued here.

---

## Energy gauges (sink nodes — needle only, no output)

Six nodes — **Tasmota Power, Current, Voltage, Apparent Power, Reactive Power, Power Factor** — subclass the abstract `PnTasmotaEnergyMeter` → core **`PnAnalogMeter`** → `PnNode` (`pn-tasmota-energy-meter.c:42`). They are **terminal gauge sinks**: `has_input=TRUE`, `has_output=FALSE`; they drive a moving-coil needle and **never re-emit or write any `data.*` member**. They differ from each other *only* in the extracted JSON key / scale / unit / glyph mode. Tasmota Power is the worked example; the others are identical bar the table row.

### Tasmota Power
**Purpose** — Gauge sink driving an analog panel-meter needle from one Tasmota energy device's live power reading. Subclasses `PnTasmotaEnergyMeter` (`pn-tasmota-power.c:27`); the reading is pulled via the inherited `key` JSON-path, set to `data/payload/ENERGY/Power` (`pn-tasmota-power.c:52`), which `PnAnalogMeter` resolves against the whole envelope with `pn_json_resolve_path` and feeds to the needle (`pn-analog-meter.c:374-397`).

**When to use** — Watch a single Sonoff Pow / Pow R2 / Pow Elite device's instantaneous wattage on a dial.

**Ports** — `has_input=TRUE`, `has_output=FALSE` (inherited, `pn-analog-meter.c:677-678`). Terminal sink — never re-emits.

**Settings** — Dialog tabs Class | Node | **Data** | **Scale** | **Colours** (schema on the base, `pn-analog-meter.c:806-832`):
- `switch-name` (string, default empty) — the Tasmota device to filter on (matched against the second-to-last topic segment). **Empty = unconfigured: rejects every message, needle parked** (`pn-tasmota-energy-meter.c:208-219`).
- `key` (string, default `data/payload/ENERGY/Power`) — JSON path to the number (`pn-tasmota-power.c:52`).
- `unit` (string `"W"`), `mode` (enum AC/DC/None, default None — power is derived so no AC/DC glyph) (`pn-tasmota-power.c:55-56`).
- `min-value`/`max-value` (double, `0.0`/`5000.0`), plus inherited scale geometry (`start-angle`/`end-angle` -90/0, `major-ticks` 4, `minor-ticks-per-major` 10, `accuracy-class` "2.5") and 5 colours (`frame`/`face`/`scale`/`needle`/`label`). The `topic` row is hidden (no output).

**Emits / consumes** — Consumes **only** messages whose topic last segment is exactly `SENSOR` (`tele/<device>/SENSOR`, `pn-tasmota-energy-meter.c:59-72`) **and** whose device equals `switch-name` (`:99-104`); all else dropped silently. Reads the number at `data/payload/ENERGY/Power`. Writes **nothing** back — the value only moves the needle (`pn_analog_meter_set_value`, `pn-analog-meter.c:396`).

**Gotchas** — Unconfigured `switch-name` = silent reject, no error glyph (`pn-tasmota-energy-meter.c:92-93`). Needle stays hidden until the first real value, so parked ≠ a genuine 0 reading (`pn-analog-meter.c:913`). Repaints throttled to 30 Hz; the needle is a damped-spring animation, not instantaneous. Non-finite/non-numeric values are ignored. The whole family shares the inherited gauge icon (FA `fa-tachometer` U+F0E4) and yellow body `(0.92,0.76,0.27)` — none override it.

### The other five (identical to Power except this row)

| Display name | GType | `key` (JSON path) | min–max | unit | `mode` | file:line |
|---|---|---|---|---|---|---|
| Tasmota Power | `PN_TYPE_TASMOTA_POWER` | `data/payload/ENERGY/Power` | 0–5000 | `W` | None | `pn-tasmota-power.c:51-57` |
| Tasmota Current | `PN_TYPE_TASMOTA_CURRENT` | `data/payload/ENERGY/Current` | 0–20 | `A` | AC | `pn-tasmota-current.c:48-54` |
| Tasmota Voltage | `PN_TYPE_TASMOTA_VOLTAGE` | `data/payload/ENERGY/Voltage` | 0–300 | `V` | AC | `pn-tasmota-voltage.c:50-56` |
| Tasmota Apparent Power | `PN_TYPE_TASMOTA_APPARENT_POWER` | `data/payload/ENERGY/ApparentPower` | 0–5000 | `VA` | None | `pn-tasmota-apparent-power.c:47-53` |
| Tasmota Reactive Power | `PN_TYPE_TASMOTA_REACTIVE_POWER` | `data/payload/ENERGY/ReactivePower` | 0–5000 | `var` | None | `pn-tasmota-reactive-power.c:48-54` |
| Tasmota Power Factor | `PN_TYPE_TASMOTA_POWER_FACTOR` | `data/payload/ENERGY/Factor` | 0–1 | `cos φ` | None | `pn-tasmota-power-factor.c:50-56` |

All six: same base, same `switch-name` filter, same SENSOR-topic gate, same Data/Scale/Colours dialog, same yellow gauge icon/colour, all sinks. AC-mains quantities (Current, Voltage) paint the IEC sine-wave `~` glyph; the derived quantities use None.

---

## Sensor filters (parse a reading and re-emit)

Unlike the gauges, **Tasmota Temperature** and **Tasmota Humidity** subclass `PnNode` directly, have `has_input=TRUE, has_output=TRUE`, and reshape the reading into the standard bag. They take **no settings** (the keys are hard-coded) and apply **no topic/device filter** — any message whose `data.payload` JSON object carries the key is accepted.

### Tasmota Temperature
**Purpose** — Extract a `Temperature` number from a Tasmota SENSOR publish and reshape it (`pn-tasmota-temperature.c:36`). Reads `data.payload` as an object and finds `Temperature` top-level or one level into each per-sensor child via `pn_tasmota_find_number` (`:67`).

**When to use** — After an MQTT Source, to feed a clean numeric temperature (+unit/device) into charts/thresholds/displays.

**Ports** — `has_input=TRUE`, `has_output=TRUE` (`pn-tasmota-temperature.c:109-110`).

**Settings** — None of its own. Icon FA `fa-thermometer-half` U+F2C9, body `(0.85,0.45,0.30)` orange-red.

**Emits / consumes** — On a hit writes: `value` (the reading), `unit` (top-level `TempUnit`, default `"C"` when the firmware omits it, `:74-77`), `device` (from the topic, when present), `success=TRUE`, and `output` like `"sonoff37: temperature 21.4 °C"`; then re-emits (`:80-94`).

**Gotchas** — No payload object or no `Temperature` key → **silent return, no emission, no `success=FALSE`** (`:64-68`) — downstream sees nothing. Raw-string (non-JSON) publishes are dropped. Unit lookup is top-level only.

### Tasmota Humidity
Same shape (`pn-tasmota-humidity.c:36`): parses `Humidity` from `data.payload`, writes `value`, `unit` (**always the literal `"%"`** — no firmware field, `:69`), `device`, `success=TRUE`, `output` like `"sonoff37: humidity 48.0%"`, then re-emits (`:60-82`). Icon FA `fa-tint` U+F043, body `(0.30,0.55,0.85)` blue. Same silent drop-on-miss as Temperature.

---

## Relay / switch control

### Tasmota Switch
**Purpose** — Bidirectional single-relay node: an on-canvas slide switch that both **reflects** a relay's reported state and **commands** it. Subclasses core **`PnSwitch`** (`pn-tasmota-switch.c:52`) so the slider widget/hit-testing come for free.

**When to use** — Show and flip one relay: `MQTT Source → Tasmota Switch → MQTT Sink`.

**Ports** — `has_input=TRUE`, `has_output=TRUE` (`pn-tasmota-switch.c:377-378`).

**Settings** —
- `switch-name` (string, default empty, **mandatory**) — device name; used both as the inbound filter and stamped into `cmnd/<switch-name>/POWER` on command. Empty = unconfigured red ❗ (`:380-394`).
- `enforce-on-startup` (boolean, default **FALSE**) — backed by the base `PnSwitch` announce flag (`pn_switch_set_announce_on_startup`); when TRUE, commands the relay to the saved latch position shortly after load (`:296-302, 396-408`).

**Emits / consumes** — *Inbound* (sync only, no emit): topics whose last segment starts with `POWER` (`stat/<device>/POWER`, `POWER1..8`) and device == `switch-name`; reads string `data.payload` "ON"/"OFF" (case-insensitive) and moves the latch (`:200-254`). *On user click*: emits topic `cmnd/<switch-name>/POWER`, `data.payload` "ON"/"OFF", `data.value` 1.0/0.0, `data.success=TRUE`, `data.device`, `data.output` `"<name>: command power on|off"` (`:105-141`).

**Gotchas** — Startup-announce **defaults OFF on purpose** — announcing would physically actuate the relay on every worksheet open; opt in via `enforce-on-startup` (`:433`). Inbound moves the latch only on literal ON/OFF (TOGGLE/Blink/PulseTime ignored). The output reflects only *user-driven* clicks, not external relay flips — shadow those with a parallel **Tasmota Relay Status**. Icon FA `fa-toggle-on` U+F205, steel-blue `(0.42,0.55,0.72)`.

### Tasmota Relay Command
**Purpose** — Write-side filter (the inverse of Relay Status): reads `data.value` and reshapes it into a relay-control publish for a downstream MQTT Sink. `PnNode` (`pn-tasmota-relay-command.c:46`).

**When to use** — Drive a relay from a numeric/boolean signal with no Format/Rewrite glue: `Switch/Knob → Tasmota Relay Command → MQTT Sink`.

**Ports** — `has_input=TRUE`, `has_output=TRUE` (`:258-259`).

**Settings** — `switch-name` (string, default empty, mandatory) — stamped into `cmnd/<switch-name>/POWER`; empty = red ❗, drops every message (`:261-271`).

**Emits / consumes** — Consumes numeric `data.value` (missing/non-numeric → ignored, no synthesized 0). Computes `on = value > 0.5` (midpoint), rewrites the cloned topic to `cmnd/<switch-name>/POWER`, sets `data.payload` "ON"/"OFF", `data.value` 1.0/0.0, `data.success=TRUE`, `data.device`, `data.output` `"<name>: command power on|off"`, emits (`:84-165`).

**Gotchas** — Boolean threshold is `value > 0.5` (`:137`). No startup-announce (passive filter). Mandatory `switch-name` has **no default by design** — to avoid flipping the wrong relay. Same icon/colour as Switch.

### Tasmota Relay Status
**Purpose** — Read-side filter: parse a relay's POWER-state publish into the canonical bag and forward. `PnNode` (`pn-tasmota-relay-status.c:50`).

**When to use** — Mirror a relay's reported state into an LED/Debug/Graph: `MQTT Source → Tasmota Relay Status → LED`.

**Ports** — `has_input=TRUE`, `has_output=TRUE` (`:294-295`).

**Settings** — `switch-name` (string, default empty, mandatory) — listens for this device (second-to-last segment); empty = red ❗, rejects all (`:297-307`).

**Emits / consumes** — Accepts topics whose last segment starts with `POWER` and whose device **exactly** equals `switch-name`; reads string `data.payload` "ON"/"OFF" (case-insensitive). Rewrites in place: `data.value` 1.0/0.0, `data.success=TRUE`, `data.device`, `data.output` `"<device>: power on|off"`, emits (`:94-198`).

**Gotchas** — Only literal "ON"/"OFF" pass (TOGGLE/PulseTime/Blink dropped, no synthesized transition). **Exact** device compare (not substring) so `sonoff1` rejects `sonoff19`. Needs ≥3 topic segments. Same icon/colour as Switch.

---

## Status & discovery

### Tasmota Status
**Purpose** — Pass-through filter that re-emits **only genuine `stat/<device>/STATUS#` replies** and drops everything else. `PnNode` (`pn-tasmota-status.c:34`).

**When to use** — Splice between an MQTT Source and a Concentrator/Debug to isolate Status replies from the rest of the broker traffic.

**Ports** — `has_input=TRUE`, `has_output=TRUE` (`:85-86`).

**Settings** — None.

**Emits / consumes** — Accepts a topic only when `topic_is_tasmota_status()` holds: starts with `stat/` **and** the last segment starts with `STATUS` (bare `STATUS` or `STATUS<n>`). Accepted messages are forwarded **verbatim** — no `data.*` member is read or written (`:45-71`).

**Gotchas** — The accept gate requires the `stat/` root, so a foreign app publishing some `…/STATUS` topic on a shared broker is no longer mistaken for a Tasmota reply (`:52-59`). Pure filter — no rate-limiting, merging, or group-topic logic.

### Tasmota Status Request
**Purpose** — Turn any inbound trigger into a `Status <n>` query a downstream MQTT Sink publishes (the read-side companion to Relay Command). `PnNode` (`pn-tasmota-status-request.c:52`).

**When to use** — `Inject/Button/Knob → Tasmota Status Request → MQTT Sink`; replies return on `stat/<device>/STATUS#` via an MQTT Source.

**Ports** — `has_input=TRUE`, `has_output=TRUE` (`:225-226`).

**Settings** —
- `device` (string, default **`"tasmotas"`** = the group topic / all devices) — stamped into `cmnd/<device>/Status`; empty is treated as the group topic. Never enters the red state (read-only query) (`:228-238`).
- `request` (enum `PnTasmotaStatusKind`, default `Everything (Status 0)`) — which section; the enum's integer is the wire argument (`:240-248`).

**Emits / consumes** — Consumes any message as a bare trigger (inbound `data.*` ignored). Reshapes in place: topic `cmnd/<target>/Status`, `data.payload` = the decimal request value (e.g. `"0"`) so the Sink puts exactly `<n>` on the wire, `data.success=TRUE`, `data.device`, `data.output` `"<target>: request Status <n>"`, emits (`:105-124`).

**Gotchas** — No rate-limiting. The `device` field round-trips `""` as-is (stable dialog notify) but `resolve_target()` substitutes `tasmotas` at send time. Needs an MQTT Sink downstream (whose default payload path publishes `data.payload`) or nothing is sent.

### Tasmota Probe
**Purpose** — Discovery half of the concentrator: watches the passive telemetry stream, learns each device name, and emits a per-device `Status <n>` poll so devices that suppress group-topic replies still get queried. `PnNode` (`pn-tasmota-probe.c:60`).

**When to use** — Auto-discover devices and trigger their status reports: `MQTT Source → Tasmota Probe → MQTT Sink` (replies gathered by a Concentrator).

**Ports** — `has_input=TRUE`, `has_output=TRUE` (`:278-279`).

**Settings** —
- `request` (enum `PnTasmotaStatusKind`, default `Everything (Status 0)`) — which section each discovered device is asked for (`:281-290`).
- `interval` (double seconds, default **300.0**, range 0–∞) — minimum gap between two probes of the **same** device; a newly-seen device is probed promptly; `0` probes on every naming message (`:292-299`).

**Emits / consumes** — Consumes only `tele/`/`stat/`/`cmnd/`-family topics (else ignored); extracts the device, skips the `tasmotas` group topic. Reads no bag member and never echoes telemetry. When a device is due, reshapes in place: topic `cmnd/<device>/Status` (capital S, no space; the numeric arg rides in the payload), `data.payload` = the stringified request enum, `data.success=TRUE`, `data.device`, `data.output` `"<device>: probe Status <n>"`, emits (`:80-189`).

**Gotchas** — Pure command emitter — never forwards telemetry. Per-device rate limit kept in a `name → last-probe-µs` hash; `interval ≤ 0` always probes. No `switch-name` and no error state (it discovers, doesn't target). Distinct violet body `(0.55,0.40,0.78)` and FA `fa-binoculars` U+F1E5.

### Tasmota Concentrator
**Purpose** — Collects per-device `stat/<device>/STATUS#` replies (plus optional `tele/<device>/STATE|SENSOR` telemetry), top-level-merges each device's sections into one record, and emits **one combined fleet snapshot** `data.devices = { <device>: {merged}, … }`, rate-limited. `PnNode` (`pn-tasmota-concentrator.c:94`).

**When to use** — On a branch off an MQTT Source, to roll a fleet's scattered Status/telemetry into a single snapshot for a Debug/table/HTTP view.

**Ports** — `has_input=TRUE`, `has_output=TRUE` (`:462-463`).

**Settings** —
- `interval` (double seconds, default **2.0**, range 0–∞) — minimum gap between emitted snapshots; debounces the STATUS# burst from one `Status 0`. `≤0` emits on every collected message (`:465-473`).
- `timeout` (double seconds, default **0.0** = keep forever) — drop a device whose last reply is older than this before the next snapshot (`:475-482`).
- `merge-telemetry` (boolean, default **TRUE**) — also fold `tele/<device>/STATE|SENSOR` into each record; off = only explicit Status sections (`:484-491`).

**Emits / consumes** — Accepts (`topic_is_accepted`): `stat/`+`STATUS…` always; `tele/`+`STATE`/`SENSOR` only when `merge-telemetry` on (`:148-168`). Reads `data.payload` as a JSON object and shallow-merges it into the device's record (`Status`, `StatusNET`, `StatusSTS` accumulate). Emits in place: topic replaced with neutral `tasmota/devices`; `data.devices` = each device → deep copy of its merged record; `data.value` = device count; `data.success=TRUE`; `data.output` `"N device(s): a, b, c"` sorted (`:251-303`).

**Gotchas** — Rate-limited by `interval`; the first accepted message always emits. Skips the `tasmotas` group topic so it's never folded in as a phantom device. Skips raw-string (non-JSON) payloads and topics with no extractable device. Snapshot records are deep-copied on emit so later merges can't mutate an already-emitted snapshot. (This node gates by its own `stat/`/`tele/` prefix check, so it predates and doesn't use `pn_tasmota_topic_is_family`.)
