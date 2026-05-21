SUBDIRS = data lib plugins src tests

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
pkgconfig_DATA = pipnode.pc

aclocaldir = $(datadir)/aclocal
aclocal_DATA = m4/pipnode-plugin.m4

EXTRA_DIST = pipnode.pc.in m4/pipnode-plugin.m4 PLUGINS
