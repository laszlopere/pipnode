# Headless split (TODO #23): the library is built in two tiers.
#
#   libpipnode-core.la  — GTK-free.  PnNode (GTK-free vtable), PnColor,
#       the factory, flow/serializer, message, all pure-logic nodes and
#       parsers, and the IO/auto-trigger nodes.  Links only glib/gobject/
#       gio, json-glib, gmodule, libsoup and gdk-pixbuf (the allowed core
#       image-data dep: PnImageMessage carries a GdkPixbuf, which has no
#       GTK widget footprint).  No gtk/gdk widget toolkit, cairo, pango,
#       webkit, gtksourceview or plplot.  A server installs only this.
#
#   libpipnode-gui.la   — links core + the GTK stack.  Worksheet renderer,
#       palette, node/preferences/document dialogs, the help browser, and
#       (until the Phase 4 dual-node split) the dual-nature visual nodes
#       whose cairo drawing + settings dialogs still pull GTK.
#
# Core is fully self-contained (TODO #23, Phase 5): it no longer hard-
# references any gui symbol — the GUI-tier node GTypes are split out
# (logic in core, painters/dialogs installed by the gui tier) and the
# plugin-disabled policy is a core-side hook the editor fills in.  So core
# links with -no-undefined and pipnode-run / the unit tests link it alone,
# never pulling the GTK stack.  The gui library still LIBADDs core.

lib_LTLIBRARIES = libpipnode-core.la libpipnode-gui.la

# ---------------------------------------------------------------------
# Core tier — compiled with NO GTK on the command line.
# ---------------------------------------------------------------------
libpipnode_core_la_SOURCES = \
	pn-plugin.h \
	pn-analog-meter.c \
	pn-analog-meter.h \
	pn-auto-injector.c \
	pn-auto-injector.h \
	pn-auto-random.c \
	pn-auto-random.h \
	pn-auto-trigger.c \
	pn-auto-trigger.h \
	pn-chat.c \
	pn-chat.h \
	pn-color.c \
	pn-color.h \
	pn-comparator.c \
	pn-comparator.h \
	pn-countdown.c \
	pn-countdown.h \
	pn-deadline.c \
	pn-deadline.h \
	pn-debug.c \
	pn-debug.h \
	pn-dedup.c \
	pn-dedup.h \
	pn-delay.c \
	pn-delay.h \
	pn-dial.c \
	pn-dial.h \
	pn-digital-clock.c \
	pn-digital-clock.h \
	pn-edge.c \
	pn-edge.h \
	pn-expr-parser.c \
	pn-expr-parser.h \
	pn-expression.c \
	pn-expression.h \
	pn-expression2.c \
	pn-expression2.h \
	pn-failure.c \
	pn-failure.h \
	pn-file-viewer.c \
	pn-file-viewer.h \
	pn-filedrop.c \
	pn-filedrop.h \
	pn-filter.c \
	pn-filter.h \
	pn-flow.c \
	pn-flow.h \
	pn-format.c \
	pn-format.h \
	pn-graph.c \
	pn-graph.h \
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
	pn-label.c \
	pn-label.h \
	pn-led.c \
	pn-led.h \
	pn-meshtastic.c \
	pn-meshtastic.h \
	pn-message.c \
	pn-message.h \
	pn-node.c \
	pn-node.h \
	pn-node-factory.c \
	pn-node-factory.h \
	pn-node-store.c \
	pn-node-store.h \
	pn-notify.c \
	pn-notify.h \
	pn-panel-display.c \
	pn-panel-display.h \
	pn-panel-input.c \
	pn-panel-input.h \
	pn-profile-schema.c \
	pn-profile-schema.h \
	pn-query.c \
	pn-query.h \
	pn-rate.c \
	pn-rate.h \
	pn-rewrite.c \
	pn-rewrite.h \
	pn-rtc.c \
	pn-rtc.h \
	pn-set.c \
	pn-set.h \
	pn-settings-schema.c \
	pn-settings-schema.h \
	pn-sound.c \
	pn-sound.h \
	pn-staircase.c \
	pn-staircase.h \
	pn-stats.c \
	pn-stats.h \
	pn-subst.c \
	pn-subst.h \
	pn-success.c \
	pn-success.h \
	pn-switch.c \
	pn-switch.h \
	pn-table-model.c \
	pn-table-model.h \
	pn-table.c \
	pn-table.h \
	pn-table-view.c \
	pn-table-view.h \
	pn-text-view.c \
	pn-text-view.h \
	pn-threshold.c \
	pn-threshold.h \
	pn-throttle.c \
	pn-throttle.h \
	pn-tts.c \
	pn-tts.h \
	pn-var-store.c \
	pn-var-store.h \
	pn-vault.c \
	pn-vault.h \
	pn-watchdog.c \
	pn-watchdog.h \
	pn-weather.c \
	pn-weather.h \
	pn-weather-report.c \
	pn-weather-report.h \
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

libpipnode_core_la_LDFLAGS = -version-info 0:0:0 -no-undefined

# ---------------------------------------------------------------------
# GUI tier — links core + the full GTK stack.
# ---------------------------------------------------------------------
libpipnode_gui_la_SOURCES = \
	pipnode.h \
	pn-node-dialog-helpers.h \
	pn-analog-meter-gui.c \
	pn-analog-meter-gui.h \
	pn-chat-gui.c \
	pn-chat-gui.h \
	pn-countdown-gui.c \
	pn-countdown-gui.h \
	pn-dial-gui.c \
	pn-dial-gui.h \
	pn-credentials-dialog.c \
	pn-credentials-dialog.h \
	pn-digital-clock-gui.c \
	pn-digital-clock-gui.h \
	pn-document-settings-dialog.c \
	pn-document-settings-dialog.h \
	pn-file-viewer-gui.c \
	pn-file-viewer-gui.h \
	pn-filedrop-gui.c \
	pn-filedrop-gui.h \
	pn-filter-gui.c \
	pn-filter-gui.h \
	pn-graph-gui.c \
	pn-graph-gui.h \
	pn-gui.c \
	pn-gui.h \
	pn-help-browser.c \
	pn-help-browser.h \
	pn-knob-gui.c \
	pn-knob-gui.h \
	pn-label-gui.c \
	pn-label-gui.h \
	pn-led-gui.c \
	pn-led-gui.h \
	pn-meshtastic-gui.c \
	pn-meshtastic-gui.h \
	pn-node-dialog.c \
	pn-node-dialog.h \
	pn-notify-gui.c \
	pn-notify-gui.h \
	pn-palette.c \
	pn-palette.h \
	pn-panel-editor.c \
	pn-panel-editor.h \
	pn-panel-geometry.h \
	pn-preferences.c \
	pn-preferences.h \
	pn-preferences-dialog.c \
	pn-preferences-dialog.h \
	pn-rate-gui.c \
	pn-rate-gui.h \
	pn-set-gui.c \
	pn-set-gui.h \
	pn-settings-renderer.c \
	pn-settings-renderer.h \
	pn-sound-gui.c \
	pn-sound-gui.h \
	pn-switch-gui.c \
	pn-switch-gui.h \
	pn-table-gui.c \
	pn-table-gui.h \
	pn-table-view-gui.c \
	pn-table-view-gui.h \
	pn-text-view-gui.c \
	pn-text-view-gui.h \
	pn-tts-gui.c \
	pn-tts-gui.h \
	pn-weather-report-gui.c \
	pn-weather-report-gui.h \
	pn-worksheet.c \
	pn-worksheet.h

libpipnode_gui_la_CFLAGS = \
	-I$(top_srcdir)/panel-widgets \
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
	$(top_builddir)/panel-widgets/libpipnode-panel-widgets.la \
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
	pn-analog-meter.h \
	pn-auto-injector.h \
	pn-auto-random.h \
	pn-auto-trigger.h \
	pn-chat.h \
	pn-color.h \
	pn-comparator.h \
	pn-countdown.h \
	pn-debug.h \
	pn-dedup.h \
	pn-delay.h \
	pn-dial.h \
	pn-digital-clock.h \
	pn-edge.h \
	pn-expr-parser.h \
	pn-expression.h \
	pn-expression2.h \
	pn-failure.h \
	pn-file-viewer.h \
	pn-filedrop.h \
	pn-filter.h \
	pn-flow.h \
	pn-format.h \
	pn-graph.h \
	pn-http.h \
	pn-image-message.h \
	pn-inject.h \
	pn-jmespath.h \
	pn-json-path.h \
	pn-knob.h \
	pn-label.h \
	pn-led.h \
	pn-meshtastic.h \
	pn-message.h \
	pn-node.h \
	pn-node-factory.h \
	pn-node-store.h \
	pn-notify.h \
	pn-panel-display.h \
	pn-panel-input.h \
	pn-profile-schema.h \
	pn-query.h \
	pn-rate.h \
	pn-rewrite.h \
	pn-rtc.h \
	pn-set.h \
	pn-settings-schema.h \
	pn-sound.h \
	pn-staircase.h \
	pn-stats.h \
	pn-subst.h \
	pn-success.h \
	pn-switch.h \
	pn-table-model.h \
	pn-table.h \
	pn-table-view.h \
	pn-text-view.h \
	pn-threshold.h \
	pn-throttle.h \
	pn-tts.h \
	pn-var-store.h \
	pn-vault.h \
	pn-watchdog.h \
	pn-weather.h \
	pn-weather-report.h \
	pn-ws.h \
	pn-wire.h \
	pn-wire-store.h

pipnode_gui_headers = \
	pipnode.h \
	pn-analog-meter-gui.h \
	pn-chat-gui.h \
	pn-countdown-gui.h \
	pn-credentials-dialog.h \
	pn-dial-gui.h \
	pn-digital-clock-gui.h \
	pn-document-settings-dialog.h \
	pn-file-viewer-gui.h \
	pn-filedrop-gui.h \
	pn-filter-gui.h \
	pn-graph-gui.h \
	pn-gui.h \
	pn-help-browser.h \
	pn-knob-gui.h \
	pn-label-gui.h \
	pn-led-gui.h \
	pn-meshtastic-gui.h \
	pn-node-dialog.h \
	pn-node-dialog-helpers.h \
	pn-notify-gui.h \
	pn-palette.h \
	pn-panel-editor.h \
	pn-panel-geometry.h \
	pn-preferences.h \
	pn-preferences-dialog.h \
	pn-rate-gui.h \
	pn-set-gui.h \
	pn-settings-renderer.h \
	pn-sound-gui.h \
	pn-switch-gui.h \
	pn-table-gui.h \
	pn-table-view-gui.h \
	pn-text-view-gui.h \
	pn-tts-gui.h \
	pn-weather-report-gui.h \
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
