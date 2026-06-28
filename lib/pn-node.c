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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-node.h"
#include "pn-message.h"
#include "pn-vector.h"
#include "pn-subst.h"
#include "pn-flow.h"

/* ------------------------------------------------------------------ */
/*  PnPoint                                                            */
/* ------------------------------------------------------------------ */

PnPoint *
pn_point_new (
        double x,
        double y)
{
    PnPoint *p = g_new (PnPoint, 1);
    p->x = x;
    p->y = y;
    return p;
}

PnPoint *
pn_point_copy (const PnPoint *self)
{
    return self ? pn_point_new (self->x, self->y) : NULL;
}

void
pn_point_free (PnPoint *self)
{
    g_free (self);
}

G_DEFINE_BOXED_TYPE (PnPoint, pn_point, pn_point_copy, pn_point_free)

/* ------------------------------------------------------------------ */
/*  PnLogEntry                                                         */
/* ------------------------------------------------------------------ */

struct _PnLogEntry
{
    gint64      timestamp;   /* g_get_real_time(): usec since the epoch */
    PnLogLevel  level;
    gchar      *message;
};

PnLogEntry *
pn_log_entry_copy (const PnLogEntry *self)
{
    PnLogEntry *copy;

    if (self == NULL)
        return NULL;

    copy            = g_new (PnLogEntry, 1);
    copy->timestamp = self->timestamp;
    copy->level     = self->level;
    copy->message   = g_strdup (self->message);
    return copy;
}

void
pn_log_entry_free (PnLogEntry *self)
{
    if (self == NULL)
        return;
    g_free (self->message);
    g_free (self);
}

G_DEFINE_BOXED_TYPE (PnLogEntry, pn_log_entry,
                     pn_log_entry_copy, pn_log_entry_free)

gint64
pn_log_entry_get_time (const PnLogEntry *self)
{
    g_return_val_if_fail (self != NULL, 0);
    return self->timestamp;
}

PnLogLevel
pn_log_entry_get_level (const PnLogEntry *self)
{
    g_return_val_if_fail (self != NULL, PN_LOG_LEVEL_INFO);
    return self->level;
}

const gchar *
pn_log_entry_get_message (const PnLogEntry *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->message;
}

const gchar *
pn_log_level_to_string (PnLogLevel level)
{
    switch (level)
    {
    case PN_LOG_LEVEL_WARNING: return "WARNING";
    case PN_LOG_LEVEL_ERROR:   return "ERROR";
    case PN_LOG_LEVEL_INFO:
    default:                   return "INFO";
    }
}

/* ------------------------------------------------------------------ */
/*  PnNode                                                             */
/* ------------------------------------------------------------------ */

/* Cap on a node's in-memory log ring.  Logging is rare relative to
 * message dispatch, so dropping the oldest entry from the head of a
 * #GPtrArray (an O(n) memmove of <= this many pointers) is cheap enough
 * not to warrant a real circular buffer. */
#define PN_NODE_LOG_MAX 200

typedef struct
{
    PnColor  color;
    gchar   *class_name;
    gchar   *name;
    gchar   *icon;
    /* Stable per-node identifier minted at construction by
     * g_uuid_string_random() and round-tripped through save/load.
     * Carried into the message envelope as `from_id` so the debug
     * pane (and any future log-replay consumer) can resolve a
     * message back to its source node even when two nodes share a
     * display name. */
    gchar   *uuid;
    PnPoint  position;
    gboolean has_input;
    gboolean has_output;
    /* Number of input ports, or 0 to mean "derive from has_input"
     * (0 → 0 ports, 1 → 1 port).  Only multi-input nodes set this
     * explicitly via pn_node_set_n_inputs(); every existing node keeps
     * the single-input behaviour with this left at 0. */
    gint     n_inputs;
    /* Per-input display names for multi-input nodes ("value1", … by
     * default; see pn_node_get_input_name).  Lazily allocated GPtrArray
     * of g_strdup'd strings (NULL slot ⇒ use the "valueN" default).
     * NULL until the first get/set. */
    GPtrArray *input_names;
    /* Opt-in (pn_node_set_collate_inputs) per-input latch of each input's
     * last /data/value, used by multi-input nodes that combine their
     * inputs.  Parallel to input_names: slot i holds an owned JsonNode
     * copy of input i's most recent /data/value (the *typed* node, not a
     * gdouble, so it keeps working when /data/value grows beyond double),
     * or NULL until input i has carried a value.  Before each receive the
     * core re-injects every latched value into the arriving message under
     * its input's display name (see pn_node_collate_inputs).  Lazily
     * allocated; NULL until first use.  Purely runtime — never serialized. */
    GPtrArray *input_latch;
    gboolean collate_inputs;
    /* Per-input short human-readable display of each input's last
     * /data/value, painted by the worksheet beside the input's name on a
     * multi-input node (see pn_node_get_input_value_display).  Parallel to
     * input_names: slot i holds an owned g_strdup'd string (vectors elided
     * to a bounded sample, "[0, 1, 2, …] (256 values)", just like the debug
     * pane), or NULL until input i has carried a value.  Unlike input_latch
     * this is maintained for EVERY multi-input node, not just collating ones
     * — it is purely a worksheet readout.  Lazily allocated; NULL until
     * first use.  Purely runtime — never serialized. */
    GPtrArray *input_value_str;
    /* Name of the GObject property a node exposes to let the user change
     * its input count (Calculator 2's "inputs"), or NULL when the count
     * is fixed.  Purely a UI hint: the node dialog renders a spin for this
     * property (range from its #GParamSpec) on a dedicated "Inputs" tab and
     * rebuilds the per-input name fields live as it changes.  The value
     * itself persists through that ordinary property; this string is
     * transient and never serialized. */
    gchar   *input_count_prop;
    gboolean disabled;
    /* Transient runtime "this node is in an error state" flag.  Set by a
     * node when its work fails (e.g. an MQTT broker connection drops or a
     * publish is rejected) and cleared on recovery.  Purely visual — the
     * worksheet painter draws the node body red with a warning glyph while
     * it is set, giving any node a uniform "I am broken" indication
     * without each type having to paint its own.  Not persisted: it is
     * recomputed at runtime, so it is deliberately left out of the on-disk
     * format (see should_serialize_property: PnNode-owned props are
     * skipped). */
    gboolean has_error;
    /* Transient "this node is doing work" indicator (TODO #42).  A
     * refcount, not a bool, so concurrent / overlapping work items — a
     * node may run several worker threads — compose: busy_count > 0 means
     * processing.  Guarded by processing_lock because
     * pn_node_processing_begin/end may be called from secondary threads.
     * busy_since_us (set on the idle→busy edge) and visible_until_us (set
     * on the busy→idle edge) drive the minimum-visible linger so a
     * sub-millisecond blip still animates.  Purely visual, recomputed at
     * runtime, never serialized — exactly like has_error above. */
    GMutex   processing_lock;
    gint     busy_count;
    gint64   busy_since_us;
    gint64   visible_until_us;
    /* Spreadsheet-style sheet tag.  No UI yet — the property is in
     * place so a future commit can grow per-sheet tabs without
     * requiring a save-format migration.  Defaults to "Worksheet"
     * so a freshly-constructed node, and any node loaded from a
     * pre-worksheet-property save, end up filed on the same
     * default sheet without callers having to know about the
     * field. */
    gchar   *worksheet;
    /* User-configured topic template, or %NULL when the node is using
     * its built-in default.  Stored verbatim — the ${nodeclass} /
     * ${nodename} / ${hostname} placeholders are kept unresolved here
     * so the configuration dialog can show and edit the template as
     * the user typed it; pn_node_resolve_topic() substitutes them at
     * emit time, which is what message envelopes carry. */
    gchar   *topic;
    /* Borrowed back-pointer to the owning #PnFlow, set by the flow when
     * the node is added to its store and cleared on removal.  Used so
     * substitution can fall back to the document globals.  Not reffed:
     * the flow owns the node through the store and always outlives it.
     * Accessed via g_atomic_pointer_* because the auto-trigger worker
     * thread reads it while the main thread sets it. */
    gpointer flow;
    /* In-memory log ring of #PnLogEntry*, oldest first, capped at
     * PN_NODE_LOG_MAX.  Filled by pn_node_log(); read by the per-node
     * Log dialog.  Purely runtime state — never serialized. */
    GPtrArray *log;
    /* The most recent message this node emitted via
     * pn_node_emit_message(), cloned and held so the automation surface
     * can read it back after the fact (GetLastOutputMessage, TODO
     * #40.13).  Purely runtime state — never serialized; %NULL until the
     * node first emits. */
    PnMessage *last_output;
} PnNodePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PnNode, pn_node, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_COLOR,
    PROP_CLASS_NAME,
    PROP_NAME,
    PROP_ICON,
    PROP_POSITION,
    PROP_HAS_INPUT,
    PROP_HAS_OUTPUT,
    PROP_DISABLED,
    PROP_HAS_ERROR,
    PROP_WORKSHEET,
    PROP_TOPIC,
    PROP_COLLATE_INPUTS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

enum {
    SIG_MESSAGE,
    SIG_REPAINT_NEEDED,
    SIG_LOG_CHANGED,
    SIG_INPUT_NAMES_CHANGED,
    SIG_PROCESSING_CHANGED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

/* Defined further down (next to the input-name helpers) but called from
 * the dispatch path above it. */
static void pn_node_latch_inputs (PnNode    *self,
                                  PnMessage *message,
                                  gint       input);

/* ------------------------------------------------------------------ */
/*  Default geometry                                                   */
/*                                                                     */
/*  Every node currently fits the canonical 140×40 Node-RED rectangle. */
/*  These constants live here (not in pn-worksheet.c) so subclasses    */
/*  that override the geometry vfuncs can describe themselves relative */
/*  to the same baseline.                                              */
/* ------------------------------------------------------------------ */

#define PN_NODE_DEFAULT_WIDTH   140.0
#define PN_NODE_DEFAULT_HEIGHT   40.0

/*  Vertical gap painted between a node's header and the body its       */
/*  paint_plot painter draws below it.  Lives here (not in the painter) */
/*  so the client-area report and the worksheet's plot rectangle agree  */
/*  on the same baseline (see pn_node_get_client_area / node_plot_rect).*/
#define PN_NODE_PLOT_GAP          4.0

static void
pn_node_default_get_size (
        PnNode *self,
        double *out_width,
        double *out_height)
{
    if (out_width  != NULL) *out_width  = PN_NODE_DEFAULT_WIDTH;
    /* Multi-input nodes grow downward by one row per input below the
     * header; single-input and input-less nodes keep the bare header
     * footprint.  Centralised here so every multi-input node gets the
     * taller body without overriding geometry. */
    if (out_height != NULL)
        *out_height = PN_NODE_DEFAULT_HEIGHT
                    + pn_node_get_input_section_height (self);
}

static double
pn_node_default_get_header_height (PnNode *self)
{
    (void) self;
    return PN_NODE_DEFAULT_HEIGHT;
}

static gboolean
pn_node_default_get_client_area (
        PnNode *self,
        double *out_x,
        double *out_y,
        double *out_w,
        double *out_h)
{
    double w, h, hh, body_h;

    pn_node_get_size          (self, &w, &h);
    hh     = pn_node_get_header_height (self);
    /* The stacked input-section of a multi-input node is part of the
     * node's solid body, not a paint_plot "client area" — subtract it so
     * a header-only multi-input node (comparator, image blends) keeps
     * reporting no client area, exactly as it did when single-height. */
    body_h = h - hh - PN_NODE_PLOT_GAP
           - pn_node_get_input_section_height (self);

    /* A node has a client area when its footprint extends below the
     * header — the body it fills with its own content (a table's grid,
     * a graph's plot, a gauge face), past the header→body gap.  This is
     * pure geometry (get_size / get_header_height), so it gives the same
     * answer in the GTK editor and the headless core — and in the editor
     * it coincides with "installs a paint_plot painter", since every
     * such node extends its height to make room for the body.  A
     * header-only node (Debug, Comparator, an LED whose 40 px footprint
     * is all header) reports an empty rectangle. */
    if (body_h <= 0.0)
    {
        if (out_x != NULL) *out_x = 0.0;
        if (out_y != NULL) *out_y = 0.0;
        if (out_w != NULL) *out_w = 0.0;
        if (out_h != NULL) *out_h = 0.0;
        return FALSE;
    }

    /* Node-local: the region under the header, past the header→body
     * gap — exactly the rectangle the worksheet hands paint_plot. */
    if (out_x != NULL) *out_x = 0.0;
    if (out_y != NULL) *out_y = hh + PN_NODE_PLOT_GAP;
    if (out_w != NULL) *out_w = w;
    if (out_h != NULL) *out_h = body_h;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_node_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnNode        *self = PN_NODE (object);
    PnNodePrivate *priv = pn_node_get_instance_private (self);

    switch (prop_id)
    {
    case PROP_COLOR:
        g_value_set_boxed (value, &priv->color);
        break;
    case PROP_CLASS_NAME:
        g_value_set_string (value, priv->class_name);
        break;
    case PROP_NAME:
        g_value_set_string (value, priv->name);
        break;
    case PROP_ICON:
        g_value_set_string (value, priv->icon);
        break;
    case PROP_POSITION:
        g_value_set_boxed (value, &priv->position);
        break;
    case PROP_HAS_INPUT:
        g_value_set_boolean (value, priv->has_input);
        break;
    case PROP_HAS_OUTPUT:
        g_value_set_boolean (value, priv->has_output);
        break;
    case PROP_DISABLED:
        g_value_set_boolean (value, priv->disabled);
        break;
    case PROP_HAS_ERROR:
        g_value_set_boolean (value, priv->has_error);
        break;
    case PROP_WORKSHEET:
        g_value_set_string (value, priv->worksheet);
        break;
    case PROP_TOPIC:
        /* Surface the resolved-against-default template so the dialog
         * always shows a useful editable string: when the user has not
         * customised the topic, the entry pre-fills with the built-in
         * default ("/pnode/${nodeclass}/${nodename}[/${hostname}]")
         * which the user can then edit in place.  Internally a %NULL
         * priv->topic continues to mean "use default" so wiping the
         * entry restores the default and stops the literal text from
         * being persisted as an override. */
        if (priv->topic != NULL)
            g_value_set_string (value, priv->topic);
        else
            g_value_take_string (value,
                                 pn_node_default_topic_template (self));
        break;
    case PROP_COLLATE_INPUTS:
        g_value_set_boolean (value, priv->collate_inputs);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_node_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnNode *self = PN_NODE (object);

    switch (prop_id)
    {
    case PROP_COLOR:
        pn_node_set_color (self, g_value_get_boxed (value));
        break;
    case PROP_CLASS_NAME:
        pn_node_set_class_name (self, g_value_get_string (value));
        break;
    case PROP_NAME:
        pn_node_set_name (self, g_value_get_string (value));
        break;
    case PROP_ICON:
        pn_node_set_icon (self, g_value_get_string (value));
        break;
    case PROP_POSITION:
        pn_node_set_position (self, g_value_get_boxed (value));
        break;
    case PROP_HAS_INPUT:
        pn_node_set_has_input (self, g_value_get_boolean (value));
        break;
    case PROP_HAS_OUTPUT:
        pn_node_set_has_output (self, g_value_get_boolean (value));
        break;
    case PROP_DISABLED:
        pn_node_set_disabled (self, g_value_get_boolean (value));
        break;
    case PROP_HAS_ERROR:
        pn_node_set_has_error (self, g_value_get_boolean (value));
        break;
    case PROP_WORKSHEET:
        pn_node_set_worksheet (self, g_value_get_string (value));
        break;
    case PROP_TOPIC:
        pn_node_set_topic (self, g_value_get_string (value));
        break;
    case PROP_COLLATE_INPUTS:
        pn_node_set_collate_inputs (self, g_value_get_boolean (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_node_finalize (GObject *object)
{
    PnNode        *self = PN_NODE (object);
    PnNodePrivate *priv = pn_node_get_instance_private (self);

    g_clear_pointer (&priv->class_name, g_free);
    g_clear_pointer (&priv->name,       g_free);
    g_clear_pointer (&priv->icon,       g_free);
    g_clear_pointer (&priv->uuid,       g_free);
    g_clear_pointer (&priv->worksheet,  g_free);
    g_clear_pointer (&priv->topic,      g_free);
    g_clear_pointer (&priv->input_names, g_ptr_array_unref);
    g_clear_pointer (&priv->input_latch, g_ptr_array_unref);
    g_clear_pointer (&priv->input_value_str, g_ptr_array_unref);
    g_clear_pointer (&priv->input_count_prop, g_free);
    g_clear_pointer (&priv->log,        g_ptr_array_unref);
    g_clear_object  (&priv->last_output);
    g_mutex_clear   (&priv->processing_lock);

    G_OBJECT_CLASS (pn_node_parent_class)->finalize (object);
}

static void
pn_node_class_init (PnNodeClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = pn_node_get_property;
    object_class->set_property = pn_node_set_property;
    object_class->finalize     = pn_node_finalize;

    /* Built-in nodes are registered "by" the running pipnode binary
     * itself rather than by an external plugin, so the inherited
     * default reads as "Internal".  Subclasses loaded from a real
     * plugin are expected to overwrite klass->plugin_name with the
     * plugin's own identifier. */
    klass->plugin_name = "Internal";

    props[PROP_COLOR] = g_param_spec_boxed (
            "color", "Color",
            "Body fill colour of the node",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_CLASS_NAME] = g_param_spec_string (
            "class-name", "Class name",
            "Identifier of the node type, also used as the displayed label",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_NAME] = g_param_spec_string (
            "name", "Name",
            "User-supplied label; falls back to the class name when empty",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ICON] = g_param_spec_string (
            "icon", "Icon",
            "Glyph drawn in the node's icon panel",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_POSITION] = g_param_spec_boxed (
            "position", "Position",
            "Worksheet coordinates of the node's top-left corner",
            PN_TYPE_POINT,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HAS_INPUT] = g_param_spec_boolean (
            "has-input", "Has input",
            "Whether the node exposes an input port on its left edge",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HAS_OUTPUT] = g_param_spec_boolean (
            "has-output", "Has output",
            "Whether the node exposes an output port on its right edge",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DISABLED] = g_param_spec_boolean (
            "disabled", "Disabled",
            "When TRUE the node is inert: it does not emit messages, "
            "does not react to incoming messages, and is painted in "
            "grey on the worksheet",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_HAS_ERROR] = g_param_spec_boolean (
            "has-error", "Has error",
            "Transient runtime flag: when TRUE the node is in an error "
            "state (its work is failing) and the worksheet paints it red "
            "with a warning glyph.  Recomputed at runtime and never "
            "persisted to the save file.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_WORKSHEET] = g_param_spec_string (
            "worksheet", "Worksheet",
            "Spreadsheet-style sheet tag the node is filed under. "
            "Reserved for a future per-sheet-tabs UI; defaults to "
            "\"Worksheet\" so existing flows round-trip unchanged.",
            "Worksheet",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TOPIC] = g_param_spec_string (
            "topic", "Topic",
            "Topic template stamped onto messages this node emits. "
            "Supports the placeholders ${nodeclass}, ${nodename} and "
            "${hostname}, all resolved at emit time. The default is "
            "/pnode/${nodeclass}/${nodename} — or, for nodes that "
            "carry a hostname property, "
            "/pnode/${nodeclass}/${nodename}/${hostname} so the "
            "executing host appears in the topic.",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_COLLATE_INPUTS] = g_param_spec_boolean (
            "collate-inputs", "Collate Inputs",
            "When TRUE and the node has two or more inputs, the core "
            "latches each input's last /data/value and re-injects every "
            "latched value into each arriving message's data bag under "
            "the inputs' display names before receive() runs, so a "
            "multi-input node sees all its inputs at once. Opt-in, set "
            "from a node's own init(); runtime-only, never serialized.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Emitted whenever the node delivers a value to its output port. */
    signals[SIG_MESSAGE] = g_signal_new (
            "message",
            PN_TYPE_NODE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            PN_TYPE_MESSAGE);

    /* Emitted by animated nodes whose appearance changes outside the
     * property-notify mechanism — for example a graph node redrawing
     * when a fresh sample arrives.  The worksheet listens and queues
     * a widget redraw. */
    signals[SIG_REPAINT_NEEDED] = g_signal_new (
            "repaint-needed",
            PN_TYPE_NODE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            0);

    /* Emitted whenever the node's log ring changes — a new entry was
     * appended (pn_node_log) or the ring was emptied (pn_node_clear_log).
     * The per-node Log dialog listens and re-reads pn_node_get_log() so
     * it updates live while the node is running. */
    signals[SIG_LOG_CHANGED] = g_signal_new (
            "log-changed",
            PN_TYPE_NODE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            0);

    /* Emitted when a per-input display name changes (pn_node_set_input_name).
     * These names round-trip to disk but live in a bespoke store rather than
     * a GObject property, so they raise no "notify"; PnFlow listens to this
     * signal to flag the document modified on a rename. */
    signals[SIG_INPUT_NAMES_CHANGED] = g_signal_new (
            "input-names-changed",
            PN_TYPE_NODE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            0);

    /* Emitted on the idle→busy and busy→idle edges of a node's processing
     * state (pn_node_processing_begin/end).  The boolean argument is the
     * new state (TRUE = now busy).  Always delivered on the thread running
     * the default main context — the emit is marshalled there even when
     * begin/end run on a node's worker thread — so a handler may safely
     * touch GTK.  The worksheet listens and animates a "busy" halo (TODO
     * #42); adding this signal changes no struct layout, so no ABI bump. */
    signals[SIG_PROCESSING_CHANGED] = g_signal_new (
            "processing-changed",
            PN_TYPE_NODE,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_BOOLEAN);

    /* Default size vfuncs return the canonical 140×40 footprint;
     * subclasses override for nodes that paint outside that box. */
    klass->get_size          = pn_node_default_get_size;
    klass->get_header_height = pn_node_default_get_header_height;
    klass->get_client_area   = pn_node_default_get_client_area;
}

static void
pn_node_init (PnNode *self)
{
    PnNodePrivate *priv = pn_node_get_instance_private (self);

    /* Sensible neutral defaults so a freshly-constructed node is
     * always paintable, even before any property has been set. */
    priv->color.red   = 0.80;
    priv->color.green = 0.80;
    priv->color.blue  = 0.82;
    priv->color.alpha = 1.0;

    priv->class_name = NULL;
    priv->name       = NULL;
    priv->icon       = NULL;
    /* Mint a fresh UUID up front so every node — including ones
     * constructed by tests or transient palette templates — has a
     * stable identity from the moment it exists.  The deserialiser
     * overwrites this with the persisted value when loading; paste
     * overwrites it again so cloned nodes never share an identity. */
    priv->uuid       = g_uuid_string_random ();
    priv->position.x = 0.0;
    priv->position.y = 0.0;
    priv->has_input  = TRUE;
    priv->has_output = TRUE;
    priv->n_inputs   = 0;   /* 0 ⇒ derive from has_input (single input) */
    /* input_latch left NULL — lazily allocated on first latch, and only
     * when a multi-input node opts in via pn_node_set_collate_inputs(). */
    priv->collate_inputs = FALSE;
    priv->disabled   = FALSE;
    priv->has_error  = FALSE;
    g_mutex_init (&priv->processing_lock);
    priv->busy_count       = 0;
    priv->busy_since_us    = 0;
    priv->visible_until_us = 0;
    /* "Worksheet" is the canonical default sheet tag — see the
     * field comment for the rationale. */
    priv->worksheet  = g_strdup ("Worksheet");
    /* %NULL means "use the built-in default template" — the dialog
     * surfaces it through pn_node_default_topic_template() so the user
     * still sees an editable string. */
    priv->topic      = NULL;

    priv->log = g_ptr_array_new_with_free_func (
            (GDestroyNotify) pn_log_entry_free);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnNode *
pn_node_new (void)
{
    return g_object_new (PN_TYPE_NODE, NULL);
}

void
pn_node_emit_message (
        PnNode    *self,
        PnMessage *message)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));
    g_return_if_fail (PN_IS_MESSAGE (message));

    /* A disabled node is inert: drop the message before any wire sees
     * it so downstream nodes stay idle. */
    priv = pn_node_get_instance_private (self);
    if (priv->disabled)
        return;

    /* Keep a private clone of the last message we put on the wire so the
     * automation surface can read it back without a Debug node (TODO
     * #40.13).  Cloned rather than reffed because downstream handlers may
     * mutate the message they receive, and a clone gives every receiver
     * its own copy already (the wire layer clones too) — holding our own
     * keeps the readback faithful to what we emitted.
     *
     * Drop the clone's source: a #PnMessage holds a STRONG reference on
     * its source node, so storing a message this node sourced (the usual
     * case) would form a reference cycle node -> last_output -> source ->
     * node that keeps the node alive forever — and an undisposed node
     * never cancels its idles/timers.  The readback carries the
     * payload/topic; the live MessageEmitted signal renders the intact
     * message, so the source stays observable there. */
    g_clear_object (&priv->last_output);
    priv->last_output = pn_message_clone (message);
    pn_message_set_source (priv->last_output, NULL);

    g_signal_emit (self, signals[SIG_MESSAGE], 0, message);
}

PnMessage *
pn_node_get_last_output_message (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    return priv->last_output;
}

/* Maximum nesting of synchronous receive→emit→receive→… dispatch.
 * Past this we assume the worksheet has a closed loop and bail
 * before the C stack runs out.  256 is well past any flow a user
 * draws by hand (a chain of 256 nodes wired end-to-end would barely
 * fit on a 4K monitor) but small enough to leave plenty of headroom
 * on a default 8 MB stack. */
#define PN_NODE_MAX_DISPATCH_DEPTH 256

static __thread int pn_node_dispatch_depth = 0;

/* Index of the input port the message currently being dispatched
 * arrived on.  Valid only for the duration of a #PnNodeClass.receive
 * call; a multi-input node reads it through pn_node_current_input().
 * Saved/restored around each receive so nested synchronous dispatch
 * (A→emit→B→emit→…) leaves the right value visible at every level,
 * just like pn_node_dispatch_depth. */
static __thread gint pn_node_current_input_idx = 0;

void
pn_node_receive_message_on_input (
        PnNode    *self,
        PnMessage *message,
        gint       input)
{
    PnNodeClass   *klass;
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));
    g_return_if_fail (PN_IS_MESSAGE (message));

    /* Loop guard: the dispatch is fully synchronous, so a wired
     * cycle (Format → Filter → Format → …) recurses on the C stack
     * until it overflows.  Counting the depth on the way in lets us
     * drop the runaway call instead of crashing.  Async paths reset
     * the counter naturally because their later emit comes from a
     * main-loop callback on a freshly unwound stack. */
    if (pn_node_dispatch_depth >= PN_NODE_MAX_DISPATCH_DEPTH)
    {
        g_warning ("pn_node_receive_message: dispatch depth exceeded "
                   "%d on \"%s\" — suspected message loop, dropping",
                   PN_NODE_MAX_DISPATCH_DEPTH,
                   pn_node_get_name (self));
        return;
    }

    /* Disabled nodes ignore their input — both the side effects of
     * processing and any downstream emission are suppressed. */
    priv = pn_node_get_instance_private (self);
    if (priv->disabled)
        return;

    klass = PN_NODE_GET_CLASS (self);
    if (klass->receive != NULL)
    {
        const gint prev_input = pn_node_current_input_idx;

        pn_node_current_input_idx = input;
        /* Latch this input's value for the worksheet readout and — when the
         * node opted into collation — surface every input under its name
         * before the node processes the message (no-op for single-input
         * nodes). */
        pn_node_latch_inputs (self, message, input);
        pn_node_dispatch_depth++;
        /* Bracket the handler with the processing indicator (TODO #42) so
         * every synchronous node lights up for free while its receive runs
         * — honest, since the work really is executing here.  Nested
         * synchronous dispatch (A→emit→B) brackets each node independently,
         * and the minimum-visible linger keeps a microsecond handler
         * perceptible.  Costs nothing when no one listens (see
         * pn_node_emit_processing_changed). */
        pn_node_processing_begin (self);
        klass->receive (self, message);
        pn_node_processing_end (self);
        pn_node_dispatch_depth--;
        pn_node_current_input_idx = prev_input;
    }
}

void
pn_node_receive_message (
        PnNode    *self,
        PnMessage *message)
{
    /* Convenience for the common single-input case and for every
     * existing caller: deliver on input 0. */
    pn_node_receive_message_on_input (self, message, 0);
}

gint
pn_node_current_input (void)
{
    return pn_node_current_input_idx;
}

gint
pn_node_get_dispatch_depth (void)
{
    return pn_node_dispatch_depth;
}

void
pn_node_get_size (
        PnNode *self,
        double *out_w,
        double *out_h)
{
    PnNodeClass *klass;

    g_return_if_fail (PN_IS_NODE (self));

    klass = PN_NODE_GET_CLASS (self);
    /* The class init wires the default implementation in for every
     * subclass that doesn't override it, so the vfunc is always set
     * — the NULL guard is purely defensive. */
    if (klass->get_size != NULL)
        klass->get_size (self, out_w, out_h);
    else
        pn_node_default_get_size (self, out_w, out_h);
}

double
pn_node_get_header_height (PnNode *self)
{
    PnNodeClass *klass;

    g_return_val_if_fail (PN_IS_NODE (self), PN_NODE_DEFAULT_HEIGHT);

    klass = PN_NODE_GET_CLASS (self);
    if (klass->get_header_height != NULL)
        return klass->get_header_height (self);
    return pn_node_default_get_header_height (self);
}

gboolean
pn_node_get_client_area (
        PnNode *self,
        double *out_x,
        double *out_y,
        double *out_w,
        double *out_h)
{
    PnNodeClass *klass;

    g_return_val_if_fail (PN_IS_NODE (self), FALSE);

    klass = PN_NODE_GET_CLASS (self);
    if (klass->get_client_area != NULL)
        return klass->get_client_area (self, out_x, out_y, out_w, out_h);
    return pn_node_default_get_client_area (self, out_x, out_y, out_w, out_h);
}

void
pn_node_request_repaint (PnNode *self)
{
    g_return_if_fail (PN_IS_NODE (self));
    g_signal_emit (self, signals[SIG_REPAINT_NEEDED], 0);
}

const PnColor *
pn_node_get_color (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    return &priv->color;
}

void
pn_node_set_color (
        PnNode        *self,
        const PnColor *color)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));
    g_return_if_fail (color != NULL);

    priv = pn_node_get_instance_private (self);
    if (pn_color_equal (&priv->color, color))
        return;

    priv->color = *color;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COLOR]);
}

/* ------------------------------------------------------------------ */
/*  Class-level accessors                                              */
/*                                                                     */
/*  These read the canonical per-type values pinned by each subclass  */
/*  on its #PnNodeClass struct.  They do not touch instance state, so */
/*  callers with only a class handle (e.g. from g_type_class_ref())   */
/*  can use them.                                                      */
/* ------------------------------------------------------------------ */

const gchar *
pn_node_class_get_plugin_name (PnNodeClass *klass)
{
    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), NULL);
    return klass->plugin_name;
}

const gchar *
pn_node_class_get_category (PnNodeClass *klass)
{
    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), NULL);
    return klass->category;
}

const gchar *
pn_node_class_get_class_name (PnNodeClass *klass)
{
    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), NULL);
    return klass->class_name;
}

const gchar *
pn_node_class_get_icon (PnNodeClass *klass)
{
    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), NULL);
    return klass->icon;
}

const PnColor *
pn_node_class_get_color (PnNodeClass *klass)
{
    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), NULL);
    return &klass->color;
}

gboolean
pn_node_class_get_has_input (PnNodeClass *klass)
{
    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), FALSE);
    return klass->has_input;
}

gboolean
pn_node_class_get_has_output (PnNodeClass *klass)
{
    g_return_val_if_fail (PN_IS_NODE_CLASS (klass), FALSE);
    return klass->has_output;
}

const gchar *
pn_node_get_plugin_name (PnNode *self)
{
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    return PN_NODE_GET_CLASS (self)->plugin_name;
}

const gchar *
pn_node_get_category (PnNode *self)
{
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    return PN_NODE_GET_CLASS (self)->category;
}

const gchar *
pn_node_get_class_name (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    if (priv->class_name != NULL && *priv->class_name != '\0')
        return priv->class_name;
    return PN_NODE_GET_CLASS (self)->class_name;
}

/* Deprecated: leaf classes should pin PnNodeClass.class_name in
 * class_init instead — pn_node_get_class_name() now falls back to
 * that, so calling this from instance init() is no longer needed
 * (and is actively harmful for subclasses: the parent's value
 * sticks unless the child re-stomps it).  Still kept public for
 * deserialization, which restores the saved instance label from
 * the .pn file via the "class-name" GObject property.  Plugins
 * may remove leftover calls from their _init() functions. */
void
pn_node_set_class_name (
        PnNode      *self,
        const gchar *class_name)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    if (g_strcmp0 (priv->class_name, class_name) == 0)
        return;

    g_free (priv->class_name);
    priv->class_name = g_strdup (class_name);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CLASS_NAME]);
}

const gchar *
pn_node_get_name (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    return priv->name;
}

void
pn_node_set_name (
        PnNode      *self,
        const gchar *name)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    if (g_strcmp0 (priv->name, name) == 0)
        return;

    g_free (priv->name);
    priv->name = g_strdup (name);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_NAME]);
}

const gchar *
pn_node_get_uuid (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    return priv->uuid;
}

void
pn_node_set_uuid (
        PnNode      *self,
        const gchar *uuid)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);

    /* %NULL request means "regenerate".  Used by paste when the
     * caller wants to ensure a fresh, non-colliding identity without
     * having to mint the string itself. */
    g_free (priv->uuid);
    priv->uuid = (uuid != NULL && *uuid != '\0')
        ? g_strdup (uuid)
        : g_uuid_string_random ();
}

const gchar *
pn_node_get_icon (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    return priv->icon;
}

void
pn_node_set_icon (
        PnNode      *self,
        const gchar *icon)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    if (g_strcmp0 (priv->icon, icon) == 0)
        return;

    g_free (priv->icon);
    priv->icon = g_strdup (icon);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_ICON]);
}

const PnPoint *
pn_node_get_position (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    return &priv->position;
}

void
pn_node_set_position (
        PnNode        *self,
        const PnPoint *position)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));
    g_return_if_fail (position != NULL);

    priv = pn_node_get_instance_private (self);
    if (priv->position.x == position->x && priv->position.y == position->y)
        return;

    priv->position = *position;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_POSITION]);
}

gboolean
pn_node_get_has_input (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), FALSE);
    priv = pn_node_get_instance_private (self);
    return priv->has_input;
}

void
pn_node_set_has_input (
        PnNode  *self,
        gboolean has_input)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    has_input = !!has_input;
    if (priv->has_input == has_input)
        return;

    priv->has_input = has_input;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_INPUT]);
}

gint
pn_node_get_n_inputs (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), 0);
    priv = pn_node_get_instance_private (self);
    /* Explicit count wins; otherwise fall back to the boolean so
     * single-input nodes (which never call pn_node_set_n_inputs) keep
     * reporting exactly one input. */
    if (priv->n_inputs > 0)
        return priv->n_inputs;
    return priv->has_input ? 1 : 0;
}

void
pn_node_set_n_inputs (
        PnNode *self,
        gint    n)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));
    g_return_if_fail (n >= 0);

    priv = pn_node_get_instance_private (self);
    if (priv->n_inputs == n)
        return;

    priv->n_inputs = n;
    /* Drop any per-input latches for inputs that no longer exist; the
     * array's free-func unrefs the discarded JsonNodes.  (Growth is
     * handled lazily by pn_node_ensure_input_latch.) */
    if (priv->input_latch != NULL && (gint) priv->input_latch->len > n)
        g_ptr_array_set_size (priv->input_latch, n);
    /* Likewise drop readout strings for inputs that no longer exist. */
    if (priv->input_value_str != NULL && (gint) priv->input_value_str->len > n)
        g_ptr_array_set_size (priv->input_value_str, n);
    /* Keep the boolean consistent so has-input consumers (and the
     * save format, which still goes through it) agree with the count. */
    pn_node_set_has_input (self, n >= 1);
}

double
pn_node_get_input_section_height (PnNode *self)
{
    gint n;

    g_return_val_if_fail (PN_IS_NODE (self), 0.0);

    n = pn_node_get_n_inputs (self);
    /* Single-input (and input-less) nodes keep the bare-header footprint
     * — the historical layout — bit-for-bit; only 2+ inputs reserve the
     * stacked lower section. */
    if (n <= 1)
        return 0.0;
    return (double) n * PN_NODE_INPUT_ROW_HEIGHT;
}

/* Grow priv->input_names so index @n-1 is addressable, padding new slots
 * with NULL (meaning "use the valueN default").  Allocates the array on
 * first use. */
static void
pn_node_ensure_input_names (PnNodePrivate *priv, gint n)
{
    if (priv->input_names == NULL)
        priv->input_names = g_ptr_array_new_with_free_func (g_free);
    while ((gint) priv->input_names->len < n)
        g_ptr_array_add (priv->input_names, NULL);
}

const gchar *
pn_node_get_input_name (PnNode *self, gint index)
{
    PnNodePrivate *priv;

    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    g_return_val_if_fail (index >= 0, NULL);

    priv = pn_node_get_instance_private (self);
    pn_node_ensure_input_names (priv, index + 1);

    /* Materialise the default on first read so we can return a stable,
     * node-owned pointer the painter can hold without copying. */
    if (priv->input_names->pdata[index] == NULL)
        priv->input_names->pdata[index] =
                g_strdup_printf ("value%d", index + 1);

    return priv->input_names->pdata[index];
}

const gchar *
pn_node_get_input_value_display (PnNode *self, gint index)
{
    PnNodePrivate *priv;

    g_return_val_if_fail (PN_IS_NODE (self), NULL);

    if (index < 0)
        return NULL;

    priv = pn_node_get_instance_private (self);
    if (priv->input_value_str == NULL ||
        index >= (gint) priv->input_value_str->len)
        return NULL;

    return priv->input_value_str->pdata[index];   /* borrowed; may be NULL */
}

void
pn_node_set_input_name (
        PnNode      *self,
        gint         index,
        const gchar *name)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));
    g_return_if_fail (index >= 0);

    priv = pn_node_get_instance_private (self);
    pn_node_ensure_input_names (priv, index + 1);

    /* NULL / "" reverts to the lazily-generated "valueN" default. */
    {
        const gchar *old    = priv->input_names->pdata[index];
        const gchar *wanted = (name != NULL && *name != '\0') ? name : NULL;

        /* No-op when unchanged so re-applying the same text (e.g. the
         * dialog echoing the current value back) does not flag the
         * document dirty. */
        if (g_strcmp0 (old, wanted) == 0)
            return;

        g_free (priv->input_names->pdata[index]);
        priv->input_names->pdata[index] =
                wanted != NULL ? g_strdup (wanted) : NULL;
    }

    g_signal_emit (self, signals[SIG_INPUT_NAMES_CHANGED], 0);
}

void
pn_node_set_input_count_property (
        PnNode      *self,
        const gchar *prop_name)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    g_free (priv->input_count_prop);
    priv->input_count_prop =
            (prop_name != NULL && *prop_name != '\0')
                    ? g_strdup (prop_name) : NULL;
}

const gchar *
pn_node_get_input_count_property (PnNode *self)
{
    PnNodePrivate *priv;

    g_return_val_if_fail (PN_IS_NODE (self), NULL);

    priv = pn_node_get_instance_private (self);
    return priv->input_count_prop;
}

gboolean
pn_node_get_collate_inputs (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), FALSE);
    priv = pn_node_get_instance_private (self);
    return priv->collate_inputs;
}

void
pn_node_set_collate_inputs (
        PnNode   *self,
        gboolean  collate)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    collate = !!collate;
    if (priv->collate_inputs == collate)
        return;

    priv->collate_inputs = collate;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_COLLATE_INPUTS]);
}

/* GPtrArray free-func for input_latch slots.  Unlike g_free, json_node_unref
 * asserts on NULL, and the latch array is padded with NULL slots for inputs
 * that have not carried a value yet — so the unref must be NULL-guarded. */
static void
pn_node_free_latched_node (gpointer node)
{
    if (node != NULL)
        json_node_unref (node);
}

/* Grow priv->input_latch so index @n-1 is addressable, padding new slots
 * with NULL ("no value latched yet").  Allocates the array on first use;
 * the free-func unrefs the owned JsonNode copies on teardown/shrink. */
static void
pn_node_ensure_input_latch (PnNodePrivate *priv, gint n)
{
    if (priv->input_latch == NULL)
        priv->input_latch =
                g_ptr_array_new_with_free_func (pn_node_free_latched_node);
    while ((gint) priv->input_latch->len < n)
        g_ptr_array_add (priv->input_latch, NULL);
}

/* Grow priv->input_value_str so index @n-1 is addressable, padding new
 * slots with NULL ("no value seen yet").  Allocates on first use; the
 * free-func g_free's the owned display strings on teardown/shrink. */
static void
pn_node_ensure_input_value_str (PnNodePrivate *priv, gint n)
{
    if (priv->input_value_str == NULL)
        priv->input_value_str = g_ptr_array_new_with_free_func (g_free);
    while ((gint) priv->input_value_str->len < n)
        g_ptr_array_add (priv->input_value_str, NULL);
}

/* Render @value (a borrowed /data/value node, resolved against @message for
 * any "$pnvector" markers) as a short human-readable string for the
 * worksheet to paint beside an input — or NULL when there is nothing worth
 * showing.  Vectors collapse to a bounded sample exactly like the debug
 * pane ("[0, 1, 2, …] (256 values)"); scalars print plainly; long strings
 * are clipped.  The caller owns the returned string. */
static gchar *
pn_node_format_value_display (
        PnMessage *message,
        JsonNode  *value)
{
    if (value == NULL)
        return NULL;

    if (JSON_NODE_HOLDS_OBJECT (value))
    {
        JsonObject *obj = json_node_get_object (value);

        /* A vector rides as a "$pnvector" marker; resolve it against the
         * live message and sample a few values, falling back to the
         * advertised length when only the descriptor survives. */
        if (json_object_has_member (obj, PN_MESSAGE_VECTOR_MARKER))
        {
            PnVector *vec = pn_message_resolve_vector (message, value);
            gint64    len;

            if (vec != NULL)
                return pn_vector_to_sample_string (vec, 4);

            len = json_object_has_member (obj, "len")
                  ? json_object_get_int_member (obj, "len") : 0;
            return g_strdup_printf (
                    "[\xE2\x80\xA6] (%" G_GINT64_FORMAT " values)", len);
        }

        /* Any other structured value: a compact placeholder rather than a
         * dumped object that would overrun the row. */
        return g_strdup ("{\xE2\x80\xA6}");
    }

    if (JSON_NODE_HOLDS_VALUE (value))
    {
        GType vt = json_node_get_value_type (value);

        if (vt == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (value) ? "true" : "false");

        if (vt == G_TYPE_STRING)
        {
            const gchar *s = json_node_get_string (value);

            if (s == NULL)
                return NULL;
            /* Clip overlong strings; the painter ellipsizes to the row
             * width too, but a hard cap keeps the copy cheap. */
            if (g_utf8_strlen (s, -1) > 24)
            {
                gchar *cut = g_utf8_substring (s, 0, 24);
                gchar *out = g_strconcat (cut, "\xE2\x80\xA6", NULL);
                g_free (cut);
                return out;
            }
            return g_strdup (s);
        }

        /* Numbers (and anything else json-glib hands back as a value)
         * print with %g, which drops trailing zeros and handles ints. */
        return g_strdup_printf ("%g", json_node_get_double (value));
    }

    /* Arrays, null, … — nothing useful to show in one row. */
    return NULL;
}

/* Latch @message's /data/value at input @input on a multi-input node.
 *
 * Two things happen, with different scope:
 *  - Always: record a short human-readable rendering of the value in
 *    input_value_str[input] for the worksheet to paint beside the input's
 *    name, requesting a repaint when it changed.  This is the per-input
 *    readout and applies to every multi-input node.
 *  - Opt-in (pn_node_set_collate_inputs): keep a typed copy in
 *    input_latch[input] and re-inject every latched value into @message's
 *    data bag under its input's display name, so a collating node (e.g. the
 *    Calculator) sees all of its inputs at once.
 *
 * No-op for single-input nodes.  Invoked from
 * pn_node_receive_message_on_input just before the node's receive(); see
 * the input_latch / input_value_str field comments. */
static void
pn_node_latch_inputs (
        PnNode    *self,
        PnMessage *message,
        gint       input)
{
    PnNodePrivate *priv = pn_node_get_instance_private (self);
    gint           n    = pn_node_get_n_inputs (self);
    JsonNode      *cur;
    gint           i;

    if (n < 2)
        return;

    /* Guard against a stray dispatch index landing outside [0, n). */
    if (input < 0)
        input = 0;
    else if (input >= n)
        input = n - 1;

    cur = pn_message_get_member (message, "value");   /* borrowed */

    /* --- Worksheet readout (every multi-input node) -------------------- */
    if (cur != NULL)
    {
        gchar   *disp = pn_node_format_value_display (message, cur);
        gchar   *old;
        gboolean changed;

        pn_node_ensure_input_value_str (priv, n);
        old     = priv->input_value_str->pdata[input];
        changed = g_strcmp0 (old, disp) != 0;
        g_free (old);
        priv->input_value_str->pdata[input] = disp;   /* may be NULL */

        /* Repaint independent of the processing-glow preference so the new
         * value shows up even when that visualisation is off. */
        if (changed)
            pn_node_request_repaint (self);
    }

    /* --- Collation (opt-in) ------------------------------------------- */
    if (!priv->collate_inputs)
        return;

    pn_node_ensure_input_latch (priv, n);

    /* Latch this message's /data/value (the typed node, copied) at the
     * arriving input.  A message with no value member keeps the prior
     * latch — the other inputs are still injected below. */
    if (cur != NULL)
    {
        JsonNode *old = priv->input_latch->pdata[input];
        /* Assign in place to preserve the index→input mapping (do not use
         * g_ptr_array_add/remove, which would reindex the slots). */
        priv->input_latch->pdata[input] = json_node_copy (cur);
        if (old != NULL)
            json_node_unref (old);
    }

    /* Inject every latched value under its input's name.  Includes the
     * just-latched current input, so /data/<name[input]> == /data/value.
     * pn_message_set_member takes ownership, so each injection is a copy
     * and our latch stays intact.  On a name collision the higher index
     * (later iteration) wins. */
    for (i = 0; i < n; i++)
    {
        JsonNode *latched = priv->input_latch->pdata[i];
        if (latched == NULL)
            continue;
        pn_message_set_member (message,
                               pn_node_get_input_name (self, i),
                               json_node_copy (latched));
    }
}

gboolean
pn_node_get_has_output (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), FALSE);
    priv = pn_node_get_instance_private (self);
    return priv->has_output;
}

void
pn_node_set_has_output (
        PnNode  *self,
        gboolean has_output)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    has_output = !!has_output;
    if (priv->has_output == has_output)
        return;

    priv->has_output = has_output;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_OUTPUT]);
}

gboolean
pn_node_get_disabled (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), FALSE);
    priv = pn_node_get_instance_private (self);
    return priv->disabled;
}

void
pn_node_set_disabled (
        PnNode  *self,
        gboolean disabled)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    disabled = !!disabled;
    if (priv->disabled == disabled)
        return;

    priv->disabled = disabled;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_DISABLED]);
}

gboolean
pn_node_get_has_error (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), FALSE);
    priv = pn_node_get_instance_private (self);
    return priv->has_error;
}

void
pn_node_set_has_error (
        PnNode  *self,
        gboolean has_error)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    has_error = !!has_error;
    if (priv->has_error == has_error)
        return;

    priv->has_error = has_error;
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HAS_ERROR]);
}

/* ------------------------------------------------------------------ */
/*  Processing-activity indicator (TODO #42)                           */
/* ------------------------------------------------------------------ */

/* Minimum time a node stays "visibly busy" after work begins, so even a
 * sub-millisecond unit of work produces a perceptible glow. */
#define PN_NODE_PROCESSING_MIN_VISIBLE_US (250 * 1000)  /* 250 ms */

typedef struct {
    PnNode  *node;   /* owns a ref for the lifetime of the deferred emit */
    gboolean busy;
} PnProcessingEdge;

static gboolean
pn_node_dispatch_processing_changed (gpointer data)
{
    PnProcessingEdge *edge = data;
    g_signal_emit (edge->node, signals[SIG_PROCESSING_CHANGED], 0, edge->busy);
    return G_SOURCE_REMOVE;
}

static void
pn_node_free_processing_edge (gpointer data)
{
    PnProcessingEdge *edge = data;
    g_object_unref (edge->node);
    g_free (edge);
}

/* Notify listeners that the node crossed a processing edge.  begin/end may
 * run on a node's worker thread while the handler (the worksheet) must run
 * on the UI thread and may touch GTK, so the emission is marshalled onto
 * the default main context rather than fired inline.  When nobody is
 * listening — a headless run, or the editor with the viz preference off and
 * the worksheet unsubscribed — there is nothing to wake, so begin/end
 * collapse to the bare atomic counter with no main-loop hop. */
static void
pn_node_emit_processing_changed (
        PnNode  *self,
        gboolean busy)
{
    PnProcessingEdge *edge;

    if (!g_signal_has_handler_pending (self, signals[SIG_PROCESSING_CHANGED],
                                       0, FALSE))
        return;

    edge = g_new0 (PnProcessingEdge, 1);
    edge->node = g_object_ref (self);
    edge->busy = busy;
    g_main_context_invoke_full (NULL, G_PRIORITY_DEFAULT,
                                pn_node_dispatch_processing_changed,
                                edge, pn_node_free_processing_edge);
}

void
pn_node_processing_begin (PnNode *self)
{
    PnNodePrivate *priv;
    gboolean       became_busy;

    g_return_if_fail (PN_IS_NODE (self));
    priv = pn_node_get_instance_private (self);

    g_mutex_lock (&priv->processing_lock);
    became_busy = (priv->busy_count == 0);
    priv->busy_count++;
    if (became_busy)
        priv->busy_since_us = g_get_monotonic_time ();
    g_mutex_unlock (&priv->processing_lock);

    /* Only the idle→busy edge wakes the UI; overlapping work items past the
     * first just bump the count. */
    if (became_busy)
        pn_node_emit_processing_changed (self, TRUE);
}

void
pn_node_processing_end (PnNode *self)
{
    PnNodePrivate *priv;
    gboolean       became_idle = FALSE;

    g_return_if_fail (PN_IS_NODE (self));
    priv = pn_node_get_instance_private (self);

    g_mutex_lock (&priv->processing_lock);
    if (priv->busy_count == 0)
    {
        g_mutex_unlock (&priv->processing_lock);
        g_warning ("pn_node_processing_end: unbalanced call on \"%s\" "
                   "(processing count already 0)", pn_node_get_name (self));
        return;
    }
    if (--priv->busy_count == 0)
    {
        /* Minimum-visible linger: hold "visibly busy" until at least
         * MIN_VISIBLE_US after work began, so a blip shorter than that
         * still produces a perceptible glow.  max() against now keeps the
         * window honest for work that already ran longer than the floor. */
        const gint64 now = g_get_monotonic_time ();
        priv->visible_until_us =
            MAX (now, priv->busy_since_us + PN_NODE_PROCESSING_MIN_VISIBLE_US);
        became_idle = TRUE;
    }
    g_mutex_unlock (&priv->processing_lock);

    /* The poll started at the begin edge animates the linger out on its
     * own; we still emit the busy→idle edge so a listener that prefers to
     * react rather than poll can repaint promptly. */
    if (became_idle)
        pn_node_emit_processing_changed (self, FALSE);
}

gboolean
pn_node_is_processing (PnNode *self)
{
    PnNodePrivate *priv;
    gboolean       busy;

    g_return_val_if_fail (PN_IS_NODE (self), FALSE);
    priv = pn_node_get_instance_private (self);

    g_mutex_lock (&priv->processing_lock);
    busy = (priv->busy_count > 0);
    g_mutex_unlock (&priv->processing_lock);
    return busy;
}

gboolean
pn_node_is_processing_visible (
        PnNode *self,
        gint64  now_us)
{
    PnNodePrivate *priv;
    gboolean       visible;

    g_return_val_if_fail (PN_IS_NODE (self), FALSE);
    priv = pn_node_get_instance_private (self);

    g_mutex_lock (&priv->processing_lock);
    visible = (priv->busy_count > 0) || (now_us < priv->visible_until_us);
    g_mutex_unlock (&priv->processing_lock);
    return visible;
}

/* ------------------------------------------------------------------ */
/*  Per-node log ring                                                  */
/* ------------------------------------------------------------------ */

void
pn_node_logv (
        PnNode      *self,
        PnLogLevel   level,
        const gchar *format,
        va_list      args)
{
    PnNodePrivate *priv;
    PnLogEntry    *entry;

    g_return_if_fail (PN_IS_NODE (self));
    g_return_if_fail (format != NULL);

    priv = pn_node_get_instance_private (self);

    entry            = g_new (PnLogEntry, 1);
    entry->timestamp = g_get_real_time ();
    entry->level     = level;
    entry->message   = g_strdup_vprintf (format, args);

    /* Drop the oldest entry once the ring is full, so the node holds
     * the most recent PN_NODE_LOG_MAX lines and no more. */
    if (priv->log->len >= PN_NODE_LOG_MAX)
        g_ptr_array_remove_index (priv->log, 0);
    g_ptr_array_add (priv->log, entry);

    g_signal_emit (self, signals[SIG_LOG_CHANGED], 0);
}

void
pn_node_log (
        PnNode      *self,
        PnLogLevel   level,
        const gchar *format,
        ...)
{
    va_list args;

    va_start (args, format);
    pn_node_logv (self, level, format, args);
    va_end (args);
}

void
pn_node_log_info (
        PnNode      *self,
        const gchar *format,
        ...)
{
    va_list args;

    va_start (args, format);
    pn_node_logv (self, PN_LOG_LEVEL_INFO, format, args);
    va_end (args);
}

void
pn_node_log_warning (
        PnNode      *self,
        const gchar *format,
        ...)
{
    va_list args;

    va_start (args, format);
    pn_node_logv (self, PN_LOG_LEVEL_WARNING, format, args);
    va_end (args);
}

void
pn_node_log_error (
        PnNode      *self,
        const gchar *format,
        ...)
{
    va_list args;

    va_start (args, format);
    pn_node_logv (self, PN_LOG_LEVEL_ERROR, format, args);
    va_end (args);
}

GPtrArray *
pn_node_get_log (PnNode *self)
{
    PnNodePrivate *priv;

    g_return_val_if_fail (PN_IS_NODE (self), NULL);

    priv = pn_node_get_instance_private (self);
    return priv->log;
}

void
pn_node_clear_log (PnNode *self)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    if (priv->log->len == 0)
        return;

    g_ptr_array_set_size (priv->log, 0);
    g_signal_emit (self, signals[SIG_LOG_CHANGED], 0);
}

const gchar *
pn_node_get_worksheet (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    return priv->worksheet;
}

void
pn_node_set_worksheet (
        PnNode      *self,
        const gchar *worksheet)
{
    PnNodePrivate *priv;
    const gchar   *value;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);

    /* %NULL or empty collapse to the default sheet tag so the
     * field never carries a non-meaningful value the rest of the
     * code would have to special-case. */
    value = (worksheet != NULL && *worksheet != '\0')
        ? worksheet
        : "Worksheet";

    if (g_strcmp0 (priv->worksheet, value) == 0)
        return;

    g_free (priv->worksheet);
    priv->worksheet = g_strdup (value);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_WORKSHEET]);
}

const gchar *
pn_node_get_topic (PnNode *self)
{
    PnNodePrivate *priv;
    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    priv = pn_node_get_instance_private (self);
    return priv->topic;
}

void
pn_node_set_topic (
        PnNode      *self,
        const gchar *topic)
{
    PnNodePrivate *priv;
    gchar         *replacement;
    gchar         *fallback = NULL;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);

    /* %NULL / "" / the literal built-in default all collapse to the
     * NULL sentinel — "use the default".  Stripping the default form
     * back to NULL means the topic property does not get persisted as
     * an override every time the dialog flushes its bound text entry,
     * which would otherwise pin the topic to a snapshot of the
     * default the moment the user opens the dialog. */
    if (topic == NULL || *topic == '\0')
    {
        replacement = NULL;
    }
    else
    {
        fallback = pn_node_default_topic_template (self);
        replacement = (g_strcmp0 (topic, fallback) == 0)
                ? NULL
                : g_strdup (topic);
    }

    if (g_strcmp0 (priv->topic, replacement) == 0)
    {
        g_free (replacement);
        g_free (fallback);
        return;
    }

    g_free (priv->topic);
    priv->topic = replacement;

    g_free (fallback);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_TOPIC]);
}

/** Returns %TRUE if the node's #GObject class declares a writable
 *  string <literal>hostname</literal> property.  Used both by the
 *  default template selector (hostname-bearing nodes get the longer
 *  default that includes ${hostname}) and by the placeholder
 *  resolver (which reads the property value when substituting
 *  ${hostname}).  The check is type-introspection rather than a
 *  PN_IS_CPU / PN_IS_LOAD / … ladder so out-of-tree plugin nodes that
 *  follow the same hostname convention pick up the same defaulting
 *  behaviour for free. */
static gboolean
pn_node_has_hostname_property (PnNode *self)
{
    GObjectClass *klass;
    GParamSpec   *pspec;

    klass = G_OBJECT_GET_CLASS (self);
    pspec = g_object_class_find_property (klass, "hostname");

    return pspec != NULL
        && (pspec->flags & G_PARAM_READABLE) != 0
        && G_PARAM_SPEC_VALUE_TYPE (pspec) == G_TYPE_STRING;
}

gchar *
pn_node_default_topic_template (PnNode *self)
{
    g_return_val_if_fail (PN_IS_NODE (self), NULL);

    if (pn_node_has_hostname_property (self))
        return g_strdup ("/pnode/${nodeclass}/${nodename}/${hostname}");
    return g_strdup ("/pnode/${nodeclass}/${nodename}");
}

/** Read the configured hostname from a node that exposes it.  Returns
 *  the dynamic property value, or %NULL when the node does not carry a
 *  hostname.  The caller frees the returned string with g_free(). */
static gchar *
pn_node_dup_configured_hostname (PnNode *self)
{
    gchar *hostname = NULL;

    if (!pn_node_has_hostname_property (self))
        return NULL;

    g_object_get (self, "hostname", &hostname, NULL);
    return hostname;
}

gchar **
pn_node_dup_subst_pairs (PnNode *self)
{
    const gchar *class_label;
    const gchar *node_name;
    gchar       *hostname_owned;
    const gchar *hostname;
    gchar      **pairs;

    g_return_val_if_fail (PN_IS_NODE (self), NULL);

    /* Prefer the class label ("CPU" / "Clock") over the GType name
     * ("PnCpu" / "PnRtc") so the topic reads naturally. */
    class_label = pn_node_get_class_name (self);
    if (class_label == NULL || *class_label == '\0')
    {
        PnNodeClass *klass = PN_NODE_GET_CLASS (self);
        class_label = pn_node_class_get_class_name (klass);
    }
    if (class_label == NULL)
        class_label = G_OBJECT_TYPE_NAME (self);

    node_name = pn_node_get_name (self);
    if (node_name == NULL || *node_name == '\0')
        node_name = class_label;

    /* Hostname: read the configured value when the node carries one;
     * fall back to the executing machine's hostname whenever the
     * configured value is empty or just the literal "localhost", which
     * is the marker the host-monitoring nodes use for "this machine".
     * Nodes without a hostname property always get the local host
     * substituted, so a user can still drop ${hostname} into a custom
     * template on any node and have it resolve sensibly. */
    hostname_owned = pn_node_dup_configured_hostname (self);
    if (hostname_owned != NULL
        && *hostname_owned != '\0'
        && g_ascii_strcasecmp (hostname_owned, "localhost") != 0)
    {
        hostname = hostname_owned;
    }
    else
    {
        hostname = g_get_host_name ();
    }

    pairs = g_new (gchar *, 7);
    pairs[0] = g_strdup ("nodeclass");
    pairs[1] = g_strdup (class_label);
    pairs[2] = g_strdup ("nodename");
    pairs[3] = g_strdup (node_name);
    pairs[4] = g_strdup ("hostname");
    pairs[5] = g_strdup (hostname);
    pairs[6] = NULL;

    g_free (hostname_owned);

    return pairs;
}

void
pn_node_set_flow (PnNode *self, PnFlow *flow)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));

    priv = pn_node_get_instance_private (self);
    /* Atomic: the auto-trigger worker thread reads this while the main
     * thread (flow add/remove) writes it.  Borrowed pointer, no ref. */
    g_atomic_pointer_set (&priv->flow, flow);
}

PnFlow *
pn_node_get_flow (PnNode *self)
{
    PnNodePrivate *priv;

    g_return_val_if_fail (PN_IS_NODE (self), NULL);

    priv = pn_node_get_instance_private (self);
    return g_atomic_pointer_get (&priv->flow);
}

gchar *
pn_node_expand_vars (PnNode *self, const gchar *tmpl)
{
    gchar          **pairs;
    PnSubstResolver  resolver;
    PnSubstResolver  globals;
    PnSubstResolver *chain[3];
    PnSubstContext   ctx;
    gchar           *result;

    g_return_val_if_fail (PN_IS_NODE (self), g_strdup (""));

    /* TEXT mode, verbatim on miss: ${nodeclass} / ${nodename} /
     * ${hostname} are substituted, the document globals are consulted
     * next, and any other placeholder is left exactly as typed so an
     * unrelated ${var} — e.g. a shell one-liner's ${HOME} — survives for
     * whoever consumes the string. */
    pairs = pn_node_dup_subst_pairs (self);

    pn_subst_resolver_strv (&resolver, (const gchar * const *) pairs);
    pn_flow_subst_resolver_globals (&globals, pn_node_get_flow (self));
    chain[0] = &resolver;   /* node vars win */
    chain[1] = &globals;    /* then document globals */
    chain[2] = NULL;
    ctx.resolvers = chain;
    ctx.mode      = PN_SUBST_TEXT;
    ctx.miss      = PN_SUBST_MISS_VERBATIM;

    result = pn_subst_expand (tmpl, &ctx);

    g_strfreev (pairs);

    return result;
}

gchar *
pn_node_resolve_topic (PnNode *self)
{
    PnNodePrivate *priv;
    const gchar   *template_str;
    gchar         *template_owned = NULL;
    gchar         *result;

    g_return_val_if_fail (PN_IS_NODE (self), g_strdup (""));

    priv = pn_node_get_instance_private (self);

    if (priv->topic != NULL && *priv->topic != '\0')
    {
        template_str = priv->topic;
    }
    else
    {
        template_owned = pn_node_default_topic_template (self);
        template_str   = template_owned;
    }

    result = pn_node_expand_vars (self, template_str);

    g_free (template_owned);

    return result;
}

/* ------------------------------------------------------------------ */
/*  Param-spec hints                                                   */
/*                                                                     */
/*  Per-property hints attached to a #GParamSpec via qdata so the      */
/*  node-settings dialog can pick a richer editor without baking a    */
/*  PN_IS_FOO type check into the host.  Tagging happens in the       */
/*  type's _class_init right after the property is installed; the    */
/*  dialog reads the tag back through the matching getter.            */
/* ------------------------------------------------------------------ */

static GQuark
pn_param_spec_multiline_quark (void)
{
    /* Single-threaded GTK app, so a plain lazy cache is fine — no
     * g_once_init_enter dance needed.  g_quark_from_static_string
     * is itself idempotent for a given literal, so a worst-case
     * second initialisation just returns the same #GQuark. */
    static GQuark q = 0;
    if (q == 0)
        q = g_quark_from_static_string ("pn-param-spec-multiline");
    return q;
}

void
pn_param_spec_set_multiline (GParamSpec *pspec)
{
    g_return_if_fail (G_IS_PARAM_SPEC (pspec));

    /* Stored as a self-pointer so the qdata slot is non-NULL even on
     * platforms where GINT_TO_POINTER (1) might collide with a real
     * tagged-pointer encoding.  Reads only check for non-NULL. */
    g_param_spec_set_qdata (pspec,
                            pn_param_spec_multiline_quark (),
                            pspec);
}

gboolean
pn_param_spec_get_multiline (GParamSpec *pspec)
{
    if (pspec == NULL)
        return FALSE;
    return g_param_spec_get_qdata (
                   pspec, pn_param_spec_multiline_quark ()) != NULL;
}

static GQuark
pn_param_spec_full_width_quark (void)
{
    /* Same single-threaded lazy cache as the multiline quark above. */
    static GQuark q = 0;
    if (q == 0)
        q = g_quark_from_static_string ("pn-param-spec-full-width");
    return q;
}

void
pn_param_spec_set_full_width (GParamSpec *pspec)
{
    g_return_if_fail (G_IS_PARAM_SPEC (pspec));

    /* Self-pointer marker, exactly like the multiline tag. */
    g_param_spec_set_qdata (pspec,
                            pn_param_spec_full_width_quark (),
                            pspec);
}

gboolean
pn_param_spec_get_full_width (GParamSpec *pspec)
{
    if (pspec == NULL)
        return FALSE;
    return g_param_spec_get_qdata (
                   pspec, pn_param_spec_full_width_quark ()) != NULL;
}

static GQuark
pn_param_spec_hostname_hint_quark (void)
{
    /* Same single-threaded lazy cache as the multiline quark above --
     * any racy second initialisation just re-resolves the literal to
     * the same #GQuark, so no g_once_init_enter dance is needed. */
    static GQuark q = 0;
    if (q == 0)
        q = g_quark_from_static_string ("pn-param-spec-hostname-hint");
    return q;
}

void
pn_param_spec_set_hostname_hint (GParamSpec *pspec)
{
    g_return_if_fail (G_IS_PARAM_SPEC (pspec));

    g_param_spec_set_qdata (pspec,
                            pn_param_spec_hostname_hint_quark (),
                            pspec);
}

gboolean
pn_param_spec_get_hostname_hint (GParamSpec *pspec)
{
    if (pspec == NULL)
        return FALSE;
    return g_param_spec_get_qdata (
                   pspec, pn_param_spec_hostname_hint_quark ()) != NULL;
}

static GQuark
pn_param_spec_profile_ref_quark (void)
{
    /* Same single-threaded lazy cache as the hint quarks above. */
    static GQuark q = 0;
    if (q == 0)
        q = g_quark_from_static_string ("pn-param-spec-profile-ref");
    return q;
}

void
pn_param_spec_set_profile_ref (GParamSpec  *pspec,
                               const gchar *type_id)
{
    g_return_if_fail (G_IS_PARAM_SPEC (pspec));
    g_return_if_fail (type_id != NULL);

    /* Unlike the boolean hint quarks, this one carries a value (the type
     * id), so store an owned copy with a destroy notify. */
    g_param_spec_set_qdata_full (pspec,
                                 pn_param_spec_profile_ref_quark (),
                                 g_strdup (type_id),
                                 g_free);
}

const gchar *
pn_param_spec_get_profile_ref (GParamSpec *pspec)
{
    if (pspec == NULL)
        return NULL;
    return g_param_spec_get_qdata (pspec,
                                   pn_param_spec_profile_ref_quark ());
}

static GQuark
pn_param_spec_profile_ref_inline_quark (void)
{
    /* Same single-threaded lazy cache as the quarks above. */
    static GQuark q = 0;
    if (q == 0)
        q = g_quark_from_static_string ("pn-param-spec-profile-ref-inline");
    return q;
}

void
pn_param_spec_set_profile_ref_inline_fields (GParamSpec  *pspec,
                                             const gchar *fields)
{
    g_return_if_fail (G_IS_PARAM_SPEC (pspec));
    g_return_if_fail (fields != NULL);

    /* Carries the comma-separated field list, so store an owned copy. */
    g_param_spec_set_qdata_full (pspec,
                                 pn_param_spec_profile_ref_inline_quark (),
                                 g_strdup (fields),
                                 g_free);
}

const gchar *
pn_param_spec_get_profile_ref_inline_fields (GParamSpec *pspec)
{
    if (pspec == NULL)
        return NULL;
    return g_param_spec_get_qdata (
                   pspec, pn_param_spec_profile_ref_inline_quark ());
}

static GQuark
pn_param_spec_profile_ref_allow_none_quark (void)
{
    /* Same single-threaded lazy cache as the quarks above. */
    static GQuark q = 0;
    if (q == 0)
        q = g_quark_from_static_string ("pn-param-spec-profile-ref-allow-none");
    return q;
}

void
pn_param_spec_set_profile_ref_allow_none (GParamSpec *pspec)
{
    g_return_if_fail (G_IS_PARAM_SPEC (pspec));

    /* Boolean tag — presence means TRUE (cf. the hostname-hint quark). */
    g_param_spec_set_qdata (pspec,
                            pn_param_spec_profile_ref_allow_none_quark (),
                            pspec);
}

gboolean
pn_param_spec_get_profile_ref_allow_none (GParamSpec *pspec)
{
    if (pspec == NULL)
        return FALSE;
    return g_param_spec_get_qdata (
                   pspec, pn_param_spec_profile_ref_allow_none_quark ()) != NULL;
}

static GQuark
pn_param_spec_secret_quark (void)
{
    /* Same single-threaded lazy cache as the hint quarks above. */
    static GQuark q = 0;
    if (q == 0)
        q = g_quark_from_static_string ("pn-param-spec-secret");
    return q;
}

void
pn_param_spec_set_secret (GParamSpec *pspec)
{
    g_return_if_fail (G_IS_PARAM_SPEC (pspec));

    g_param_spec_set_qdata (pspec,
                            pn_param_spec_secret_quark (),
                            pspec);
}

gboolean
pn_param_spec_get_secret (GParamSpec *pspec)
{
    if (pspec == NULL)
        return FALSE;
    return g_param_spec_get_qdata (
                   pspec, pn_param_spec_secret_quark ()) != NULL;
}
