AC_INIT([pipnode], [0.2.0-dev], [laszlopere@gmail.com])
AC_COPYRIGHT([Copyright (C) 2024-2026 Laszlo Pere.
GPL-3.0-or-later WITH the Pipnode Plugin Exception; see COPYING and
LICENSE.PLUGIN-EXCEPTION.])
AC_CONFIG_SRCDIR([src/main.c])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIRS([m4])

AM_INIT_AUTOMAKE([foreign -Wall -Werror subdir-objects])

# Required for building shared libraries
AM_PROG_AR
LT_INIT

# Checks for programs
AC_PROG_CC
AC_PROG_INSTALL

# --- Core tier dependencies (headless split, TODO #23) ----------------
# The GTK-free core library (libpipnode-core) links only these.  glib /
# gobject / gio are the base; we check them explicitly rather than relying
# on GTK's transitive closure so the core compiles and links with no GTK
# on the command line.
PKG_CHECK_MODULES([GLIB], [glib-2.0 >= 2.40 gobject-2.0 >= 2.40 gio-2.0 >= 2.40])

# gdk-pixbuf is an allowed *core* image-data dependency (see the headless
# split plan, §6 decision 1): PnImageMessage carries a GdkPixbuf so image
# messages flow between logic nodes headlessly.  It pulls no GTK/GDK widget
# toolkit code, so it does not compromise the "no graphics toolkit" rule.
PKG_CHECK_MODULES([GDK_PIXBUF], [gdk-pixbuf-2.0 >= 2.36])

# --- GUI tier dependencies --------------------------------------------
# Check for GTK+ 3
PKG_CHECK_MODULES([GTK], [gtk+-3.0 >= 3.22])

# Check for GtkSourceView 4.  Used by the Rewrite node's settings
# dialog to give the JSON template editor syntax highlighting, line
# numbers and auto-indent — the same widget toolkit gedit / Builder
# use, dropped in for one row inside our own dialog.
PKG_CHECK_MODULES([GTKSOURCEVIEW], [gtksourceview-4 >= 4.0])

# Check for GtkSheet 4 (the maintained fork of the widget that used to
# live in Gtk+-Extra; upstream is fpaquet/gtksheet).  Provides the
# spreadsheet-style cell matrix used by the gui tier.  LGPL-2+, so it
# stays linkable from proprietary plugins as well as the GPL core --
# unlike GNU spread-sheet-widget, which is GPLv3+.
PKG_CHECK_MODULES([GTKSHEET], [gtksheet-4.0 >= 4.3.12])

# Check for JSON-GLib
PKG_CHECK_MODULES([JSON_GLIB], [json-glib-1.0 >= 1.0])

# Check for GModule (used by the plugin loader in
# lib/pn-node-factory.c).  Bundled with GLib but exposed as its own
# pkg-config module so the linker pulls the right libdl-style backend
# on every supported platform — we ask for it explicitly rather than
# relying on the GTK transitive closure to avoid surprises if the GTK
# link line ever stops including it.
PKG_CHECK_MODULES([GMODULE], [gmodule-2.0 >= 2.40])

# Check for libsoup 3 (used directly for the websocket client; also a
# transitive dependency of WebKit2GTK, but we pin it explicitly so the
# program builds even when --with-webview=no).
PKG_CHECK_MODULES([LIBSOUP], [libsoup-3.0 >= 3.0])

# Check for GnuTLS.  Used by the bundled network plugin's HTTPS Tunnel
# Receiver node to mint a self-signed certificate on the fly when the
# user has not supplied a real cert/key pair, so the node works out of
# the box without any manual setup.
PKG_CHECK_MODULES([GNUTLS], [gnutls >= 3.0])

# Check for PLplot (double-precision C core).  Used by plot/graph
# nodes that render data series — for example the rolling-window
# output of the Stats filter.
PKG_CHECK_MODULES([PLPLOT], [plplot >= 5.15])

# Check for MathGL (libmgl).  Used by the gui tier only, to render the
# multi-series 3D graph views of the Graph node (PLplot has no
# grid-of-towers 3D bar primitive).  Debian/Ubuntu package libmgl-dev
# ships no .pc file, so probe the header + library manually rather than
# with PKG_CHECK_MODULES.  The C entry points are exported with C
# linkage (extern "C"), so a plain C probe resolves them; the library's
# own NEEDED entries pull in libstdc++ transitively at link time.
AC_CHECK_HEADER([mgl2/mgl_cf.h], [],
    [AC_MSG_ERROR([MathGL headers not found; install libmgl-dev (or equivalent)])])
AC_CHECK_LIB([mgl], [mgl_create_graph],
    [MGL_LIBS="-lmgl"
     MGL_CFLAGS=""],
    [AC_MSG_ERROR([MathGL runtime not found; install libmgl (or equivalent)])])
AC_SUBST([MGL_CFLAGS])
AC_SUBST([MGL_LIBS])

# Check for libmosquitto (MQTT C client).  Used by the bundled
# network plugin's MQTT Source node.  Debian/Ubuntu package
# libmosquitto-dev does not ship a .pc file, so we fall back to a
# manual header + library probe rather than PKG_CHECK_MODULES.
AC_CHECK_HEADER([mosquitto.h], [],
    [AC_MSG_ERROR([libmosquitto headers not found; install libmosquitto-dev (or equivalent)])])
AC_CHECK_LIB([mosquitto], [mosquitto_lib_init],
    [MOSQUITTO_LIBS="-lmosquitto"
     MOSQUITTO_CFLAGS=""],
    [AC_MSG_ERROR([libmosquitto runtime not found; install libmosquitto1 (or equivalent)])])
AC_SUBST([MOSQUITTO_CFLAGS])
AC_SUBST([MOSQUITTO_LIBS])

# Optional WebKit2GTK help browser.  Without it, the help browser
# falls back to a lightweight GtkTextView-based renderer so the
# program can run over X11 forwarding on hosts where WebKit's
# EGL/GL/Wayland stack would otherwise prevent startup.
AC_ARG_WITH([webview],
    [AS_HELP_STRING([--with-webview@<:@=auto|yes|no@:>@],
        [use WebKit2GTK for the in-application help browser
         @<:@default=auto@:>@])],
    [],
    [with_webview=auto])

have_webkit=no
AS_IF([test "x$with_webview" != "xno"], [
    PKG_CHECK_MODULES([WEBKIT2GTK], [webkit2gtk-4.1],
        [have_webkit=yes
         AC_DEFINE([HAVE_WEBKIT], [1],
             [Define to 1 if WebKit2GTK is available for the
              help browser.])],
        [AS_IF([test "x$with_webview" = "xyes"], [
            AC_MSG_ERROR([WebKit2GTK requested via --with-webview=yes but webkit2gtk-4.1 was not found])
         ], [
            AC_MSG_NOTICE([WebKit2GTK not found; help browser will use the text renderer fallback])
            WEBKIT2GTK_CFLAGS=
            WEBKIT2GTK_LIBS=
            AC_SUBST([WEBKIT2GTK_CFLAGS])
            AC_SUBST([WEBKIT2GTK_LIBS])
         ])])
], [
    WEBKIT2GTK_CFLAGS=
    WEBKIT2GTK_LIBS=
    AC_SUBST([WEBKIT2GTK_CFLAGS])
    AC_SUBST([WEBKIT2GTK_LIBS])
])
AM_CONDITIONAL([HAVE_WEBKIT], [test "x$have_webkit" = "xyes"])

# Optional libqrencode: used by the Meshtastic dialog's Share page to
# render channel-set / full-config QR codes for the Meshtastic Android
# / iOS apps.  Auto-detected; when missing the page falls back to a
# copyable URL only.
have_qrencode=no
PKG_CHECK_MODULES([QRENCODE], [libqrencode >= 3.4.4],
    [have_qrencode=yes
     AC_DEFINE([HAVE_QRENCODE], [1],
         [Define to 1 if libqrencode is available for rendering
          shareable QR codes.])],
    [AC_MSG_NOTICE([libqrencode not found; Meshtastic Share page will show URLs only])
     QRENCODE_CFLAGS=
     QRENCODE_LIBS=
     AC_SUBST([QRENCODE_CFLAGS])
     AC_SUBST([QRENCODE_LIBS])])
AM_CONDITIONAL([HAVE_QRENCODE], [test "x$have_qrencode" = "xyes"])

# Optional in-process audio playback for the core Sound node.  Playing a
# clip without a helper needs both a decoder (libsndfile, for the
# freedesktop .oga theme sounds and common file formats) and an output
# path (libpulse-simple, to stream PCM to the running PulseAudio/PipeWire
# server).  When either is missing the Sound node falls back to spawning
# `paplay`, and its settings dialog reports which mode is in effect.
have_pn_audio=no
PKG_CHECK_MODULES([PN_AUDIO], [sndfile libpulse-simple],
    [have_pn_audio=yes
     AC_DEFINE([HAVE_PN_AUDIO], [1],
         [Define to 1 if libsndfile + libpulse-simple are available for
          in-process Sound playback.])],
    [AC_MSG_NOTICE([libsndfile + libpulse-simple not both found; Sound node will spawn paplay])
     PN_AUDIO_CFLAGS=
     PN_AUDIO_LIBS=
     AC_SUBST([PN_AUDIO_CFLAGS])
     AC_SUBST([PN_AUDIO_LIBS])])
AM_CONDITIONAL([HAVE_PN_AUDIO], [test "x$have_pn_audio" = "xyes"])

# Embed git commit hash for --version
GIT_VERSION=$(git -C "$srcdir" rev-parse --short HEAD 2>/dev/null || echo "unknown")
AC_SUBST([GIT_VERSION])

# Used by data/Makefile.am to refresh the hicolor cache after installing
# the app icon.  Fall back to ":" (a no-op) when the tool is absent.
AC_PATH_PROG([GTK_UPDATE_ICON_CACHE], [gtk-update-icon-cache], [:])

# Optional XFCE panel applets (panel-plugins/).  Auto-detected: built
# only when libxfce4panel-2.0 is available, so a box without XFCE still
# builds the rest of pipnode.  Force with --enable/--disable-panel-plugins.
AC_ARG_ENABLE([panel-plugins],
    [AS_HELP_STRING([--enable-panel-plugins=@<:@yes/no/auto@:>@],
                    [build XFCE panel plugins @<:@default=auto@:>@])],
    [], [enable_panel_plugins=auto])
have_panel_plugins=no
AS_IF([test "x$enable_panel_plugins" != "xno"], [
    PKG_CHECK_MODULES([LIBXFCE4PANEL],
        [libxfce4panel-2.0 >= 4.14 libxfce4util-1.0 >= 4.14],
        [have_panel_plugins=yes], [have_panel_plugins=no])
    AS_IF([test "x$enable_panel_plugins" = "xyes" -a "x$have_panel_plugins" = "xno"],
        [AC_MSG_ERROR([--enable-panel-plugins requested but libxfce4panel-2.0 not found])])
])
# A panel applet must be installed where the *running* xfce4-panel scans
# (its own libdir/datadir), not under pipnode's --prefix — the panel only
# looks in compiled-in directories.  Derive both from libxfce4panel-2.0.pc.
AS_IF([test "x$have_panel_plugins" = "xyes"], [
    LIBXFCE4PANEL_PLUGINDIR=`$PKG_CONFIG --variable=libdir libxfce4panel-2.0`/xfce4/panel/plugins
    LIBXFCE4PANEL_DESKTOPDIR=`$PKG_CONFIG --variable=datarootdir libxfce4panel-2.0`/xfce4/panel/plugins
    AC_SUBST([LIBXFCE4PANEL_PLUGINDIR])
    AC_SUBST([LIBXFCE4PANEL_DESKTOPDIR])
    AC_MSG_NOTICE([panel applets install into $LIBXFCE4PANEL_PLUGINDIR])
])
AM_CONDITIONAL([BUILD_PANEL_PLUGINS], [test "x$have_panel_plugins" = "xyes"])

AC_CONFIG_FILES([
    Makefile
    pipnode.pc
    pipnode-core.pc
    pipnode-gui.pc
    data/Makefile
    examples/Makefile
    panel-widgets/Makefile
    lib/Makefile
    plugins/Makefile
    plugins/network/Makefile
    plugins/shell/Makefile
    plugins/tasmota/Makefile
    plugins/sound-effects/Makefile
    plugins/image/Makefile
    plugins/host-monitoring/Makefile
    plugins/ollama/Makefile
    src/Makefile
    viewers/Makefile
    viewers/pipnode-desktop/Makefile
    tests/Makefile
    tests/unit/Makefile
    tests/plugins/Makefile
    tests/plugins/echo/Makefile
    tests/plugins/vaultecho/Makefile
    panel-plugins/Makefile
    panel-plugins/pipnode-xfce-applet/Makefile
])

AC_OUTPUT
