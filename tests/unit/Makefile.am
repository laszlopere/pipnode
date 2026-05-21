# C unit tests for individual nodes.  Each program drives one node in
# process — constructing it, feeding it messages, and asserting on what
# it emits — without touching the filesystem, the network, or a display
# (gtk_init() is never called).  Built and run by `make check`.
#
# The shared harness (pntest.[ch]) layers a minimal, one-line-per-binary
# reporter on top of GLib's GTest helpers; pass --verbose to a test for
# the per-check breakdown.

AM_CPPFLAGS = \
	-I$(top_srcdir)/lib \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS)

# The public node headers still transitively include <gtk/gtk.h> (the
# coupling TODO #23 will break), so the tests need GTK_CFLAGS to compile
# even though they never call into GTK at runtime.  JSON_GLIB_CFLAGS adds
# the json-glib include path used by pn-message.h.  All of libpipnode's
# transitive link-time dependencies come in through the .la below; no
# display is ever opened because gtk_init() is never called.
LDADD = \
	$(top_builddir)/lib/libpipnode.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS)

check_PROGRAMS = \
	test-pn-expression \
	test-pn-format \
	test-pn-dedup \
	test-pn-throttle \
	test-pn-query

test_pn_expression_SOURCES = test-pn-expression.c pntest.c pntest.h
test_pn_format_SOURCES     = test-pn-format.c     pntest.c pntest.h
test_pn_dedup_SOURCES      = test-pn-dedup.c      pntest.c pntest.h
test_pn_throttle_SOURCES   = test-pn-throttle.c   pntest.c pntest.h
test_pn_query_SOURCES      = test-pn-query.c      pntest.c pntest.h

TESTS = $(check_PROGRAMS)
