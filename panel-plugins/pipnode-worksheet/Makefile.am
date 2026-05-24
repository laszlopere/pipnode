# Test applet: a panel button that opens an associated pipnode worksheet
# in pipnode-editor.  Built as an "internal" XFCE panel module, i.e. a
# loadable lib<name>.so that the panel dlopen()s on demand.
#
# Install into the running panel's own module/descriptor dirs (derived
# from libxfce4panel-2.0.pc in configure.ac), NOT pipnode's --prefix: the
# panel only scans its compiled-in directories.
plugindir = $(LIBXFCE4PANEL_PLUGINDIR)

plugin_LTLIBRARIES = libpipnode-worksheet.la

libpipnode_worksheet_la_SOURCES = \
	pipnode-worksheet-plugin.c

libpipnode_worksheet_la_CFLAGS = \
	$(LIBXFCE4PANEL_CFLAGS) \
	$(GTK_CFLAGS) \
	$(GLIB_CFLAGS) \
	-DPIPNODE_EDITOR_PATH=\"$(bindir)/pipnode-editor\"

# -module        : loadable module
# -avoid-version : ship plain libpipnode-worksheet.so (no SONAME version)
# -no-undefined  : all symbols must resolve at link time
# -export-symbols-regex : the panel only needs the macro-generated entry
#                  points; hide everything else.
libpipnode_worksheet_la_LDFLAGS = \
	-avoid-version -module -no-undefined \
	-export-symbols-regex '^xfce_panel_module_(preinit|init|construct)'

libpipnode_worksheet_la_LIBADD = \
	$(LIBXFCE4PANEL_LIBS) \
	$(GTK_LIBS) \
	$(GLIB_LIBS)

# Panel descriptor: tells xfce4-panel that this module exists.  The panel
# turns X-XFCE-Module=pipnode-worksheet into "libpipnode-worksheet.so".
desktopdir = $(LIBXFCE4PANEL_DESKTOPDIR)
desktop_DATA = pipnode-worksheet.desktop

EXTRA_DIST = $(desktop_DATA)
