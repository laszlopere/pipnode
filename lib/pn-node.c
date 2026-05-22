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
#include "pn-subst.h"

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
/*  PnNode                                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    GdkRGBA  color;
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
    gboolean disabled;
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
    PROP_WORKSHEET,
    PROP_TOPIC,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

enum {
    SIG_MESSAGE,
    SIG_REPAINT_NEEDED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

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

static void
pn_node_default_get_size (
        PnNode *self,
        double *out_width,
        double *out_height)
{
    (void) self;
    if (out_width  != NULL) *out_width  = PN_NODE_DEFAULT_WIDTH;
    if (out_height != NULL) *out_height = PN_NODE_DEFAULT_HEIGHT;
}

static double
pn_node_default_get_header_height (PnNode *self)
{
    (void) self;
    return PN_NODE_DEFAULT_HEIGHT;
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
    case PROP_WORKSHEET:
        pn_node_set_worksheet (self, g_value_get_string (value));
        break;
    case PROP_TOPIC:
        pn_node_set_topic (self, g_value_get_string (value));
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
            GDK_TYPE_RGBA,
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

    /* Default size vfuncs return the canonical 140×40 footprint;
     * subclasses override for nodes that paint outside that box. */
    klass->get_size          = pn_node_default_get_size;
    klass->get_header_height = pn_node_default_get_header_height;
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
    priv->disabled   = FALSE;
    /* "Worksheet" is the canonical default sheet tag — see the
     * field comment for the rationale. */
    priv->worksheet  = g_strdup ("Worksheet");
    /* %NULL means "use the built-in default template" — the dialog
     * surfaces it through pn_node_default_topic_template() so the user
     * still sees an editable string. */
    priv->topic      = NULL;
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

    g_signal_emit (self, signals[SIG_MESSAGE], 0, message);
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
        pn_node_dispatch_depth++;
        klass->receive (self, message);
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

void
pn_node_request_repaint (PnNode *self)
{
    g_return_if_fail (PN_IS_NODE (self));
    g_signal_emit (self, signals[SIG_REPAINT_NEEDED], 0);
}

const GdkRGBA *
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
        const GdkRGBA *color)
{
    PnNodePrivate *priv;

    g_return_if_fail (PN_IS_NODE (self));
    g_return_if_fail (color != NULL);

    priv = pn_node_get_instance_private (self);
    if (gdk_rgba_equal (&priv->color, color))
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

const GdkRGBA *
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
    return priv->class_name;
}

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
    /* Keep the boolean consistent so has-input consumers (and the
     * save format, which still goes through it) agree with the count. */
    pn_node_set_has_input (self, n >= 1);
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

gchar *
pn_node_expand_vars (PnNode *self, const gchar *tmpl)
{
    gchar          **pairs;
    PnSubstResolver  resolver;
    PnSubstResolver *chain[2];
    PnSubstContext   ctx;
    gchar           *result;

    g_return_val_if_fail (PN_IS_NODE (self), g_strdup (""));

    /* TEXT mode, verbatim on miss: ${nodeclass} / ${nodename} /
     * ${hostname} are substituted, but any other placeholder is left
     * exactly as typed so an unrelated ${var} — e.g. a shell one-liner's
     * ${HOME} — survives for whoever consumes the string. */
    pairs = pn_node_dup_subst_pairs (self);

    pn_subst_resolver_strv (&resolver, (const gchar * const *) pairs);
    chain[0] = &resolver;
    chain[1] = NULL;
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
