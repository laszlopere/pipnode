# XFCE-panel applet for pipnode.  Renders an associated pipnode worksheet's
# panel-band widgets (PnLedDisplay, PnLedLamp, switches, …) in the XFCE
# panel and talks to the pipnode engine (pipnode-editor as a D-Bus service)
# to keep them live.  Built as an "internal" XFCE panel module, i.e. a
# loadable lib<name>.so that the panel dlopen()s on demand.
#
# It is a pure D-Bus client: it links GTK / GLib / libxfce4panel only
# (GDBus comes via GTK's gio-2.0 dependency) and NO pipnode runtime
# library, so an engine/node crash can never take down xfce4-panel.  The
# only pipnode code it embeds is the GTK/Cairo-only panel-widgets
# convenience library (PnLedDisplay and friends, plus the PnWidgetMirror
# that drives them from the engine's layout JSON), statically linked in;
# it shares those with the editor's layout editors and with the standalone
# desktop viewer, but pulls in none of the node runtime, so the isolation
# above is preserved.
#
# Install into the running panel's own module/descriptor dirs (derived
# from libxfce4panel-2.0.pc in configure.ac), NOT pipnode's --prefix: the
# panel only scans its compiled-in directories.
plugindir = $(LIBXFCE4PANEL_PLUGINDIR)

plugin_LTLIBRARIES = libpipnode-xfce-applet.la

libpipnode_xfce_applet_la_SOURCES = \
	pipnode-xfce-applet-plugin.c

libpipnode_xfce_applet_la_CFLAGS = \
	-I$(top_srcdir)/panel-widgets \
	$(LIBXFCE4PANEL_CFLAGS) \
	$(GTK_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	-DPKGDATADIR=\"$(pkgdatadir)\"

# -module        : loadable module
# -avoid-version : ship plain libpipnode-xfce-applet.so (no SONAME version)
# -no-undefined  : all symbols must resolve at link time
# -export-symbols-regex : the panel only needs the macro-generated entry
#                  points; hide everything else.
libpipnode_xfce_applet_la_LDFLAGS = \
	-avoid-version -module -no-undefined \
	-export-symbols-regex '^xfce_panel_module_(preinit|init|construct)'

libpipnode_xfce_applet_la_LIBADD = \
	$(top_builddir)/panel-widgets/libpipnode-panel-widgets.la \
	$(LIBXFCE4PANEL_LIBS) \
	$(GTK_LIBS) \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS)

# Panel descriptor: tells xfce4-panel that this module exists.  The panel
# turns X-XFCE-Module=pipnode-xfce-applet into "libpipnode-xfce-applet.so".
desktopdir = $(LIBXFCE4PANEL_DESKTOPDIR)
desktop_DATA = pipnode-xfce-applet.desktop

# Starter worksheet: a copy is instantiated for each new applet so the user
# begins with a working flow instead of a blank sheet.  Installed into
# pipnode's own datadir (an absolute PKGDATADIR path the applet reads),
# not the panel's prefix.  dist_ ships it in the tarball too.
dist_pkgdata_DATA = pipnode-xfce-applet-default.json

EXTRA_DIST = $(desktop_DATA)
