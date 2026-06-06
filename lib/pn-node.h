/*
 * Copyright (C) 2024-2026 Laszlo Pere
 *
 * This file is part of Pipnode.  Pipnode is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General
 * Public License version 3, with the additional permission described in
 * LICENSE.PLUGIN-EXCEPTION, as published by the Free Software Foundation.
 *
 * Pipnode is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; see the GNU General Public License for more details.  You
 * should have received a copy of the license in the file COPYING.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PN_NODE_H
#define PN_NODE_H

#include <stdarg.h>

#include <glib-object.h>

#include "pn-color.h"

G_BEGIN_DECLS

/* Headless-core invariant: this header — the plugin/class ABI — must
 * compile against gobject alone, so it pulls in neither GTK nor GDK.
 * The few toolkit types that appear in the class vtable are referenced
 * by pointer only, so opaque forward declarations suffice here; the GUI
 * tier (which does include <gtk/gtk.h> / cairo) sees these resolve to
 * the real types in the same translation unit because the struct tags
 * match GTK's own.  The one by-value colour slot uses #PnColor, which is
 * layout-identical to #GdkRGBA (see pn-color.h). */
typedef struct _cairo       cairo_t;
typedef struct _GtkWidget   GtkWidget;
typedef struct _GtkWindow   GtkWindow;
typedef struct _GtkNotebook GtkNotebook;

/* ------------------------------------------------------------------ */
/*  PnPoint                                                            */
/*                                                                     */
/*  Boxed (x, y) pair used for node positions.  GdkPoint is integer    */
/*  only and not registered as a GType, so we provide our own type so  */
/*  it can be used as a #GValue and a GObject property.                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_POINT (pn_point_get_type ())

typedef struct _PnPoint PnPoint;

struct _PnPoint
{
    double x;
    double y;
};

GType    pn_point_get_type (void) G_GNUC_CONST;
PnPoint *pn_point_new      (double x, double y);
PnPoint *pn_point_copy     (const PnPoint *self);
void     pn_point_free     (PnPoint *self);

/* ------------------------------------------------------------------ */
/*  PnLogEntry                                                         */
/*                                                                     */
/*  One line in a node's in-memory log ring.  Nodes report their own   */
/*  diagnostics through pn_node_log() instead of printing to stdout /  */
/*  stderr (invisible when pipnode is launched from a desktop icon);   */
/*  the most recent entries are kept on the node and surfaced in the   */
/*  per-node Log dialog.  Boxed so the GUI tier can copy an entry out  */
/*  of the ring for display.                                           */
/* ------------------------------------------------------------------ */

/**
 * PnLogLevel:
 * @PN_LOG_LEVEL_INFO:    informational progress note
 * @PN_LOG_LEVEL_WARNING: something unexpected the node recovered from
 * @PN_LOG_LEVEL_ERROR:   an operation failed
 *
 * Severity of a #PnLogEntry, used by the Log dialog to colour the row.
 */
typedef enum
{
    PN_LOG_LEVEL_INFO,
    PN_LOG_LEVEL_WARNING,
    PN_LOG_LEVEL_ERROR,
} PnLogLevel;

#define PN_TYPE_LOG_ENTRY (pn_log_entry_get_type ())

typedef struct _PnLogEntry PnLogEntry;

GType        pn_log_entry_get_type    (void) G_GNUC_CONST;
PnLogEntry  *pn_log_entry_copy        (const PnLogEntry *self);
void         pn_log_entry_free        (PnLogEntry *self);

/**
 * pn_log_entry_get_time:
 * @self: a log entry
 *
 * Returns: the wall-clock time the entry was recorded, in microseconds
 *   since the Unix epoch (as produced by g_get_real_time()).
 */
gint64       pn_log_entry_get_time    (const PnLogEntry *self);

/**
 * pn_log_entry_get_level:
 * @self: a log entry
 *
 * Returns: the entry's #PnLogLevel.
 */
PnLogLevel   pn_log_entry_get_level   (const PnLogEntry *self);

/**
 * pn_log_entry_get_message:
 * @self: a log entry
 *
 * Returns: (transfer none): the entry's text.  Owned by the entry.
 */
const gchar *pn_log_entry_get_message (const PnLogEntry *self);

/**
 * pn_log_level_to_string:
 * @level: a #PnLogLevel
 *
 * Returns: (transfer none): a short uppercase label ("INFO", "WARNING",
 *   "ERROR") for @level — a static string, never freed.
 */
const gchar *pn_log_level_to_string   (PnLogLevel level);

/* ------------------------------------------------------------------ */
/*  PnNode                                                             */
/*                                                                     */
/*  Derivable base class shared by every node type drawn on the        */
/*  worksheet.  It carries the visual attributes needed by the         */
/*  painter; concrete node types inherit from it to add behaviour.    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NODE (pn_node_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnNode, pn_node, PN, NODE, GObject)

/* Forward declaration: pn-message.h pulls in this header, so we
 * cannot include it from here without creating a circular include. */
typedef struct _PnMessage PnMessage;

/* Forward declaration: pn-flow.h includes this header, so the owning
 * flow is referenced by pointer only (see pn_node_get_flow). */
typedef struct _PnFlow PnFlow;

struct _PnNodeClass
{
    GObjectClass parent_class;

    /**
     * PnNodeClass::receive:
     * @self:    receiving node
     * @message: (transfer none): message arriving on the input port
     *
     * Called when a message is delivered to this node's input.
     * Subclasses override to react.  The default implementation is a
     * no-op.
     */
    void (*receive) (PnNode *self, PnMessage *message);

    /**
     * PnNodeClass.palette_icon:
     *
     * Optional override of the icon glyph the palette shows for this
     * node type.  Subclasses that flip their instance icon based on
     * runtime state (e.g. a "configuration required" warning glyph)
     * can pin a stable, recognisable glyph for the palette here.
     * %NULL falls back to the instance icon of a freshly-constructed
     * template.  The string is owned by the class and must outlive
     * every instance — typically a static literal.
     */
    const gchar *palette_icon;

    /**
     * PnNodeClass.class_name:
     *
     * Canonical type label shown in the inspector / settings dialog
     * and the palette ("HTTP Client", "DNS Check", …).  Each concrete
     * leaf class pins its own value; abstract intermediates leave it
     * %NULL.  Read via pn_node_class_get_class_name() (or the
     * instance-side pn_node_get_class_name() — note the latter
     * returns the per-instance override property if set, falling back
     * to this class-level value otherwise).  The string is owned by
     * the class and must outlive every instance.
     */
    const gchar *class_name;

    /**
     * PnNodeClass.icon:
     *
     * Default glyph used for instances of this type — the "rest"
     * state for nodes that flip the instance icon based on runtime
     * state (e.g. a red warning glyph while unconfigured).  Read via
     * pn_node_class_get_icon().  The string is owned by the class
     * and must outlive every instance.
     */
    const gchar *icon;

    /**
     * PnNodeClass.color:
     *
     * Default body fill colour for instances of this type — the
     * "rest" state colour, before any unconfigured-state red flip.
     * Subclasses that leave this zero-initialised get the parent's
     * colour at runtime.  Read via pn_node_class_get_color().
     */
    PnColor color;

    /**
     * PnNodeClass.has_input:
     * PnNodeClass.has_output:
     *
     * Port topology of the type — whether instances of this class
     * accept incoming messages and/or emit outgoing ones.  The type,
     * not the user, decides this; the per-instance has-input /
     * has-output properties exist only because save / load went
     * through them historically and are no longer offered as
     * editable in the dialog.  Read via
     * pn_node_class_get_has_input() / pn_node_class_get_has_output().
     */
    gboolean has_input;
    gboolean has_output;

    /**
     * PnNodeClass.plugin_name:
     *
     * Name of the plugin that registered this node type.  Built-in
     * nodes leave this at the inherited #PnNode default of
     * <literal>"Internal"</literal>; out-of-tree node implementations
     * loaded as plugins are expected to override it with the plugin's
     * identifier so the palette / inspector can surface a node's
     * provenance.  Read via pn_node_class_get_plugin_name() (or its
     * instance-side convenience pn_node_get_plugin_name()).  The
     * string is owned by the class and must outlive every instance.
     */
    const gchar *plugin_name;

    /**
     * PnNodeClass.category:
     *
     * Display group the node belongs to in the palette
     * (<literal>"Sources"</literal>, <literal>"Network"</literal>,
     * <literal>"Filters"</literal>, <literal>"Sinks"</literal>, …).
     * Each concrete leaf class sets its own value; abstract
     * intermediates leave it %NULL.  Read via
     * pn_node_class_get_category() / pn_node_get_category().  The
     * string is owned by the class and must outlive every instance.
     */
    const gchar *category;

    /**
     * PnNodeClass::get_size:
     * @self:       node instance
     * @out_width:  (out): full footprint width in worksheet pixels
     * @out_height: (out): full footprint height in worksheet pixels
     *
     * Reports the bounding rectangle the worksheet should paint and
     * hit-test against.  Subclasses that extend the canonical 140×40
     * footprint (for example, a graph node that draws a plot below
     * its header) override this to widen or lengthen the rectangle.
     * The default returns the canonical size.
     */
    void (*get_size) (PnNode *self,
                      double *out_width,
                      double *out_height);

    /**
     * PnNodeClass::get_header_height:
     * @self: node instance
     *
     * Reports the height of the *header* portion of the node — the
     * part painted in the standard Node-RED style and to which the
     * input/output ports are anchored.  For nodes that extend their
     * footprint downward (e.g. graph nodes) this stays at the
     * canonical 40 px while #get_size returns the full extended
     * height.  The default matches the height returned by #get_size.
     */
    double (*get_header_height) (PnNode *self);

    /**
     * PnNodeClass::paint_plot:
     * @self: node instance
     * @cr:   cairo context the worksheet is drawing into
     * @x:    plot rectangle's top-left x in worksheet coordinates
     * @y:    plot rectangle's top-left y in worksheet coordinates
     * @w:    plot rectangle width in worksheet pixels
     * @h:    plot rectangle height in worksheet pixels
     *
     * Optional post-pass painter for nodes whose footprint extends
     * below the header (graph, table, …).  When non-%NULL the
     * worksheet (a) draws this painter into the rectangle directly
     * under the header during the per-frame redraw, (b) treats
     * primary-button presses inside that rectangle as a request to
     * lift the node into the centred zoom overlay, and (c) calls
     * this painter again with the animated rectangle to draw the
     * lifted copy.  %NULL leaves the node as a header-only standard-
     * style node with no plot extension.
     */
    void (*paint_plot) (PnNode  *self,
                        cairo_t *cr,
                        double   x,
                        double   y,
                        double   w,
                        double   h);

    /**
     * PnNodeClass.paint_plot_skip_shadow:
     *
     * When %FALSE (the default) the worksheet paints its standard
     * drop shadow underneath the rectangle handed to
     * #PnNodeClass.paint_plot — what every existing plot extension
     * (graph, table, …) expects so the plot reads as a card lifted
     * off the canvas.  Node types whose plot extension is not
     * actually rectangular (e.g. a circular dial face) set this to
     * %TRUE so the rectangular shadow does not leak out around the
     * non-rectangular shape.  Node types whose plot extension is a
     * rounded rectangle should instead declare
     * #PnNodeClass.paint_plot_corner_radius so the shadow follows
     * the rounded silhouette.
     */
    gboolean paint_plot_skip_shadow;

    /**
     * PnNodeClass.paint_plot_corner_radius:
     *
     * Corner radius (worksheet pixels) of the plot extension's
     * silhouette.  The worksheet uses it when painting the drop
     * shadow under #PnNodeClass.paint_plot so the shadow's outline
     * matches a rounded plot body (a character-LCD bezel, a beveled
     * readout) instead of leaking out around its corners.  Defaults
     * to %0.0 — a sharp-cornered rectangular silhouette, which is
     * the shape every legacy plot extension (graph, table) draws.
     * Ignored when #PnNodeClass.paint_plot_skip_shadow is %TRUE.
     */
    gdouble paint_plot_corner_radius;

    /**
     * PnNodeClass.paint_plot_skip_zoom:
     *
     * When %FALSE (the default) a primary-button press inside the
     * plot extension lifts the node into the worksheet's centred
     * zoom overlay — the right answer for graph / table plots, where
     * the at-rest footprint is too small to read the data and the
     * enlarged copy is how the user actually looks at it.  Node
     * types whose at-rest footprint is already the "real" reading
     * surface (analog meters, gauges, status dials) set this to
     * %TRUE so a press selects / drags the node like any other
     * header-only node instead of lifting it.
     */
    gboolean paint_plot_skip_zoom;

    /**
     * PnNodeClass.paint_plot_zoom_keep_aspect:
     *
     * When %FALSE (the default) the centred zoom overlay stretches the
     * lifted plot to fill the viewport (minus a fixed margin) on both
     * axes — the right answer for graph / table plots, whose content
     * reflows to whatever rectangle it is given.  Node types whose plot
     * extension shows fixed-aspect content (an image preview, a file
     * thumbnail) set this to %TRUE so the enlarged copy is fitted inside
     * the viewport while preserving the at-rest plot rectangle's aspect
     * ratio, then centred — the maximised view keeps the same shape as
     * the minimised one instead of distorting the image.
     */
    gboolean paint_plot_zoom_keep_aspect;

    /**
     * PnNodeClass::paint_header_overlay:
     * @self:    node instance
     * @cr:      cairo context the worksheet is drawing into
     * @x:       node rectangle's top-left x in worksheet coordinates
     * @y:       node rectangle's top-left y in worksheet coordinates
     * @w:       full footprint width in worksheet pixels
     * @h:       header height in worksheet pixels
     *
     * Optional decoration painter called by the worksheet *after* the
     * standard header (body fill, icon, label, outline) has been drawn
     * and *before* any #PnNodeClass.paint_plot extension.  Gives a node
     * type a chance to overlay extra graphics on top of its own
     * rectangle without having to take over the standard painter — used
     * by node types whose visual identity includes a small fixed
     * decoration that should live inside the rectangle (e.g. a status
     * LED on the right edge).  Combine with
     * #PnNodeClass.right_decoration_width to keep the label area from
     * sliding under the decoration.
     */
    void (*paint_header_overlay) (PnNode  *self,
                                  cairo_t *cr,
                                  double   x,
                                  double   y,
                                  double   w,
                                  double   h);

    /**
     * PnNodeClass.right_decoration_width:
     *
     * Number of pixels on the right edge of the node header that the
     * standard label painter should treat as reserved for a class-owned
     * decoration painted by #PnNodeClass.paint_header_overlay.  The
     * label area shrinks by this amount on the right so the centred
     * label does not slide under (or past) the decoration.  Defaults to
     * 0 — most nodes have no decoration and the entire post-icon area
     * is label space, exactly as before.
     */
    double paint_right_decoration_width;

    /**
     * PnNodeClass::scroll:
     * @self: node instance
     * @dy:   smooth-scroll delta on the Y axis (positive = scroll
     *        down).  Discrete wheel notches arrive as ±1.
     *
     * Optional handler invoked by the worksheet when this node is
     * the currently-zoomed overlay and the user spins the mouse
     * wheel over it.  %NULL means wheel events over the overlay are
     * ignored — the worksheet propagates them so an enclosing
     * scrollable can handle them instead.
     */
    void (*scroll) (PnNode *self, double dy);

    /**
     * PnNodeClass::build_property_editor:
     * @self:           node instance whose settings dialog is being built
     * @pspec:          the GObject property the editor row is being built for
     * @target:         the #GObject the editor must bind to (the same
     *                  instance as @self, cast to #GObject — provided
     *                  for symmetry with the host-side default editor
     *                  helper that takes a generic #GObject)
     * @dialog_parent:  the #PnNodeDialog #GtkWindow, suitable for use
     *                  as the transient-for of any child dialog (file
     *                  chooser, confirmation prompt) the editor wants
     *                  to spawn
     *
     * Optional override letting a subclass build a custom editor
     * widget for one specific property in this class's auto-generated
     * property tab — used when a #GtkEntry / #GtkSpinButton / etc. is
     * the wrong shape for the value (e.g. a serial-port path that
     * should pick from a sysfs walk, a file path that should open a
     * #GtkFileChooserButton, a numeric value with non-trivial
     * dependencies on another property).  The widget is expected to
     * bind itself to @target via the standard GObject machinery
     * (g_object_bind_property(), g_signal_connect_object()) so the
     * dialog's normal lifetime semantics — handlers auto-disconnect
     * when the editor widget is destroyed alongside the dialog —
     * still apply.  The host's default editor builder is exposed as
     * pn_node_dialog_default_editor() so subclasses can delegate
     * properties they do not want to customise back to it.  Returning
     * %NULL falls back to the host default for @pspec — the typical
     * shape for a class that customises one or two properties and
     * leaves the rest alone.
     */
    GtkWidget *(*build_property_editor) (PnNode      *self,
                                         GParamSpec  *pspec,
                                         GObject     *target,
                                         GtkWindow   *dialog_parent);

    /**
     * PnNodeClass::build_class_tab:
     * @self:           node instance whose settings dialog is being built
     * @dialog_parent:  the #PnNodeDialog #GtkWindow
     *
     * Optional override replacing this class's auto-generated
     * property tab with a fully custom widget.  Returning %NULL keeps
     * the introspection-driven default that lays out one editor row
     * per writable property the class declares.  Used by node types
     * whose configuration is not naturally a flat property form —
     * #PnFilter (a list of {path, op, literal} rules with per-row
     * widgets and an Add button), #PnSet (a list of {path, literal}
     * assignments), and the same shape of structured / list-shaped
     * configuration in third-party plugins (route tables, key
     * bindings, channel maps).  The returned widget is dropped into
     * the class's notebook tab as-is.  pn_node_dialog_new_property_grid()
     * and pn_node_dialog_attach_row() are exposed as helpers so a
     * partially-custom tab can keep the host's standard layout for
     * the rows it does not want to take over.
     *
     * Ignored when #PnNodeClass.build_class_tabs is also set --
     * `build_class_tabs` takes precedence and contributes multiple
     * top-level pages of its own.
     */
    GtkWidget *(*build_class_tab) (PnNode    *self,
                                   GtkWindow *dialog_parent);

    /**
     * PnNodeClass::build_class_tabs:
     * @self:           node instance whose settings dialog is being built
     * @notebook:       the dialog's #GtkNotebook
     * @dialog_parent:  the #PnNodeDialog #GtkWindow
     *
     * Optional override letting a class contribute *multiple*
     * top-level pages in place of its single auto-generated property
     * tab -- the right shape for node types that carry enough
     * properties to make a single tab uncomfortably tall and whose
     * properties group naturally by theme.  #PnDial is the worked
     * in-tree example: 21 properties grouped into Data / Scale /
     * Zones / Colours, contributed as four sibling pages so the
     * dialog shows
     *
     *     Class | Node | Data | Scale | Zones | Colours
     *
     * with each page only as tall as its own row set, rather than a
     * single Dial tab tall enough to summarise all four categories.
     *
     * Implementations call pn_node_dialog_append_page() once per page
     * they want -- the helper appends straight onto the dialog's
     * top-level notebook so the contributed pages sit alongside the
     * Class / Node / sibling-class tabs without any visible nesting.
     *
     * When non-%NULL this hook is called *instead of* the standard
     * per-class tab path (no #PnNodeClass.build_class_tab override
     * and no introspection-driven default tab are added for this
     * class).  #PnNodeClass.build_extra_pages still runs after every
     * class in the chain has had its turn.
     */
    void (*build_class_tabs) (PnNode      *self,
                              GtkNotebook *notebook,
                              GtkWindow   *dialog_parent);

    /**
     * PnNodeClass::build_extra_pages:
     * @self:           node instance whose settings dialog is being built
     * @notebook:       the dialog's #GtkNotebook
     * @dialog_parent:  the #PnNodeDialog #GtkWindow
     *
     * Optional hook for appending additional notebook pages beyond
     * the per-class property tab the dialog auto-generates.  Called
     * once per dialog open, after the per-class tabs have been
     * populated and in chain order so the leaf class's pages append
     * last.  Implementations call pn_node_dialog_append_page() (or
     * gtk_notebook_append_page() directly) for each extra page they
     * want — typically things that are not naturally a property form:
     * a Diagnostics tab streaming live signal strength, a Logs tail,
     * a one-shot "Scan for devices" button.  Signal handlers wired
     * to the node should use g_signal_connect_object() against a
     * widget owned by the new page so they auto-disconnect with the
     * dialog.
     */
    void (*build_extra_pages) (PnNode      *self,
                               GtkNotebook *notebook,
                               GtkWindow   *dialog_parent);

    /**
     * PnNodeClass::get_client_area:
     * @self:  node instance
     * @out_x: (out) (optional): client-area left edge, in node-local
     *         coordinates (an offset from the node's top-left origin)
     * @out_y: (out) (optional): client-area top edge, node-local
     * @out_w: (out) (optional): client-area width in worksheet pixels
     * @out_h: (out) (optional): client-area height in worksheet pixels
     *
     * Reports the node's "client area" — the rectangular region below
     * the header that the node fills with its own content (a table's
     * grid, a graph's plot, a text view's body), as distinct from the
     * standard Node-RED-style header every node draws.  Returns %TRUE
     * and fills the rectangle (relative to the node origin) for a node
     * that has a body; %FALSE for a header-only node (Debug, Comparator,
     * …), in which case the out-parameters are zeroed.
     *
     * The default derives this from pure geometry: when #get_size
     * reports a footprint taller than #get_header_height (past the
     * header→body gap) the surplus below the header is the client area —
     * exactly the rectangle the worksheet hands #PnNodeClass.paint_plot
     * for a node that installs one.  A node whose body is not that simple
     * under-the-header rectangle can override this to describe its own.
     * Appended at the end of the vtable in plugin ABI v4 so existing
     * field offsets stay stable.
     */
    gboolean (*get_client_area) (PnNode *self,
                                 double *out_x,
                                 double *out_y,
                                 double *out_w,
                                 double *out_h);
};

/**
 * pn_node_new:
 *
 * Creates a bare base-class node instance.  Most callers will create
 * instances of a derived type instead; this exists so the painter and
 * tests can build minimal node objects without subclassing.
 */
PnNode         *pn_node_new            (void);

/* Property accessors.  The "position" accessors return a borrowed
 * pointer into the node's internal storage; copy with pn_point_copy()
 * if you need to outlive the node. */

const PnColor  *pn_node_get_color      (PnNode *self);
void            pn_node_set_color      (PnNode *self, const PnColor *color);

const gchar    *pn_node_get_class_name (PnNode *self);
void            pn_node_set_class_name (PnNode *self, const gchar *class_name);

/* Class-level accessors.  These are the canonical "static" methods
 * for the type — they read the values pinned by the class on its
 * #PnNodeClass struct and do not touch any instance state, so they
 * can be called with just a class pointer (e.g. one obtained from
 * g_type_class_ref() on a GType, without instantiating the node). */
const gchar    *pn_node_class_get_plugin_name (PnNodeClass *klass);
const gchar    *pn_node_class_get_category    (PnNodeClass *klass);
const gchar    *pn_node_class_get_class_name  (PnNodeClass *klass);
const gchar    *pn_node_class_get_icon        (PnNodeClass *klass);
const PnColor  *pn_node_class_get_color       (PnNodeClass *klass);
gboolean        pn_node_class_get_has_input   (PnNodeClass *klass);
gboolean        pn_node_class_get_has_output  (PnNodeClass *klass);

/* Instance-side conveniences that resolve the class internally so
 * callers with an instance in hand do not have to spell out
 * PN_NODE_GET_CLASS at every site.  Note: pn_node_get_class_name(),
 * pn_node_get_icon(), pn_node_get_color() are existing instance
 * accessors that read the per-instance override property; the
 * class-level forms above bypass that and report the type's
 * canonical value. */
const gchar    *pn_node_get_plugin_name (PnNode *self);
const gchar    *pn_node_get_category    (PnNode *self);

const gchar    *pn_node_get_name       (PnNode *self);
void            pn_node_set_name       (PnNode *self, const gchar *name);

/**
 * pn_node_get_uuid:
 * @self: the node
 *
 * Returns a stable, per-node identifier minted at construction and
 * preserved across save/load.  Used by the message envelope's
 * `from_id` field so consumers (e.g. the debug pane) can map a
 * received message back to its source node unambiguously even when
 * two nodes share a display name.  The returned string is owned by
 * the node and remains valid for its lifetime.
 */
const gchar    *pn_node_get_uuid       (PnNode *self);

/**
 * pn_node_set_uuid:
 * @self: the node
 * @uuid: replacement identifier, or %NULL to mint a fresh one
 *
 * Replaces the node's UUID.  Intended for the deserialiser (which
 * restores the value persisted on disk) and the clipboard paster
 * (which mints a fresh UUID so a pasted node never collides with the
 * original).  Application code outside those two paths should leave
 * the construction-time value alone.
 */
void            pn_node_set_uuid       (PnNode *self, const gchar *uuid);

const gchar    *pn_node_get_icon       (PnNode *self);
void            pn_node_set_icon       (PnNode *self, const gchar *icon);

const PnPoint  *pn_node_get_position   (PnNode *self);
void            pn_node_set_position   (PnNode *self, const PnPoint *position);

gboolean        pn_node_get_has_input  (PnNode *self);
void            pn_node_set_has_input  (PnNode *self, gboolean has_input);

/**
 * pn_node_get_n_inputs:
 *
 * Number of input ports the node exposes.  Single-input nodes report
 * 1 (0 when they have no input at all); a multi-input node reports the
 * count it set with pn_node_set_n_inputs().  The worksheet uses this
 * to lay out and hit-test the input tabs, and wires address a specific
 * port by index (see pn_wire_get_target_input()).
 */
gint            pn_node_get_n_inputs   (PnNode *self);

/**
 * pn_node_set_n_inputs:
 * @n: number of input ports (>= 0)
 *
 * Declares that the node has @n inputs.  Messages delivered to input
 * @i are dispatched via pn_node_receive_message_on_input(); inside its
 * `receive` handler the node learns @i from pn_node_current_input().
 * Also keeps the has-input boolean consistent (TRUE when @n >= 1).
 */
void            pn_node_set_n_inputs   (PnNode *self, gint n);

/* Height of one stacked input row in the lower section of a multi-input
 * node.  When a node has more than one input the ports no longer sit on
 * the header edge; instead the footprint grows downward by one row per
 * input below the header (a section visually fenced off by a framing
 * line) and each port is drawn on its own row beside its name.  Shared
 * by the core footprint maths (pn_node_get_size) and the GTK painter so
 * both agree on where the rows sit. */
#define PN_NODE_INPUT_ROW_HEIGHT 20.0

/**
 * pn_node_get_input_section_height:
 * @self: the node
 *
 * Extra height, in pixels, that a multi-input node reserves below its
 * header for the stacked per-input rows.  Returns 0 for input-less and
 * single-input nodes (their footprint is the header alone).  Folded into
 * the default #PnNodeClass.get_size so every multi-input node grows
 * automatically without overriding geometry.
 *
 * Returns: the lower-section height (0 when @self has <= 1 input).
 */
double          pn_node_get_input_section_height (PnNode *self);

/**
 * pn_node_get_input_name:
 * @self: the node
 * @index: 0-based input index
 *
 * Human-readable name painted beside input @index on a multi-input node.
 * Defaults to "value1", "value2", … when the node has not set one with
 * pn_node_set_input_name().  The returned string is owned by @self.
 *
 * Returns: (transfer none): the input's display name.
 */
const gchar *   pn_node_get_input_name (PnNode *self, gint index);

/**
 * pn_node_get_input_value_display:
 * @self: the node
 * @index: 0-based input index
 *
 * Short human-readable rendering of input @index's most recent /data/value,
 * maintained for every multi-input node so the worksheet can paint it beside
 * the input's name.  Vectors are elided to a bounded sample exactly like the
 * debug pane ("[0, 1, 2, …] (256 values)").  Returns %NULL until that input
 * has carried a value (and for single-input nodes, which keep no readout).
 * The returned string is owned by @self and only valid until the next
 * message arrives on that input.
 *
 * Returns: (transfer none) (nullable): the input's last-value display string.
 */
const gchar *   pn_node_get_input_value_display (PnNode *self, gint index);

/**
 * pn_node_set_input_name:
 * @self: the node
 * @index: 0-based input index
 * @name: (nullable): display name, or %NULL/empty to restore the default
 *
 * Overrides the name painted beside input @index (see
 * pn_node_get_input_name()).  Passing %NULL or "" reverts that input to
 * its "valueN" default.
 */
void            pn_node_set_input_name (PnNode *self,
                                        gint    index,
                                        const gchar *name);

/**
 * pn_node_set_input_count_property:
 * @self: the node
 * @prop_name: (nullable): name of an int GObject property controlling the
 *   input count, or %NULL/empty to declare the count fixed
 *
 * Declares that the node's input count is user-adjustable through the named
 * (integer) GObject property — e.g. the Calculator's "inputs".  The node
 * dialog uses this purely as a UI hint: it renders a spin button for the
 * property (its range read from the property's #GParamSpec) on a dedicated
 * "Inputs" tab and rebuilds the per-input name fields live as the count
 * changes.  The count value itself is carried by that ordinary property
 * (and so persists through normal serialization); this declaration is
 * transient and never serialized.
 */
void            pn_node_set_input_count_property (PnNode      *self,
                                                  const gchar *prop_name);

/**
 * pn_node_get_input_count_property:
 * @self: the node
 *
 * Returns the property name set by pn_node_set_input_count_property(), or
 * %NULL when the node has a fixed input count.  The string is owned by
 * @self.
 *
 * Returns: (transfer none) (nullable): the input-count property name.
 */
const gchar *   pn_node_get_input_count_property (PnNode *self);

/**
 * pn_node_get_collate_inputs:
 * @self: the node
 *
 * Returns whether the node opted into core input-value collation.
 */
gboolean        pn_node_get_collate_inputs (PnNode *self);

/**
 * pn_node_set_collate_inputs:
 * @self: the node
 * @collate: %TRUE to enable core input-value collation
 *
 * Opt a multi-input node into automatic value collation.  While enabled,
 * and the node has two or more inputs, the core latches each input's last
 * /data/value (as a typed node) and re-injects every latched value into
 * each arriving message's data bag under its input's display name (see
 * pn_node_get_input_name) before the node's receive() runs.  A node that
 * combines its inputs — e.g. the Calculator — thus sees all of them at
 * once without tracking per-input state itself.  Typically called once
 * from the node's own init().  Runtime-only; never serialized.
 */
void            pn_node_set_collate_inputs (PnNode *self, gboolean collate);

gboolean        pn_node_get_has_output (PnNode *self);
void            pn_node_set_has_output (PnNode *self, gboolean has_output);

gboolean        pn_node_get_disabled   (PnNode *self);
void            pn_node_set_disabled   (PnNode *self, gboolean disabled);

/**
 * pn_node_get_has_error:
 * @self: the node
 *
 * Returns: whether the node is currently in its error state.
 */
gboolean        pn_node_get_has_error  (PnNode *self);

/**
 * pn_node_set_has_error:
 * @self:      the node
 * @has_error: the new error state
 *
 * Sets the node's transient "I am in an error state" flag.  A node calls
 * this when its work fails (e.g. a network connection drops or an operation
 * is rejected) and clears it on recovery.  While set, the worksheet paints
 * the node body red with a warning glyph, giving every node type a uniform
 * error indication for free.  The flag is purely visual runtime state and is
 * never written to the save file.
 */
void            pn_node_set_has_error  (PnNode *self, gboolean has_error);

/**
 * pn_node_processing_begin:
 * @self: the node
 *
 * Marks the start of a unit of work on @self, lighting its transient
 * "processing" indicator.  Calls nest: this is a refcount, so a node may
 * bracket several overlapping or concurrent work items (e.g. one per worker
 * thread) and the node reads as busy until every one has ended.  Pair each
 * call with exactly one pn_node_processing_end().
 *
 * Thread-safe: may be called from any thread.  The "processing-changed"
 * signal it raises on the idle→busy edge is marshalled onto the default
 * main context, so handlers always run on the UI thread.  The state is
 * purely visual and is never serialized.
 */
void            pn_node_processing_begin (PnNode *self);

/**
 * pn_node_processing_end:
 * @self: the node
 *
 * Marks the end of a unit of work started with pn_node_processing_begin().
 * When the outstanding count returns to zero the node keeps reading as
 * "visibly busy" for a short minimum linger (~250 ms) so even a
 * sub-millisecond unit of work still produces a perceptible glow.
 * Thread-safe; an unbalanced call (count already zero) is a no-op warning.
 */
void            pn_node_processing_end   (PnNode *self);

/**
 * pn_node_is_processing:
 * @self: the node
 *
 * Returns: %TRUE while one or more units of work are outstanding (the
 * refcount is above zero), ignoring the minimum-visible linger.
 */
gboolean        pn_node_is_processing    (PnNode *self);

/**
 * pn_node_is_processing_visible:
 * @self:   the node
 * @now_us: the current monotonic time (g_get_monotonic_time()), passed in
 *          so a caller polling many nodes samples the clock once
 *
 * Returns: %TRUE if the node should be drawn as busy at @now_us — either
 * work is outstanding, or it ended so recently that the minimum-visible
 * linger has not yet elapsed.  This is what the worksheet's poll consults.
 */
gboolean        pn_node_is_processing_visible (PnNode *self, gint64 now_us);

/**
 * pn_node_log:
 * @self:   the node
 * @level:  severity of the entry
 * @format: a printf-style format string
 * @...:    arguments for @format
 *
 * Records a diagnostic line in the node's in-memory log ring.  This is
 * the canonical way for a node implementation to report what it is
 * doing or why it failed: the message is kept on the node (the oldest
 * entries are dropped once the ring is full) and shown in the per-node
 * Log dialog, rather than printed to stdout / stderr where it would be
 * invisible under a desktop launcher.  Emits #PnNode::log-changed so an
 * open Log dialog refreshes live.  Safe to call from the node's own
 * code at any time.
 */
void            pn_node_log            (PnNode      *self,
                                        PnLogLevel   level,
                                        const gchar *format,
                                        ...) G_GNUC_PRINTF (3, 4);

/**
 * pn_node_logv:
 * @self:   the node
 * @level:  severity of the entry
 * @format: a printf-style format string
 * @args:   arguments for @format as a #va_list
 *
 * <function>va_list</function> variant of pn_node_log().
 */
void            pn_node_logv           (PnNode      *self,
                                        PnLogLevel   level,
                                        const gchar *format,
                                        va_list      args) G_GNUC_PRINTF (3, 0);

/* Convenience wrappers around pn_node_log() that pin the level. */
void            pn_node_log_info       (PnNode      *self,
                                        const gchar *format,
                                        ...) G_GNUC_PRINTF (2, 3);
void            pn_node_log_warning    (PnNode      *self,
                                        const gchar *format,
                                        ...) G_GNUC_PRINTF (2, 3);
void            pn_node_log_error      (PnNode      *self,
                                        const gchar *format,
                                        ...) G_GNUC_PRINTF (2, 3);

/**
 * pn_node_get_log:
 * @self: the node
 *
 * Returns: (transfer none) (element-type PnLogEntry): the node's log
 *   ring, oldest entry first.  Borrowed — owned by the node and valid
 *   until the next pn_node_log() / pn_node_clear_log() call; copy any
 *   entry you need to outlive that with pn_log_entry_copy().
 */
GPtrArray      *pn_node_get_log        (PnNode *self);

/**
 * pn_node_clear_log:
 * @self: the node
 *
 * Empties the node's log ring and emits #PnNode::log-changed.
 */
void            pn_node_clear_log      (PnNode *self);

/**
 * pn_node_get_worksheet:
 * @self: the node
 *
 * Returns the name of the worksheet (a spreadsheet-style sheet
 * tag) the node is filed under.  Multiple worksheets are not yet
 * surfaced in the UI; the property exists so a future commit can
 * grow per-sheet tabs without forcing a save-format migration.
 * Defaults to <literal>"Worksheet"</literal> on every freshly-
 * constructed node and on every node loaded from a worksheet
 * file that predates the field, which keeps existing flows
 * round-tripping unchanged.  The returned string is owned by the
 * node and remains valid for its lifetime.
 */
const gchar    *pn_node_get_worksheet  (PnNode *self);
void            pn_node_set_worksheet  (PnNode *self, const gchar *worksheet);

/**
 * pn_node_get_topic:
 * @self: the node
 *
 * Returns the raw topic template the user configured for this node,
 * or %NULL when the node is using its built-in default.  The returned
 * string may contain unresolved <literal>${nodeclass}</literal>,
 * <literal>${nodename}</literal> and <literal>${hostname}</literal>
 * placeholders — the configuration dialog shows the template as-is so
 * the user can edit the literal form.  Use pn_node_resolve_topic() to
 * get the fully-substituted topic that should be stamped onto an
 * outgoing message envelope.  The returned string is owned by the
 * node and remains valid for its lifetime.
 */
const gchar    *pn_node_get_topic      (PnNode *self);

/**
 * pn_node_set_topic:
 * @self:  the node
 * @topic: replacement template, or %NULL / "" to revert to the
 *         built-in default
 *
 * Stores @topic as the per-node topic template.  Setting %NULL or an
 * empty string clears the override and makes the node fall back to
 * pn_node_default_topic_template() — the dialog uses this so the user
 * can wipe the entry to restore the default.
 */
void            pn_node_set_topic      (PnNode *self, const gchar *topic);

/**
 * pn_node_default_topic_template:
 * @self: the node
 *
 * Returns the topic template the node would use if the user has not
 * configured an override.  The default is
 * <literal>"/pnode/${nodeclass}/${nodename}"</literal>; nodes whose
 * #GObject class declares a writable string <literal>hostname</literal>
 * property (every host-monitoring node and every shell/SSH-flavoured
 * node) get
 * <literal>"/pnode/${nodeclass}/${nodename}/${hostname}"</literal>
 * instead so the executing host appears in the topic.  Returned string
 * is freshly allocated; caller frees with g_free().
 */
gchar          *pn_node_default_topic_template (PnNode *self);

/**
 * pn_node_resolve_topic:
 * @self: the node
 *
 * Returns the node's effective topic with every
 * <literal>${nodeclass}</literal>, <literal>${nodename}</literal> and
 * <literal>${hostname}</literal> placeholder substituted.  The template
 * is the user override (pn_node_get_topic()) when set, otherwise
 * pn_node_default_topic_template().
 *
 * Substitutions:
 *   - <literal>${nodeclass}</literal> — the type's display label
 *     (PnNodeClass.class_name), e.g. "CPU" / "Clock".
 *   - <literal>${nodename}</literal> — the user-supplied instance name,
 *     falling back to the class label when the user has not named the
 *     node.
 *   - <literal>${hostname}</literal> — for nodes with a
 *     <literal>hostname</literal> property, the configured value; when
 *     that value is empty or <literal>"localhost"</literal>, or for
 *     nodes without a <literal>hostname</literal> property at all, the
 *     executing host's name as reported by g_get_host_name().  This is
 *     deliberately resolved at emit time so the same worksheet file
 *     stamps host1 when run on host1 and host2 when run on host2.
 *
 * The returned string is freshly allocated; caller frees with g_free().
 */
gchar          *pn_node_resolve_topic  (PnNode *self);

/**
 * pn_node_dup_subst_pairs:
 * @self: the node
 *
 * Builds the node's substitution variables as a %NULL-terminated
 * { "nodeclass", value, "nodename", value, "hostname", value, %NULL }
 * array suitable for pn_subst_resolver_strv().  The values follow the
 * same rules as pn_node_resolve_topic() (class label, instance name with
 * class-label fallback, configured-or-local hostname).  Both the keys
 * and values are freshly allocated; free the whole array with
 * g_strfreev().
 *
 * This lets any node interpolate ${nodeclass} / ${nodename} /
 * ${hostname} into one of its own settings (a shell command, a URL, an
 * MQTT topic) without a message in hand.
 */
gchar         **pn_node_dup_subst_pairs (PnNode *self);

/**
 * pn_node_expand_vars:
 * @self: the node
 * @tmpl: a template, or %NULL (treated as "")
 *
 * Expands <literal>${nodeclass}</literal>, <literal>${nodename}</literal>
 * and <literal>${hostname}</literal> in @tmpl against the node's own
 * variables (see pn_node_dup_subst_pairs()), then falls back to the
 * owning flow's document globals.  Any other placeholder is left
 * verbatim, so an unrelated <literal>${var}</literal> — such as a
 * shell one-liner's <literal>${HOME}</literal> — survives untouched for
 * whoever consumes the string.  Use this for node settings that should
 * interpolate node identity without an incoming message (a shell
 * command, a request URL).  The returned string is freshly allocated;
 * caller frees with g_free().
 */
gchar          *pn_node_expand_vars     (PnNode *self, const gchar *tmpl);

/**
 * pn_node_get_flow:
 * @self: the node
 *
 * Returns: (transfer none) (nullable): the #PnFlow that owns this node,
 *   or %NULL when the node has not been added to a flow.  Borrowed.
 */
PnFlow         *pn_node_get_flow        (PnNode *self);

/**
 * pn_node_set_flow: (skip)
 * @self: the node
 * @flow: (nullable): the owning flow, or %NULL to clear
 *
 * Library-internal: called by #PnFlow when the node is added to or
 * removed from its store.  Stores a borrowed pointer (the flow outlives
 * the node).  Not for application use.
 */
void            pn_node_set_flow        (PnNode *self, PnFlow *flow);

/**
 * pn_node_get_size:
 * @self:  the node
 * @out_w: (out): width in worksheet pixels
 * @out_h: (out): height in worksheet pixels
 *
 * Convenience wrapper around #PnNodeClass.get_size.  Always returns
 * a non-zero size — the default implementation produces the canonical
 * 140×40 footprint when the class did not override the vfunc.
 */
void            pn_node_get_size       (PnNode *self,
                                        double *out_w,
                                        double *out_h);

/**
 * pn_node_get_header_height:
 * @self: the node
 *
 * Convenience wrapper around #PnNodeClass.get_header_height.  Used
 * by the worksheet to anchor port positions and label centring to
 * the standard-style portion of nodes whose full footprint extends
 * past the header.
 */
double          pn_node_get_header_height (PnNode *self);

/**
 * pn_node_get_client_area:
 * @self:  the node
 * @out_x: (out) (optional): client-area left, in node-local coordinates
 * @out_y: (out) (optional): client-area top, node-local
 * @out_w: (out) (optional): client-area width
 * @out_h: (out) (optional): client-area height
 *
 * Convenience wrapper around #PnNodeClass.get_client_area.  Returns
 * %TRUE and fills the node-local client-area rectangle when the node
 * has a body below its header (table, graph, text view, …); %FALSE with
 * the out-parameters zeroed for a header-only node.  Add the node's
 * position to the returned (x, y) to place the rectangle on the
 * worksheet.
 */
gboolean        pn_node_get_client_area (PnNode *self,
                                         double *out_x,
                                         double *out_y,
                                         double *out_w,
                                         double *out_h);

/**
 * pn_node_request_repaint:
 * @self: the node
 *
 * Emits the #PnNode::repaint-needed signal.  Animated nodes whose
 * appearance changes outside the property-notify mechanism (for
 * example, a graph node that redraws when fresh data arrives) call
 * this from their receive path so the worksheet can queue a redraw.
 */
void            pn_node_request_repaint (PnNode *self);

/**
 * pn_node_emit_message:
 * @self:    the source node
 * @message: (transfer none): message to deliver
 *
 * Emits the "message" signal on @self.  This is the canonical way
 * for a node implementation to deliver a value to whatever is
 * connected to its output port; the worksheet (or a router) is
 * expected to forward the signal to downstream nodes.
 */
void            pn_node_emit_message   (PnNode *self, PnMessage *message);

/**
 * pn_node_get_last_output_message:
 * @self: the node
 *
 * Returns: (transfer none) (nullable): the most recent message @self
 * emitted via pn_node_emit_message(), or %NULL if it has not emitted
 * yet.  A private clone owned by the node (replaced on each emission);
 * the automation surface reads it back through GetLastOutputMessage
 * (TODO #40.13).  Never serialized.
 */
PnMessage      *pn_node_get_last_output_message (PnNode *self);

/**
 * pn_node_receive_message:
 * @self:    receiving node
 * @message: (transfer none): message to deliver
 *
 * Hands @message to @self's input.  Internally this dispatches to
 * the #PnNodeClass.receive virtual method, so concrete node types
 * react by overriding `receive` rather than by calling this from
 * within their own implementation.  Routers and tests call this to
 * inject a message into a node.  Equivalent to
 * pn_node_receive_message_on_input() with @input 0.
 */
void            pn_node_receive_message (PnNode *self, PnMessage *message);

/**
 * pn_node_receive_message_on_input:
 * @self:    receiving node
 * @message: (transfer none): message to deliver
 * @input:   index of the input port the message arrives on
 *
 * Like pn_node_receive_message() but tags the delivery with the input
 * port it arrived on, so a multi-input node can tell its ports apart.
 * The wire layer calls this with the wire's target input index; the
 * handler reads the index back with pn_node_current_input().
 */
void            pn_node_receive_message_on_input (PnNode    *self,
                                                  PnMessage *message,
                                                  gint       input);

/**
 * pn_node_current_input:
 *
 * Index of the input port the message currently being dispatched
 * arrived on.  Only meaningful while a #PnNodeClass.receive call is on
 * the stack; returns 0 otherwise.  Multi-input nodes call this from
 * their `receive` handler to route the message to the right port.
 */
gint            pn_node_current_input  (void);

/**
 * pn_node_get_dispatch_depth:
 *
 * Current synchronous message-dispatch recursion depth on this thread:
 * 0 at a top-level emission, incremented once per hop as a message
 * cascades through wired nodes.  Read from a "message-passed" handler
 * it gives the hop index of the wire that just fired, which the
 * worksheet uses to stagger the message-flow animation hop by hop.
 */
gint            pn_node_get_dispatch_depth (void);

/**
 * pn_param_spec_set_multiline:
 * @pspec: a string-typed #GParamSpec
 *
 * Marks @pspec as wanting a multi-line text editor in the node
 * settings dialog instead of the single-line entry that string
 * properties default to.  The hint is stored as #GQuark-keyed qdata
 * on the param spec, so it survives across the GObject machinery
 * without requiring a subclass-of-#PnNode runtime check in the host
 * dialog — node types implemented in plugins (whose concrete C type
 * is unknown to the host) can request the wider editor the same way
 * the in-tree types do.  Read with pn_param_spec_get_multiline().
 * Typically called once per property from the type's `_class_init`.
 */
void     pn_param_spec_set_multiline (GParamSpec *pspec);

/**
 * pn_param_spec_get_multiline:
 * @pspec: a #GParamSpec
 *
 * Returns whether @pspec was tagged via pn_param_spec_set_multiline().
 * The node-settings dialog calls this to decide whether to render the
 * tall #GtkTextView editor for a string property.  Returns %FALSE for
 * any param spec that was never tagged, including %NULL.
 */
gboolean pn_param_spec_get_multiline (GParamSpec *pspec);

/**
 * pn_param_spec_set_full_width:
 * @pspec: a #GParamSpec
 *
 * Marks @pspec as wanting its editor to span the full width of the
 * settings dialog row, with no caption label to its left.  The node
 * dialog normally lays each property out as a "<nick>: <editor>" pair;
 * a property tagged this way instead gets its editor attached across
 * both grid columns and the label is omitted.  Use it for a node whose
 * single editor is self-explanatory (e.g. a lone multi-line text body),
 * where the caption would only repeat the node's own name.  Stored as
 * #GQuark-keyed qdata like pn_param_spec_set_multiline(), so plugin
 * node types can opt in the same way.  Read with
 * pn_param_spec_get_full_width().
 */
void     pn_param_spec_set_full_width (GParamSpec *pspec);

/**
 * pn_param_spec_get_full_width:
 * @pspec: a #GParamSpec
 *
 * Returns whether @pspec was tagged via pn_param_spec_set_full_width().
 * The node-settings dialog calls this to decide whether to drop the
 * caption label and let the editor span the row.  Returns %FALSE for
 * any param spec that was never tagged, including %NULL.
 */
gboolean pn_param_spec_get_full_width (GParamSpec *pspec);

/**
 * pn_param_spec_set_hostname_hint:
 * @pspec: a string-typed #GParamSpec (typically the `"hostname"`
 *         property of a host-monitoring node)
 *
 * Marks @pspec so the node-settings dialog renders its #GtkEntry with
 * the local computer's name (as reported by g_get_host_name()) shown
 * as gray placeholder text whenever the field is empty.  The
 * placeholder is a visual hint only -- it tells the user which host
 * the node will actually probe when they leave the field blank -- and
 * does not change the stored value.  Tagging happens once in the
 * type's `_class_init`, right after the property is installed.
 */
void     pn_param_spec_set_hostname_hint (GParamSpec *pspec);

/**
 * pn_param_spec_get_hostname_hint:
 * @pspec: a #GParamSpec
 *
 * Returns whether @pspec was tagged via
 * pn_param_spec_set_hostname_hint().  The node-settings dialog calls
 * this to decide whether to attach the
 * g_get_host_name()-derived placeholder text to the entry it builds
 * for a string property.  Returns %FALSE for an untagged or %NULL
 * param spec.
 */
gboolean pn_param_spec_get_hostname_hint (GParamSpec *pspec);

/**
 * pn_param_spec_set_profile_ref:
 * @pspec:   a string-typed #GParamSpec that stores a profile *id*
 * @type_id: the profile type the property references (e.g. "mqtt-broker")
 *
 * Marks @pspec as a reference to a host-provisioned profile of @type_id (a
 * profile type registered with pn_node_factory_register_profile_type()).  The
 * property itself stores only the chosen profile's non-secret id, which is what
 * gets serialized into the workflow file; the real values (host, secrets,
 * permission grants) live in the vault and are resolved at runtime with
 * pn_node_get_profile().  The node-settings dialog renders a tagged property as
 * a profile picker (a combo of the vault's profiles of @type_id, with an empty
 * "Default" entry) instead of a plain entry.  Tag once in `_class_init`, right
 * after installing the property.
 */
void         pn_param_spec_set_profile_ref (GParamSpec  *pspec,
                                            const gchar *type_id);

/**
 * pn_param_spec_get_profile_ref:
 * @pspec: a #GParamSpec
 *
 * Returns: (transfer none) (nullable): the profile type id @pspec was tagged
 *   with via pn_param_spec_set_profile_ref(), or %NULL for an untagged or
 *   %NULL param spec.  Borrowed — valid for the lifetime of @pspec.
 */
const gchar *pn_param_spec_get_profile_ref (GParamSpec *pspec);

/**
 * PN_PROFILE_REF_CUSTOM:
 *
 * Reserved profile-ref value meaning "no profile — use the node's own inline
 * settings instead" (the "Custom settings" choice in the picker).  A profile-ref
 * property normally stores either "" (follow the type's primary profile) or a
 * vault profile id; this sentinel is the third state.  pn_node_get_profile()
 * returns %NULL for it (no primary fallback), so a node resolves to its inline
 * fields.  The leading '@' keeps it from colliding with a real vault profile id.
 * Only profile-ref properties tagged with
 * pn_param_spec_set_profile_ref_custom_trigger() offer this value.
 */
#define PN_PROFILE_REF_CUSTOM "@custom"

/**
 * pn_param_spec_set_profile_ref_inline_fields:
 * @pspec:  a string #GParamSpec already tagged with pn_param_spec_set_profile_ref()
 * @fields: comma-separated list of sibling inline property names that make up the
 *          node's "custom settings" (e.g. "url,username,password,client-id")
 *
 * Opts a profile-ref property into offering a "Custom settings" entry
 * (#PN_PROFILE_REF_CUSTOM) in its picker, alongside the "Default" entry and the
 * vault profiles.  @fields names the node's own inline properties that the
 * picker treats as the custom configuration: their editors are greyed out while
 * any non-Custom option is selected, and cleared (set to "") when the user picks
 * a non-Custom option — so inline data exists only in "Custom settings" mode.
 * Tag once in `_class_init`, after pn_param_spec_set_profile_ref().  The string
 * is copied.
 */
void         pn_param_spec_set_profile_ref_inline_fields (GParamSpec  *pspec,
                                                          const gchar *fields);

/**
 * pn_param_spec_get_profile_ref_inline_fields:
 * @pspec: a #GParamSpec
 *
 * Returns: (transfer none) (nullable): the comma-separated inline-field list
 *   @pspec was tagged with via pn_param_spec_set_profile_ref_inline_fields(), or
 *   %NULL when untagged.  A non-%NULL result means the picker should offer
 *   "Custom settings".  Borrowed — valid for the lifetime of @pspec.
 */
const gchar *pn_param_spec_get_profile_ref_inline_fields (GParamSpec *pspec);

/**
 * pn_param_spec_set_secret:
 * @pspec: a string-typed #GParamSpec holding a credential
 *
 * Marks @pspec as a secret (a password / token / key).  A secret property is
 * NEVER written to the workflow file — pn_flow's serializer skips it — so
 * secrets do not leak into shareable documents.  Credentials belong in the
 * host vault (#PnVault), reached through a profile reference; the inline
 * secret properties on the bundled nodes carry this tag only so legacy files
 * that still hold a plaintext value load (the value is imported into a
 * profile) without re-persisting the plaintext on the next save.  Tag once in
 * `_class_init`, right after installing the property.
 */
void     pn_param_spec_set_secret (GParamSpec *pspec);

/**
 * pn_param_spec_get_secret:
 * @pspec: a #GParamSpec
 *
 * Returns: whether @pspec was tagged via pn_param_spec_set_secret().  %FALSE
 *   for an untagged or %NULL param spec.
 */
gboolean pn_param_spec_get_secret (GParamSpec *pspec);

G_END_DECLS

#endif /* PN_NODE_H */
