# Headless split (TODO #23): the library is built in two tiers.
#
#   libpipnode-core.la  — GTK-free.  PnNode (GTK-free vtable), PnColor,
#       the factory, flow/serializer, message, all pure-logic nodes and
#       parsers, and the IO/auto-trigger nodes.  Links only glib/gobject/
#       gio, json-glib, gmodule, libsoup and gdk-pixbuf (the allowed core
#       image-data dep — see HEADLESS-SPLIT-PLAN.md §6 decision 1).  No
#       gtk/gdk widget toolkit, cairo, pango, webkit, gtksourceview or
#       plplot.  A server installs only this.
#
#   libpipnode-gui.la   — links core + the GTK stack.  Worksheet renderer,
#       palette, node/preferences/document dialogs, the help browser, and
#       (until the Phase 4 dual-node split) the dual-nature visual nodes
#       whose cairo drawing + settings dialogs still pull GTK.
#
# The two libraries are mutually referential for now: the core factory
# still hard-registers the GUI-tier node GTypes (pn_*_get_type) and asks
# PnPreferences whether a plugin is disabled.  Those symbols resolve from
# the gui library, which every current binary links alongside core; the
# core-only headless wiring (so pipnode-run needs no GUI symbols) is
# Phase 4/5 work.  Hence core carries undefined references and is *not*
# built with -no-undefined.

lib_LTLIBRARIES = libpipnode-core.la libpipnode-gui.la

# ---------------------------------------------------------------------
# Core tier — compiled with NO GTK on the command line.
# ---------------------------------------------------------------------
libpipnode_core_la_SOURCES = \
	pn-plugin.h \
	pn-ambient.c \
	pn-ambient.h \
	pn-auto-injector.c \
	pn-auto-injector.h \
	pn-auto-random.c \
	pn-auto-random.h \
	pn-auto-trigger.c \
	pn-auto-trigger.h \
	pn-color.c \
	pn-color.h \
	pn-comparator.c \
	pn-comparator.h \
	pn-connections.c \
	pn-connections.h \
	pn-cpu.c \
	pn-cpu.h \
	pn-debug.c \
	pn-debug.h \
	pn-dedup.c \
	pn-dedup.h \
	pn-delay.c \
	pn-delay.h \
	pn-disk-io.c \
	pn-disk-io.h \
	pn-edge.c \
	pn-edge.h \
	pn-expr-parser.c \
	pn-expr-parser.h \
	pn-flow.c \
	pn-flow.h \
	pn-format.c \
	pn-format.h \
	pn-http.c \
	pn-http.h \
	pn-image-message.c \
	pn-image-message.h \
	pn-inject.c \
	pn-inject.h \
	pn-jmespath.c \
	pn-jmespath.h \
	pn-json-path.c \
	pn-json-path.h \
	pn-knob.c \
	pn-knob.h \
	pn-led.c \
	pn-led.h \
	pn-load.c \
	pn-load.h \
	pn-memory.c \
	pn-memory.h \
	pn-message.c \
	pn-message.h \
	pn-net-io.c \
	pn-net-io.h \
	pn-node.c \
	pn-node.h \
	pn-node-factory.c \
	pn-node-factory.h \
	pn-node-store.c \
	pn-node-store.h \
	pn-query.c \
	pn-query.h \
	pn-rtc.c \
	pn-rtc.h \
	pn-staircase.c \
	pn-staircase.h \
	pn-stats.c \
	pn-stats.h \
	pn-subst.c \
	pn-subst.h \
	pn-switch.c \
	pn-switch.h \
	pn-table-model.c \
	pn-table-model.h \
	pn-temp.c \
	pn-temp.h \
	pn-threshold.c \
	pn-threshold.h \
	pn-throttle.c \
	pn-throttle.h \
	pn-var-store.c \
	pn-var-store.h \
	pn-watchdog.c \
	pn-watchdog.h \
	pn-weather.c \
	pn-weather.h \
	pn-ws.c \
	pn-ws.h \
	pn-wire.c \
	pn-wire.h \
	pn-wire-store.c \
	pn-wire-store.h

libpipnode_core_la_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(LIBSOUP_CFLAGS) \
	$(GMODULE_CFLAGS) \
	$(GDK_PIXBUF_CFLAGS) \
	-DPKGDATADIR=\"$(pkgdatadir)\" \
	-DPKGLIBDIR=\"$(pkglibdir)\" \
	-DSRCDIR=\"$(abs_top_srcdir)\" \
	-DABS_TOP_BUILDDIR=\"$(abs_top_builddir)\"

libpipnode_core_la_LIBADD = \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(LIBSOUP_LIBS) \
	$(GMODULE_LIBS) \
	$(GDK_PIXBUF_LIBS) \
	-lm

libpipnode_core_la_LDFLAGS = -version-info 0:0:0

# ---------------------------------------------------------------------
# GUI tier — links core + the full GTK stack.
# ---------------------------------------------------------------------
libpipnode_gui_la_SOURCES = \
	pipnode.h \
	pn-node-dialog-helpers.h \
	pn-analog-meter.c \
	pn-analog-meter.h \
	pn-chat.c \
	pn-chat.h \
	pn-dial.c \
	pn-dial.h \
	pn-document-settings-dialog.c \
	pn-document-settings-dialog.h \
	pn-expression.c \
	pn-expression.h \
	pn-expression2.c \
	pn-expression2.h \
	pn-file-viewer.c \
	pn-file-viewer.h \
	pn-filedrop.c \
	pn-filedrop.h \
	pn-filter.c \
	pn-filter.h \
	pn-graph.c \
	pn-graph.h \
	pn-gui.c \
	pn-gui.h \
	pn-help-browser.c \
	pn-help-browser.h \
	pn-knob-gui.c \
	pn-knob-gui.h \
	pn-led-gui.c \
	pn-led-gui.h \
	pn-meshtastic.c \
	pn-meshtastic.h \
	pn-node-dialog.c \
	pn-node-dialog.h \
	pn-notify.c \
	pn-notify.h \
	pn-ollama.c \
	pn-ollama.h \
	pn-palette.c \
	pn-palette.h \
	pn-preferences.c \
	pn-preferences.h \
	pn-preferences-dialog.c \
	pn-preferences-dialog.h \
	pn-rate.c \
	pn-rate.h \
	pn-rewrite.c \
	pn-rewrite.h \
	pn-set.c \
	pn-set.h \
	pn-sound.c \
	pn-sound.h \
	pn-switch-gui.c \
	pn-switch-gui.h \
	pn-table.c \
	pn-table.h \
	pn-table-view.c \
	pn-table-view.h \
	pn-text-view.c \
	pn-text-view.h \
	pn-tts.c \
	pn-tts.h \
	pn-weather-report.c \
	pn-weather-report.h \
	pn-worksheet.c \
	pn-worksheet.h

libpipnode_gui_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(GTKSOURCEVIEW_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(LIBSOUP_CFLAGS) \
	$(GDK_PIXBUF_CFLAGS) \
	$(PLPLOT_CFLAGS) \
	$(WEBKIT2GTK_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-DPKGDATADIR=\"$(pkgdatadir)\" \
	-DPKGLIBDIR=\"$(pkglibdir)\" \
	-DSRCDIR=\"$(abs_top_srcdir)\" \
	-DABS_TOP_BUILDDIR=\"$(abs_top_builddir)\"

libpipnode_gui_la_LIBADD = \
	libpipnode-core.la \
	$(GTK_LIBS) \
	$(GTKSOURCEVIEW_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(LIBSOUP_LIBS) \
	$(GDK_PIXBUF_LIBS) \
	$(PLPLOT_LIBS) \
	$(WEBKIT2GTK_LIBS) \
	$(GMODULE_LIBS) \
	-lm

libpipnode_gui_la_LDFLAGS = -version-info 0:0:0

# ---------------------------------------------------------------------
# Installed public headers.  Kept in one directory ($(includedir)/pipnode)
# so existing includes resolve unchanged; grouped by tier to document the
# split (a future distro packaging step can ship them as core/gui dev
# sub-packages — Phase 8).
# ---------------------------------------------------------------------
pipnodeincludedir = $(includedir)/pipnode

pipnode_core_headers = \
	pn-plugin.h \
	pn-ambient.h \
	pn-auto-injector.h \
	pn-auto-random.h \
	pn-auto-trigger.h \
	pn-color.h \
	pn-comparator.h \
	pn-connections.h \
	pn-cpu.h \
	pn-debug.h \
	pn-dedup.h \
	pn-delay.h \
	pn-disk-io.h \
	pn-edge.h \
	pn-expr-parser.h \
	pn-flow.h \
	pn-format.h \
	pn-http.h \
	pn-image-message.h \
	pn-inject.h \
	pn-jmespath.h \
	pn-json-path.h \
	pn-knob.h \
	pn-led.h \
	pn-load.h \
	pn-memory.h \
	pn-message.h \
	pn-net-io.h \
	pn-node.h \
	pn-node-factory.h \
	pn-node-store.h \
	pn-query.h \
	pn-rtc.h \
	pn-staircase.h \
	pn-stats.h \
	pn-subst.h \
	pn-switch.h \
	pn-table-model.h \
	pn-temp.h \
	pn-threshold.h \
	pn-throttle.h \
	pn-var-store.h \
	pn-watchdog.h \
	pn-weather.h \
	pn-ws.h \
	pn-wire.h \
	pn-wire-store.h

pipnode_gui_headers = \
	pipnode.h \
	pn-analog-meter.h \
	pn-chat.h \
	pn-dial.h \
	pn-document-settings-dialog.h \
	pn-expression.h \
	pn-expression2.h \
	pn-file-viewer.h \
	pn-filedrop.h \
	pn-filter.h \
	pn-graph.h \
	pn-gui.h \
	pn-help-browser.h \
	pn-knob-gui.h \
	pn-led-gui.h \
	pn-meshtastic.h \
	pn-node-dialog.h \
	pn-node-dialog-helpers.h \
	pn-notify.h \
	pn-ollama.h \
	pn-palette.h \
	pn-preferences.h \
	pn-preferences-dialog.h \
	pn-rate.h \
	pn-rewrite.h \
	pn-set.h \
	pn-sound.h \
	pn-switch-gui.h \
	pn-table.h \
	pn-table-view.h \
	pn-text-view.h \
	pn-tts.h \
	pn-weather-report.h \
	pn-worksheet.h

pipnodeinclude_HEADERS = $(pipnode_core_headers) $(pipnode_gui_headers)

# Refresh the dynamic linker cache after the .so lands in $(libdir),
# so a freshly-installed `pipnode` / `pipnode-run` does not error out
# with "libpipnode.so.0: cannot open shared object file" the first
# time the user runs it.  Skipped when $(DESTDIR) is non-empty (the
# install is going into a packaging staging tree, not the live root,
# and the host's ldconfig would either fail or — worse — pollute the
# real cache with paths from the buildroot).  The trailing `|| true`
# keeps a non-root `make install --prefix=$HOME/.local` from failing
# the whole install just because ldconfig refused.
install-data-hook:
	@if test -z "$(DESTDIR)" && command -v ldconfig >/dev/null 2>&1; then \
		echo "  LDCONFIG"; \
		ldconfig || true; \
	fi
