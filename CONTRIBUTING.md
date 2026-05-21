# Contributing to Pipnode

Thanks for your interest in improving Pipnode! This document covers how to
build, how to propose changes, and the licensing terms your contributions are
made under.

## Licensing of contributions — please read first

Pipnode is **open core**. The core (the `src/` application and the `lib/` host
library) is GPLv3 *with the Pipnode Plugin Exception* (see `LICENSE`,
`COPYING`, and `LICENSE.PLUGIN-EXCEPTION`). The project also ships some
**proprietary, sponsor-only plugins**, and may offer commercial builds.

To keep that model possible, **every contributor must sign the Contributor
License Agreement in [`CLA.md`](CLA.md) before their first contribution can be
merged.** The CLA lets you keep the copyright to your work while granting the
project maintainer the rights needed to (re)license it — including in
proprietary builds. We expect to automate signature collection with the
CLA-Assistant GitHub app; until then, indicate your agreement in your first
pull request as described in `CLA.md`.

> You do **not** need a CLA to write your own plugin — the Plugin Exception
> already lets plugins use any license. The CLA only applies to changes to the
> Pipnode core in this repository.

## Building from source

Pipnode uses the GNU autotools. From a clean checkout:

```sh
autoreconf -fi
./configure
make
```

Runtime/build dependencies (GTK 3, GLib/GModule, GtkSourceView 4, JSON-GLib,
libsoup 3, GnuTLS, PLplot, libmosquitto; WebKit2GTK optional) are listed in
`configure.ac`. See `INSTALL` for notes on optional voice/TTS data.

## Writing plugins vs. changing the core

- **New node types or other plugin functionality** should usually be a
  *plugin*, not a core change. Read [`PLUGINS`](PLUGINS) — it documents the
  plugin ABI, the loader, and how to build and install a plugin. Plugins can
  live outside this repository and under any license.
- **Core changes** (anything in `src/` or `lib/`) require the CLA and should
  come with a clear rationale.

## Pull request checklist

- [ ] I have read and agreed to [`CLA.md`](CLA.md).
- [ ] `autoreconf -fi && ./configure && make` builds cleanly.
- [ ] New source files carry the standard license header (see any existing
      file for the exact block).
- [ ] Code matches the style of the surrounding code.
- [ ] User-visible or ABI changes are documented (and `PN_PLUGIN_ABI_VERSION`
      is bumped if the plugin contract changed — see `PLUGINS`).

## Reporting bugs

Open a GitHub issue with steps to reproduce, your platform, and the relevant
worksheet (`.json`) if applicable.
