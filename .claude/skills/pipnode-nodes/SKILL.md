---
name: pipnode-nodes
description: Detailed reference for the built-in (core) nodes shipped in the pipnode project — what each node does, when to reach for it, the message-bag fields it reads/writes, and how to configure it. Load this when you need specific knowledge about a core node's behaviour, settings, ports, or wiring (NOT for plugin nodes, which live in separate repos under plugins/).
---

# Pipnode core nodes

Pipnode is a desktop visual flow editor (inspired by Node-RED). You wire
data-processing **nodes** on a worksheet, save the diagram as JSON, and run it
locally. This skill documents the **core / built-in** nodes — the ones
registered in `lib/pn-node-factory.c` (`register_builtins`) and implemented in
`lib/pn-*.c`. Nodes from the bundled plugins under `plugins/` (host-monitoring,
network/Ping/DNS, ollama, image, tasmota, shell, sound-effects, …) are **not**
covered here — they are separate projects.

Use the per-category reference files for full detail. Each entry lists: purpose,
when to use it, input/output ports, the settings-dialog fields, the message-bag
members it reads and writes, and gotchas.

- [`reference/sources.md`](reference/sources.md) — **Sources** (emit messages): Clock, Injector, FileDrop, Switch, Knob, Panel Input, AutoInjector, AutoRandom, Astronomical
- [`reference/network.md`](reference/network.md) — **Network**: Http Client, MQTT Source, MQTT Sink, Weather, Meshtastic
- [`reference/filters-gate-compute.md`](reference/filters-gate-compute.md) — **Filters/Gate, Expressions, Compute & AI**: Comparator, Dedup, Edge, Filter, Success, Failure, Threshold, Calculator, Calculator 2, JMESPath, FX Converter, Throughput
- [`reference/filters-reshape-timing.md`](reference/filters-reshape-timing.md) — **Filters/Reshape, Timing, Deadline**: Format, Rewrite, Set, Text, Topic, Value, Table Model, Delay, Staircase, Throttle, Watchdog, Deadline
- [`reference/sinks.md`](reference/sinks.md) — **Sinks** (consume/visualise): Debug Print, Graph, XY Graph, Weather Report, Sun Path, Chat, Sound, Text to Speech, Notify, FileViewer, Text View, Table, Table View, Panel Display
- [`reference/gui-displays-gauges.md`](reference/gui-displays-gauges.md) — **GUI/Displays & Gauges + indicator sinks**: Numeric, Segment16, Matrix57, DigitalClock, Label, AnalogClock, AnalogMeter, Dial, LED, Countdown

## The message contract (read this first)

Every wire carries a `PnMessage`. The **envelope** is `topic`, `id`, `created`,
plus a back-reference to the source node. The **data bag** (`data.*`) is a
schemaless JSON object the producer fills with its payload, but **three members
are mandatory on every message** and form the contract that lets nodes mix and
match without configuration:

- **`data.value`** — the canonical numeric/primitive reading. Booleans ride here
  as `0.0` (false/off) / `1.0` (true/on); detect "on" with `value > 0.5`.
- **`data.output`** — the human-readable summary string (what Debug Print / Text
  View / TTS / Notify show by default).
- **`data.success`** — boolean; whether the producing operation succeeded.

A `Graph` fed from a sensor "just works" because both agree the reading lives at
`data.value`; a `Debug Print` after a `Format` "just works" because both agree
the summary lives at `data.output`. Honour these names; reach for a more
specific bag key only when the concept genuinely doesn't fit.

**Forking:** when an output has several wires, each wire delivers an independent
deep-copy — a filter that mutates its message in place cannot contaminate a
sibling branch. `id` is preserved across the fork so copies stay correlatable.

### Envelope fields

| Field | Meaning |
|---|---|
| `topic` | Routing tag stamped on emitted messages. Configured per node on the **Node** tab as a template, resolved at emit time. Default `/pnode/${nodeclass}/${nodename}` (hostname-carrying nodes append `/${hostname}`). Placeholders: `${nodeclass}`, `${nodename}`, `${hostname}`. Empty entry restores the default. Filter/transformer nodes forward the received topic unchanged. |
| `id` | Unique message id; lasts the lifetime of the message, preserved across forks. |
| `created` | ISO-8601 creation timestamp. |
| `from` / `from_id` | Synthesised by Debug Print at render time (source name + UUID); not real wire fields. |

## How nodes are built (orientation for reading the source)

Each node is a `GObject` subclass of `PnNode` in `lib/pn-<name>.c`. Key things
to look at when you need detail beyond these docs:

- **`*_class_init`** sets `node_class->class_name` (palette/inspector label),
  `category` (slash-separated palette path), `icon`/`palette_icon` (FontAwesome
  4.7 glyph), and `has_input`/`has_output` (whether the node has ports).
- **`process` vfunc** (the `process_message` / handler) is where an input
  message is consumed — this is the node's runtime behaviour.
- **GObject properties** (`g_object_class_install_property`) are the persisted
  settings; the settings dialog (`PnSettingsSchema` / `pn-settings-schema.c`,
  or a bespoke `*-gui.c`) renders them as the **Settings** tab fields.
- **`-gui.c` companion** holds the GTK/cairo widget for display nodes (the
  headless core / GTK split — see the project memory). Logic lives in the plain
  `.c`; the visual rendering in `-gui.c`.
- **Per-node help page**: `data/help/<ClassName>.html` (e.g. `PnKnob.html`) is
  the user-facing manual page and a good cross-check for intended behaviour.

When the reference files are insufficient, read `lib/pn-<name>.c` + its help
page directly; the categories above map one node per row to one `pn-*.c` file.
