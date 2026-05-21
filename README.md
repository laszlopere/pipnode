# Pipnode

A desktop visual flow editor for Linux, inspired by Node-RED. Drag nodes onto a
worksheet, wire them together, and build dataflows — network probes, MQTT,
sensors, gauges, image processing, LLM calls, and more — without writing glue
code. Pipnode is a native GTK 3 application written in C, with a first-class
plugin system so the node catalogue can grow without forking the core.

![Meters](screenshots/meters.png)

## Features

- Node-graph worksheets with live message flow, saved as plain JSON.
- A rich built-in node catalogue and bundled plugins (network, image,
  Tasmota, sound, shell, …).
- A dynamic **plugin system**: node types ship as `.so` modules loaded at
  start-up. Anyone can write plugins, and they can be open source *or*
  proprietary (see [Licensing](#licensing)).
- A debug view, expression/format/filter nodes, gauges and meters, and
  built-in per-node HTML help.

<p>
  <img src="screenshots/temperature.png" width="32%" alt="Temperature graph">
  <img src="screenshots/ollama.png" width="32%" alt="Ollama node">
  <img src="screenshots/meshtastic.png" width="32%" alt="Meshtastic node">
</p>

## Building

Pipnode uses GNU autotools:

```sh
autoreconf -fi
./configure
make
sudo make install        # optional
```

Main dependencies: GTK 3 (≥ 3.22), GLib/GModule, GtkSourceView 4, JSON-GLib,
libsoup 3, GnuTLS, PLplot, and libmosquitto; WebKit2GTK is optional. Exact
version requirements live in `configure.ac`, and `INSTALL` has notes on
optional voice/TTS data.

## Writing plugins

Plugins are the supported way to add node types (and, over time, other
extension kinds). The [`PLUGINS`](PLUGINS) guide walks through the ABI, the
loader, and how to build and install a plugin — a minimal one is about fifty
lines of C. Reference plugins live in `plugins/` and `tests/plugins/echo/`.

## Ideas & feature requests

Got an idea for a node or a feature? Open one on the
[**Ideas board**](https://github.com/laszlopere/pipnode/discussions/categories/ideas)
(GitHub Discussions) and 👍 the ones you'd like to see. It's a wishlist, not a
roadmap — every suggestion is read (sponsor input especially), but nothing there
is a promise.

## Licensing

Pipnode is **open core**:

- The **core** (`src/` + `lib/`) is **GPLv3-or-later, with the Pipnode Plugin
  Exception**. See [`LICENSE`](LICENSE), [`COPYING`](COPYING), and
  [`LICENSE.PLUGIN-EXCEPTION`](LICENSE.PLUGIN-EXCEPTION).
- **Plugins may use any license, including proprietary**, as long as they talk
  to pipnode through the documented plugin interface. The Plugin Exception
  makes this explicit — it applies to everyone, not just the project author.
- Some **premium plugins are distributed as sponsor-only binaries** (see
  below). The core never depends on them.

Contributions to the **core** require signing the
[Contributor License Agreement](CLA.md); see [`CONTRIBUTING.md`](CONTRIBUTING.md).
You do *not* need a CLA to publish your own plugin.

## Sponsorship

Pipnode is free and open source, and developing it takes real time. If it is
useful to you, please consider sponsoring the project. Higher sponsorship tiers
get access to **premium, closed-source plugins** (for example, plugins that
integrate with heavier backends such as MySQL/Galera). Sponsoring funds
continued development of the open core for everyone.

See the **Sponsor** button on the GitHub repository, or `.github/FUNDING.yml`.

## Copyright

Copyright (C) 2024-2026 Laszlo Pere. Licensed under GPLv3-or-later with the
Pipnode Plugin Exception.
