# Bundled sound-effects plugin.  Installed alongside the host binaries
# so the default plugin search path ($pkglibdir/plugins) picks it up
# without any post-install configuration.
#
# Ships the PnSciFiSound sink node.  No audio clips are bundled —
# the user downloads them at runtime through a button on the node's
# settings dialog, which fetches a curated set of classic Star Trek
# sound effects from a public fan-site mirror into the per-user
# cache directory the node scans at start-up.
#
# Two-tier plugin (TODO #23, Phase 8): the logic module
# pipnode_sound_effects.so is GTK-free and links libpipnode-core ALONE,
# so a headless server installs it and plays the configured clip on every
# incoming message under pipnode-run with no GTK (and no libsoup — the
# download manager lives entirely in the GUI tier).  The settings dialog
# — a per-pack download/delete grid and a clip picker, neither of which
# fits the declarative settings schema — ships in the companion module
# pipnode_sound_effects-gui.so, which the editor loads next to the logic
# .so (via pn_plugin_gui_init) and a server never installs.  The core
# plugin loader skips *-gui.so during a logic-plugin scan.  Both modules
# compile the shared, GTK-free cache/player helpers in pn-sci-fi-clips.c.
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_sound_effects.la pipnode_sound_effects-gui.la

# -------------------------------------------------------------------
# Logic half — GTK-free, core only.  Loaded by pipnode-run and the
# editor; exports pn_plugin_init and registers the PnSciFiSound #GType.
# -------------------------------------------------------------------
pipnode_sound_effects_la_SOURCES = \
	pn-sound-effects-plugin.c \
	pn-sci-fi-sound.c \
	pn-sci-fi-sound.h \
	pn-sci-fi-clips.c \
	pn-sci-fi-clips.h

pipnode_sound_effects_la_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_sound_effects_la_LDFLAGS = -module -avoid-version -shared

pipnode_sound_effects_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)

# -------------------------------------------------------------------
# Companion GUI module — links the GTK tier + libsoup.  Automake
# canonicalises the `pipnode_sound_effects-gui.la` target to the
# `pipnode_sound_effects_gui_la_` prefix.  Loaded by the editor only;
# resolves PnSciFiSound by name (the logic .so is BIND_LOCAL) and
# installs its settings-dialog vfunc slot.
# -------------------------------------------------------------------
pipnode_sound_effects_gui_la_SOURCES = \
	pn-sci-fi-sound-gui.c \
	pn-sci-fi-clips.c \
	pn-sci-fi-clips.h

pipnode_sound_effects_gui_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	$(LIBSOUP_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_sound_effects_gui_la_LDFLAGS = -module -avoid-version -shared

pipnode_sound_effects_gui_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS) \
	$(LIBSOUP_LIBS)

pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnSciFiSound.html

EXTRA_DIST = $(pnhelp_DATA)
