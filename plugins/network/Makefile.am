# Bundled network plugin.  Installed alongside the host binaries so
# the default plugin search path ($pkglibdir/plugins) picks it up
# without any post-install configuration on the user's part.
#
# Built as a libtool module (-module + -avoid-version) so the
# resulting file lands as a single shared object whose suffix
# matches the platform's GModule convention (`pipnode_network.so`
# on Linux, `pipnode_network.dylib` on macOS, `pipnode_network.dll`
# on Windows).
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_network.la

pipnode_network_la_SOURCES = \
	pn-network-plugin.c \
	pn-dns.c \
	pn-dns.h \
	pn-https-tunnel-receiver.c \
	pn-https-tunnel-receiver.h \
	pn-https-tunnel-sender.c \
	pn-https-tunnel-sender.h \
	pn-mqtt.c \
	pn-mqtt.h \
	pn-mqtt-sink.c \
	pn-mqtt-sink.h \
	pn-ping.c \
	pn-ping.h

pipnode_network_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	$(LIBSOUP_CFLAGS) \
	$(GNUTLS_CFLAGS) \
	$(MOSQUITTO_CFLAGS) \
	-I$(top_srcdir)/lib

# -module      : produce a loadable module (no `lib` prefix);
# -avoid-version : drop the .0.0.0 SONAME version suffix so the
#                  installed file is a plain `pipnode_network.so`
#                  that g_module_open finds without any sysv-style
#                  symlink chasing.
# -shared      : force the shared form even if libtool would
#                otherwise also emit a static archive.
pipnode_network_la_LDFLAGS = -module -avoid-version -shared

pipnode_network_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS) \
	$(LIBSOUP_LIBS) \
	$(GNUTLS_LIBS) \
	$(MOSQUITTO_LIBS)

# Per-node help pages.  Installed into the same flat
# $(pkgdatadir)/help/ directory the host's data/Makefile.am ships its
# own files into, so the shared help.css and the back-link to
# help-index.html resolve with a sibling href and the host's
# resolve_help_path() finds these files alongside its own through a
# single directory probe at $PKGDATADIR/help/.
pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnPing.html \
	help/PnDns.html \
	help/PnHttpsTunnelReceiver.html \
	help/PnHttpsTunnelSender.html \
	help/PnMqtt.html \
	help/PnMqttSink.html

EXTRA_DIST = $(pnhelp_DATA)
