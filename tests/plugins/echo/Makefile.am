# Sample pipnode plugin built only for the test suite (noinst).  The
# host loads it via PIPNODE_PLUGIN_PATH so the .so does not need to
# install into $pkglibdir/plugins.  Both halves are built as libtool
# modules (-module + -avoid-version) so libtool produces a single
# shared object per half whose name has the platform-native suffix
# g_module_open is willing to pick up — `pn_echo.so` / `pn_echo-gui.so`
# on Linux, `.dylib` on macOS, `.dll` on Windows.
#
# This is the reference two-tier plugin (TODO #23, Phase 6):
#
#   pn_echo.so       — logic half.  Links libpipnode-core ONLY (no GTK),
#                      exports pn_plugin_init, and is the only half a
#                      headless server installs.  test_plugin_load.py
#                      runs it under pipnode-run with no GTK.
#   pn_echo-gui.so   — companion GUI module.  Links libpipnode-gui + the
#                      GTK stack, exports pn_plugin_gui_init, and is
#                      loaded by the editor (only) to install the Echo
#                      node's settings-dialog vfuncs onto the class the
#                      logic half registered.  test_plugin_gui_companion.py
#                      drives the editor and asserts it loaded.
#
# The companion's filename is the logic basename with the "-gui" infix
# (PN_PLUGIN_GUI_INFIX) before the module suffix; the editor's GUI loader
# derives it from each loaded logic plugin and the core loader skips it
# during a logic-plugin directory scan.
noinst_LTLIBRARIES = pn_echo.la pn_echo-gui.la

# -------------------------------------------------------------------
# Logic half — GTK-free, core only.
# -------------------------------------------------------------------
pn_echo_la_SOURCES = \
	pn-echo.c \
	pn-echo.h

pn_echo_la_CFLAGS = \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

# -module      : produce a loadable module (no `lib` prefix);
# -avoid-version : drop the .0.0.0 SONAME version suffix so the plain
#                  `pn_echo.so` lands in .libs/ next to its .la — that
#                  is the file g_module_open looks for under the
#                  PIPNODE_PLUGIN_PATH directory.
# -shared      : force the shared form even if libtool would otherwise
#                build a static archive in addition.
# -rpath       : libtool refuses to build a shared module without one;
#                we pass $(libdir) but the path is meaningless for a
#                noinst module — it never gets installed, the test
#                harness loads it from the build tree.
pn_echo_la_LDFLAGS = -module -avoid-version -shared -rpath $(libdir)

pn_echo_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)

# -------------------------------------------------------------------
# Companion GUI module — links the GTK tier.  Automake canonicalises the
# `pn_echo-gui.la` target to the `pn_echo_gui_la_` variable prefix.
# -------------------------------------------------------------------
# The companion does NOT include pn-echo.h: it resolves the node type by
# name (g_type_from_name) because the logic .so is loaded BIND_LOCAL, so
# its pn_echo_get_type symbol is not visible across the module boundary.
pn_echo_gui_la_SOURCES = \
	pn-echo-gui.c

pn_echo_gui_la_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

pn_echo_gui_la_LDFLAGS = -module -avoid-version -shared -rpath $(libdir)

pn_echo_gui_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)
