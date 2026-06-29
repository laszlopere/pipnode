# Pipnode

[![License: GPLv3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
![Language: C](https://img.shields.io/badge/language-C-555.svg)
![GTK 3](https://img.shields.io/badge/GTK-3-green.svg)
[![Sponsor](https://img.shields.io/badge/Sponsor-%E2%9D%A4-db61a2.svg)](https://github.com/sponsors/laszlopere)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-555.svg)
[![Last commit](https://img.shields.io/github/last-commit/laszlopere/pipnode.svg)](https://github.com/laszlopere/pipnode/commits)

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
- Optional **Pipnode Plus** — a growing set of sponsor-only plugins
  (Kodi media-centre control to start), $15/month on GitHub Sponsors.
  See [Sponsorship](#sponsorship).

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
libsoup 3, GnuTLS, PLplot, MathGL (libmgl), and libmosquitto; WebKit2GTK is
optional. Exact
version requirements live in `configure.ac`, and `INSTALL` has notes on
optional voice/TTS data.

To build the sandboxed **Flatpak** community edition (a single-file bundle that
runs on any distro), see [`FLATPAK.md`](FLATPAK.md).

## Running headless (servers)

The editor (`pipnode-editor`) is the GTK application. A second binary,
`pipnode-run`, executes a saved worksheet **without the GUI** — load JSON,
run the flow, emit messages — so a worksheet can run on a server or under
cron/systemd:

```sh
pipnode-run my-flow.json            # run until interrupted
pipnode-run --timeout=30 my-flow.json
```

Pipnode is built as two libraries: a GTK-free **core** (`libpipnode-core`)
with the flow engine and all node logic, and a **GUI** tier
(`libpipnode-gui`) with the editor, canvas rendering and dialogs.
`pipnode-run` and `libpipnode-core` pull **no GTK at runtime**, so a host
with no display can execute flows. Bundled plugins are either core-only
(network, image, Tasmota) or two-tier — a headless logic `.so` plus an
editor-only `-gui.so` companion (shell, sound-effects) — so they install
and run on a server too. A distribution can therefore ship a `pipnode-core`
package with no GTK dependency alongside the full `pipnode-gui`; see
`INSTALL` for the packaging split and `PLUGINS` for writing
server-installable plugins.

## Credentials and connection profiles

Hosts, usernames, passwords and tokens are **not** stored in workflow files.
A plugin declares what a connection needs (e.g. the network plugin's *MQTT
Broker* and *HTTP Basic Auth* types); you provision named **profiles** once in
the editor under **Edit → Credentials**, and a node references one (or follows
the type's *primary* profile by default). A workflow file therefore carries
only a non-secret profile id, so it stays shareable.

The values live in `$XDG_CONFIG_HOME/pipnode/credentials.json` (typically
`~/.config/pipnode/credentials.json`), written at mode **0600**, separate from
`preferences.json`. On a **headless** host you do not need the editor — provide
secrets either by editing that file directly, or via environment variables,
which take precedence over the file:

```sh
# PIPNODE_PROFILE_<PROFILE-ID>_<FIELD>, upper-cased, non-alphanumerics -> _
PIPNODE_PROFILE_HOME_BROKER_PASSWORD=s3cr3t pipnode-run my-flow.json
PIPNODE_CREDENTIALS_FILE=/run/secrets/pipnode.json pipnode-run my-flow.json
```

`PLUGINS` (section 18) documents the API for plugin authors.

## XFCE panel applets

A worksheet can also drive an **XFCE panel applet** — a button on the panel
that runs a flow and shows a value. Because an applet is just a live mirror of
one worksheet's panel band, it is as flexible as the flow behind it: there is no
fixed catalogue of panel gadgets to pick from. Whatever you can wire up in
pipnode you can dock on the panel — a deadline countdown, a sensor readout, a
build-status lamp, a one-click switch — and reshape it at any time. See the
built-in help for the details.

![XFCE panel applet](screenshots/xfce-panel-applet.png)

*A pipnode worksheet mirrored as an XFCE panel applet.*

## Writing plugins

Plugins are the supported way to add node types (and, over time, other
extension kinds). The [`PLUGINS`](PLUGINS) guide walks through the ABI, the
loader, and how to build and install a plugin — a minimal one is about fifty
lines of C. Reference plugins live in `plugins/` and `tests/plugins/echo/`.

## Automating the editor (D-Bus)

The editor exposes a stable, introspectable D-Bus automation interface on the
session bus so scripts — or an AI agent like Claude Code — can author and edit
worksheets: read the graph, mutate nodes/wires/sheets, drive a running flow,
and observe changes live. The [`DBUS-API.md`](DBUS-API.md) guide documents the
contract, bus-name discovery, and worked examples; `tests/pndbus.py` is a
dependency-light reference client (and the easiest thing to copy). Run several
isolated instances with `pipnode-editor --dbus-name=<id>`.

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
- Some plugins — the **Pipnode Plus** set — are distributed as sponsor-only
  GitHub repositories (see below). The core never depends on them.

Contributions to the **core** require signing the
[Contributor License Agreement](CLA.md); see [`CONTRIBUTING.md`](CONTRIBUTING.md).
You do *not* need a CLA to publish your own plugin.

## Sponsorship

Pipnode is free and open source, and developing it takes real time. If it is
useful to you, please consider sponsoring the project on
[**GitHub Sponsors**](https://github.com/sponsors/laszlopere). Sponsoring at
any tier funds continued development of the open core for everyone.

The **$15 / month Pipnode Plus** tier additionally unlocks access to a curated
and growing set of private plugins — **Pipnode Plus** — built on top of the
open core. Today that's:

- **pipnode-kodi-plugin** — control your Kodi media centre from a worksheet:
  play/pause, library search, random episode, volume, notifications, full
  JSON-RPC pass-through.

New Pipnode Plus plugins are added under the same tier without bumping the
price. Repository invites are sent by hand within a day of your sponsorship
starting; they're revoked when the sponsorship ends.

## Copyright

Copyright (C) 2024-2026 Laszlo Pere. Licensed under GPLv3-or-later with the
Pipnode Plugin Exception.
