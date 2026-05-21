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
	test-pn-query \
	test-pn-edge \
	test-pn-switch \
	test-pn-set \
	test-pn-filter \
	test-pn-rewrite \
	test-pn-expression2 \
	test-pn-stats \
	test-pn-inject \
	test-pn-knob \
	test-pn-table-model \
	test-pn-auto-injector \
	test-pn-auto-random \
	test-pn-rtc \
	test-pn-watchdog \
	test-pn-flow

test_pn_expression_SOURCES   = test-pn-expression.c   pntest.c pntest.h
test_pn_format_SOURCES       = test-pn-format.c       pntest.c pntest.h
test_pn_dedup_SOURCES        = test-pn-dedup.c        pntest.c pntest.h
test_pn_throttle_SOURCES     = test-pn-throttle.c     pntest.c pntest.h
test_pn_query_SOURCES        = test-pn-query.c        pntest.c pntest.h
test_pn_edge_SOURCES         = test-pn-edge.c         pntest.c pntest.h
test_pn_switch_SOURCES       = test-pn-switch.c       pntest.c pntest.h
test_pn_set_SOURCES          = test-pn-set.c          pntest.c pntest.h
test_pn_filter_SOURCES       = test-pn-filter.c       pntest.c pntest.h
test_pn_rewrite_SOURCES      = test-pn-rewrite.c      pntest.c pntest.h
test_pn_expression2_SOURCES  = test-pn-expression2.c  pntest.c pntest.h
test_pn_stats_SOURCES        = test-pn-stats.c        pntest.c pntest.h
test_pn_inject_SOURCES       = test-pn-inject.c       pntest.c pntest.h
test_pn_knob_SOURCES         = test-pn-knob.c         pntest.c pntest.h
test_pn_table_model_SOURCES  = test-pn-table-model.c  pntest.c pntest.h
test_pn_auto_injector_SOURCES = test-pn-auto-injector.c pntest.c pntest.h pntimer.h
test_pn_auto_random_SOURCES   = test-pn-auto-random.c   pntest.c pntest.h pntimer.h
test_pn_rtc_SOURCES           = test-pn-rtc.c           pntest.c pntest.h pntimer.h
test_pn_watchdog_SOURCES      = test-pn-watchdog.c      pntest.c pntest.h pntimer.h
test_pn_flow_SOURCES          = test-pn-flow.c          pntest.c pntest.h

TESTS = $(check_PROGRAMS)
