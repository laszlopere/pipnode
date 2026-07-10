# Filters — Gate, Expressions, Compute & AI

Filters are `PnNode` subclasses that sit *inline* on a wire: they all set both `has_input` and `has_output`, so a message arrives, the node transforms or gates it, and (usually) one message goes out the other side. Three sub-categories appear under the palette's **Filters** group: **Filters/Gate** nodes decide pass/block (and some rewrite `data.value` to a clean 0.0/1.0 boolean), **Filters/Expressions** nodes recompute the bag (Calculator, Calculator 2, JMESPath, Parse JSON), and **Filters/Compute & AI** nodes derive new values (FX Converter, Throughput). Every node forwards the *received* topic unchanged unless noted (Throughput is the exception — it mints a fresh `stats` message). Each output wire receives a deep copy. The message contract: envelope `topic`/`id`/`created` plus a schemaless `data.*` bag whose mandatory members are `data.value` (canonical number; boolean = 0.0/1.0, test `> 0.5`), `data.output` (human summary) and `data.success` (boolean).

---

## Comparator

**Purpose** — Two-input ordering comparator with hysteresis: latches the last numeric `data.value` on each of two inputs (A, B) and tracks one on/off state answering "is A ≥ B?". Emits only on state change. (`lib/pn-comparator.c:92` receive.)

**When to use** — Compare two live streams against each other (e.g. tank-A level vs tank-B level, temperature vs setpoint that itself arrives on a wire). The live-reference sibling of **Threshold**, which compares against a *fixed* number instead of a second stream. Use **Filter** instead when you need a non-numeric or multi-field predicate.

**Ports** — `has_input` + `has_output`; **two inputs** (`pn_node_set_n_inputs(node, 2)`). Input 0 = A, input 1 = B (`pn_node_current_input() <= 0` → A, else B). Single output.

**Settings**
- `hysteresis` (double, 0.0–1e9, default **0.0**): dead-band width centered on equality A==B. State turns on when A reaches `B + hysteresis/2`, off when A drops below `B − hysteresis/2`. 0 = plain "A ≥ B" on every crossing.

**Behaviour** — Reads `data.value` (must be int64 or double) on the arriving input; non-numeric → ignored, state and latches untouched. No comparison until *both* inputs have carried a number; the message completing the pair sets the initial state (A ≥ B → on) and is always forwarded. Thereafter forwards only when the boolean state flips.

**Writes** — On emit, overwrites `data.value` to 1.0 (A ≥ B) / 0.0 (A < B). Topic, id, other members untouched. Does **not** set `data.success`/`data.output`.

**Gotchas** — Hysteresis band only applies once a prior state exists; the very first comparison splits cleanly at equality. Yellow body, fa-balance-scale icon.

---

## Dedup

**Purpose** — Drops repeated values seen at a configurable JSON path, with a time-based expiry so a value becomes "fresh" again after a window. (`lib/pn-dedup.c:128` receive.)

**When to use** — Collapse a polling/duplicate stream keyed on a payload field (e.g. only forward each new `data/blockhash` once, suppress repeated alerts within 15 min). Contrast **Edge**, which keys specifically on the `data.success` *boolean transition* rather than an arbitrary value path with a timeout.

**Ports** — `has_input` + `has_output`, single each.

**Settings**
- `key` (string, default **`"data/value"`**): `/`-separated JSON path resolved by `pn_json_resolve_path` against the whole-message root (topic/id/created/data reachable, same root as Format placeholders). Empty key disables the node (every message dropped). Changing the key clears the seen-set.
- `timeout` (uint, 1–1440 min, default **15**): minutes to remember a value before it counts as fresh again. Pruned lazily on each receive (no background timer).

**Behaviour** — Resolves `key` → stringifies the JsonNode (scalars natural form, objects/arrays compact JSON). NULL / missing / JSON-null → message dropped, not remembered. If the string is already in the seen-table (and not expired) → drop. Otherwise insert with a monotonic timestamp and forward unchanged.

**Writes** — Nothing; the forwarded message is byte-for-byte the input.

**Gotchas** — Identity is the stringified value, so two structurally distinct objects collide only when serialised identically. Expiry is measured from *insertion*, not last-seen (no sliding refresh).

---

## Edge

**Purpose** — Edge detector on `data.success`: forwards a message only when its `success` boolean differs from the previous message's. (`lib/pn-edge.c:65` receive.)

**When to use** — Turn a polling stream (many same-value messages) into an event stream (one message per OK↔fail transition) — e.g. notify only when a Ping flips up or down. Compare **Dedup** (arbitrary value path + timeout) and **Success**/**Failure** (forward *every* matching message, not just transitions).

**Ports** — `has_input` + `has_output`, single each. No properties.

**Behaviour** — Reads `data.success` (must be a boolean). Missing/non-boolean → dropped, state untouched. First well-formed message latches the value and is **never forwarded** (no prior state). Thereafter forwards (and re-latches) only when the value changed.

**Writes** — Nothing; message forwarded unchanged.

**Gotchas** — Keys strictly on `data.success`; ignores `data.value`. The first transition you actually see is the *second* well-formed message at the earliest. Yellow body, fa-bolt icon.

---

## Filter

**Purpose** — General-purpose multi-rule gate. Forwards a message iff **every** configured rule (path, operator, literal) matches; rules are ANDed. (`lib/pn-filter.c:415` receive; GUI rule-row editor in `lib/pn-filter-gui.c`.)

**When to use** — Any pass/block decision more involved than the single-field gates: compare an arbitrary data-bag member or the topic against a number/string/boolean, AND several conditions, glob-match topics. Use the dedicated **Success**/**Failure**/**Threshold**/**Edge** when the decision is exactly their one field — they need no config.

**Ports** — `has_input` + `has_output`, single each.

**Settings**
- `rules` (string, default **`"[]"`**): canonical JSON array of `{path, op, literal}` objects, ANDed. Persisted as one string property so it round-trips. The GUI (`pn_filter_build_class_tab`) renders one widget-row per rule + an "Add rule" button; an empty list passes everything.
  - **path**: a bare data-bag member (`value`, `success`, `output`, `endpoint`, preloaded) or **Custom…** for any key, including dotted nested paths (`foo.bar`, descends objects). A path starting with `/` addresses the **envelope** — only `/topic` is supported.
  - **op** by literal type: number → `< <= == != >= >`; boolean → `== !=`; string → `== != contains`.
  - **literal** type combo number/string/boolean drives valid ops and comparison kind.

**Behaviour** — For each rule, resolve the path (envelope synthesises a transient node for `/topic`; data-bag is borrowed). The literal's value type must match the member's value type or the rule fails (→ drop). String `==`/`!=` literals containing `*` or `?` are pre-compiled to a `GPatternSpec` glob (`stat/sonoff19/*` matches that prefix; `?` = one char); wildcard-free strings stay byte-exact. Any rule failing or path missing → drop. All pass → forward unchanged.

**Writes** — Nothing; message forwarded unchanged.

**Gotchas** — Strict type matching: a number literal won't match a string member even if textually equal. `contains` is substring (string only). The GUI suggests a literal type per known path (`success`→boolean, `value`→number). Receive logic is GTK-free; the row editor is installed only in the GUI tier via `pn_filter_gui_install` (`lib/pn-filter-gui.c:772`).

---

## Success

**Purpose** — Pass-through gate: forwards a message only when `data.success` is boolean **true**. (`lib/pn-success.c:36` receive.)

**When to use** — Branch a flow on the success bit alone (route OK HTTP responses one way, mute alerts while green) without configuring a Filter rule. Mirror of **Failure**; feed both from the same upstream to split good/bad. **Edge** forwards only *transitions* of `data.success` instead of every true one.

**Ports** — `has_input` + `has_output`, single each. No properties.

**Behaviour** — Reads `data.success`; missing or non-boolean → dropped. `true` → forward unchanged; `false` → drop.

**Writes** — Nothing; forwarded unchanged.

**Gotchas** — Requires `success` to be a genuine JSON boolean (not 1.0). Yellow body, fa-check icon.

---

## Failure

**Purpose** — Mirror of Success: forwards a message only when `data.success` is boolean **false**. (`lib/pn-failure.c:36` receive.)

**When to use** — The "bad" branch — alert on a failed Ping, route an HTTP error to a notification sink, count failures separately via Throughput. Pair with **Success** off the same upstream to split the stream with zero config.

**Ports** — `has_input` + `has_output`, single each. No properties.

**Behaviour** — Reads `data.success`; missing or non-boolean → dropped. `false` → forward unchanged; `true` → drop.

**Writes** — Nothing; forwarded unchanged.

**Gotchas** — Same boolean-only requirement as Success. Yellow body, fa-times icon.

---

## Threshold

**Purpose** — Schmitt-trigger comparator against a fixed number: tracks one on/off state from `data.value` vs a constant threshold (with hysteresis), forwards only on state change. (`lib/pn-threshold.c:89` receive.)

**When to use** — Trip an output when a single reading crosses a fixed level (temp ≥ 30 → on) with chatter suppression. The fixed-reference sibling of **Comparator** (which compares two live streams). Use **Filter** for non-rewriting / multi-condition gating.

**Ports** — `has_input` + `has_output`, single each.

**Settings**
- `threshold` (double, −1e9–1e9, default **0.0**): value at/above which output is on.
- `hysteresis` (double, 0.0–1e9, default **0.0**): dead-band centered on the threshold. On when `value ≥ threshold + hysteresis/2`, off when `value < threshold − hysteresis/2`. 0 = plain "value ≥ threshold" on every crossing.

**Behaviour** — Reads `data.value` (int64/double); non-numeric → ignored, state held. First numeric message sets initial state (≥ threshold → on) and always emits. Thereafter forwards only when the state flips.

**Writes** — On emit, overwrites `data.value` to 1.0 (on) / 0.0 (off). Topic/id/other members untouched. Does **not** set `data.success`/`data.output`.

**Gotchas** — Hysteresis band applies only after a first state exists. Yellow body, fa-level-up icon.

---

## Calculator

**Purpose** — Evaluates a small algebraic expression on every message and writes the result to `data.value`. Binds every numeric data-bag member as a variable by its member name. (`lib/pn-expression.c:95` receive; parser `lib/pn-expr-parser.c`, variable binding `lib/pn-var-store.c`.)

**When to use** — Transform or combine numeric readings in one node — unit conversions `(value * 1.8) + 32`, derive a field from siblings, threshold-as-arithmetic `(value > 100) * 5`. Single-input; for combining values arriving on **separate** wires use **Calculator 2**.

**Ports** — `has_input` + `has_output`, single each.

**Settings**
- `expression` (string, multiline, default **`"value"`** — passes `data.value` straight through). Recompiled to an AST on set; parse failure stashes a reason. Settings UI = single full-width multiline editor on an "Expression" tab (declarative `PnSettingsSchema`, no `-gui.c` companion).

**Behaviour** — Binds every numeric (int64/double) member of `data` as a variable under its name (`data.value`→`value`, `data.temp`→`temp`). Language: numbers, `+ - * /`, parens, unary minus; comparisons `< > <= >= == !=` (looser binding, yield 1.0/0.0); one-arg functions `sin cos tan log log10 exp sqrt abs floor ceil`. Multiple newline-separated statements evaluate in order; `name = expr` binds `name` for later lines; `data.value` = the last statement's value.

**Writes** — Success: `data.value` = result, `data.success` = true, `data.output` = formatted result (`%g`); every assigned name added as a numeric member. Failure (parse or eval error, e.g. unknown variable/function): `data.success` = false, `data.error` + `data.output` = reason, `data.value` **left unchanged**. Message forwarded either way (never dropped).

**Gotchas** — Reserved `value`/`success`/`output` are always set from the final result, so assigning them has no lasting effect. Empty expression → forwarded as failed ("no expression set"). Purple body, fa-calculator icon.

---

## Calculator 2

**Purpose** — Multi-input variant of Calculator: evaluates an expression combining the latched `data.value` of several input ports (`value1 + value2`, …). (`lib/pn-expression2.c:115` receive; same parser/var-store as Calculator.)

**When to use** — Fold values arriving on **distinct wires** into one result — sum/difference/percentage of independent streams. Defaults to 2 inputs, scalable to 8. Use plain **Calculator** when all inputs ride one wire as sibling fields.

**Ports** — `has_input` + `has_output`. **User-settable 2–8 inputs** with **named inputs**: `pn_node_set_input_count_property(node, "inputs")` exposes a spin + editable per-input names on an "Inputs" tab; `pn_node_set_collate_inputs(node, TRUE)` makes the core latch each input's last `data.value` and inject it under the input's name. Single output.

**Settings**
- `inputs` (int, 2–8, default **2**): input-port count; setting it resizes live ports and repaints. Each input's remembered value binds as `value1 … valueN` (or the renamed input names).
- `expression` (string, multiline, default **`"value1 + value2"`**). Same AST compile/error handling and "Expression2" single-editor schema tab as Calculator.

**Behaviour** — Builds the variable set from the *collated* bag: (1) each input's latched headline value bound under its input name (`value1`, `value2`, … — present even for inputs that didn't just fire, which is what makes `value1 + value2` resolve); (2) other numeric siblings of *this* message only, suffixed with the 1-based arriving input number (`data.temp` on input 1 → `temp1`; not latched across inputs). Same operators/functions/multi-statement rules as Calculator.

**Writes** — Identical to Calculator: success sets `data.value`/`data.success`/`data.output` + assigned names; failure sets `success`=false + `data.error`/`data.output`, leaves `data.value`. Forwarded either way. Note: a referenced input that hasn't fired yet (e.g. `value2` still unknown) → eval failure, flagged not dropped.

**Gotchas** — `is_input_name()` distinguishes core-injected per-input values (bound by bare name) from this message's siblings (suffixed). The bare reserved `value` member is skipped in the sibling pass. Re-open the dialog to refresh the Input-name fields after changing `inputs`. Purple body, fa-calculator icon.

---

## JMESPath

**Purpose** — Runs each message through a JMESPath query and writes the result back onto the message (never drops). (`lib/pn-query.c:107` receive; engine `lib/pn-jmespath.c`, pointer resolution `lib/pn-json-path.c`.)

**When to use** — Reshape / extract structured payloads — pull a field out of a nested API/Ethereum response, project/filter an array, build a summary object — without writing C. The data-restructuring tool, vs Calculator's pure-arithmetic role.

**Ports** — `has_input` + `has_output`, single each.

**Settings**
- `expression` (string, default **`""`**): the JMESPath query. Empty → JSON-null result.
- `result-field` (string, default **`"result"`**): where the result goes. A non-empty name writes that one data-bag member, leaving siblings. **`""`** or the literal **`"data"`** replace the *entire* data bag with the result; a non-object result is wrapped under `value` so the bag stays a valid object.
- `source-field` (string, default **`""`**): `/`-separated JSON pointer the query runs against. Empty = whole message root (`topic`, `id`, `created`, `data` visible as top-level members); unresolvable path → query runs against JSON null.

**Behaviour** — Builds the input node (root or resolved source-field copy), runs `pn_jmespath_search`. If replacing the bag: clears it, copies an object result in, else wraps non-object/error under `value`. Else writes `result-field`; on success removes any stale `data.error`. Subset supported: identifier paths, index/slice, projections `[*] .* [?expr] []`, multi-select `{k:a} [a,b]`, pipes `|`, comparisons, `&& || !`, expr-refs `&expr`, raw/backtick + single-quote literals, `@`; functions `length type keys values contains starts_with ends_with to_string to_number to_array abs ceil floor sum avg max min max_by min_by sort sort_by reverse not_null merge join map`.

**Writes** — Sets `result-field` (or rewrites the whole `data` bag). On parse/type/no-match failure sets `data.error` and a JSON-null result; **always emits** (failures never drop the message). Does not touch `data.success`/`data.output`.

**Gotchas** — `result-field` = `""` or `"data"` is destructive (drops all other bag members) — picking `"data"` literally would otherwise nest `data.data`, hence the special-case. Teal body, fa-crosshairs icon.

---

## Parse JSON

**Purpose** — Parses a JSON *string* held in one data-bag member into real structure, so the rest of the pipeline can address it (never drops). (`lib/pn-parse.c:149` receive.)

**When to use** — Between **Http Client** and **JMESPath**. Http Client leaves its response body as an opaque string in `data.output`; JMESPath runs against the message *tree*, where that body is still one long string, so none of its fields are reachable. This node is the bridge. Also for JSON arriving as text over MQTT, a file read, or a shell command's stdout.

**Ports** — `has_input` + `has_output`, single each.

**Settings**
- `source-field` (string, default **`"data/output"`**): `/`-separated JSON pointer to the string to parse. Must resolve, and must address a string.
- `result-field` (string, default **`"result"`**): where the parsed document goes. Same semantics as JMESPath's: a non-empty name writes that one member, leaving siblings (including the raw body) in place; **`""`** or the literal **`"data"`** replace the *entire* data bag with the document, wrapping a non-object document under `value`.

**Behaviour** — Copies the source string out *before* touching the bag (the lookup root references the live bag, which a replace-the-bag result then clears). Parses with `JsonParser`. On success writes `result-field`, removes any stale `data.error`, and clears the node's error lamp.

**Writes** — Sets `result-field` (or rewrites the whole `data` bag). On failure sets `data.error` and `pn_node_set_has_error(TRUE)`; **always emits**. Does not touch `data.success`/`data.output`.

**Gotchas** — Failure is deliberately **non-destructive**: the bag is forwarded unchanged apart from `data.error`, so the body you could not parse survives for inspection. (This differs from JMESPath, which on a bag-replacing failure clears the bag and writes a null `value`.) Error strings: `source-field is empty`, `no such field`, `field is not a string` (already-structured data needs no parsing), `empty document` (whitespace only), else the JSON parser's own message. Blue body, fa-code icon.

---

## FX Converter

**Purpose** — Crypto/fiat exchange-rate converter: multiplies `data.value` by a cached `from→to` factor and forwards, stamping the factor and target code. (`lib/pn-rate.c:541` receive.) Subclass of **PnHttp** (→ PnAutoTrigger) — periodically fetches the rate itself.

**When to use** — Convert a numeric value to another currency before a Debug/Graph sink (e.g. ETH→USD). Wire after any node emitting a numeric `data.value`.

**Ports** — `has_input` + `has_output`, single each. Category **Filters/Compute & AI**.

**Settings**
- `from` (enum `PnCurrency`, default **ETH**) / `to` (default **USD**): 23-currency set (ADA ATOM AVAX BCH BNB BTC CRO DOGE DOT ETH LINK LTC MATIC PLS SOL TRX UNI USD USDC USDT XLM XMR XRP); USD is the pivot. `from == to` marks the node unconfigured and skips fetches. Changing the pair clears `last-update` and kicks an immediate refresh.
- `rate` (double, default **1.0**): cached factor (1 `from` = `rate` `to`); persisted, overwritten on each successful fetch.
- `last-update` (string ISO-8601, default **`""`**): timestamp of last fetch; persisted. Drives the cache-freshness gate.
- `url` (inherited from PnHttp, default CoinGecko `simple/price`): endpoint base; node appends `?ids=…&vs_currencies=usd`.
- `period` (inherited, default/floor **600 s** = 10 min): refresh cadence; sub-600 values silently clamped up (`on_period_notify`).

**Behaviour** — On receive: if `data.value` is numeric, rewrites it to `value * rate`; non-numeric/missing left untouched (message still passes). The fetch path (worker thread) pivots both currencies through USD in one CoinGecko request, computes `rate = price_from / price_to`, stamps `rate`/`last-update` under a mutex. The trigger override (`pn_rate_trigger`) short-circuits when the cached `last-update` is still within `period` — so reopening a worksheet doesn't burn a request.

**Writes** — Rewrites `data.value` (× rate), and always stamps `data.rate` (the factor) and `data.currency` (the `to` nick, e.g. "USD"). Does not set `data.success`/`data.output`. Notably does **not** emit on its own periodic ticks — rate updates are internal state; output is 1:1 with input messages.

**Gotchas** — `rate` defaults to 1.0, so an unconfigured/not-yet-fetched node passes values through scaled by 1. State is mutex-guarded (worker writes, main thread reads on receive). 10-minute floor protects CoinGecko's free-tier rate limit. GUI (icon+ticker combos, read-only cache rows) lives in companion `pn-rate-gui.c`. Gold body, fa-exchange icon.

---

## Throughput

**Purpose** — Measures message arrival rate and inter-arrival timing over a rolling window; **consumes** the input message and emits a fresh statistics message. (`lib/pn-stats.c:157` receive.) Category **Filters/Compute & AI**.

**When to use** — Monitor how busy a stream is (messages/min/hour/day) and how regular its timing, e.g. feeding a Graph sink with a "messages in the last N seconds" curve, or comparing success vs failure rates after a Success/Failure split.

**Ports** — `has_input` + `has_output`, single each.

**Settings**
- `window` (uint, 1–86400 s, default **60**): rolling window length; emitted stats summarise only arrivals in the previous `window` seconds.
- `bins` (uint, 1–3600, default **60**): equal-sized buckets the window is split into (circular buffer). Memory is O(bins); the window slides one bin at a time, not one message. Changing either property reallocates the ring and resets state.

**Behaviour** — Ignores the message *content* entirely — only its arrival time matters. Each receive tallies into the current bin (count + inter-arrival gap min/max/sum, gap attributed to the bin where the second arrival landed), then aggregates across all bins whose epoch is within the live window. Rates are `count / window` scaled to per-minute/-hour/-day. Memory stays O(bins) — no per-message retention.

**Writes** — Mints a **new** message via `pn_message_new(node, NULL)` (topic `stats`; the original message and its topic are **not** forwarded). Fields: `data.count` (int64), `data.min_seconds`, `data.max_seconds`, `data.average_seconds`, `data.per_minute`, `data.per_hour`, `data.per_day` (all double). Does not carry `data.value`/`success`/`output` from input.

**Gotchas** — The only node here that doesn't forward the received topic — downstream sees topic `stats`. Gaps larger than a single bin (even larger than the whole window) are still recorded as one measurement at the bin where the later arrival landed. Window/bin change drops the ring and the last-arrival latch (no migration). Yellow body, fa-bar-chart icon.
