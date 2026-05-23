# Bundled sound-effects plugin.  Installed alongside the host binaries
# so the default plugin search path ($pkglibdir/plugins) picks it up
# without any post-install configuration.
#
# Ships the PnSciFiSound sink node.  No audio clips are bundled —
# the user downloads them at runtime through a button on the node's
# settings dialog, which fetches a curated set of classic Star Trek
# sound effects from a public fan-site mirror into the per-user
# cache directory the node scans at start-up.
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_sound_effects.la

pipnode_sound_effects_la_SOURCES = \
	pn-sound-effects-plugin.c \
	pn-sci-fi-sound.c \
	pn-sci-fi-sound.h

pipnode_sound_effects_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	$(LIBSOUP_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_sound_effects_la_LDFLAGS = -module -avoid-version -shared

pipnode_sound_effects_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS) \
	$(LIBSOUP_LIBS)

pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnSciFiSound.html

EXTRA_DIST = $(pnhelp_DATA)
