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
	test-pn-delay \
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
	test-pn-flow \
	test-pn-debug \
	test-pn-text-view \
	test-pn-rate \
	test-pn-led \
	test-pn-analog-meter \
	test-pn-load \
	test-pn-memory \
	test-pn-cpu \
	test-pn-net-io \
	test-pn-disk-io \
	test-pn-var-store \
	test-pn-expr-parser \
	test-pn-json-path \
	test-pn-jmespath \
	test-pn-subst \
	test-pn-auto-trigger

test_pn_expression_SOURCES   = test-pn-expression.c   pntest.c pntest.h
test_pn_format_SOURCES       = test-pn-format.c       pntest.c pntest.h
test_pn_dedup_SOURCES        = test-pn-dedup.c        pntest.c pntest.h
test_pn_delay_SOURCES        = test-pn-delay.c        pntest.c pntest.h
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
test_pn_debug_SOURCES         = test-pn-debug.c         pntest.c pntest.h
test_pn_text_view_SOURCES     = test-pn-text-view.c     pntest.c pntest.h
test_pn_rate_SOURCES          = test-pn-rate.c          pntest.c pntest.h
test_pn_led_SOURCES           = test-pn-led.c           pntest.c pntest.h
test_pn_analog_meter_SOURCES  = test-pn-analog-meter.c  pntest.c pntest.h
test_pn_load_SOURCES          = test-pn-load.c          pntest.c pntest.h
test_pn_memory_SOURCES        = test-pn-memory.c        pntest.c pntest.h
test_pn_cpu_SOURCES           = test-pn-cpu.c           pntest.c pntest.h
test_pn_net_io_SOURCES        = test-pn-net-io.c        pntest.c pntest.h
test_pn_disk_io_SOURCES       = test-pn-disk-io.c        pntest.c pntest.h
test_pn_var_store_SOURCES     = test-pn-var-store.c     pntest.c pntest.h
test_pn_expr_parser_SOURCES   = test-pn-expr-parser.c   pntest.c pntest.h
test_pn_json_path_SOURCES     = test-pn-json-path.c     pntest.c pntest.h
test_pn_jmespath_SOURCES      = test-pn-jmespath.c      pntest.c pntest.h
test_pn_subst_SOURCES         = test-pn-subst.c         pntest.c pntest.h
test_pn_auto_trigger_SOURCES  = test-pn-auto-trigger.c  pntest.c pntest.h

TESTS = $(check_PROGRAMS)
