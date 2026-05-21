dnl  pipnode-plugin.m4 -- autoconf macro for out-of-tree pipnode plugins.
dnl
dnl  Plugin developers drop this file into their own m4/ directory (or
dnl  rely on aclocal picking it up from $(aclocaldir) after a system
dnl  install of pipnode) and call PIPNODE_PLUGIN_CHECK from their
dnl  configure.ac.  The macro verifies the host pipnode library is
dnl  present, exposes the usual PIPNODE_CFLAGS / PIPNODE_LIBS
dnl  substitutions, and pulls the host's canonical install paths
dnl  (plugindir, helpdir) out of pipnode.pc so the plugin's
dnl  Makefile.am can install into the directories the host actually
dnl  scans at run-time -- without hard-coding $(libdir)/pipnode/plugins
dnl  or $(datadir)/pipnode/help in the plugin tree.

# PIPNODE_PLUGIN_CHECK([MIN-VERSION])
# -----------------------------------
# AC_SUBST:
#   PIPNODE_CFLAGS    -- compile flags for pipnode + its public deps
#   PIPNODE_LIBS      -- link line against libpipnode
#   PIPNODE_PLUGINDIR -- install dir for plugin .so files
#   PIPNODE_HELPDIR   -- install dir for per-node HTML help files
AC_DEFUN([PIPNODE_PLUGIN_CHECK],
[
    AC_REQUIRE([PKG_PROG_PKG_CONFIG])

    m4_ifval([$1],
        [PKG_CHECK_MODULES([PIPNODE], [pipnode >= $1])],
        [PKG_CHECK_MODULES([PIPNODE], [pipnode])])

    PIPNODE_PLUGINDIR=`$PKG_CONFIG --variable=plugindir pipnode`
    AS_IF([test -z "$PIPNODE_PLUGINDIR"],
        [AC_MSG_ERROR([pipnode.pc does not expose a 'plugindir' variable; pipnode is too old])])
    AC_SUBST([PIPNODE_PLUGINDIR])

    PIPNODE_HELPDIR=`$PKG_CONFIG --variable=helpdir pipnode`
    AC_SUBST([PIPNODE_HELPDIR])

    AC_MSG_NOTICE([pipnode plugins will install into $PIPNODE_PLUGINDIR])
])
