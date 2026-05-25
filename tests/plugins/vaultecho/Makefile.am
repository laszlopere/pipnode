# Sample plugin built only for the test suite (noinst): the reference
# host-provisioned-profiles plugin (PLUGINS section 18, plugin ABI v5).
# It declares a "vaultecho" profile type and a node that resolves it, so
# test_credentials_profile.py can run it under pipnode-run and check the
# resolved values.  Core-only and GTK-free — the profile picker is built
# host-side, so no companion -gui.so is needed.
#
# Built as a libtool module (-module + -avoid-version) so the loadable
# object lands in .libs/ under the platform-native suffix g_module_open
# accepts (pn_vaultecho.so on Linux); the host loads it from the build
# tree via PIPNODE_PLUGIN_PATH.
noinst_LTLIBRARIES = pn_vaultecho.la

pn_vaultecho_la_SOURCES = \
	pn-vaultecho.c \
	pn-vaultecho.h

pn_vaultecho_la_CFLAGS = \
	$(JSON_GLIB_CFLAGS) \
	$(GMODULE_CFLAGS) \
	-I$(top_srcdir)/lib

pn_vaultecho_la_LDFLAGS = -module -avoid-version -shared -rpath $(libdir)

pn_vaultecho_la_LIBADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(JSON_GLIB_LIBS) \
	$(GMODULE_LIBS)
