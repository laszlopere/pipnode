# Bundled Ollama plugin.  Installed alongside the host binaries so the
# default plugin search path ($pkglibdir/plugins) picks it up without any
# post-install configuration.
#
# Ships the PnOllama filter node: it submits a message's data.output to a
# local or remote Ollama server's /api/generate endpoint and emits the
# model's reply.
#
# Two-tier plugin (TODO #23, Phase 8): the logic module pipnode_ollama.so
# is GTK-free and links libpipnode-core plus libsoup/json-glib for the
# HTTP call, so a headless server installs it and runs the node under
# pipnode-run with no GTK.  The settings dialog's model picker — a
# /api/tags combo with a Refresh button, which the declarative settings
# schema cannot express — ships in the companion module
# pipnode_ollama-gui.so, which the editor loads next to the logic .so (via
# pn_plugin_gui_init) and a server never installs.  The core plugin loader
# skips *-gui.so during a logic-plugin scan.  The /api/tags enumeration is
# only used by that dialog, so it lives wholly in the companion (the logic
# .so is opened BIND_LOCAL, so its symbols are invisible to the companion,
# which resolves the node's #GType by name).
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_ollama.la pipnode_ollama-gui.la

# -------------------------------------------------------------------
# Logic half — GTK-free, core only (+ libsoup/json-glib for the HTTP
# call).  Loaded by pipnode-run and the editor; exports pn_plugin_init
# and registers the PnOllama #GType.
# -------------------------------------------------------------------
pipnode_ollama_la_SOURCES = \
	pn-ollama-plugin.c \
	pn-ollama.c \
	pn-ollama.h

pipnode_ollama_la_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	$(LIBSOUP_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_ollama_la_LDFLAGS = -module -avoid-version -shared

pipnode_ollama_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS) \
	$(LIBSOUP_LIBS)

# -------------------------------------------------------------------
# Companion GUI module — links the GTK tier + libsoup/json-glib.
# Automake canonicalises the `pipnode_ollama-gui.la` target to the
# `pipnode_ollama_gui_la_` prefix.  Loaded by the editor only; resolves
# PnOllama by name (the logic .so is BIND_LOCAL) and installs its
# settings-dialog vfunc slot.
# -------------------------------------------------------------------
pipnode_ollama_gui_la_SOURCES = \
	pn-ollama-gui.c

pipnode_ollama_gui_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	$(LIBSOUP_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_ollama_gui_la_LDFLAGS = -module -avoid-version -shared

pipnode_ollama_gui_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS) \
	$(LIBSOUP_LIBS)

pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnOllama.html

EXTRA_DIST = $(pnhelp_DATA)
