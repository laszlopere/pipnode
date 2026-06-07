# Bundled tasmota plugin.  Same install layout as plugins/network.
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_tasmota.la

pipnode_tasmota_la_SOURCES = \
	pn-tasmota-plugin.c \
	pn-tasmota-common.c \
	pn-tasmota-common.h \
	pn-tasmota-energy-meter.c \
	pn-tasmota-energy-meter.h \
	pn-tasmota-apparent-power.c \
	pn-tasmota-apparent-power.h \
	pn-tasmota-current.c \
	pn-tasmota-current.h \
	pn-tasmota-humidity.c \
	pn-tasmota-humidity.h \
	pn-tasmota-power.c \
	pn-tasmota-power.h \
	pn-tasmota-power-factor.c \
	pn-tasmota-power-factor.h \
	pn-tasmota-reactive-power.c \
	pn-tasmota-reactive-power.h \
	pn-tasmota-status.c \
	pn-tasmota-status.h \
	pn-tasmota-status-request.c \
	pn-tasmota-status-request.h \
	pn-tasmota-probe.c \
	pn-tasmota-probe.h \
	pn-tasmota-concentrator.c \
	pn-tasmota-concentrator.h \
	pn-tasmota-relay-status.c \
	pn-tasmota-relay-status.h \
	pn-tasmota-relay-command.c \
	pn-tasmota-relay-command.h \
	pn-tasmota-switch.c \
	pn-tasmota-switch.h \
	pn-tasmota-temperature.c \
	pn-tasmota-temperature.h \
	pn-tasmota-voltage.c \
	pn-tasmota-voltage.h

# Core-only plugin (TODO #23, Phase 8): every node here is pure logic.
# The sensor nodes subclass the *core* logic half of #PnAnalogMeter and
# the relay/switch nodes subclass #PnSwitch (both split in Phase 4), so
# the painter/dialog vfunc slots are installed by the gui tier onto the
# base classes at editor startup and inherited by these subclasses — the
# plugin itself carries no GTK and links libpipnode-core ALONE, loading
# under pipnode-run with no GTK present.
pipnode_tasmota_la_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_tasmota_la_LDFLAGS = -module -avoid-version -shared

pipnode_tasmota_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)

pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnTasmotaTemperature.html \
	help/PnTasmotaHumidity.html \
	help/PnTasmotaStatus.html \
	help/PnTasmotaStatusRequest.html \
	help/PnTasmotaProbe.html \
	help/PnTasmotaConcentrator.html \
	help/PnTasmotaRelayStatus.html \
	help/PnTasmotaRelayCommand.html \
	help/PnTasmotaSwitch.html \
	help/PnTasmotaVoltage.html \
	help/PnTasmotaCurrent.html \
	help/PnTasmotaPower.html \
	help/PnTasmotaApparentPower.html \
	help/PnTasmotaReactivePower.html \
	help/PnTasmotaPowerFactor.html

EXTRA_DIST = $(pnhelp_DATA)
