# Building and running the Pipnode Flatpak

Pipnode ships an open-source **community edition** as a Flatpak. This document
covers building it, turning it into a single distributable file, and running it.

- Application ID: `org.pipas.pipnode`
- Manifest: [`org.pipas.pipnode.yml`](org.pipas.pipnode.yml) (repo root)
- Runtime: `org.gnome.Platform` / `org.gnome.Sdk` version **50**

The GNOME runtime provides GTK 3, libsoup 3, json-glib, GnuTLS, gdk-pixbuf and
webkit2gtk-4.1. Five libraries Pipnode needs are **not** in any runtime and are
built as modules by the manifest: **gtksourceview-4**, **libmosquitto**,
**PLplot**, **MathGL** (the multi-series 3D graph views) and **GtkSheet** (the
spreadsheet widget). The manifest also bundles **FontAwesome 4.7** (the node
glyph font).

---

## Prerequisites

Install the Flatpak tooling and the runtime/SDK (one-time):

```sh
sudo apt install flatpak flatpak-builder            # Debian/Ubuntu
flatpak remote-add --user --if-not-exists flathub \
    https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.gnome.Platform//50 org.gnome.Sdk//50
```

> The GNOME 50 SDK includes `webkit2gtk-4.1` for the HTML help browser. If you
> bump the runtime version, verify it is still present:
> `flatpak run --command=pkg-config org.gnome.Sdk//50 --exists webkit2gtk-4.1 && echo yes`

---

## Build and install locally

From the repository root:

```sh
flatpak-builder --user --install --force-clean --repo=repo build-dir org.pipas.pipnode.yml
```

`build-dir`, `repo` and `.flatpak-builder/` are git-ignored. PLplot is the
heaviest module; to iterate on the dependency modules without building the app,
stop early:

```sh
flatpak-builder --user --force-clean --stop-at=pipnode build-dir org.pipas.pipnode.yml
```

Then run it (see **Running** below).

> If the app comes up **white/unstyled** (no grey backgrounds), a local build
> didn't pull your GTK theme into the sandbox — install it once with
> `flatpak install --user flathub org.gtk.Gtk3theme.<YourTheme>//3.22`. See
> [GTK theme](#gtk-theme-why-a-local-build-can-look-unstyled) for details.

---

## Make a single-file bundle

A `.flatpak` bundle is a single file you can copy to another machine or attach
to a GitHub release:

```sh
flatpak build-bundle repo pipnode.flatpak org.pipas.pipnode \
    --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo
```

The bundle does **not** contain the GNOME runtime. `--runtime-repo` lets a user's
`flatpak install ./pipnode.flatpak` pull the runtime from Flathub automatically
(so they need the Flathub remote, added as in *Prerequisites*).

Install the bundle on another machine:

```sh
flatpak install --user ./pipnode.flatpak
```

---

## Running

```sh
flatpak run org.pipas.pipnode                                   # the editor (GUI)
flatpak run --command=pipnode-run org.pipas.pipnode flow.json   # headless runner
```

Notes:

- **Display:** the editor is a GTK app and needs a desktop session. It works
  best on a local session (Wayland or X11). Running over plain SSH X11
  forwarding falls back to software GL — slow, and WebKit may not render.
- **File dialogs:** open/save of arbitrary paths uses the desktop portal. If
  dialogs misbehave on a minimal system, install a portal backend:
  `sudo apt install xdg-desktop-portal-gtk`. The bundled example worksheets
  (File ▸ Open Example) load without a portal.

---

## GTK theme (why a local build can look unstyled)

A Flatpak can only use a GTK theme that exists **inside the sandbox**. The host's
system themes (in `/usr/share/themes`) are not visible there, so Flatpak ships
each theme as a separate runtime extension, `org.gtk.Gtk3theme.<Name>`, which is
mounted into the sandbox on demand.

For a **normal install** (from Flathub, or from the bundle with the Flathub
remote configured) this is automatic: the runtime's extension point is declared
`download-if = active-gtk-theme`, so Flatpak downloads the extension matching the
user's active GTK theme. The app looks themed out of the box.

For a **local `flatpak-builder --install` build**, that auto-download does **not**
happen. With no matching theme in the sandbox, GTK falls back to its bare
built-in default — an unstyled, mostly **white** look (no grey backgrounds). The
fix is to install the extension yourself, **matching the runtime's extension-point
version, which is `3.22` for the GNOME 50 runtime** (not the theme's own version):

```sh
# Find your active GTK theme name:
gsettings get org.gnome.desktop.interface gtk-theme     # GNOME
xfconf-query -c xsettings -p /Net/ThemeName             # XFCE (e.g. "Greybird")

# Install the matching Gtk3 theme extension on the 3.22 branch:
flatpak install --user flathub org.gtk.Gtk3theme.Greybird//3.22
```

> On XFCE the GTK theme is the **xsettings** value (e.g. `Greybird`), which can
> differ from what `org.gnome.desktop.interface gtk-theme` reports. The theme the
> sandbox actually resolves can be checked with:
> ```sh
> flatpak run --command=gjs org.pipas.pipnode -c \
>   'imports.gi.versions.Gtk="3.0"; const {Gtk}=imports.gi; Gtk.init(null); \
>    print(Gtk.Settings.get_default().gtk_theme_name)'
> ```

If your theme has no `org.gtk.Gtk3theme.*` extension on Flathub, drop a copy into
`~/.local/share/themes/` or `~/.themes/` (both are shared read-only into the
sandbox by the manifest), or force a runtime-provided theme that always works,
e.g. `flatpak run --env=GTK_THEME=Adwaita org.pipas.pipnode`.

---

## What the manifest builds (modules, in order)

| Module | Build system | Notes |
|--------|--------------|-------|
| `gtksourceview4` | meson | `--libdir=lib`; GNOME runtime ships gtksourceview-5 (GTK4), so 4.x is bundled |
| `libmosquitto`   | cmake | client library only (`-DWITH_BROKER=OFF …`, `-DDOCUMENTATION=OFF`, `-DCMAKE_INSTALL_LIBDIR=lib`) |
| `plplot`         | cmake | double-precision C core + cairo drivers only; all language bindings off |
| `gtksheet`       | autotools | release tarball ships a pregenerated `configure`; `--disable-tests --disable-gtk-doc` |
| `pipnode`        | autotools | the app + all 7 bundled plugins; `--with-webview=auto` |
| `font-awesome`   | simple | installs FontAwesome 4.7 into `/app/share/fonts` |

The desktop entry, AppStream metainfo and application icon are installed by the
app's own `make install` (wired up in `data/Makefile.am`), so they land in the
bundle automatically.

---

## Sandbox permissions and limitations

`finish-args` grants: Wayland + X11 fallback, `dri`, PulseAudio, network, the
`xdg-documents`/`xdg-download` folders, and read-only access to the user's
themes/icons (`~/.themes`, `~/.icons` and the `xdg-data` equivalents). Note that
**system** GTK themes are delivered separately as `org.gtk.Gtk3theme.*`
extensions, not through these mounts — see [GTK theme](#gtk-theme-why-a-local-build-can-look-unstyled)
above. WebKit is run with `WEBKIT_DISABLE_DMABUF_RENDERER=1` and
`WEBKIT_DISABLE_COMPOSITING_MODE=1` so the help browser stays usable on
software-rendering hosts (VMs, remote sessions, GPU-less machines).

**Sandbox limitation:** the `shell` and `host-monitoring` nodes run **inside the
sandbox**, not against the host — so shell commands execute in the runtime and
host metrics describe the sandbox. Everything network-based (HTTP, MQTT,
WebSocket, DNS, Ollama, Tasmota), image processing, the gauges/plots and the
editor work normally. For full host access, use a native build from source
(see [`INSTALL`](INSTALL)).

---

## Maintainer notes

- **Reproducible release builds:** the manifest's `pipnode` module uses
  `type: dir` (the local working tree) for convenient iteration. For a tagged
  release, switch it to a clean checkout:
  ```yaml
  sources:
    - type: git
      url: https://github.com/laszlopere/pipnode.git
      tag: v0.1.0
  ```
- **Runtime lifecycle:** target a supported GNOME runtime (48 and earlier are
  end-of-life). Re-check `webkit2gtk-4.1` availability when bumping.
- **autotools:** the project has no `AM_MAINTAINER_MODE`; after editing any
  `Makefile.am`, re-run `autoreconf -fi && ./configure` before a native
  `make install`. (flatpak-builder runs `autogen.sh` from a clean tree itself.)
- **Publishing:** once a build is verified, tag and attach the bundle:
  ```sh
  git tag -a v0.1.0 -m "Pipnode 0.1.0"
  git push origin v0.1.0
  gh release create v0.1.0 ./pipnode.flatpak --title "Pipnode 0.1.0" --notes "…"
  ```
