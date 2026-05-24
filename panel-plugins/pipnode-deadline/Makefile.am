# Panel applet that counts down to a deadline by running an associated
# pipnode worksheet via the background engine (pipnode-editor as a D-Bus
# service) and showing its value on the panel.  Built as an "internal"
# XFCE panel module, i.e. a loadable lib<name>.so that the panel
# dlopen()s on demand.
#
# It is a pure D-Bus client: it links GTK / GLib / libxfce4panel only
# (GDBus comes via GTK's gio-2.0 dependency) and NO pipnode library, so
# an engine/node crash can never take down xfce4-panel.
#
# Install into the running panel's own module/descriptor dirs (derived
# from libxfce4panel-2.0.pc in configure.ac), NOT pipnode's --prefix: the
# panel only scans its compiled-in directories.
plugindir = $(LIBXFCE4PANEL_PLUGINDIR)

plugin_LTLIBRARIES = libpipnode-deadline.la

libpipnode_deadline_la_SOURCES = \
	pipnode-deadline-plugin.c \
	pn-led-display.c \
	pn-led-display.h

libpipnode_deadline_la_CFLAGS = \
	$(LIBXFCE4PANEL_CFLAGS) \
	$(GTK_CFLAGS) \
	$(GLIB_CFLAGS) \
	-DPKGDATADIR=\"$(pkgdatadir)\"

# -module        : loadable module
# -avoid-version : ship plain libpipnode-deadline.so (no SONAME version)
# -no-undefined  : all symbols must resolve at link time
# -export-symbols-regex : the panel only needs the macro-generated entry
#                  points; hide everything else.
libpipnode_deadline_la_LDFLAGS = \
	-avoid-version -module -no-undefined \
	-export-symbols-regex '^xfce_panel_module_(preinit|init|construct)'

libpipnode_deadline_la_LIBADD = \
	$(LIBXFCE4PANEL_LIBS) \
	$(GTK_LIBS) \
	$(GLIB_LIBS)

# Panel descriptor: tells xfce4-panel that this module exists.  The panel
# turns X-XFCE-Module=pipnode-deadline into "libpipnode-deadline.so".
desktopdir = $(LIBXFCE4PANEL_DESKTOPDIR)
desktop_DATA = pipnode-deadline.desktop

# Starter worksheet: a copy is instantiated for each new applet so the user
# begins with a working deadline flow instead of a blank sheet.  Installed
# into pipnode's own datadir (an absolute PKGDATADIR path the applet reads),
# not the panel's prefix.  dist_ ships it in the tarball too.
dist_pkgdata_DATA = pipnode-deadline-default.json

EXTRA_DIST = $(desktop_DATA)
