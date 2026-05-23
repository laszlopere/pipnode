# Bundled shell plugin.  Installed alongside the host binaries so the
# default plugin search path ($pkglibdir/plugins) picks it up without
# any post-install configuration on the user's part.
plugindir = $(pkglibdir)/plugins

plugin_LTLIBRARIES = pipnode_shell.la

pipnode_shell_la_SOURCES = \
	pn-shell-plugin.c \
	pn-shell-host.c \
	pn-shell-host.h \
	pn-shell-command.c \
	pn-shell-command.h \
	pn-free-command.c \
	pn-free-command.h \
	pn-df-command.c \
	pn-df-command.h \
	pn-lxc-ls-command.c \
	pn-lxc-ls-command.h \
	pn-tmux-monitor.c \
	pn-tmux-monitor.h

pipnode_shell_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

pipnode_shell_la_LDFLAGS = -module -avoid-version -shared

pipnode_shell_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)

pnhelpdir = $(pkgdatadir)/help
pnhelp_DATA = \
	help/PnShellCommand.html \
	help/PnFreeCommand.html \
	help/PnDfCommand.html \
	help/PnLxcLsCommand.html \
	help/PnTmuxMonitor.html

EXTRA_DIST = $(pnhelp_DATA)
