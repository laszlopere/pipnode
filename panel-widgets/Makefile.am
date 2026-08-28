# Shared GUI widgets for the surfaces a worksheet drives.
#
# These are the self-contained Cairo widgets that render a worksheet's GUI
# widgets (the seven-segment readouts, lamps, toggles and labels), plus
# PnWidgetMirror, the client half of the engine's layout protocol that
# drives a set of them from JSON.  They are used in processes that never
# meet:
#
#   * the panel applet .so loaded by xfce4-panel,
#   * the standalone desktop viewer (viewers/pipnode-desktop), and
#   * the pipnode editor's GUI layout editors (PnLayoutEditor).
#
# They deliberately depend on GTK / GLib / Cairo / json-glib ONLY — never
# on a pipnode library — so a consumer can embed them without dragging the
# node runtime into its address space, where an engine/node crash could
# take xfce4-panel (or the viewer) down with it.  Keep that invariant:
# nothing here may #include or link pn-node.h, libpipnode-core, or
# libpipnode-gui.
#
# Built as a noinst libtool convenience library so each consumer embeds a
# private copy at link time; there is no installed .so and therefore no
# runtime coupling.  -fPIC keeps the objects usable inside the editor's
# shared libpipnode-gui.
noinst_LTLIBRARIES = libpipnode-panel-widgets.la

libpipnode_panel_widgets_la_SOURCES = \
	pn-injector-widget.c \
	pn-injector-widget.h \
	pn-led-display.c \
	pn-led-display.h \
	pn-led-lamp.c \
	pn-led-lamp.h \
	pn-matrix57-display.c \
	pn-matrix57-display.h \
	pn-numeric-display.c \
	pn-numeric-display.h \
	pn-switch-widget.c \
	pn-switch-widget.h \
	pn-text-display.c \
	pn-text-display.h \
	pn-widget-mirror.c \
	pn-widget-mirror.h

libpipnode_panel_widgets_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	-fPIC

libpipnode_panel_widgets_la_LIBADD = \
	$(GTK_LIBS) \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS)
