SUBDIRS = plugins unit

TESTS = \
	test_auto_injector_filter_debug.py \
	test_clipboard_ops.py \
	test_clock_debug_format.py \
	test_dbus_batch_properties.py \
	test_dbus_discovery.py \
	test_dbus_document.py \
	test_dbus_editor_interface.py \
	test_dbus_error_contract.py \
	test_dbus_node_lifecycle.py \
	test_dbus_port_wiring.py \
	test_dbus_uuid_addressing.py \
	test_debug_view_toggle.py \
	test_debug_view_wide_window.py \
	test_debug_focus.py \
	test_https_tunnel.py \
	test_multi_worksheet.py \
	test_node_dialog_dial.py \
	test_node_dialog_led.py \
	test_plugin_load.py \
	test_plugin_gui_companion.py \
	test_devices_menu_providers.py \
	test_credentials_profile.py \
	test_credentials_migration.py \
	test_no_plugins_flag.py

EXTRA_DIST = $(TESTS)

# Point the test at the in-tree binary regardless of where automake's
# test driver runs us from (in-tree vs VPATH builds).
#
# The plugin tests look for the in-tree sample plugin under
# tests/plugins/echo/.libs/, where libtool drops the loadable .so for
# a -module -avoid-version build.  Forwarding the path here keeps the
# tests independent of make's working directory and lets a developer
# also drive them by hand with the same env vars set.
#
# PIPNODE_PLUGIN_PATH is the env var the host's plugin loader reads
# at startup; we point it at the build-tree directories of the
# bundled network plugin so a `make check` run
# before `make install` still loads PnPing / PnDns the same
# way an installed binary does.  Multiple paths are colon-separated.
# The echo-plugin path is *not* added here because the dedicated
# plugin test ships its own per-process PIPNODE_PLUGIN_PATH override.
TESTS_ENVIRONMENT = \
	PIPNODE='$(abs_top_builddir)/src/pipnode-editor' \
	PIPNODE_RUN='$(abs_top_builddir)/src/pipnode-run' \
	PIPNODE_PLUGIN_PATH='$(abs_top_builddir)/plugins/network/.libs' \
	PIPNODE_PLUGIN_DIR='$(abs_top_builddir)/tests/plugins/echo/.libs'
