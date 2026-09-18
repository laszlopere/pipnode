# CPU — building blocks for a clocked machine

The **CPU** palette group is a small kit for wiring an ultra-primitive clocked computer (fetch / decode / execute) on a worksheet: **Register**, **Counter**, **RAM**, **Mux**, plus the two multi-output timing blocks **Shift Register** and **Clock Divider**. All six are GTK-free core nodes (`lib/pn-*.c`) with the shared indigo body. The rest of a machine comes from ordinary nodes: **PROM** (program memory, `filters-reshape-timing.md`), **Calculator** / **Calculator 2** (decode and ALU, `filters-gate-compute.md` — `%`, `& | ^ << >> ~` make field extraction one line: `op = value >> 4`, `arg = value & 15`), **Value Router** (opcode dispatch, `filters-gate-compute.md`) and **Set** (raise `data.write` for RAM).

## The rule that makes feedback loops work

Every *store* port in the kit is **silent**: Register `write`/`reset`, Counter `load`/`reset`, and a RAM write latch the value and emit nothing. Only a *read* strobe (Register `read`, Counter `tick`, a RAM read) emits. So a result can be wired back into the register or program counter it came from without forming a **synchronous** cycle — the stored value surfaces on the *next* clock tick, and each machine cycle stays one bounded chain well under `PN_NODE_MAX_DISPATCH_DEPTH` (256). A loop built only from nodes that emit on every input has no such break: it recurses synchronously until the dispatch-depth guard stops it.

A typical cycle, as in `examples/controls-and-logic/toy-cpu-accumulator.json`:

```
AutoInjector (clock) ─► Counter "PC" (tick) ─► PROM "Program" ─► Calculator "Decode"
      op = value >> 4 / arg = value & 15 ─► Value Router on path=op ─► one branch per opcode:
        LOAD → Register ACC.write          ADD → ACC.read → Calculator value+arg → ACC.write
        OUT  → ACC.read → display          JMP → Counter PC.load (takes effect next tick)
        STO  → ACC.read → Set write=true → RAM        LDM → RAM read → ACC.write
```

Runs headless under `pipnode-run` (prints 13, 5, 13, 5 … — the JMP loop plus a RAM store/load).

## Register

**Purpose** — A wire-writable one-word latch: accumulator, general register or flag store. (`lib/pn-register.c`)

**When to use** — Hold a number between clock ticks and read it back on demand, especially inside a feedback loop. Contrast **Switch** (a boolean latch that forwards on change) and **Value** (a constant from the settings, not from a wire).

**Ports** — 3 inputs: 0 `write`, 1 `read`, 2 `reset` (use `target_input`). 1 output.

**Settings**
- `initial` (double, default `0.0`) — the value `reset` restores. Setting it also seeds the latch, so a node loaded with `initial = 7` reads 7 before its first write.

**Behaviour** — `write`: latches a numeric `data.value` (int or double); a message without one is ignored, latch untouched. **Silent.** `read`: rewrites the *arriving* message and emits it. `reset`: latch = `initial`. **Silent.**

**Writes** (on `read`) — `data.value` = stored word, `data.success` = TRUE, `data.output` = `%g` of the word. Topic, id and every other member of the strobe message pass through — useful for carrying decode fields (`op`, `arg`) past the register to the ALU.

**Gotchas** — The latch is runtime state, not saved: reopening the worksheet starts from `initial`. A write and a read in the same synchronous chain see the order they arrive in — wire the read *after* the write if you want the new value.

## Counter

**Purpose** — A self-advancing register: the program counter. Each tick emits the current value, then adds `step`. (`lib/pn-counter.c`)

**When to use** — Stepping through PROM/RAM addresses, sequencing, counting ticks as a value. Contrast **Clock Divider** (forwards every Nth tick, doesn't report the count) and **Round Robin** (deals messages to outputs in turn).

**Ports** — 3 inputs: 0 `tick`, 1 `load`, 2 `reset`. 1 output.

**Settings**
- `initial` (double, default `0.0`) — start value and the value `reset` restores (setting it also sets the current count).
- `step` (double, default `1.0`) — added after each tick; may be negative or fractional.
- `modulo` (double, `>= 0`, default `0.0`) — when `> 0`, the *advanced* value wraps into `[0, modulo)` (floored: a count-down that goes below 0 wraps up to the top, e.g. 0 − 1 → `modulo − 1`); `0` disables wrapping.

**Behaviour** — `tick`: emits the current value (rewriting the tick message), then advances. `load`: sets the next value from a numeric `data.value` (JUMP) — **silent**, stored verbatim (no modulo). `reset`: next value = `initial` — **silent**.

**Writes** (on `tick`) — `data.value` = the address emitted, `data.success` = TRUE, `data.output` = `%g`.

**Gotchas** — **JUMP during a tick**: a `load` or `reset` that arrives *while the tick's own emit is still running* (i.e. decoded from the instruction this tick fetched) is recorded as pending and replaces the normal advance once the emit returns. So a JMP decoded in cycle N makes cycle N+1 fetch the target, exactly like hardware. The count is runtime state, not saved.

## RAM

**Purpose** — Addressable read/write memory, PROM-compatible on the wire. (`lib/pn-ram.c`)

**When to use** — Data memory for a CPU worksheet, or any "store a number under a numeric key and read it back later" job. Use **PROM** for read-only tables fixed in the settings.

**Ports** — 1 input, 1 output.

**Settings**
- `contents` (string, multiline, default `""`) — optional seed image in PROM's syntax: one `<address> <word>` pair per line, decimal or `0x…` hex, `#` comments, blank lines skipped. Reloaded (memory wiped and re-seeded) whenever the property changes. A malformed line sets the node's error state (red ❗) while the good lines still load. Edited on a full-width "Contents" tab.

**Behaviour** — The address is `data.value`, rounded to the nearest cell (`llround`, halves away from zero). No numeric `value` → message dropped. **Write** when `data.write` is TRUE (boolean, or a number `> 0.5`): stores `data.word` (missing / non-numeric stores `0.0`) — **silent**. **Read** otherwise: emits the message with the stored word.

**Writes** (on read) — `data.value` = stored word (never-written cells read `0.0`), `data.address` = decoded cell — the same shape PROM emits.

**Gotchas** — Written cells are runtime state, not saved; only the `contents` seed survives a reload. A stored `0` and an unwritten cell look identical. To write from a flow you usually need two shaping steps: a Calculator to move the data into `word` and put the address in `value` (`word = value` then `arg`), and a **Set** to add `write = true`.

## Mux

**Purpose** — A value-selected multiplexer: forwards the value of the data input chosen by the `select` input. (`lib/pn-mux.c`)

**When to use** — Choose an ALU operand or a register source by a number decoded at run time; pick one of N live readings by a mode knob. Contrast **Value Router** (one input, *routes* it to one of N outputs — the opposite direction) and **Calculator 2** (combines inputs arithmetically).

**Ports** — `1 + inputs` inputs: 0 `select`, then `in1` … `inN`. 1 output.

**Settings**
- `inputs` (int, `2`..`8`, default `2`) — number of data inputs; resizes the ports live.

**Behaviour** — The core latches every input's last `data.value` under the input's name (collated inputs). Selector = `llround(select)`, clamped into `0 .. inputs-1` (0 picks `in1`); before `select` has carried a number it reads as 0. Emits when the **selector** arrives, or when the **currently selected** data input arrives; an update on an unselected line is only latched. Nothing is emitted while the selected line has never carried a value.

**Writes** — `data.value` = selected line's value, `data.success` = TRUE, `data.output` = `%g`, on the arriving message (so its topic and other members pass through).

**Gotchas** — Select is 0-based while port names are 1-based (`select = 0` → `in1`). Out-of-range selectors are clamped, not dropped. Only `data.value` is multiplexed — other members come from whichever message triggered the emit.

## Shift Register

**Purpose** — Open-ended (non-looping) shift register of whole messages: one input, `outputs` outputs, one per stage. (`lib/pn-shift-register.c`, palette group **CPU**)

**When to use** — Keep the last N messages available at once: a moving window for a downstream Calculator 2 (moving sum/average), "previous value" comparisons, or a delay line measured in messages rather than time (contrast **Delay**, which shifts in time).

**Ports** — 1 input; `outputs` outputs (`out1` … `outN`), stacked down the right edge. Wire a specific output with `ConnectPorts` over D-Bus; on disk the wire carries `source_output`.

**Settings**
- `outputs` (int, `2`..`16`, default `4`) — stage count. Shrinking keeps the newest stages; wires on removed outputs stop carrying messages.

**Behaviour** — Each arriving message is cloned into stage 1 and every stored stage moves along one; the last stage's message is dropped. Then every *filled* stage k is emitted on output k (0-based k−1), **oldest stage first, `out1` last**. Unfilled stages are silent: the first message produces only `out1`, the second `out1`+`out2`, etc. Each emission is a fresh clone whose source is the Shift Register.

**Writes** — Nothing; stored messages go out verbatim (topic and all `data.*`).

**Gotchas** — One input message causes up to N emissions, so a downstream collating node (Calculator 2) computes N times; only the result triggered by `out1` sees all stages updated. The stages are runtime state, not saved with the worksheet.

## Clock Divider

**Purpose** — Derives slower tick streams from one: inputs `tick` and `reset`, `outputs` outputs, each forwarding every tick whose count is a multiple of its divisor. (`lib/pn-clock-divider.c`, gui tab `lib/pn-clock-divider-gui.c`, palette group **CPU**)

**When to use** — One Auto Injector drives chains at several rates (every 2 s, 4 s, 60 s …) instead of several independent timers drifting apart; clock phases for CPU-kit worksheets. Contrast **Throttle** (time-based thinning of an arbitrary stream) and **Counter** (emits the count itself).

**Ports** — 2 inputs: 0 `tick`, 1 `reset` (use `target_input: 1`). `outputs` outputs labelled with their divisor (`/2`, `/4`, …); wire a specific one with `ConnectPorts` / `source_output`.

**Settings**
- `outputs` (int, `2`..`16`, default `4`).
- `divisors` (string, JSON array of ints `1`..`1000000`, default `"[]"`) — one per output; missing/non-numeric entries use the positional default 2^(k+1) (2, 4, 8, … 65536). Always 16 slots, so shrinking `outputs` keeps the removed divisors; saved form trims trailing defaults. The gui tab shows one spin per output.

**Behaviour** — A tick increments a 64-bit count, then output k fires iff `count % divisor(k) == 0` (so /4 fires on ticks 4, 8, 12 — not on tick 1). Firing outputs are emitted **largest divisor first, smallest last**, ties highest output first (pure seam `pn_clock_divider_plan()`). Each emission is a clone of the tick message (topic and `data.*` unchanged), source = the divider. A `reset` message zeroes the count and emits nothing (safe to wire from downstream).

**Writes** — Nothing; the tick message is forwarded verbatim.

**Gotchas** — The count is runtime state, not saved: a reopened worksheet starts from zero, and a divisor change takes effect against the running count (no re-phase — send `reset` for that). Example: `examples/controls-and-logic/clock-divider.json`.
