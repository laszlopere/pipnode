# Standalone viewers for the GUI surfaces a worksheet drives.
#
# Unlike src/ (the editor and the headless runner, which link the node
# runtime) and plugins/ (node plugins loaded by the editor), this tree
# holds thin D-Bus CLIENTS of the background engine: programs that show a
# worksheet's widgets without executing the worksheet themselves.  They
# link GTK / GLib / json-glib and the GTK-only panel-widgets convenience
# library only, never libpipnode-core or libpipnode-gui, so a node or
# engine crash can never take a viewer down with it.
#
# The XFCE panel applet in panel-plugins/ is the same kind of program for
# the panel surface; it lives apart only because it is a panel module
# rather than a binary, and because it needs libxfce4panel.
#
# Each viewer is its own subdirectory; add new ones here as the layout
# editor family grows (web and mobile are the planned next surfaces).
SUBDIRS = pipnode-desktop
