# Help pages.  Installed into $(pkgdatadir)/help/ so the host's
# resolver in src/pn-window.c::resolve_help_path() finds them in a
# single flat directory alongside the per-node files shipped by the
# bundled plugins (plugins/network/help/) —
# every <ClassName>.html links to the shared help.css with a sibling
# href and the per-node files cross-link back to help-index.html the
# same way.
pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/help.css \
	help/help-index.html \
	help/PnAnalogMeter.html \
	help/PnAutoInjector.html \
	help/PnAutoRandom.html \
	help/PnChat.html \
	help/PnComparator.html \
	help/PnCountdown.html \
	help/PnDeadline.html \
	help/PnDebug.html \
	help/PnDedup.html \
	help/PnDelay.html \
	help/PnDial.html \
	help/PnDigitalClock.html \
	help/PnEdge.html \
	help/PnExpression.html \
	help/PnExpression2.html \
	help/PnFailure.html \
	help/PnFileDrop.html \
	help/PnFileViewer.html \
	help/PnFilter.html \
	help/PnFormat.html \
	help/PnGraph.html \
	help/PnHttp.html \
	help/PnInject.html \
	help/PnKnob.html \
	help/PnLabel.html \
	help/PnLed.html \
	help/PnMeshtastic.html \
	help/PnNotify.html \
	help/PnPanelDisplay.html \
	help/PnPanelInput.html \
	help/PnQuery.html \
	help/PnRate.html \
	help/PnRewrite.html \
	help/PnRtc.html \
	help/PnSet.html \
	help/PnSound.html \
	help/PnStaircase.html \
	help/PnStats.html \
	help/PnSuccess.html \
	help/PnSwitch.html \
	help/PnTable.html \
	help/PnTableModel.html \
	help/PnTableView.html \
	help/PnTextView.html \
	help/PnThreshold.html \
	help/PnThrottle.html \
	help/PnTts.html \
	help/PnWatchdog.html \
	help/PnWeather.html \
	help/PnWeatherReport.html

# Crypto-currency icons used by the Rate node's settings dialog.
# Installed under $(pkgdatadir)/icons/ so the dialog code can resolve
# them at run-time the same way it resolves help-index.html.
pniconsdir = $(pkgdatadir)/icons
pnicons_DATA = \
	icons/ada.png \
	icons/atom.png \
	icons/avax.png \
	icons/bch.png \
	icons/bnb.png \
	icons/btc.png \
	icons/cro.png \
	icons/doge.png \
	icons/dot.png \
	icons/eth.png \
	icons/link.png \
	icons/ltc.png \
	icons/matic.png \
	icons/pls.png \
	icons/sol.png \
	icons/trx.png \
	icons/uni.png \
	icons/usd.png \
	icons/usdc.png \
	icons/usdt.png \
	icons/xlm.png \
	icons/xmr.png \
	icons/xrp.png

# Desktop integration: launcher entry, AppStream metainfo and the
# application icon installed into the standard freedesktop locations so
# both native (make install) and Flatpak builds pick them up.
desktopdir = $(datadir)/applications
desktop_DATA = org.pipas.pipnode.desktop

metainfodir = $(datadir)/metainfo
metainfo_DATA = org.pipas.pipnode.metainfo.xml

appicon256dir = $(datadir)/icons/hicolor/256x256/apps
appicon256_DATA = icons/hicolor/256x256/apps/org.pipas.pipnode.png

appicon128dir = $(datadir)/icons/hicolor/128x128/apps
appicon128_DATA = icons/hicolor/128x128/apps/org.pipas.pipnode.png

# D-Bus activation for the background engine: the first method call from
# an XFCE panel applet auto-starts "pipnode-editor --gapplication-service".
# Generated from the .in so the Exec carries the install-time absolute
# path ($(bindir) is fully expanded by make for the default prefix).
servicedir = $(datadir)/dbus-1/services
service_DATA = org.pipas.pipnode.service

org.pipas.pipnode.service: org.pipas.pipnode.service.in Makefile
	$(AM_V_GEN)sed -e 's|@bindir[@]|$(bindir)|g' $< > $@

CLEANFILES = org.pipas.pipnode.service

EXTRA_DIST = $(pnhelp_DATA) $(pnicons_DATA) \
	$(desktop_DATA) $(metainfo_DATA) \
	$(appicon256_DATA) $(appicon128_DATA) \
	org.pipas.pipnode.service.in

# Refresh the hicolor icon cache after (un)installing the app icon, so the
# themed name "org.pipas.pipnode" resolves immediately — otherwise a stale
# cache shadows the freshly installed PNGs and consumers (e.g. the XFCE
# panel applet) get the broken-image placeholder.  Skipped under DESTDIR
# (the packager runs it); a no-op if gtk-update-icon-cache is unavailable.
gtk_update_icon_cache = $(GTK_UPDATE_ICON_CACHE) -q -t -f

install-data-hook: update-icon-cache
uninstall-hook: update-icon-cache

update-icon-cache:
	@if test -n "$(DESTDIR)"; then \
		echo "*** Icon cache not updated (DESTDIR set); after install run:"; \
		echo "***   $(gtk_update_icon_cache) $(datadir)/icons/hicolor"; \
	else \
		echo "Updating hicolor icon cache."; \
		$(gtk_update_icon_cache) "$(datadir)/icons/hicolor" || :; \
	fi
.PHONY: update-icon-cache
