# XFCE panel applets are optional (see BUILD_PANEL_PLUGINS in
# configure.ac); only recurse into them when libxfce4panel was found.
if BUILD_PANEL_PLUGINS
PANEL_PLUGINS_SUBDIR = panel-plugins
endif

# panel-widgets builds before lib: libpipnode-gui embeds the convenience
# library so the editor's PnLayoutEditor can preview applet widgets.
SUBDIRS = data examples panel-widgets lib plugins src viewers tests $(PANEL_PLUGINS_SUBDIR)

ACLOCAL_AMFLAGS = -I m4

# Plugin-developer helpers.  Installed into the standard pkg-config
# and aclocal search paths so an out-of-tree plugin project can do
#
#     PKG_CHECK_MODULES([PIPNODE], [pipnode])     # bare
#     PIPNODE_PLUGIN_CHECK([0.1.0])               # or via the macro
#
# from its own configure.ac without hand-rolling the include / link
# flags or hard-coding $libdir/pipnode/plugins for the install target.
pkgconfigdir = $(libdir)/pkgconfig
# Headless split (TODO #23): pipnode-core (GTK-free) + pipnode-gui (GTK
# tier) + pipnode (compatibility alias for pipnode-gui).
pkgconfig_DATA = pipnode-core.pc pipnode-gui.pc pipnode.pc

aclocaldir = $(datadir)/aclocal
aclocal_DATA = m4/pipnode-plugin.m4

EXTRA_DIST = pipnode-core.pc.in pipnode-gui.pc.in pipnode.pc.in \
	m4/pipnode-plugin.m4 PLUGINS DBUS-API.md
