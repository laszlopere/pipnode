---
name: pipnode-nodes
description: Detailed reference for the nodes shipped in the pipnode project — both the core/built-in nodes and the in-tree bundled plugins under plugins/ (e.g. tasmota) — what each node does, when to reach for it, the message-bag fields it reads/writes, and how to configure it. Load this when you need specific knowledge about a node's behaviour, settings, ports, or wiring. Only nodes from plugins in SEPARATE repositories (kodi, zigbee, rtl-sdr, ethereum) are out of scope.
---

# Pipnode nodes (core + bundled plugins)

Pipnode is a desktop visual flow editor (inspired by Node-RED). You wire
data-processing **nodes** on a worksheet, save the diagram as JSON, and run it
locally. This skill documents the **core / built-in** nodes — the ones
registered in `lib/pn-node-factory.c` (`register_builtins`) and implemented in
`lib/pn-*.c` — **plus the nodes from the in-tree bundled plugins** under
`plugins/` (host-monitoring, network/Ping/DNS, ollama, image, tasmota, shell,
sound-effects, …), which ship in this same repo and are part of the project.
Each bundled plugin gets its own `reference/plugins-<name>.md` file (see the
index below); so far **tasmota** is documented. Nodes from plugins that live in
**separate repositories** (kodi, zigbee, rtl-sdr, ethereum, …) are **not**
covered here — those are external projects.

Use the per-category reference files for full detail. Each entry lists: purpose,
when to use it, input/output ports, the settings-dialog fields, the message-bag
members it reads and writes, and gotchas.

Core nodes, by category:

- [`reference/sources.md`](reference/sources.md) — **Sources** (emit messages): Clock, Injector, FileDrop, Switch, Knob, Panel Input, AutoInjector, AutoRandom, Astronomical
- [`reference/network.md`](reference/network.md) — **Network**: Http Client, MQTT Source, MQTT Sink, Weather, Meshtastic
- [`reference/filters-gate-compute.md`](reference/filters-gate-compute.md) — **Filters/Gate, Expressions, Compute & AI**: Comparator, Dedup, Edge, Filter, Success, Failure, Threshold, Calculator, Calculator 2, JMESPath, FX Converter, Throughput
- [`reference/filters-reshape-timing.md`](reference/filters-reshape-timing.md) — **Filters/Reshape, Timing, Deadline**: Format, Rewrite, Set, Text, Topic, Value, Table Model, Delay, Staircase, Throttle, Watchdog, Deadline
- [`reference/sinks.md`](reference/sinks.md) — **Sinks** (consume/visualise): Debug Print, Graph, XY Graph, Weather Report, Sun Path, Chat, Sound, Text to Speech, Notify, FileViewer, Text View, Table, Table View, Panel Display
- [`reference/gui-displays-gauges.md`](reference/gui-displays-gauges.md) — **GUI/Displays & Gauges + indicator sinks**: Numeric, Segment16, Matrix57, DigitalClock, Label, AnalogClock, AnalogMeter, Dial, LED, Countdown

Bundled-plugin nodes (in-tree, under `plugins/`):

- [`reference/plugins-tasmota.md`](reference/plugins-tasmota.md) — **Tasmota** (smart plugs/switches/sensors over MQTT): Power, Current, Voltage, Apparent Power, Reactive Power, Power Factor, Temperature, Humidity, Switch, Relay Command, Relay Status, Status, Status Request, Probe, Concentrator
- [`reference/plugins-host-monitoring.md`](reference/plugins-host-monitoring.md) — **Host monitoring** (periodic local/SSH host metrics): CPU, Memory, System Load, CPU Temperature, Ambient Temperature, Disk I/O, Network I/O, Net Connections
- [`reference/plugins-network.md`](reference/plugins-network.md) — **Network** (the plugin, distinct from the core Network category): Network Ping, DNS Check, HTTPS Tunnel Sender, HTTPS Tunnel Receiver
- [`reference/plugins-shell.md`](reference/plugins-shell.md) — **Shell** (periodic CLI-runner sources, the documented subprocess exception): Shell Command, Shell Script, Df Command, Free Command, Lxc Ls Command, Tmux Monitor
- [`reference/plugins-image.md`](reference/plugins-image.md) — **Image Processing** (55 pure-C pixbuf filters): Grayscale, Brightness, Gaussian Blur, Sobel Edge, Multiply, Crop, Resize, Rotate, … (Color/Adjust/Blur/Sharpen/Edge/Composite/Geometry/Stylize)
- [`reference/plugins-ollama.md`](reference/plugins-ollama.md) — **Ollama** (LLM completion via a local/remote Ollama server)
- [`reference/plugins-sound-effects.md`](reference/plugins-sound-effects.md) — **Sound effects**: SciFi Sound (downloadable Star Trek clip player)

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
