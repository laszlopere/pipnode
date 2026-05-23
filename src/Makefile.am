bin_PROGRAMS = pipnode-editor pipnode-run

pipnode_editor_SOURCES = \
	main.c \
	pn-application.c \
	pn-application.h \
	pn-window.c \
	pn-window.h

pipnode_editor_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	-I$(top_srcdir)/lib \
	-DPKGDATADIR=\"$(pkgdatadir)\" \
	-DSRCDIR=\"$(abs_top_srcdir)\" \
	-DGIT_VERSION=\"$(GIT_VERSION)\"

# Both core and gui are listed explicitly: main.o calls core symbols
# (PnFlow, the factory) directly, so the linker needs libpipnode-core on
# the command line (not just transitively via gui) under --as-needed.
pipnode_editor_LDADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS)

# Headless companion: loads a saved worksheet through PnFlow and runs the
# message-dispatch GMainLoop without instantiating any GTK widget.  It
# uses only core symbols, but for now still links the gui library too:
# the core factory hard-registers the GUI-tier node GTypes, so those
# symbols must resolve at link time.  Phase 5 makes this core-only.
pipnode_run_SOURCES = \
	pn-run.c

pipnode_run_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	-I$(top_srcdir)/lib \
	-DPKGDATADIR=\"$(pkgdatadir)\" \
	-DSRCDIR=\"$(abs_top_srcdir)\" \
	-DGIT_VERSION=\"$(GIT_VERSION)\"

# --no-as-needed keeps libpipnode-gui on the link line even though this
# headless binary references no gui symbol directly: the core factory
# still hard-registers the GUI-tier node GTypes, so libpipnode-core has
# undefined gui symbols that gui must resolve.  Removed in Phase 5 when
# pipnode-run becomes core-only.
pipnode_run_LDFLAGS = -Wl,--no-as-needed

pipnode_run_LDADD = \
	$(top_builddir)/lib/libpipnode-gui.la \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS)
