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
# links libpipnode-core ONLY — the core is GTK-free and self-contained
# (no undefined gui symbols), so this binary never pulls the GTK stack
# (TODO #23, Phase 5).  GLIB_CFLAGS/LIBS cover pn-run.c's direct glib +
# gio + glib-unix use; JSON_GLIB_CFLAGS is needed only for the include
# path PnFlow's headers (pn-subst.h) pull transitively.
pipnode_run_SOURCES = \
	pn-run.c

pipnode_run_CFLAGS = \
	$(GLIB_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	-I$(top_srcdir)/lib \
	-DPKGDATADIR=\"$(pkgdatadir)\" \
	-DSRCDIR=\"$(abs_top_srcdir)\" \
	-DGIT_VERSION=\"$(GIT_VERSION)\"

pipnode_run_LDADD = \
	$(top_builddir)/lib/libpipnode-core.la \
	$(GLIB_LIBS)
