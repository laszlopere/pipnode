# Sample pipnode plugin built only for the test suite (noinst).  The
# host loads it via PIPNODE_PLUGIN_PATH so the .so does not need to
# install into $pkglibdir/plugins.  Built as a libtool module
# (-module + -avoid-version) so libtool produces a single shared
# object whose name has the platform-native suffix g_module_open is
# willing to pick up — `pn_echo.so` on Linux, `pn_echo.dylib` on
# macOS, `pn_echo.dll` on Windows.
noinst_LTLIBRARIES = pn_echo.la

pn_echo_la_SOURCES = pn-echo.c

pn_echo_la_CFLAGS = \
	$(GTK_CFLAGS) \
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
	$(top_builddir)/lib/libpipnode-gui.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)
