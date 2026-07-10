# Filters — Reshape, Timing & Deadline

Filter nodes sit mid-flow with both an input and an output port: they receive a `PnMessage` and (usually) forward a transformed or time-shifted copy. Every wire carries the same envelope (`topic`/`id`/`created`) plus a schemaless `data.*` bag whose mandatory members are `data.value` (canonical numeric; booleans encoded as `0.0`/`1.0`), `data.output` (human-readable summary string), and `data.success` (boolean). The **reshape** filters here mutate one part of that message in place and re-emit it (Format/Text rewrite `data.output`; Value rewrites `data.value`; PROM rewrites `data.value` by looking the old one up as an address in a table; Set/Rewrite rewrite bag members; Topic rewrites the envelope topic; Table Model attaches `data.table`). The **timing** filters (Delay, Staircase, Throttle, Watchdog, Deadline) shape *when* and *whether* messages flow rather than their content. All forward the received topic unchanged except Topic (which overwrites it), Staircase's synthetic turn-off (which reuses the trigger's topic), and Rewrite (which may set it from the template).

## Format

**Purpose** — Rewrites `data.output` from one of two user-authored template strings, choosing which by the incoming `data.success` boolean. Templates embed `${path/to/field}` placeholders expanded against the message JSON. (`lib/pn-format.c:104`)

**When to use** — Turn a raw message into a human sentence with success/failure variants, e.g. `"${topic} OK — ${data/output}"` vs `"${topic} DOWN at ${data/host}"`, feeding a Debug/TTS/notification sink. Distinct from **Text**: both write `data.output`, but Text emits one fixed constant string (no placeholders, no success/failure split), whereas Format interpolates live message fields and branches on `data.success`.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `success-text` (string, default `""`) — template written to `data.output` when `data.success` is `true`. (`lib/pn-format.c:229`)
- `failure-text` (string, default `""`) — template written when `data.success` is `false` or the member is absent. (`lib/pn-format.c:236`)

**Placeholder syntax** — `${path/to/field}` resolves against the message lookup root, which carries `topic`, `id`, `created`, and `data` (the payload bag). So `${topic}` → envelope topic, `${data/value}` → the bag's `value` member, `${data/host}` → `data.host`. Expanded in `PN_SUBST_TEXT` mode (plain text). Document globals are a fallback resolver after message fields (`${...}` on a flow's globals). Unknown path → empty string (`PN_SUBST_MISS_EMPTY`); an unterminated `${` is emitted verbatim. (`lib/pn-format.c:77`)

**Behaviour** — Reads `data.success` (defaults to `FALSE` if absent/non-boolean), picks the matching template, expands placeholders, and overwrites `data.output`. Then re-emits the same message. (`lib/pn-format.c:116`)

**Writes** — `data.output` only. Topic, id, value, success and all other members untouched.

**Gotchas** — A missing/non-boolean `data.success` is treated as failure, so the failure template runs. An empty template writes `""` to `data.output` (it does not skip the write).

## PROM

**Purpose** — A programmable read-only memory: a lookup table from address to stored word. The incoming `data.value` is the address; it is rounded to the nearest whole cell, and the word programmed at that address replaces `data.value`. (`lib/pn-prom.c:199`)

**When to use** — Map a small set of discrete numeric inputs onto arbitrary output values without writing an expression: a mode number → a setpoint, a sensor code → a scale factor, a step index → a waveform sample. Distinct from **Value** (one constant, ignores the input) and from **Calculator** (an algebraic formula over the input); PROM is a literal table with no arithmetic between entries — an address either was burnt or reads back as zero.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `contents` (string, default `"0x0000 0xff\n0x0001 0x1a\n"`) — the whole memory image, one `<address> <word>` pair per line. Rendered as a full-width `PN_EDITOR_MULTILINE` editor on its own "Contents" tab via the declarative settings schema (no `-gui.so` companion, no caption label). (`lib/pn-prom.c:306`, schema at `lib/pn-prom.c:326`)

**Image syntax** — One pair per line, columns separated by spaces/tabs. Either column may be hexadecimal (`0x…`/`0X…`, optionally signed) or decimal; the word may be fractional or negative (`2 1.5`, `0x0b -2`). A bare leading zero is *not* octal — `010` is ten. Blank lines and `#`-comments (whole-line or trailing) are skipped. A later line for the same address reprograms that cell (last wins). (`parse_number`, `lib/pn-prom.c:67`; `prom_recompile`, `lib/pn-prom.c:120`)

**Behaviour** — Reads `data.value`; a message whose `value` is missing or non-numeric (string/bool/object) drives no address line and is **dropped** without emitting. Otherwise the address is rounded with `llround` — nearest whole cell, halves away from zero (`0.49`→0, `0.5`→1, `1.5`→2) — and looked up in the compiled table. A hit yields the stored word; a **miss yields `0.0`**, the way an unburnt PROM cell reads `0x00`. A miss still emits. (`address_of`, `lib/pn-prom.c:97`; `read_address`, `lib/pn-prom.c:182`)

**Writes** — `data.value` (the word read out) and `data.address` (the decoded integer address, as a double, so a downstream Debug/Format can show which cell was read). Topic, id, output, success and all other members pass through untouched.

**Gotchas** — A malformed line (not exactly two parseable columns) puts the node into the generic error state — `pn_node_set_has_error`, painted red with ❗ on the worksheet — while the lines that *did* parse keep working; fixing the image clears it. Because a miss and a genuinely-stored `0` are indistinguishable on the wire, don't use `data.value == 0` to test "was this address programmed". The image is parsed at property-set time, not per message, so large tables cost nothing per message (`GHashTable` lookup).

## Rewrite

**Purpose** — Replaces the outgoing message with one assembled from a full **JSON template** authored in the settings dialog. Placeholders expand against the incoming message, the result is parsed as JSON, and its envelope/data members are applied onto the message. (`lib/pn-rewrite.c:200`)

**When to use** — When you need to restructure the whole data bag or rewrite the topic *and* multiple fields at once (a richer transform than Set's per-member assignments or Format's single-string output). Distinct from **Set**: Set merges a list of `{path, literal}` assignments onto the existing bag (keeps everything else); Rewrite *replaces* the bag (and optionally the topic) wholesale from a template, and can splice live values into both keys and string contents.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `template` (string) — JSON template the outgoing message is built from. Default (`lib/pn-rewrite.c:43`):
  ```json
  {
    "topic": "${topic}",
    "data": {
      "value":  ${data/value},
      "output": "${data/output}",
      "source": "${topic}",
      "id":     "${id}"
    }
  }
  ```
  Settings schema renders it as a full-width `PN_EDITOR_CODE` GtkSourceView (JSON highlighting, line numbers, bracket matching) on a "Rewrite" tab — declarative schema, no `-gui.so` companion. (`lib/pn-rewrite.c:342`)

**Placeholder syntax** — `${path/to/field}` against root `topic`/`id`/`created`/`data`. Expanded in **`PN_SUBST_JSON`** mode (context-aware): in a value slot the placeholder emits a complete JSON token (strings quoted, numbers/booleans/null bare, objects/arrays compact JSON); inside a string literal it emits the escaped contents without surrounding quotes. Document globals are a fallback resolver. Miss policy is `PN_SUBST_MISS_VERBATIM`: an unknown path is left as the literal `${path}` — harmless inside a string, but in a value slot it yields invalid JSON. (`lib/pn-rewrite.c:88`)

**Behaviour** — If `template` is empty, forwards the message unchanged. Otherwise expands placeholders, parses the result; **on a JSON parse failure it logs a `g_warning` and forwards the message unchanged** (does not null fields). On success, `apply_rendered` (`lib/pn-rewrite.c:122`): if the rendered object has a top-level `topic` or `data` member it is treated as an envelope — a string `topic` overrides the outgoing topic, and a `data` object *replaces the entire data bag* (every existing member dropped, then a deep copy of each rendered member grafted on). If neither `topic` nor `data` is present, the whole object becomes the new data bag. Message `id` and `created` are always preserved.

**Writes** — Potentially `topic` and the entire `data` bag (replaced). Never touches `id`/`created`.

**Gotchas** — The data bag is *replaced*, not merged: any incoming `data.*` member you do not re-emit in the template is lost. A placeholder in a value slot that resolves to a missing path breaks JSON parsing → message forwarded unchanged (silent except the warning, which is invisible from a desktop launch with no terminal).

## Set

**Purpose** — The complement of Filter: forwards every message unchanged except it applies a configured list of `{path, literal}` assignments onto the `data` bag first. (`lib/pn-set.c:217`)

**When to use** — Tag/stamp constant members on passing messages: force a known `success`/`value` through a downstream Format template during a test, stamp a constant `endpoint` on a synthetic source, or add a marker the next stage branches on. Distinct from **Value** (which only writes the single numeric `data.value` via a spin button) and from **Rewrite** (Set *merges*, keeping existing members; Rewrite *replaces* the bag).

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `props` (string, default `"[]"`) — a JSON array of `{ "path": "...", "literal": <json value> }` records, applied in order to every message. Stored as the canonical string; a compiled cache (`CompiledSet[]`) is rebuilt whenever the string changes. Invalid JSON logs a warning and yields no assignments. (`lib/pn-set.c:344`, compile at `lib/pn-set.c:86`)
- GUI editor (`pn-set-gui.c`, installed via `pn_set_gui_install`): one row per assignment — a path combo (`value`/`success`/`output`/`endpoint`/`Custom…`), a literal-type combo (`number`/`string`/`boolean`), and a value entry or true/false combo, plus an **Add property** button. The type for a known path is auto-suggested (`success`→boolean, `value`→number, `output`/`endpoint`→string). Round-trips through `props` via `g_object_get`/`set`. (`lib/pn-set-gui.c:599`)

**Behaviour** — Empty list (or no compiled entries) is a pass-through no-op. Otherwise each assignment writes `literal` (deep-copied) at its `path`: a bare name like `value` sets `data.value`; a **dotted path** like `foo.bar` descends/creates intermediate `JsonObject`s on demand (missing or non-object segments are replaced with a fresh object). Existing members are overwritten; missing ones added. The literal's JSON value type (number/string/boolean) drives the encoding. (`assign_path`, `lib/pn-set.c:161`)

**Writes** — The data-bag members named by each assignment's `path` (possibly nested). Everything else, including topic/id, passes through.

**Gotchas** — A dotted intermediate segment that currently holds a non-object value is silently *replaced* with a fresh object (jq-style `.foo.bar = …` semantics). The boolean encoding follows the bag convention only if you author it as a JSON boolean; numeric `data.value` booleans stay `0.0`/`1.0`.

## Text

**Purpose** — Overwrites `data.output` with a fixed, pre-configured constant string on every message, regardless of input. (`lib/pn-text.c:76`)

**When to use** — Inject a constant payload body — a label, a canned notification, a literal string — into a flow. Distinct from **Format**: Text is a fixed constant with no placeholders and no success/failure branching; Format interpolates live fields. Use Text when the output never depends on the message.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `text` (string, default `""`) — the fixed string written to `data.output`. Rendered as a multi-line, full-width editor in the dialog (`pn_param_spec_set_multiline` / `set_full_width`, no caption label since the node is itself named "Text"). (`lib/pn-text.c:172`)

**Behaviour** — On every message, strips leading and trailing newline characters (`\n`/`\r`) from `text` (interior newlines and other whitespace preserved), writes the result to `data.output`, and forwards. The trim happens at emit time, not at store time, so the editor never fights the user mid-keystroke. (`strip_edge_newlines`, `lib/pn-text.c:53`)

**Writes** — `data.output` only. Topic, id, value, and all other members untouched.

**Gotchas** — Only edge *newlines* are trimmed — leading/trailing spaces and tabs survive. The message is always forwarded (no gating).

## Topic

**Purpose** — Rewrites the message's envelope topic to this node's own resolved topic, then forwards otherwise untouched. (`lib/pn-topic.c:34`)

**When to use** — Re-tag a stream mid-flow so it carries a stable topic before reaching an MQTT sink or a topic-routing subscriber. The only reshape filter here that touches the envelope topic rather than the data bag.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings** — *No node-specific property.* The topic it stamps is the ordinary base `PnNode` "topic" field every node carries, edited in the settings dialog. Supports the standard emitter placeholders `${nodeclass}`, `${nodename}`, `${hostname}`, resolved at emit time via `pn_node_resolve_topic`. Left blank, falls back to the default `/pnode/${nodeclass}/${nodename}` template.

**Behaviour** — Calls `pn_node_resolve_topic(node)`, sets it as the message topic, re-emits. (`lib/pn-topic.c:44`)

**Writes** — Envelope `topic`. Data bag untouched.

**Gotchas** — Uses the same placeholder set as every node's outgoing-topic stamping — *not* the `${path/to/field}` data-lookup syntax used by Format/Rewrite.

## Value

**Purpose** — Overwrites `data.value` with a fixed number on every message, regardless of input. (`lib/pn-value.c:45`)

**When to use** — Inject a constant numeric payload — a setpoint, a threshold, a literal reading. Distinct from **Set**: Value is the single-purpose, spin-button version that only writes `data.value`; Set can write any member (including `value`) and several at once via its list editor.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `value` (double, range `-G_MAXDOUBLE`..`G_MAXDOUBLE`, default `0.0`) — the fixed number written to `data.value`, set via a spin button. (`lib/pn-value.c:127`)

**Behaviour** — Writes `value` to `data.value` and forwards. Always emits. (`lib/pn-value.c:52`)

**Writes** — `data.value` only. Topic, id, output, and other members untouched.

**Gotchas** — Since booleans are encoded as `0.0`/`1.0` on `data.value`, this node can also force a boolean (set value to `1.0`/`0.0`).

## Table Model

**Purpose** — Parses an ASCII / fixed-width text table out of `data.output` and attaches the structured result as `data.table` before forwarding. (`lib/pn-table-model.c:275`)

**When to use** — Downstream of a Shell/SSH command emitting column-aligned Unix tool output (`df`, `ps`, `ls -l`, `free`, `uptime`, …) so a consumer can index a row's cell by column position instead of re-parsing text.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `parse-header` (boolean, default `TRUE`) — when true, the first non-empty line becomes `data.table.header.cells` and the rest become `data.table.rows`; when false, the first line is still used to detect column positions but is emitted as the first row with no `header` member. (`lib/pn-table-model.c:369`)

**Behaviour** — Reads `data.output` (string; absent/non-string → empty table). Right-strips lines and drops blanks. Detects column starts in two passes: (1) word-start offsets on the reference line; (2) each non-zero boundary shifted left until the position just before it is whitespace in *every* row (so a wide right-aligned numeric column doesn't bleed into the previous one). Slices each row at the boundaries and whitespace-trims each cell. Cells are emitted as `{ "text": "..." }` objects (schema-extensible); short lines yield empty trailing cells so every row reports the header's width. Emitted shape (`lib/pn-table-model.c:202`):
```json
{
  "header": { "cells": [ { "text": "..." }, ... ] },   // only when parse-header
  "rows":   [ { "cells": [ { "text": "..." }, ... ] }, ... ]
}
```

**Writes** — `data.table` (a new member). `data.output`, topic, id, and every other field survive intact, so a Format/Debug after a Table Model still sees the original payload.

**Gotchas** — Column detection is heuristic and tuned for whitespace-aligned columns; values containing internal spaces (paths, command names with args) can split across cells. An empty/absent `data.output` still produces a `data.table` with an empty `rows` array.

## Delay

**Purpose** — Time shifter: forwards every message unchanged, but only after `delay-ms` milliseconds. (`lib/pn-delay.c:98`)

**When to use** — Defer messages without dropping any; a burst arrives downstream as the same burst shifted later, order preserved, nothing coalesced. The *opposite* of **Throttle** (which thins a stream) — Delay keeps every message, Throttle discards the ones inside the window.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `delay-ms` (uint, range `0`..`3600000` ms = 1 hour, default `1000`) — milliseconds to hold each message before forwarding. (`lib/pn-delay.c:215`)

**Behaviour** — Each received message is ref'd and given its **own** one-shot `g_timeout_add_full` timer; when it fires the message is emitted and the source torn down. A set of live timer ids is tracked. A delay of `0` still defers to the next main-loop iteration (not inline). (`lib/pn-delay.c:104`)

**Writes** — Nothing; the message content is forwarded verbatim.

**Gotchas** — Independent per-message timers mean concurrency is unbounded (one source per in-flight message). Changing `delay-ms` only affects newly-arriving messages; already-armed timers keep their original schedule (`lib/pn-delay.c:151`). Messages still waiting when the node is disposed are **discarded, not forwarded** (`pn_delay_dispose` cancels every timer, `lib/pn-delay.c:173`).

## Staircase

**Purpose** — Monostable ("stairwell light") timer: a trigger is forwarded, the node goes "on" for `on-time-ms`, then emits a synthetic turn-off (`data.value = 0.0`) and returns to rest. (`lib/pn-staircase.c:132`)

**When to use** — Auto-off behaviour: press once (trigger), stay on a while, turn off automatically — e.g. drive a light/relay that should switch off after a timeout. Unlike **Delay** (delays each message) or **Throttle** (rate-limits), Staircase generates its *own* off edge after a single trigger and swallows in-window triggers.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `on-time-ms` (uint, `0`..`3600000` ms = 1 hour, default `5000`) — how long the node stays on after a trigger before emitting the `value = 0.0` turn-off. (`lib/pn-staircase.c:270`)
- `retriggerable` (boolean, default `TRUE`) — when on, a fresh trigger during the window restarts the timer (pushing the turn-off later) instead of being ignored. (`lib/pn-staircase.c:279`)

**Behaviour** — A **trigger** is any message with numeric `data.value > 0.5`; anything else (off edges, valueless messages, non-numeric value) is **dropped**. From rest: a trigger is forwarded unchanged, its topic remembered, and the one-shot turn-off timer armed (`lib/pn-staircase.c:143`). While on: a trigger re-arms the timer if `retriggerable`, else is swallowed — either way it is **never re-forwarded** (downstream is already on). When the timer fires, a fresh `PnMessage` reusing the trigger's topic is emitted with `data.value = 0.0`; state returns to rest *before* emit (re-entrancy safe). (`pn_staircase_fire`, `lib/pn-staircase.c:97`)

**Writes** — Trigger pass-through: nothing changed. Turn-off message: a new message with `data.value = 0.0` and the trigger's topic.

**Gotchas** — Trigger threshold is `> 0.5` (the bag's boolean midpoint), not `> 0.0`. Changing `on-time-ms` mid-window does not move the current deadline; it governs the next trigger. A window still counting down when the node is disposed is **cancelled without emitting the turn-off** (`lib/pn-staircase.c:235`).

## Throttle

**Purpose** — Rate limiter: forwards a message only when at least `interval` seconds have passed since the previous forwarded one; otherwise drops it. (`lib/pn-throttle.c:56`)

**When to use** — Thin a noisy stream to at most one message per N seconds (leading-edge). Opposite of **Delay** (which keeps all messages, shifted) — Throttle *discards* the messages inside the window; there is no queue. Differs from **Staircase** (which is about on/off timing, not rate-limiting).

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `interval` (uint, `1`..`3600` seconds, default `1`) — minimum seconds between forwarded messages. (`lib/pn-throttle.c:146`)

**Behaviour** — Leading-edge: the **first** message is always forwarded (a quiet stream is never silenced). Thereafter a message is forwarded only if `now - last_forward >= interval` seconds (monotonic clock); messages arriving sooner are silently discarded. The forward timestamp is set on each pass. (`lib/pn-throttle.c:56`)

**Writes** — Nothing; forwarded messages pass through verbatim.

**Gotchas** — This is **leading-edge, no trailing emit** — the last message inside a window is dropped, not deferred and emitted at window end. `interval` is in whole seconds (minimum 1), unlike the millisecond-resolution Delay/Staircase. No queue, so dropped messages are gone.

## Watchdog

**Purpose** — Auto-triggered silence detector: consumes incoming messages (never forwards them) and, on each tick, emits an alarm message if **no** input arrived during the previous interval. Subclasses `PnAutoTrigger`. (`lib/pn-watchdog.c:55`)

**When to use** — Detect when a flaky upstream stream *goes quiet*. Wire it after the source and into a Debug/TTS sink to be told about silence. Contrast with **Deadline**, which fires based on a fixed wall-clock *target time* relative to a `data.value` epoch carried on each message; Watchdog fires on *absence of traffic* over a rolling tick period.

**Ports** — `has_input` = TRUE, `has_output` = TRUE (input is consumed, output carries only alarms).

**Settings**
- `period` (uint, inherited from `PnAutoTrigger`, `1`..`3600` seconds, default `1`) — tick interval = the maximum tolerated silence. (`lib/pn-auto-trigger.c:441`)
- `autostart` (boolean, inherited) — whether the periodic worker thread spawns at construction.
- `output-text` (string, default `""`) — placed in `data.output` of the alarm message. (`lib/pn-watchdog.c:180`)

**Behaviour** — `receive()` (main thread) just atomically sets a `saw_message` flag; the message is **not forwarded** (`lib/pn-watchdog.c:55`). `trigger()` (worker thread) atomically read-and-clears the flag: if a message *was* seen, stays silent; if not, builds an alarm message with `data.success = FALSE` and `data.output = output-text` and hands it to the main thread via `pn_auto_trigger_emit_on_main`. The flag is **pre-armed to 1 at construction** so the first tick (~1s after load) silently consumes it instead of raising a false alarm before upstream has produced. (`lib/pn-watchdog.c:78`, init at `:191`)

**Writes** — Alarm message only: `data.success = FALSE`, `data.output = output-text`. No topic-specific stamping (alarm topic is the node's default).

**Gotchas** — It is a *silence* detector — normal traffic is silently dropped, only the absence produces output. The pre-arm prevents a spurious alarm one second after every worksheet load. `period` is the `PnAutoTrigger` tick; the first tick fires ~1s after construction regardless of a longer configured period (`PN_AUTO_TRIGGER_FIRST_DELAY`), but the prearm absorbs it.

## Deadline

**Purpose** — Reports how far a fixed target instant is from "now": each message must carry the current epoch in `data.value` (as the Clock node emits); Deadline rewrites `data.value` to `target − now` seconds remaining and forwards. (`lib/pn-deadline.c:150`)

**When to use** — Count down to a scheduled wall-clock moment; a downstream node detects "passed" with `value ≤ 0`. Contrast with **Watchdog** (fires on traffic *silence* over a rolling period) — Deadline measures against an absolute calendar target supplied as a config string, and acts only when a message (with an epoch) arrives.

**Ports** — `has_input` = TRUE, `has_output` = TRUE.

**Settings**
- `target` (string, default = current local date/time, format `yyyy-mm-dd hh:MM:ss`) — the target wall-clock instant. Rendered as a `PN_EDITOR_ENTRY`. Unparseable → node emits nothing. (`lib/pn-deadline.c:251`, init `:281`)
- `timezone` (string, default `PN_TZ_NOT_SET` = "Not Set") — the fixed-offset zone the target is read in, chosen from a `PN_EDITOR_COMBO` built from the shared `pn-tz-table` (same list as Digital Clock). Abbreviation + spelled-out name (e.g. `CET — Central European Time`), never a city; DST variants are separate offsets (`CET` vs `CEST`). (`lib/pn-deadline.c:257`)

**Behaviour** — Reads `data.value` as the "now" epoch (double or int64; absent/non-numeric → stays silent). Resolves the offset: a fixed configured zone wins; otherwise reads the message's `timezone` member (a bare abbreviation, as the Clock node emits) through the table, falling back to GMT (UTC+0) when unknown/missing/ambiguous. Parses `target` as a wall clock at that offset into a Unix epoch (`parse_target_epoch` builds it as UTC then subtracts the offset, staying on the GLib 2.40 baseline). Writes `data.value = target_epoch − now_epoch` (seconds remaining; counts down to 0 at the target, then negative) and forwards. (`lib/pn-deadline.c:150`, `resolve_offset_minutes` `:75`)

**Writes** — `data.value` (seconds remaining). All other members pass through untouched.

**Gotchas** — Schema declarative only, no `-gui.so` companion. No DST guessing — pick the correct `CET`/`CEST` variant yourself, or leave `Not Set` and let the incoming `timezone` member drive it. A message without a numeric `data.value`, or an unparseable `target`, makes the node **emit nothing** (silent drop). Despite the `fa-hourglass-half` icon shared with Delay, this is not a timer — it computes a difference each time a message arrives.
