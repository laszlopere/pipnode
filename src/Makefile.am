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

pipnode_editor_LDADD = \
	$(top_builddir)/lib/libpipnode.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS)

# Headless companion: loads a saved worksheet through PnFlow and
# runs the message-dispatch GMainLoop without instantiating any
# GTK widget.  Same libpipnode link as the GUI binary, so the
# transitive GTK_LIBS dep stays — pipnode-run never calls into it
# at runtime, the linker just resolves the imports.
pipnode_run_SOURCES = \
	pn-run.c

pipnode_run_CFLAGS = \
	$(GTK_CFLAGS) \
	$(JSON_GLIB_CFLAGS) \
	-I$(top_srcdir)/lib \
	-DPKGDATADIR=\"$(pkgdatadir)\" \
	-DSRCDIR=\"$(abs_top_srcdir)\" \
	-DGIT_VERSION=\"$(GIT_VERSION)\"

pipnode_run_LDADD = \
	$(top_builddir)/lib/libpipnode.la \
	$(GTK_LIBS) \
	$(JSON_GLIB_LIBS)
