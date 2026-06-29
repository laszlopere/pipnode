# Bundled shell plugin.  Installed alongside the host binaries so the
# default plugin search path ($pkglibdir/plugins) picks it up without
# any post-install configuration on the user's part.
#
# Two-tier plugin (TODO #23, Phase 8): the logic module pipnode_shell.so
# is GTK-free and links libpipnode-core ALONE, so a headless server
# installs it and runs every shell node under pipnode-run with no GTK.
# The Tmux Monitor node alone needs a custom settings dialog (a live
# session combobox, a deferred-commit host entry, a runtime status row);
# that GTK code ships in the companion module pipnode_shell-gui.so, which
# the editor loads next to the logic .so (via pn_plugin_gui_init) and a
# server never installs.  The core plugin loader skips *-gui.so during a
# logic-plugin scan.
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_shell.la pipnode_shell-gui.la

# -------------------------------------------------------------------
# Logic half — GTK-free, core only.  Loaded by pipnode-run and the
# editor; exports pn_plugin_init and registers every shell #GType.
# -------------------------------------------------------------------
pipnode_shell_la_SOURCES = \
	pn-shell-plugin.c \
	pn-shell-host.c \
	pn-shell-host.h \
	pn-shell-command.c \
	pn-shell-command.h \
	pn-shell-script.c \
	pn-shell-script.h \
	pn-free-command.c \
	pn-free-command.h \
	pn-df-command.c \
	pn-df-command.h \
	pn-lxc-ls-command.c \
	pn-lxc-ls-command.h \
	pn-tmux-monitor.c \
	pn-tmux-monitor.h

pipnode_shell_la_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_shell_la_LDFLAGS = -module -avoid-version -shared

pipnode_shell_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GLIB_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)

# -------------------------------------------------------------------
# Companion GUI module — links the GTK tier.  Automake canonicalises the
# `pipnode_shell-gui.la` target to the `pipnode_shell_gui_la_` prefix.
# Loaded by the editor only; resolves PnTmuxMonitor by name (the logic
# .so is BIND_LOCAL) and installs its dialog vfunc slots.
# -------------------------------------------------------------------
pipnode_shell_gui_la_SOURCES = \
	pn-tmux-monitor-gui.c

pipnode_shell_gui_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_shell_gui_la_LDFLAGS = -module -avoid-version -shared

pipnode_shell_gui_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)

pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnShellCommand.html \
	help/PnShellScript.html \
	help/PnFreeCommand.html \
	help/PnDfCommand.html \
	help/PnLxcLsCommand.html \
	help/PnTmuxMonitor.html

EXTRA_DIST = $(pnhelp_DATA)
