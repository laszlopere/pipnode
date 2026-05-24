# Shared panel-applet GUI widgets.
#
# These are the self-contained Cairo widgets that render a panel applet's
# face (currently the seven-segment deadline readout, PnLedDisplay).  They
# are used in two processes that never meet:
#
#   * the panel applet .so loaded by xfce4-panel, and
#   * the pipnode editor's panel-layout editor (PnPanelEditor).
#
# They deliberately depend on GTK / GLib / Cairo ONLY — never on a pipnode
# library — so the applet can embed them without dragging the node runtime
# into xfce4-panel's address space, where an engine/node crash could take
# the panel down.  Keep that invariant: nothing here may #include or link
# pn-node.h, libpipnode-core, or libpipnode-gui.
#
# Built as a noinst libtool convenience library so each consumer embeds a
# private copy at link time; there is no installed .so and therefore no
# runtime coupling.  -fPIC keeps the objects usable inside the editor's
# shared libpipnode-gui.
noinst_LTLIBRARIES = libpipnode-panel-widgets.la

libpipnode_panel_widgets_la_SOURCES = \
	pn-led-display.c \
	pn-led-display.h

libpipnode_panel_widgets_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(GLIB_CFLAGS) \
	-fPIC

libpipnode_panel_widgets_la_LIBADD = \
	$(GTK_LIBS) \
	$(GLIB_LIBS)
