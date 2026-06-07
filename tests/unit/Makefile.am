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
	-I$(top_srcdir)/plugins/shell \
	-I$(top_srcdir)/plugins/host-monitoring \
	-I$(top_srcdir)/plugins/tasmota \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	$(LIBSOUP_CFLAGS)

# Every test exercises GTK-free core logic, so they link libpipnode-core
# ONLY — core is self-contained since Phase 5 (no undefined gui symbols),
# so the gui library and -Wl,--no-as-needed are no longer needed.  The one
# exception is test-pn-color, which cross-checks PnColor against
# gdk_rgba_parse/to_string: GTK_LIBS stays on the line for it (and --as-
# needed drops it from every test that does not reference a gdk symbol).
# GTK_CFLAGS stays in AM_CPPFLAGS for that test's <gtk/gtk.h> include.  No
# display is ever opened because gtk_init() is never called.
LDADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS)

check_PROGRAMS = \
	test-pn-expression \
	test-pn-format \
	test-pn-comparator \
	test-pn-dedup \
	test-pn-delay \
	test-pn-threshold \
	test-pn-throttle \
	test-pn-query \
	test-pn-edge \
	test-pn-success \
	test-pn-failure \
	test-pn-switch \
	test-pn-panel-input \
	test-pn-panel-display \
	test-pn-set \
	test-pn-text \
	test-pn-topic \
	test-pn-value \
	test-pn-filter \
	test-pn-rewrite \
	test-pn-ramp \
	test-pn-reduce \
	test-pn-expression2 \
	test-pn-stats \
	test-pn-inject \
	test-pn-knob \
	test-pn-table-model \
	test-pn-auto-injector \
	test-pn-auto-random \
	test-pn-astronomical \
	test-pn-sun-path \
	test-pn-geocode \
	test-pn-rtc \
	test-pn-deadline \
	test-pn-watchdog \
	test-pn-flow \
	test-pn-debug \
	test-pn-logger \
	test-pn-text-view \
	test-pn-rate \
	test-pn-led \
	test-pn-dial \
	test-pn-graph \
	test-pn-xy-graph \
	test-pn-oscilloscope \
	test-pn-table \
	test-pn-chat \
	test-pn-analog-meter \
	test-pn-countdown \
	test-pn-numeric \
	test-pn-digital-clock \
	test-pn-analog-clock \
	test-pn-label \
	test-pn-load \
	test-pn-memory \
	test-pn-cpu \
	test-pn-temp \
	test-pn-weather \
	test-pn-tts \
	test-pn-net-io \
	test-pn-disk-io \
	test-pn-var-store \
	test-pn-expr-parser \
	test-pn-json-path \
	test-pn-jmespath \
	test-pn-subst \
	test-pn-staircase \
	test-pn-auto-trigger \
	test-pn-free-command \
	test-pn-df-command \
	test-pn-lxc-ls-command \
	test-pn-color \
	test-pn-vector \
	test-pn-message \
	test-pn-settings-schema \
	test-pn-profile-schema \
	test-pn-vault \
	test-pn-secret-scrub \
	test-pn-ws \
	test-pn-mesh-pb \
	test-pn-mesh-frame \
	test-pn-mesh-qr \
	test-pn-mesh-inuse \
	test-pn-mesh-discover \
	test-pn-mesh-node \
	test-pn-mqtt \
	test-pn-mqtt-sink \
	test-pn-mqtt-profile \
	test-pn-mqtt-util \
	test-pn-mqtt-resolve \
	test-pn-tasmota-probe \
	test-pn-tasmota-concentrator \
	test-pn-node-log \
	test-pn-node-collate \
	test-pn-node-processing

test_pn_node_log_SOURCES     = test-pn-node-log.c     pntest.c pntest.h
test_pn_node_collate_SOURCES = test-pn-node-collate.c pntest.c pntest.h
test_pn_node_processing_SOURCES = test-pn-node-processing.c pntest.c pntest.h
test_pn_color_SOURCES        = test-pn-color.c        pntest.c pntest.h
test_pn_vector_SOURCES       = test-pn-vector.c       pntest.c pntest.h
test_pn_message_SOURCES      = test-pn-message.c      pntest.c pntest.h
test_pn_settings_schema_SOURCES = test-pn-settings-schema.c pntest.c pntest.h
test_pn_profile_schema_SOURCES = test-pn-profile-schema.c pntest.c pntest.h
test_pn_vault_SOURCES        = test-pn-vault.c        pntest.c pntest.h
test_pn_secret_scrub_SOURCES = test-pn-secret-scrub.c pntest.c pntest.h
test_pn_ws_SOURCES           = test-pn-ws.c           pntest.c pntest.h
test_pn_mesh_pb_SOURCES      = test-pn-mesh-pb.c      pntest.c pntest.h
test_pn_mesh_frame_SOURCES   = test-pn-mesh-frame.c   pntest.c pntest.h
test_pn_mesh_qr_SOURCES      = test-pn-mesh-qr.c      pntest.c pntest.h
test_pn_mesh_inuse_SOURCES   = test-pn-mesh-inuse.c   pntest.c pntest.h
test_pn_mesh_discover_SOURCES = test-pn-mesh-discover.c pntest.c pntest.h
test_pn_mesh_node_SOURCES    = test-pn-mesh-node.c    pntest.c pntest.h
test_pn_expression_SOURCES   = test-pn-expression.c   pntest.c pntest.h
test_pn_format_SOURCES       = test-pn-format.c       pntest.c pntest.h
test_pn_comparator_SOURCES   = test-pn-comparator.c   pntest.c pntest.h
test_pn_ramp_SOURCES         = test-pn-ramp.c         pntest.c pntest.h
test_pn_reduce_SOURCES       = test-pn-reduce.c       pntest.c pntest.h
test_pn_dedup_SOURCES        = test-pn-dedup.c        pntest.c pntest.h
test_pn_delay_SOURCES        = test-pn-delay.c        pntest.c pntest.h
test_pn_threshold_SOURCES    = test-pn-threshold.c    pntest.c pntest.h
test_pn_throttle_SOURCES     = test-pn-throttle.c     pntest.c pntest.h
test_pn_query_SOURCES        = test-pn-query.c        pntest.c pntest.h
test_pn_edge_SOURCES         = test-pn-edge.c         pntest.c pntest.h
test_pn_success_SOURCES      = test-pn-success.c      pntest.c pntest.h
test_pn_failure_SOURCES      = test-pn-failure.c      pntest.c pntest.h
test_pn_switch_SOURCES       = test-pn-switch.c       pntest.c pntest.h
test_pn_panel_input_SOURCES  = test-pn-panel-input.c  pntest.c pntest.h
test_pn_panel_display_SOURCES = test-pn-panel-display.c pntest.c pntest.h
test_pn_set_SOURCES          = test-pn-set.c          pntest.c pntest.h
test_pn_text_SOURCES         = test-pn-text.c         pntest.c pntest.h
test_pn_topic_SOURCES        = test-pn-topic.c        pntest.c pntest.h
test_pn_value_SOURCES        = test-pn-value.c        pntest.c pntest.h
test_pn_filter_SOURCES       = test-pn-filter.c       pntest.c pntest.h
test_pn_rewrite_SOURCES      = test-pn-rewrite.c      pntest.c pntest.h
test_pn_expression2_SOURCES  = test-pn-expression2.c  pntest.c pntest.h
test_pn_stats_SOURCES        = test-pn-stats.c        pntest.c pntest.h
test_pn_inject_SOURCES       = test-pn-inject.c       pntest.c pntest.h
test_pn_knob_SOURCES         = test-pn-knob.c         pntest.c pntest.h
test_pn_table_model_SOURCES  = test-pn-table-model.c  pntest.c pntest.h
test_pn_auto_injector_SOURCES = test-pn-auto-injector.c pntest.c pntest.h pntimer.h
test_pn_auto_random_SOURCES   = test-pn-auto-random.c   pntest.c pntest.h pntimer.h
test_pn_astronomical_SOURCES  = test-pn-astronomical.c  pntest.c pntest.h
test_pn_sun_path_SOURCES      = test-pn-sun-path.c      pntest.c pntest.h
test_pn_geocode_SOURCES       = test-pn-geocode.c       pntest.c pntest.h
test_pn_rtc_SOURCES           = test-pn-rtc.c           pntest.c pntest.h pntimer.h
test_pn_deadline_SOURCES      = test-pn-deadline.c      pntest.c pntest.h pntimer.h
test_pn_watchdog_SOURCES      = test-pn-watchdog.c      pntest.c pntest.h pntimer.h
test_pn_flow_SOURCES          = test-pn-flow.c          pntest.c pntest.h
test_pn_debug_SOURCES         = test-pn-debug.c         pntest.c pntest.h
test_pn_logger_SOURCES        = test-pn-logger.c        pntest.c pntest.h
test_pn_text_view_SOURCES     = test-pn-text-view.c     pntest.c pntest.h
test_pn_rate_SOURCES          = test-pn-rate.c          pntest.c pntest.h
test_pn_led_SOURCES           = test-pn-led.c           pntest.c pntest.h
test_pn_dial_SOURCES          = test-pn-dial.c          pntest.c pntest.h
test_pn_graph_SOURCES         = test-pn-graph.c         pntest.c pntest.h
test_pn_xy_graph_SOURCES      = test-pn-xy-graph.c      pntest.c pntest.h
test_pn_oscilloscope_SOURCES  = test-pn-oscilloscope.c  pntest.c pntest.h
test_pn_table_SOURCES         = test-pn-table.c         pntest.c pntest.h
test_pn_chat_SOURCES          = test-pn-chat.c          pntest.c pntest.h
test_pn_analog_meter_SOURCES  = test-pn-analog-meter.c  pntest.c pntest.h
test_pn_countdown_SOURCES     = test-pn-countdown.c     pntest.c pntest.h
test_pn_numeric_SOURCES       = test-pn-numeric.c       pntest.c pntest.h
test_pn_digital_clock_SOURCES = test-pn-digital-clock.c pntest.c pntest.h
test_pn_analog_clock_SOURCES  = test-pn-analog-clock.c  pntest.c pntest.h
test_pn_label_SOURCES         = test-pn-label.c         pntest.c pntest.h
# The host-monitoring nodes live in their own loadable module
# (pipnode_host_monitoring.la), not in libpipnode, so their /proc and
# /sys parsers reach the test binary by compiling the node .c directly —
# the same arrangement the shell-plugin node tests below use.
test_pn_load_SOURCES          = test-pn-load.c          pntest.c pntest.h \
	$(top_srcdir)/plugins/host-monitoring/pn-load.c
test_pn_memory_SOURCES        = test-pn-memory.c        pntest.c pntest.h \
	$(top_srcdir)/plugins/host-monitoring/pn-memory.c
test_pn_cpu_SOURCES           = test-pn-cpu.c           pntest.c pntest.h \
	$(top_srcdir)/plugins/host-monitoring/pn-cpu.c
test_pn_temp_SOURCES          = test-pn-temp.c          pntest.c pntest.h \
	$(top_srcdir)/plugins/host-monitoring/pn-temp.c
test_pn_weather_SOURCES       = test-pn-weather.c       pntest.c pntest.h
test_pn_tts_SOURCES           = test-pn-tts.c           pntest.c pntest.h
test_pn_net_io_SOURCES        = test-pn-net-io.c        pntest.c pntest.h \
	$(top_srcdir)/plugins/host-monitoring/pn-net-io.c
test_pn_disk_io_SOURCES       = test-pn-disk-io.c        pntest.c pntest.h \
	$(top_srcdir)/plugins/host-monitoring/pn-disk-io.c
test_pn_var_store_SOURCES     = test-pn-var-store.c     pntest.c pntest.h
test_pn_expr_parser_SOURCES   = test-pn-expr-parser.c   pntest.c pntest.h
test_pn_json_path_SOURCES     = test-pn-json-path.c     pntest.c pntest.h
test_pn_jmespath_SOURCES      = test-pn-jmespath.c      pntest.c pntest.h
test_pn_subst_SOURCES         = test-pn-subst.c         pntest.c pntest.h
test_pn_staircase_SOURCES     = test-pn-staircase.c     pntest.c pntest.h
test_pn_auto_trigger_SOURCES  = test-pn-auto-trigger.c  pntest.c pntest.h

# The shell-plugin nodes live in their own loadable module (pipnode_shell.la),
# not in libpipnode, so their table parsers reach the test binary by compiling
# the node .c directly.  pn-shell-host.c comes along because the node's trigger
# (linked whole, object-file granularity) references pn_shell_wrap_argv; the
# parser under test itself only needs json-glib.
test_pn_free_command_SOURCES = test-pn-free-command.c pntest.c pntest.h \
	$(top_srcdir)/plugins/shell/pn-free-command.c \
	$(top_srcdir)/plugins/shell/pn-shell-host.c
test_pn_df_command_SOURCES = test-pn-df-command.c pntest.c pntest.h \
	$(top_srcdir)/plugins/shell/pn-df-command.c \
	$(top_srcdir)/plugins/shell/pn-shell-host.c
test_pn_lxc_ls_command_SOURCES = test-pn-lxc-ls-command.c pntest.c pntest.h \
	$(top_srcdir)/plugins/shell/pn-lxc-ls-command.c \
	$(top_srcdir)/plugins/shell/pn-shell-host.c

test_pn_mqtt_SOURCES         = test-pn-mqtt.c         pntest.c pntest.h
test_pn_mqtt_sink_SOURCES    = test-pn-mqtt-sink.c    pntest.c pntest.h
test_pn_mqtt_profile_SOURCES = test-pn-mqtt-profile.c pntest.c pntest.h
test_pn_mqtt_util_SOURCES    = test-pn-mqtt-util.c    pntest.c pntest.h
test_pn_mqtt_resolve_SOURCES = test-pn-mqtt-resolve.c pntest.c pntest.h

# The tasmota-plugin nodes live in their own loadable module
# (pipnode_tasmota.la), not in libpipnode, so the probe node and its
# shared helpers reach the test binary by compiling those .c directly —
# the same arrangement the shell / host-monitoring node tests use above.
test_pn_tasmota_probe_SOURCES = test-pn-tasmota-probe.c pntest.c pntest.h \
	$(top_srcdir)/plugins/tasmota/pn-tasmota-probe.c \
	$(top_srcdir)/plugins/tasmota/pn-tasmota-common.c

test_pn_tasmota_concentrator_SOURCES = test-pn-tasmota-concentrator.c \
	pntest.c pntest.h \
	$(top_srcdir)/plugins/tasmota/pn-tasmota-concentrator.c \
	$(top_srcdir)/plugins/tasmota/pn-tasmota-common.c

TESTS = $(check_PROGRAMS)
