# pipnode-desktop — the desktop viewer.
#
# A plain window that shows one worksheet's DESKTOP layout: every widget
# the user placed in the editor's Desktop Layout tab, at the same
# window-relative position, in a window of the size and title that layout
# defines.  It is the desktop twin of the XFCE panel applet and is built
# the same way: a pure D-Bus client of the background engine
# (pipnode-editor as a service), executing nothing itself.
#
# It links GTK / GLib / json-glib (GDBus comes via gio) and NO pipnode
# runtime library, so an engine or node crash can never take the viewer
# down.  The only pipnode code it embeds is the GTK/Cairo-only
# panel-widgets convenience library — the widget faces plus the
# PnWidgetMirror that drives them from the engine's layout JSON — shared
# with the applet and the editor, and statically linked in.
bin_PROGRAMS = pipnode-desktop

pipnode_desktop_SOURCES = \
	pipnode-desktop.c

pipnode_desktop_CPPFLAGS = \
	-I$(top_srcdir)/panel-widgets \
	-I$(top_builddir)

pipnode_desktop_CFLAGS = \
	$(GTK_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS)

pipnode_desktop_LDADD = \
	$(top_builddir)/panel-widgets/libpipnode-panel-widgets.la \
	$(GTK_LIBS) \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS) \
	-lm

# Desktop entry: the viewer takes a worksheet as its argument, so it
# accepts a file from a file manager and falls back to a chooser when
# launched with none.
desktopdir = $(datadir)/applications
dist_desktop_DATA = org.pipas.pipnode.Desktop.desktop
