# Bundled host-monitoring plugin.  Installed alongside the host binaries
# so the default plugin search path ($pkglibdir/plugins) picks it up
# without any post-install configuration on the user's part.
#
# Built as a libtool module (-module + -avoid-version) so the resulting
# file lands as a single shared object whose suffix matches the
# platform's GModule convention (`pipnode_host_monitoring.so` on Linux,
# `.dylib` on macOS, `.dll` on Windows).
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_host_monitoring.la

pipnode_host_monitoring_la_SOURCES = \
	pn-host-monitoring-plugin.c \
	pn-ambient.c \
	pn-ambient.h \
	pn-connections.c \
	pn-connections.h \
	pn-cpu.c \
	pn-cpu.h \
	pn-disk-io.c \
	pn-disk-io.h \
	pn-load.c \
	pn-load.h \
	pn-memory.c \
	pn-memory.h \
	pn-net-io.c \
	pn-net-io.h \
	pn-temp.c \
	pn-temp.h

# Core-only plugin (TODO #23, Phase 8): every node here is pure logic
# (it samples /proc, /sys, or a remote sensor), so the plugin compiles
# with no GTK on the command line and links libpipnode-core ALONE.  A
# headless server installs it and runs it under pipnode-run with no GTK
# present.  The node body colour uses PnColor (the de-GTK'd core type),
# not GdkRGBA.
pipnode_host_monitoring_la_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

# -module        : produce a loadable module (no `lib` prefix);
# -avoid-version : drop the .0.0.0 SONAME version suffix so the installed
#                  file is a plain `pipnode_host_monitoring.so` that
#                  g_module_open finds without any sysv-style symlink
#                  chasing.
# -shared        : force the shared form even if libtool would otherwise
#                  also emit a static archive.
pipnode_host_monitoring_la_LDFLAGS = -module -avoid-version -shared

pipnode_host_monitoring_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)

# Per-node help pages.  Installed into the same flat $(pkgdatadir)/help/
# directory the host's data/Makefile.am ships its own files into, so the
# shared help.css and the back-link to help-index.html resolve with a
# sibling href and the host's resolve_help_path() finds these files
# alongside its own through a single directory probe at $PKGDATADIR/help/.
pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnLoad.html \
	help/PnConnections.html \
	help/PnDiskIo.html \
	help/PnNetIo.html \
	help/PnCpu.html \
	help/PnTemp.html \
	help/PnAmbient.html \
	help/PnMemory.html

EXTRA_DIST = $(pnhelp_DATA)
