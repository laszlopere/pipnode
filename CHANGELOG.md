# Changelog

All notable changes to Pipnode are recorded here. Versions follow
[Semantic Versioning](https://semver.org/); while the major version is `0`
the public surface (node names, on-the-wire messages and the plugin ABI) is
still allowed to change between releases — see the stability note under each
release.

## [0.1.0] — 2026-05-24

The first public release of Pipnode: a desktop visual flow editor for Linux.
Drop nodes onto a worksheet, wire them together, and build dataflows — network
probes, MQTT, sensors, gauges, image processing, LLM calls and more — without
writing glue code. It is a native GTK 3 application written in C.

### Stability

This is an **early but stable** release. It is tested and usable for real work:

- It does not crash in normal use.
- The GUI is complete and unobtrusive — no dead buttons, no placeholder panes;
  every menu, dialog and node that ships does something useful.
- The bundled node catalogue and the editor are together enough to build and
  run genuinely useful flows day to day.

It is **not yet mature**, and the public surface is **not frozen**. Expect the
following to change in future `0.x` releases, possibly without a migration path:

- the set of nodes, their names and their behaviour;
- the structure of the messages passed between nodes ("on the wire");
- the plugin ABI and the host API that plugins build against.

In short: it works and it is pleasant to use, but treat saved worksheets and
third-party plugins as tied to this version for now.

### Editor

- Node-graph **worksheets** with live message flow, saved as plain JSON.
- Drag-and-drop node palette, wiring by dragging between ports (including
  pulling a connection from an input port).
- A **Debug view** for inspecting messages flowing through the graph.
- **Document Settings**: typed, named global values stored in the worksheet
  and referenced from nodes via `${…}`.
- A **File ▸ Open Example** submenu of installed, categorised sample
  worksheets.
- Built-in **per-node HTML help**, plus a general user manual.
- An **About** dialog with the project links and licensing.

### Nodes (bundled)

- **Flow & logic**: inject, auto-injector, auto-trigger, auto-random,
  staircase, delay, throttle, dedup, filter, threshold, comparator, edge,
  watchdog, switch, set.
- **Transform**: expression, format, rewrite, JSONPath / JMESPath, query,
  JSON handling.
- **Display & input**: analog meter, dial, gauge/graph (PLplot), LED, knob,
  table, text view, file viewer, notify.
- **I/O & integrations**: HTTP, MQTT and WebSocket (network plugin), Tasmota
  smart-plug power/energy/sensors, host monitoring (CPU, memory, load, disk,
  network, temperature), shell commands / `df` / `free` / tmux monitoring,
  image processing (blur, sharpen, colour, geometry, edge detection, …),
  sound effects, local LLM via Ollama, weather, RTC, Meshtastic, crypto rate.

### Headless operation

- A second binary, **`pipnode-run`**, executes a saved worksheet **without the
  GUI** (load JSON, run the flow, emit messages) — suitable for servers and
  cron/systemd.
- Pipnode is split into a GTK-free **core** (`libpipnode-core`, flow engine +
  all node logic) and a **GUI** tier (`libpipnode-gui`). `pipnode-run` and the
  core pull no GTK at runtime, so a host with no display can run flows.
- Bundled plugins are either core-only (network, image, Tasmota, host
  monitoring) or two-tier — a headless logic `.so` plus an editor-only
  `-gui.so` companion (shell, sound-effects) — so they install on a server too.

### Plugins & licensing

- A first-class **plugin system**: node types ship as `.so` modules loaded at
  start-up. A minimal plugin is about fifty lines of C; the `PLUGINS` guide
  documents the ABI and loader.
- **Open core**: the core is GPLv3-or-later with the **Pipnode Plugin
  Exception**, so plugins may use any licence, including proprietary, as long
  as they talk to Pipnode through the documented plugin interface.

### Packaging

- GNU autotools build (`autoreconf -fi && ./configure && make`).
- A sandboxed **Flatpak community edition** (app id `org.pipas.pipnode`,
  GNOME 48 runtime) — a single-file bundle that runs on any distro. Note that,
  as a sandboxed Flatpak, the Shell and host-monitoring nodes operate inside
  the application sandbox rather than against the host.

[0.1.0]: https://github.com/laszlopere/pipnode/releases/tag/v0.1.0
