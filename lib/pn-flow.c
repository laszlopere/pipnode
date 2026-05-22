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

#include "pn-flow.h"

#include "pn-node.h"
#include "pn-node-factory.h"
#include "pn-node-store.h"
#include "pn-wire.h"
#include "pn-wire-store.h"
#include "pn-debug.h"

#include <json-glib/json-glib.h>

/* ------------------------------------------------------------------ */
/*  PnFlow                                                             */
/*                                                                     */
/*  Headless container for a worksheet's nodes and wires.  Owns the   */
/*  two stores, knows how to (de)serialise itself as JSON, and        */
/*  re-emits status-message text from PnDebug nodes so a host (the    */
/*  worksheet widget, or a future CLI) can surface it.                */
/* ------------------------------------------------------------------ */

struct _PnFlow
{
    GObject parent_instance;

    PnNodeStore *nodes;
    PnWireStore *wires;

    /* Spreadsheet-style sheet list.  Always non-empty; sheets[0] is
     * the leftmost tab.  Each entry is a strdup'd name; the array
     * itself owns the strings via g_ptr_array_new_with_free_func. */
    GPtrArray   *sheets;        /* gchar *  */
    gchar       *active_sheet;  /* always one of @sheets             */

    /* Has-unsaved-changes flag.  Hosts (PnWorksheet, PnWindow)
     * subscribe to "modified-changed" instead of polling.  A fresh,
     * just-cleared, or just-saved flow reports %FALSE. */
    gboolean     modified;

    /* Suppresses "modified-changed" emissions during a load: the
     * cascade of node-added / wire-added events while the file is
     * being applied to the stores would otherwise flip the flag back
     * to %TRUE the moment we tried to settle on %FALSE. */
    gboolean     loading;
};

G_DEFINE_TYPE (PnFlow, pn_flow, G_TYPE_OBJECT)

enum {
    SIG_STATUS_MESSAGE,
    SIG_DEBUG_MESSAGE,
    SIG_SHEET_ADDED,
    SIG_SHEET_REMOVED,
    SIG_SHEET_RENAMED,
    SIG_SHEETS_REORDERED,
    SIG_ACTIVE_SHEET_CHANGED,
    SIG_MODIFIED_CHANGED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

#define PN_FLOW_FILE_FORMAT      "pipnode"
#define PN_FLOW_CLIPBOARD_FORMAT "pipnode-clipboard"
#define PN_FLOW_FILE_VERSION     1

static GQuark
flow_error_quark (void)
{
    return g_quark_from_static_string ("pn-flow-error");
}

#define PN_FLOW_ERROR (flow_error_quark ())

/* ------------------------------------------------------------------ */
/*  Default-naming                                                     */
/* ------------------------------------------------------------------ */

static gboolean
name_in_use (
        PnFlow      *self,
        const gchar *name,
        PnNode      *skip)
{
    const guint count = pn_node_store_get_length (self->nodes);
    guint i;

    for (i = 0; i < count; i++)
    {
        PnNode      *n  = pn_node_store_get_node (self->nodes, i);
        const gchar *nm;

        if (n == skip)
            continue;

        nm = pn_node_get_name (n);
        if (nm != NULL && g_strcmp0 (nm, name) == 0)
            return TRUE;
    }

    return FALSE;
}

/** Make sure @node carries a unique name.  No-op when one is already
 *  set; otherwise picks "<class> <N>" with the lowest free N. */
static void
flow_assign_default_name (
        PnFlow *self,
        PnNode *node)
{
    const gchar *existing = pn_node_get_name (node);
    const gchar *base;
    guint        suffix;

    if (existing != NULL && *existing != '\0')
        return;

    base = pn_node_get_class_name (node);
    if (base == NULL || *base == '\0')
        base = "node";

    for (suffix = 1; ; suffix++)
    {
        gchar *candidate = g_strdup_printf ("%s %u", base, suffix);
        if (!name_in_use (self, candidate, node))
        {
            pn_node_set_name (node, candidate);
            g_free (candidate);
            return;
        }
        g_free (candidate);
    }
}

/* ------------------------------------------------------------------ */
/*  Sheet list                                                         */
/* ------------------------------------------------------------------ */

#define PN_FLOW_DEFAULT_SHEET "Worksheet"

/** Index of the sheet whose name equals @name, or -1 when absent.
 *  Comparison is case-sensitive: "Worksheet" and "worksheet" are
 *  distinct sheets. */
static gint
sheet_find_index (PnFlow *self, const gchar *name)
{
    guint i;

    if (name == NULL)
        return -1;

    for (i = 0; i < self->sheets->len; i++)
    {
        const gchar *s = self->sheets->pdata[i];
        if (g_strcmp0 (s, name) == 0)
            return (gint) i;
    }
    return -1;
}

/** Mint a fresh "Sheet N" name with the lowest free N.  Caller frees
 *  with g_free.  Skips %"Worksheet" itself so the default landing tab
 *  cannot be overshadowed by an auto-named one. */
static gchar *
sheet_mint_default_name (PnFlow *self)
{
    guint suffix;

    for (suffix = 2; ; suffix++)
    {
        gchar *candidate = g_strdup_printf ("Sheet %u", suffix);
        if (sheet_find_index (self, candidate) < 0)
            return candidate;
        g_free (candidate);
    }
}

/** Reset the sheet list to the canonical single-sheet default and pick
 *  it as active.  Called from pn_flow_init and pn_flow_clear so an
 *  empty flow always carries exactly one sheet named %"Worksheet". */
static void
sheet_list_reset_default (PnFlow *self)
{
    g_ptr_array_set_size (self->sheets, 0);
    g_ptr_array_add (self->sheets, g_strdup (PN_FLOW_DEFAULT_SHEET));

    g_free (self->active_sheet);
    self->active_sheet = g_strdup (PN_FLOW_DEFAULT_SHEET);
}

/* ------------------------------------------------------------------ */
/*  Modified flag                                                      */
/* ------------------------------------------------------------------ */

void
pn_flow_set_modified (PnFlow *self, gboolean modified)
{
    g_return_if_fail (PN_IS_FLOW (self));

    if (self->loading)
        return;
    if (self->modified == modified)
        return;

    self->modified = modified;
    g_signal_emit (self, signals[SIG_MODIFIED_CHANGED], 0, modified);
}

gboolean
pn_flow_is_modified (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    return self->modified;
}

static void
on_store_changed_mark_modified (gpointer user_data)
{
    pn_flow_set_modified (PN_FLOW (user_data), TRUE);
}

static void
on_node_store_node_added_mark (
        PnNodeStore *store,
        PnNode      *node,
        guint        index,
        gpointer     user_data)
{
    (void) store; (void) node; (void) index;
    on_store_changed_mark_modified (user_data);
}

static void
on_node_store_node_removed_mark (
        PnNodeStore *store,
        PnNode      *node,
        guint        index,
        gpointer     user_data)
{
    (void) store; (void) node; (void) index;
    on_store_changed_mark_modified (user_data);
}

static void
on_wire_store_wire_added_mark (
        PnWireStore *store,
        PnWire      *wire,
        guint        index,
        gpointer     user_data)
{
    (void) store; (void) wire; (void) index;
    on_store_changed_mark_modified (user_data);
}

static void
on_wire_store_wire_removed_mark (
        PnWireStore *store,
        PnWire      *wire,
        guint        index,
        gpointer     user_data)
{
    (void) store; (void) wire; (void) index;
    on_store_changed_mark_modified (user_data);
}

/* Forward decl: the same predicate node_to_json uses to pick which
 * subclass properties round-trip to disk (see below). */
static gboolean should_serialize_property (GParamSpec *pspec);

/** Decide whether a property-change notification should mark the flow
 *  modified.  A change matters only if it alters what node_to_json
 *  actually writes: the serialisable subclass properties, plus the
 *  handful of #PnNode-owned fields that round-trip on disk (name,
 *  worksheet, disabled, position, topic).  The remaining #PnNode
 *  properties — icon, colour, class-name and the port flags — are
 *  class-derived or live-status decorations that never persist, so the
 *  runtime self-updates driving them (e.g. #PnMqtt flipping its body
 *  green on a CONNACK that lands after the load completes) must not
 *  masquerade as unsaved edits. */
static gboolean
property_affects_document (GParamSpec *pspec)
{
    if (should_serialize_property (pspec))
        return TRUE;

    if (pspec->owner_type == PN_TYPE_NODE)
        return g_strcmp0 (pspec->name, "name")      == 0
            || g_strcmp0 (pspec->name, "worksheet") == 0
            || g_strcmp0 (pspec->name, "disabled")  == 0
            || g_strcmp0 (pspec->name, "position")  == 0
            || g_strcmp0 (pspec->name, "topic")     == 0;

    return FALSE;
}

static void
on_node_any_notify_mark (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;

    /* Ignore purely cosmetic / live-status notifications so a node
     * settling into its runtime appearance after load does not flag the
     * document dirty. */
    if (!property_affects_document (pspec))
        return;

    on_store_changed_mark_modified (user_data);
}

/* ------------------------------------------------------------------ */
/*  Status-message routing                                             */
/* ------------------------------------------------------------------ */

static void
on_debug_status_message (
        PnDebug     *debug,
        const gchar *text,
        gpointer     user_data)
{
    PnFlow *self = PN_FLOW (user_data);
    (void) debug;

    g_signal_emit (self, signals[SIG_STATUS_MESSAGE], 0, text);
}

static void
on_debug_debug_message (
        PnDebug     *debug,
        const gchar *text,
        gpointer     user_data)
{
    PnFlow *self = PN_FLOW (user_data);
    (void) debug;

    g_signal_emit (self, signals[SIG_DEBUG_MESSAGE], 0, text);
}

static void
on_node_added (
        PnNodeStore *store,
        PnNode      *node,
        guint        index,
        gpointer     user_data)
{
    PnFlow *self = PN_FLOW (user_data);
    (void) store;
    (void) index;

    if (PN_IS_DEBUG (node))
    {
        g_signal_connect (node, "status-message",
                          G_CALLBACK (on_debug_status_message), self);
        g_signal_connect (node, "debug-message",
                          G_CALLBACK (on_debug_debug_message), self);
    }

    /* Modified-flag plumbing.  Any g_object_set on the new node (the
     * configure dialog, disable toggles, drag-induced position writes,
     * the worksheet-property reassignment when sheets are renamed, …)
     * marks the flow modified.  Per-node connection so that runtime
     * self-updates that bypass g_object_set — PnRate writing the
     * cached rate field directly during a poll — do not falsely
     * trigger this hook. */
    g_signal_connect (node, "notify",
                      G_CALLBACK (on_node_any_notify_mark), self);
}

static void
on_node_removed (
        PnNodeStore *store,
        PnNode      *node,
        guint        index,
        gpointer     user_data)
{
    PnFlow *self = PN_FLOW (user_data);
    (void) store; (void) index;

    g_signal_handlers_disconnect_by_func (
            node, G_CALLBACK (on_node_any_notify_mark), self);
    if (PN_IS_DEBUG (node))
    {
        g_signal_handlers_disconnect_by_func (
                node, G_CALLBACK (on_debug_status_message), self);
        g_signal_handlers_disconnect_by_func (
                node, G_CALLBACK (on_debug_debug_message), self);
    }
}

/* ------------------------------------------------------------------ */
/*  JSON serialisation                                                 */
/* ------------------------------------------------------------------ */

/** Decide whether a #GParamSpec describes a property that round-trips
 *  through the on-disk format.  We persist anything writable that a
 *  subclass of #PnNode introduces; anything declared on #PnNode itself
 *  (or higher up at #GObject) is either stored elsewhere or rebuilt by
 *  the type's init(). */
static gboolean
should_serialize_property (GParamSpec *pspec)
{
    if ((pspec->flags & G_PARAM_WRITABLE) == 0 ||
        (pspec->flags & G_PARAM_READABLE) == 0)
        return FALSE;

    /* Construct-only properties can only be set at construction time, so
     * re-applying one on load via g_object_set_property would raise a
     * GObject-CRITICAL.  They are not user state worth persisting anyway
     * (e.g. PnAutoTrigger:autostart is a test seam, always TRUE in
     * normal use), so keep them out of the on-disk bag entirely. */
    if ((pspec->flags & G_PARAM_CONSTRUCT_ONLY) != 0)
        return FALSE;

    return pspec->owner_type != PN_TYPE_NODE
        && pspec->owner_type != G_TYPE_OBJECT;
}

/** Convert a #GValue carrying one of the supported scalar types into a
 *  fresh #JsonNode.  Returns %NULL for unsupported types. */
static JsonNode *
gvalue_to_json (const GValue *value)
{
    GType type = G_VALUE_TYPE (value);
    JsonNode *out = NULL;

    if (G_TYPE_IS_ENUM (type))
    {
        GEnumClass *klass = g_type_class_ref (type);
        GEnumValue *ev    = g_enum_get_value (klass, g_value_get_enum (value));

        out = json_node_new (JSON_NODE_VALUE);
        json_node_set_string (out, ev != NULL ? ev->value_nick : "");
        g_type_class_unref (klass);
        return out;
    }

    if (type == GDK_TYPE_RGBA)
    {
        const GdkRGBA *rgba = g_value_get_boxed (value);
        out = json_node_new (JSON_NODE_VALUE);
        if (rgba != NULL)
        {
            gchar *s = gdk_rgba_to_string (rgba);
            json_node_set_string (out, s);
            g_free (s);
        }
        else
            json_node_set_string (out, "");
        return out;
    }

    switch (type)
    {
    case G_TYPE_STRING:
        {
            const gchar *s = g_value_get_string (value);
            out = json_node_new (JSON_NODE_VALUE);
            if (s != NULL)
                json_node_set_string (out, s);
            else
                json_node_set_string (out, "");
        }
        break;
    case G_TYPE_BOOLEAN:
        out = json_node_new (JSON_NODE_VALUE);
        json_node_set_boolean (out, g_value_get_boolean (value));
        break;
    case G_TYPE_INT:
        out = json_node_new (JSON_NODE_VALUE);
        json_node_set_int (out, g_value_get_int (value));
        break;
    case G_TYPE_UINT:
        out = json_node_new (JSON_NODE_VALUE);
        json_node_set_int (out, g_value_get_uint (value));
        break;
    case G_TYPE_INT64:
        out = json_node_new (JSON_NODE_VALUE);
        json_node_set_int (out, g_value_get_int64 (value));
        break;
    case G_TYPE_UINT64:
        out = json_node_new (JSON_NODE_VALUE);
        json_node_set_int (out, (gint64) g_value_get_uint64 (value));
        break;
    case G_TYPE_FLOAT:
        out = json_node_new (JSON_NODE_VALUE);
        json_node_set_double (out, g_value_get_float (value));
        break;
    case G_TYPE_DOUBLE:
        out = json_node_new (JSON_NODE_VALUE);
        json_node_set_double (out, g_value_get_double (value));
        break;
    default:
        break;
    }

    return out;
}

/** Inverse of gvalue_to_json: pull the value out of a #JsonNode and
 *  store it in @out_value, which must already be initialised to the
 *  GType the property expects.  Returns %FALSE when the JSON shape
 *  does not match the declared type. */
static gboolean
json_to_gvalue (
        JsonNode    *json,
        GParamSpec  *pspec,
        GValue      *out_value)
{
    GType type = G_PARAM_SPEC_VALUE_TYPE (pspec);

    if (G_TYPE_IS_ENUM (type))
    {
        GEnumClass *klass;
        GEnumValue *ev;
        const gchar *nick;

        if (!JSON_NODE_HOLDS_VALUE (json) ||
            json_node_get_value_type (json) != G_TYPE_STRING)
            return FALSE;

        nick  = json_node_get_string (json);
        klass = g_type_class_ref (type);
        ev    = g_enum_get_value_by_nick (klass, nick);
        if (ev == NULL)
            ev = g_enum_get_value_by_name (klass, nick);

        if (ev == NULL)
        {
            g_type_class_unref (klass);
            return FALSE;
        }

        g_value_set_enum (out_value, ev->value);
        g_type_class_unref (klass);
        return TRUE;
    }

    if (type == GDK_TYPE_RGBA)
    {
        GdkRGBA rgba;

        if (!JSON_NODE_HOLDS_VALUE (json) ||
            json_node_get_value_type (json) != G_TYPE_STRING)
            return FALSE;

        if (!gdk_rgba_parse (&rgba, json_node_get_string (json)))
            return FALSE;

        g_value_set_boxed (out_value, &rgba);
        return TRUE;
    }

    if (!JSON_NODE_HOLDS_VALUE (json))
        return FALSE;

    switch (type)
    {
    case G_TYPE_STRING:
        g_value_set_string (out_value, json_node_get_string (json));
        return TRUE;
    case G_TYPE_BOOLEAN:
        g_value_set_boolean (out_value, json_node_get_boolean (json));
        return TRUE;
    case G_TYPE_INT:
        g_value_set_int (out_value, (gint) json_node_get_int (json));
        return TRUE;
    case G_TYPE_UINT:
        g_value_set_uint (out_value, (guint) json_node_get_int (json));
        return TRUE;
    case G_TYPE_INT64:
        g_value_set_int64 (out_value, json_node_get_int (json));
        return TRUE;
    case G_TYPE_UINT64:
        g_value_set_uint64 (out_value, (guint64) json_node_get_int (json));
        return TRUE;
    case G_TYPE_FLOAT:
        g_value_set_float (out_value, (gfloat) json_node_get_double (json));
        return TRUE;
    case G_TYPE_DOUBLE:
        g_value_set_double (out_value, json_node_get_double (json));
        return TRUE;
    default:
        return FALSE;
    }
}

/** Serialise @node to a fresh #JsonObject.  The caller owns the
 *  returned object. */
static JsonObject *
node_to_json (PnNode *node)
{
    JsonObject    *obj   = json_object_new ();
    JsonObject    *pos   = json_object_new ();
    JsonObject    *props = json_object_new ();
    const PnPoint *p     = pn_node_get_position (node);
    const gchar   *name  = pn_node_get_name     (node);
    GParamSpec   **specs;
    guint          n_specs;
    guint          i;

    json_object_set_string_member (obj, "type", G_OBJECT_TYPE_NAME (node));
    /* Stable per-node identity carried across save/load and into the
     * message envelope.  Always written so reloading round-trips it
     * exactly; older worksheets that lack the field gain a fresh
     * UUID at load time. */
    {
        const gchar *uuid = pn_node_get_uuid (node);
        if (uuid != NULL)
            json_object_set_string_member (obj, "uuid", uuid);
    }
    if (name != NULL)
        json_object_set_string_member (obj, "name", name);

    /* Sheet tag for the future multi-worksheet UI.  Persisted
     * always so the field round-trips exactly; older worksheets
     * that lack it pick up the "Worksheet" default at load. */
    {
        const gchar *ws = pn_node_get_worksheet (node);
        if (ws != NULL)
            json_object_set_string_member (obj, "worksheet", ws);
    }

    /* Only persist when set: keeps the on-disk format unchanged for
     * existing flows where every node is enabled. */
    if (pn_node_get_disabled (node))
        json_object_set_boolean_member (obj, "disabled", TRUE);

    /* Topic template override.  Persist only when the user actually
     * customised it — pn_node_get_topic() returns %NULL while the
     * node is using its built-in default, so a freshly-created node
     * round-trips through save/load without the field appearing on
     * disk.  Surfaced at the node level rather than inside the
     * `properties` bag because the property is owned by PnNode and
     * the bag intentionally excludes base-class properties. */
    {
        const gchar *topic = pn_node_get_topic (node);
        if (topic != NULL && *topic != '\0')
            json_object_set_string_member (obj, "topic", topic);
    }

    json_object_set_double_member (pos, "x", p ? p->x : 0.0);
    json_object_set_double_member (pos, "y", p ? p->y : 0.0);
    json_object_set_object_member (obj, "position", pos);

    specs = g_object_class_list_properties (G_OBJECT_GET_CLASS (node), &n_specs);
    for (i = 0; i < n_specs; i++)
    {
        GParamSpec *pspec = specs[i];
        GValue      value = G_VALUE_INIT;
        JsonNode   *jn;

        if (!should_serialize_property (pspec))
            continue;

        g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
        g_object_get_property (G_OBJECT (node), pspec->name, &value);
        jn = gvalue_to_json (&value);
        if (jn != NULL)
            json_object_set_member (props, pspec->name, jn);
        else
            g_warning ("pn-flow: property %s::%s of type %s is not "
                       "serialisable; skipping",
                       G_OBJECT_TYPE_NAME (node), pspec->name,
                       g_type_name (G_PARAM_SPEC_VALUE_TYPE (pspec)));
        g_value_unset (&value);
    }
    g_free (specs);

    json_object_set_object_member (obj, "properties", props);
    return obj;
}

/** Build a #PnNode from its JSON representation.  Returns %NULL on
 *  failure with @error set; otherwise the caller owns the new node. */
static PnNode *
node_from_json (
        JsonObject *obj,
        GError    **error)
{
    const gchar *type_name;
    GType        type;
    PnNode      *node;
    JsonObject  *props;

    if (!json_object_has_member (obj, "type"))
    {
        g_set_error_literal (error, PN_FLOW_ERROR, 0,
                             "node entry is missing required \"type\" field");
        return NULL;
    }

    type_name = json_object_get_string_member (obj, "type");
    type      = pn_node_factory_lookup (pn_node_factory_get_default (),
                                        type_name);

    if (type == G_TYPE_INVALID || !g_type_is_a (type, PN_TYPE_NODE))
    {
        g_set_error (error, PN_FLOW_ERROR, 0,
                     "unknown node type \"%s\"", type_name);
        return NULL;
    }

    node = pn_node_factory_create_for_type (
            pn_node_factory_get_default (), type);

    if (json_object_has_member (obj, "position"))
    {
        JsonObject *pos = json_object_get_object_member (obj, "position");
        PnPoint     p   = { 0.0, 0.0 };

        if (pos != NULL)
        {
            if (json_object_has_member (pos, "x"))
                p.x = json_object_get_double_member (pos, "x");
            if (json_object_has_member (pos, "y"))
                p.y = json_object_get_double_member (pos, "y");
        }
        pn_node_set_position (node, &p);
    }

    /* Restore the persisted UUID before any other field so the rest
     * of the load path sees the node with its on-disk identity.
     * Older worksheets without this field keep the random UUID
     * minted by pn_node_init — they will then save back with the new
     * field populated. */
    if (json_object_has_member (obj, "uuid"))
    {
        const gchar *u = json_object_get_string_member (obj, "uuid");
        if (u != NULL && *u != '\0')
            pn_node_set_uuid (node, u);
    }

    if (json_object_has_member (obj, "name"))
    {
        const gchar *nm = json_object_get_string_member (obj, "name");
        if (nm != NULL)
            pn_node_set_name (node, nm);
    }

    /* Older worksheets predate this field — leave the init-time
     * "Worksheet" default in place when missing. */
    if (json_object_has_member (obj, "worksheet"))
    {
        const gchar *ws = json_object_get_string_member (obj, "worksheet");
        if (ws != NULL)
            pn_node_set_worksheet (node, ws);
    }

    if (json_object_has_member (obj, "disabled"))
        pn_node_set_disabled (
                node,
                json_object_get_boolean_member (obj, "disabled"));

    /* Restore the topic-template override, when present. */
    if (json_object_has_member (obj, "topic"))
    {
        const gchar *t = json_object_get_string_member (obj, "topic");
        if (t != NULL)
            pn_node_set_topic (node, t);
    }

    if (json_object_has_member (obj, "properties"))
        props = json_object_get_object_member (obj, "properties");
    else
        props = NULL;

    if (props != NULL)
    {
        GList *members = json_object_get_members (props);
        GList *iter;

        for (iter = members; iter != NULL; iter = iter->next)
        {
            const gchar *key   = iter->data;
            JsonNode    *jn    = json_object_get_member (props, key);
            GParamSpec  *pspec = g_object_class_find_property (
                    G_OBJECT_GET_CLASS (node), key);
            GValue       value = G_VALUE_INIT;

            /* Migration path for pre-PnNode-topic flows where the only
             * node carrying a "topic" property was #PnChat (which
             * owned it on the subclass and persisted it inside the
             * properties bag).  PnChat has since dropped its own
             * topic property in favour of the base-class one, so on
             * load we forward any "topic" found in the bag to the
             * #PnNode setter rather than warning about an unknown
             * property and losing the user's value. */
            if (g_strcmp0 (key, "topic") == 0 && JSON_NODE_HOLDS_VALUE (jn)
                && json_node_get_value_type (jn) == G_TYPE_STRING)
            {
                pn_node_set_topic (node, json_node_get_string (jn));
                continue;
            }

            if (pspec == NULL || !should_serialize_property (pspec))
            {
                /* Older worksheets may carry construct-only keys (e.g.
                 * PnAutoTrigger:autostart) that we now exclude from the
                 * bag.  They cannot be applied post-construction; skip
                 * them quietly rather than crying "unknown property". */
                if (pspec != NULL &&
                    (pspec->flags & G_PARAM_CONSTRUCT_ONLY) != 0)
                    continue;

                g_warning ("pn-flow: ignoring unknown property %s::%s",
                           type_name, key);
                continue;
            }

            g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
            if (json_to_gvalue (jn, pspec, &value))
                g_object_set_property (G_OBJECT (node), key, &value);
            else
                g_warning ("pn-flow: property %s::%s — JSON value does "
                           "not match expected type %s",
                           type_name, key,
                           g_type_name (G_PARAM_SPEC_VALUE_TYPE (pspec)));
            g_value_unset (&value);
        }
        g_list_free (members);
    }

    return node;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnNodeStore *
pn_flow_get_nodes (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), NULL);
    return self->nodes;
}

PnWireStore *
pn_flow_get_wires (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), NULL);
    return self->wires;
}

void
pn_flow_clear (PnFlow *self)
{
    guint i;

    g_return_if_fail (PN_IS_FLOW (self));

    /* Suppress modified-changed during the cascade of node-removed /
     * wire-removed notifications: a clear-to-fresh-state should leave
     * the flow reporting unmodified once it settles. */
    self->loading = TRUE;
    pn_node_store_clear (self->nodes);
    pn_wire_store_clear (self->wires);

    /* Tell every host that all but the canonical default sheet just
     * went away.  Walk a copy so emitting "sheet-removed" in the loop
     * cannot trip over the live array. */
    {
        GPtrArray *removed = g_ptr_array_new_with_free_func (g_free);
        for (i = 0; i < self->sheets->len; i++)
        {
            const gchar *name = self->sheets->pdata[i];
            if (g_strcmp0 (name, PN_FLOW_DEFAULT_SHEET) != 0)
                g_ptr_array_add (removed, g_strdup (name));
        }
        sheet_list_reset_default (self);
        for (i = 0; i < removed->len; i++)
            g_signal_emit (self, signals[SIG_SHEET_REMOVED], 0,
                           (const gchar *) removed->pdata[i]);
        g_signal_emit (self, signals[SIG_ACTIVE_SHEET_CHANGED], 0,
                       self->active_sheet);
        g_ptr_array_unref (removed);
    }

    self->loading = FALSE;

    if (self->modified)
    {
        self->modified = FALSE;
        g_signal_emit (self, signals[SIG_MODIFIED_CHANGED], 0, FALSE);
    }
}

void
pn_flow_add_node (PnFlow *self, PnNode *node)
{
    g_return_if_fail (PN_IS_FLOW (self));
    g_return_if_fail (PN_IS_NODE (node));

    flow_assign_default_name (self, node);
    pn_node_store_add (self->nodes, node);
}

const gchar *const *
pn_flow_get_sheets (PnFlow *self, guint *out_n)
{
    g_return_val_if_fail (PN_IS_FLOW (self), NULL);

    if (out_n != NULL)
        *out_n = self->sheets->len;
    return (const gchar *const *) self->sheets->pdata;
}

const gchar *
pn_flow_get_active_sheet (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), NULL);
    return self->active_sheet;
}

void
pn_flow_set_active_sheet (PnFlow *self, const gchar *name)
{
    g_return_if_fail (PN_IS_FLOW (self));

    if (name == NULL || sheet_find_index (self, name) < 0)
        return;
    if (g_strcmp0 (self->active_sheet, name) == 0)
        return;

    g_free (self->active_sheet);
    self->active_sheet = g_strdup (name);
    g_signal_emit (self, signals[SIG_ACTIVE_SHEET_CHANGED], 0,
                   self->active_sheet);
}

gboolean
pn_flow_add_sheet (PnFlow *self, const gchar *name)
{
    gchar *minted = NULL;
    const gchar *use;

    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);

    if (name == NULL || *name == '\0')
    {
        minted = sheet_mint_default_name (self);
        use    = minted;
    }
    else
    {
        if (sheet_find_index (self, name) >= 0)
            return FALSE;
        use = name;
    }

    g_ptr_array_add (self->sheets, g_strdup (use));

    /* Borrowed pointer into the array — safe for the duration of the
     * synchronous emission since nothing else mutates the array
     * mid-emit. */
    g_signal_emit (self, signals[SIG_SHEET_ADDED], 0,
                   (const gchar *) self->sheets->pdata[self->sheets->len - 1]);
    pn_flow_set_modified (self, TRUE);

    g_free (minted);
    return TRUE;
}

gboolean
pn_flow_remove_sheet (PnFlow *self, const gchar *name)
{
    gint  index;
    gchar *removed_name;
    gboolean was_active;
    guint  i;
    GPtrArray *victims;

    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);

    if (self->sheets->len <= 1)
        return FALSE;

    index = sheet_find_index (self, name);
    if (index < 0)
        return FALSE;

    /* Snapshot the names before mutating: every node that lives on the
     * outgoing sheet is about to be removed from the store, which will
     * fire node-removed for each one — those notifications are fine,
     * but we want them to settle before we drop the sheet entry. */
    victims = g_ptr_array_new ();
    {
        const guint count = pn_node_store_get_length (self->nodes);
        for (i = 0; i < count; i++)
        {
            PnNode *node = pn_node_store_get_node (self->nodes, i);
            if (g_strcmp0 (pn_node_get_worksheet (node), name) == 0)
                g_ptr_array_add (victims, node);
        }
    }
    for (i = 0; i < victims->len; i++)
        pn_node_store_remove (self->nodes, victims->pdata[i]);
    g_ptr_array_unref (victims);

    was_active = (g_strcmp0 (self->active_sheet, name) == 0);
    removed_name = g_strdup (name);
    g_ptr_array_remove_index (self->sheets, (guint) index);

    if (was_active)
    {
        guint pick = (guint) index;
        if (pick >= self->sheets->len)
            pick = self->sheets->len - 1;

        g_free (self->active_sheet);
        self->active_sheet = g_strdup (self->sheets->pdata[pick]);
        g_signal_emit (self, signals[SIG_ACTIVE_SHEET_CHANGED], 0,
                       self->active_sheet);
    }

    g_signal_emit (self, signals[SIG_SHEET_REMOVED], 0, removed_name);
    pn_flow_set_modified (self, TRUE);
    g_free (removed_name);
    return TRUE;
}

gboolean
pn_flow_rename_sheet (
        PnFlow      *self,
        const gchar *from,
        const gchar *to)
{
    gint  index;
    gint  collide;
    guint i;
    guint count;

    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    g_return_val_if_fail (from != NULL, FALSE);

    if (to == NULL || *to == '\0')
        return FALSE;

    if (g_strcmp0 (from, to) == 0)
        return TRUE;            /* trivial no-op success */

    index   = sheet_find_index (self, from);
    if (index < 0)
        return FALSE;

    collide = sheet_find_index (self, to);
    if (collide >= 0 && collide != index)
        return FALSE;

    /* Replace the array entry first so notify::worksheet observers see
     * the new name reflected in the sheet list. */
    g_free (self->sheets->pdata[index]);
    self->sheets->pdata[index] = g_strdup (to);

    if (g_strcmp0 (self->active_sheet, from) == 0)
    {
        g_free (self->active_sheet);
        self->active_sheet = g_strdup (to);
        g_signal_emit (self, signals[SIG_ACTIVE_SHEET_CHANGED], 0,
                       self->active_sheet);
    }

    /* Walk the node store and migrate the per-node tag.  The notify
     * callbacks marked the flow modified along the way; emit
     * sheet-renamed at the end so the host can relabel exactly once. */
    count = pn_node_store_get_length (self->nodes);
    for (i = 0; i < count; i++)
    {
        PnNode *node = pn_node_store_get_node (self->nodes, i);
        if (g_strcmp0 (pn_node_get_worksheet (node), from) == 0)
            pn_node_set_worksheet (node, to);
    }

    g_signal_emit (self, signals[SIG_SHEET_RENAMED], 0, from, to);
    pn_flow_set_modified (self, TRUE);
    return TRUE;
}

void
pn_flow_reorder_sheet (
        PnFlow      *self,
        const gchar *name,
        guint        new_index)
{
    gint  index;
    gpointer entry;

    g_return_if_fail (PN_IS_FLOW (self));

    index = sheet_find_index (self, name);
    if (index < 0)
        return;

    if (new_index >= self->sheets->len)
        new_index = self->sheets->len - 1;
    if ((guint) index == new_index)
        return;

    /* Steal the strdup'd pointer and re-insert it without invoking the
     * array's free_func: we are repositioning, not deleting. */
    entry = g_ptr_array_steal_index (self->sheets, (guint) index);
    g_ptr_array_insert (self->sheets, (gint) new_index, entry);

    g_signal_emit (self, signals[SIG_SHEETS_REORDERED], 0);
    pn_flow_set_modified (self, TRUE);
}

gboolean
pn_flow_load_from_file (
        PnFlow      *self,
        const gchar *path,
        GError     **error)
{
    JsonParser *parser;
    JsonNode   *root;
    JsonObject *obj;
    JsonArray  *nodes_arr;
    JsonArray  *conns_arr;
    GPtrArray  *new_nodes;
    GPtrArray  *new_wires;
    GHashTable *by_name;
    GHashTable *by_uuid;
    guint       n;
    guint       i;
    gboolean    ok;

    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    /* Touch the factory once up front so every built-in (and any
     * already-loaded plugin) class is registered before node_from_json
     * starts looking type names up.  The factory is a singleton — this
     * is a no-op after the first call. */
    (void) pn_node_factory_get_default ();

    parser = json_parser_new ();
    ok     = json_parser_load_from_file (parser, path, error);
    if (!ok)
    {
        g_object_unref (parser);
        return FALSE;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_set_error_literal (error, PN_FLOW_ERROR, 0,
                             "worksheet file root is not a JSON object");
        g_object_unref (parser);
        return FALSE;
    }

    obj = json_node_get_object (root);

    /* Build the new model on the side and only commit once parsing has
     * fully succeeded — a malformed file leaves the existing contents
     * untouched. */
    new_nodes = g_ptr_array_new_with_free_func (g_object_unref);
    new_wires = g_ptr_array_new_with_free_func (g_object_unref);
    by_name   = g_hash_table_new (g_str_hash, g_str_equal);
    by_uuid   = g_hash_table_new (g_str_hash, g_str_equal);

    nodes_arr = json_object_has_member (obj, "nodes")
        ? json_object_get_array_member (obj, "nodes")
        : NULL;
    n = nodes_arr != NULL ? json_array_get_length (nodes_arr) : 0;

    for (i = 0; i < n; i++)
    {
        JsonNode   *entry = json_array_get_element (nodes_arr, i);
        JsonObject *node_obj;
        PnNode     *node;
        const gchar *nm;

        if (!JSON_NODE_HOLDS_OBJECT (entry))
            continue;

        node_obj = json_node_get_object (entry);
        node     = node_from_json (node_obj, error);
        if (node == NULL)
            goto fail;

        /* Index by UUID for the new wire-resolution path, and by name
         * for the legacy fallback used by older worksheets where wires
         * still encode endpoints as `{ source, target }` name strings.
         * If the file omitted a name or two nodes share one, fall
         * through to the default-namer so each entry still gets a
         * unique label after the commit. */
        {
            const gchar *uid = pn_node_get_uuid (node);
            if (uid != NULL && *uid != '\0' &&
                !g_hash_table_contains (by_uuid, uid))
                g_hash_table_insert (by_uuid, (gpointer) uid, node);
        }

        nm = pn_node_get_name (node);
        if (nm != NULL && *nm != '\0' && !g_hash_table_contains (by_name, nm))
            g_hash_table_insert (by_name, (gpointer) nm, node);

        g_ptr_array_add (new_nodes, node);
    }

    conns_arr = json_object_has_member (obj, "connections")
        ? json_object_get_array_member (obj, "connections")
        : NULL;
    n = conns_arr != NULL ? json_array_get_length (conns_arr) : 0;

    for (i = 0; i < n; i++)
    {
        JsonNode    *entry = json_array_get_element (conns_arr, i);
        JsonObject  *cobj;
        const gchar *src_id;
        const gchar *dst_id;
        const gchar *src_name;
        const gchar *dst_name;
        gint         dst_input;
        PnNode      *src = NULL;
        PnNode      *dst = NULL;

        if (!JSON_NODE_HOLDS_OBJECT (entry))
            continue;

        cobj     = json_node_get_object (entry);
        src_id   = json_object_has_member (cobj, "source_id")
            ? json_object_get_string_member (cobj, "source_id") : NULL;
        dst_id   = json_object_has_member (cobj, "target_id")
            ? json_object_get_string_member (cobj, "target_id") : NULL;
        src_name = json_object_has_member (cobj, "source")
            ? json_object_get_string_member (cobj, "source") : NULL;
        dst_name = json_object_has_member (cobj, "target")
            ? json_object_get_string_member (cobj, "target") : NULL;
        /* Which input port of the target this wire feeds; absent (the
         * usual single-input case) means port 0. */
        dst_input = json_object_has_member (cobj, "target_input")
            ? (gint) json_object_get_int_member (cobj, "target_input") : 0;

        /* Prefer UUID-keyed endpoints; fall back to the legacy
         * name-keyed shape so worksheets written before the migration
         * keep loading. */
        if (src_id != NULL && *src_id != '\0')
            src = g_hash_table_lookup (by_uuid, src_id);
        if (src == NULL && src_name != NULL)
            src = g_hash_table_lookup (by_name, src_name);

        if (dst_id != NULL && *dst_id != '\0')
            dst = g_hash_table_lookup (by_uuid, dst_id);
        if (dst == NULL && dst_name != NULL)
            dst = g_hash_table_lookup (by_name, dst_name);

        if (src == NULL || dst == NULL)
        {
            g_warning ("pn-flow: connection refers to unknown node "
                       "(%s -> %s); skipping",
                       src_id != NULL ? src_id : (src_name ? src_name : "?"),
                       dst_id != NULL ? dst_id : (dst_name ? dst_name : "?"));
            continue;
        }

        /* Cross-sheet wires are forbidden by design — the GUI rejects
         * them at creation time, but a hand-edited file or a file
         * coming from a future cross-sheet feature could still carry
         * them.  Drop with a warning so loading degrades gracefully. */
        if (g_strcmp0 (pn_node_get_worksheet (src),
                       pn_node_get_worksheet (dst)) != 0)
        {
            const gchar *src_uid = pn_node_get_uuid (src);
            const gchar *dst_uid = pn_node_get_uuid (dst);
            g_warning ("pn-flow: dropping cross-sheet wire %s -> %s "
                       "(sheets %s vs %s)",
                       src_uid != NULL ? src_uid : "?",
                       dst_uid != NULL ? dst_uid : "?",
                       pn_node_get_worksheet (src),
                       pn_node_get_worksheet (dst));
            continue;
        }

        g_ptr_array_add (new_wires, pn_wire_new_full (src, dst, dst_input));
    }

    /* Commit.  During the cascade of node-added / wire-added events
     * fired by the stores below the modified-flag plumbing would
     * otherwise flip the flag to TRUE; the loading guard suppresses
     * those emissions so a freshly-loaded file reports as unmodified. */
    pn_flow_clear (self);
    self->loading = TRUE;
    for (i = 0; i < new_nodes->len; i++)
    {
        PnNode *node = new_nodes->pdata[i];
        flow_assign_default_name (self, node);
        pn_node_store_add (self->nodes, node);
    }
    for (i = 0; i < new_wires->len; i++)
        pn_wire_store_add (self->wires, new_wires->pdata[i]);

    /* Sheet list: explicit "sheets" array if the file carries one,
     * otherwise derive from the distinct per-node worksheet tags
     * (every node carries the "Worksheet" default if the file predates
     * the per-node worksheet field).  Append nodes' own sheet names
     * not already covered, so a hand-edited file with extra worksheet
     * tags but no top-level "sheets" array still surfaces every sheet
     * in the UI. */
    {
        JsonArray  *sheets_arr = json_object_has_member (obj, "sheets")
            ? json_object_get_array_member (obj, "sheets") : NULL;
        guint       s;
        const gchar *active_in_file;
        GPtrArray  *old_names;

        /* Snapshot the current sheet names (just "Worksheet" right
         * now, since pn_flow_clear above reset us to the default) so
         * we can fire one "sheet-removed" per stale entry below. */
        old_names = g_ptr_array_new_with_free_func (g_free);
        for (s = 0; s < self->sheets->len; s++)
            g_ptr_array_add (old_names,
                             g_strdup (self->sheets->pdata[s]));

        g_ptr_array_set_size (self->sheets, 0);

        if (sheets_arr != NULL)
        {
            const guint sn = json_array_get_length (sheets_arr);
            for (s = 0; s < sn; s++)
            {
                const gchar *name = json_array_get_string_element (sheets_arr, s);
                if (name == NULL || *name == '\0')
                    continue;
                if (sheet_find_index (self, name) >= 0)
                    continue;
                g_ptr_array_add (self->sheets, g_strdup (name));
            }
        }

        /* Backfill from node tags so any sheet referenced by a node
         * but missing from the explicit list (or any sheet at all when
         * the list was absent) still appears in the tab order. */
        for (i = 0; i < new_nodes->len; i++)
        {
            PnNode      *node = new_nodes->pdata[i];
            const gchar *ws   = pn_node_get_worksheet (node);
            if (ws != NULL && *ws != '\0' &&
                sheet_find_index (self, ws) < 0)
                g_ptr_array_add (self->sheets, g_strdup (ws));
        }

        if (self->sheets->len == 0)
            g_ptr_array_add (self->sheets,
                             g_strdup (PN_FLOW_DEFAULT_SHEET));

        g_clear_pointer (&self->active_sheet, g_free);
        active_in_file = json_object_has_member (obj, "active_sheet")
            ? json_object_get_string_member (obj, "active_sheet") : NULL;
        if (active_in_file != NULL &&
            sheet_find_index (self, active_in_file) >= 0)
            self->active_sheet = g_strdup (active_in_file);
        else
            self->active_sheet = g_strdup (self->sheets->pdata[0]);

        /* Drop the stale tabs first so a "Worksheet"-only tab strip
         * left behind by pn_flow_clear above can still be replaced
         * cleanly by a multi-sheet payload that does NOT include
         * "Worksheet". */
        for (s = 0; s < old_names->len; s++)
        {
            const gchar *old = old_names->pdata[s];
            if (sheet_find_index (self, old) < 0)
                g_signal_emit (self, signals[SIG_SHEET_REMOVED], 0, old);
        }
        for (s = 0; s < self->sheets->len; s++)
        {
            const gchar *new_name = self->sheets->pdata[s];
            gboolean was_present = FALSE;
            guint o;
            for (o = 0; o < old_names->len; o++)
                if (g_strcmp0 (old_names->pdata[o], new_name) == 0)
                { was_present = TRUE; break; }
            if (!was_present)
                g_signal_emit (self, signals[SIG_SHEET_ADDED], 0, new_name);
        }
        g_signal_emit (self, signals[SIG_SHEETS_REORDERED], 0);
        g_signal_emit (self, signals[SIG_ACTIVE_SHEET_CHANGED], 0,
                       self->active_sheet);
        g_ptr_array_unref (old_names);
    }

    self->loading = FALSE;
    if (self->modified)
    {
        self->modified = FALSE;
        g_signal_emit (self, signals[SIG_MODIFIED_CHANGED], 0, FALSE);
    }

    g_ptr_array_unref  (new_wires);
    g_ptr_array_unref  (new_nodes);
    g_hash_table_destroy (by_name);
    g_hash_table_destroy (by_uuid);
    g_object_unref     (parser);
    return TRUE;

fail:
    g_ptr_array_unref  (new_wires);
    g_ptr_array_unref  (new_nodes);
    g_hash_table_destroy (by_name);
    g_hash_table_destroy (by_uuid);
    g_object_unref     (parser);
    return FALSE;
}

/** Build the root JSON object for a serialisation pass.  When @subset
 *  is %NULL every node and wire in @self is included; otherwise the
 *  subset is taken as-is and wires are filtered to those whose source
 *  AND target both appear in @subset.  Caller owns the returned node
 *  and must json_node_free() it. */
static JsonNode *
flow_build_root (
        PnFlow      *self,
        GList       *subset,
        const gchar *format_tag)
{
    JsonObject *obj         = json_object_new ();
    JsonArray  *nodes       = json_array_new ();
    JsonArray  *connections = json_array_new ();
    JsonNode   *root;
    GHashTable *included    = NULL; /* set of PnNode* in @subset */
    guint       count;
    guint       i;

    json_object_set_string_member (obj, "format",  format_tag);
    json_object_set_int_member    (obj, "version", PN_FLOW_FILE_VERSION);
    json_object_set_array_member  (obj, "nodes",   nodes);
    json_object_set_array_member  (obj, "connections", connections);

    /* Sheet list and active sheet are written for the on-disk format
     * (whole-flow save) but skipped for clipboard payloads — pasting
     * is a per-node operation, not a worksheet replacement, and the
     * destination flow already has its own sheets. */
    if (g_strcmp0 (format_tag, PN_FLOW_FILE_FORMAT) == 0)
    {
        JsonArray *sheets_arr = json_array_new ();
        guint      s;

        for (s = 0; s < self->sheets->len; s++)
            json_array_add_string_element (sheets_arr,
                                           self->sheets->pdata[s]);
        json_object_set_array_member (obj, "sheets", sheets_arr);

        if (self->active_sheet != NULL)
            json_object_set_string_member (obj, "active_sheet",
                                           self->active_sheet);
    }

    if (subset != NULL)
    {
        GList *l;
        included = g_hash_table_new (g_direct_hash, g_direct_equal);
        for (l = subset; l != NULL; l = l->next)
        {
            PnNode     *node  = PN_NODE (l->data);
            JsonObject *entry = node_to_json (node);
            json_array_add_object_element (nodes, entry);
            g_hash_table_add (included, node);
        }
    }
    else
    {
        count = pn_node_store_get_length (self->nodes);
        for (i = 0; i < count; i++)
        {
            PnNode     *node  = pn_node_store_get_node (self->nodes, i);
            JsonObject *entry = node_to_json (node);
            json_array_add_object_element (nodes, entry);
        }
    }

    count = pn_wire_store_get_length (self->wires);
    for (i = 0; i < count; i++)
    {
        PnWire      *wire    = pn_wire_store_get_wire (self->wires, i);
        PnNode      *src     = pn_wire_get_source (wire);
        PnNode      *dst     = pn_wire_get_target (wire);
        const gchar *src_uid = src != NULL ? pn_node_get_uuid (src) : NULL;
        const gchar *dst_uid = dst != NULL ? pn_node_get_uuid (dst) : NULL;
        JsonObject  *entry;

        if (src_uid == NULL || dst_uid == NULL)
            continue;

        if (included != NULL &&
            (!g_hash_table_contains (included, src) ||
             !g_hash_table_contains (included, dst)))
            continue;

        entry = json_object_new ();
        json_object_set_string_member (entry, "source_id", src_uid);
        json_object_set_string_member (entry, "target_id", dst_uid);
        /* Only multi-input targets need the port index; omit it for the
         * single-input common case so existing files stay byte-identical. */
        {
            const gint dst_input = pn_wire_get_target_input (wire);
            if (dst_input > 0)
                json_object_set_int_member (entry, "target_input", dst_input);
        }
        json_array_add_object_element (connections, entry);
    }

    if (included != NULL)
        g_hash_table_destroy (included);

    root = json_node_new (JSON_NODE_OBJECT);
    json_node_take_object (root, obj);
    return root;
}

gboolean
pn_flow_save_to_file (
        PnFlow      *self,
        const gchar *path,
        GError     **error)
{
    JsonGenerator *gen;
    JsonNode      *root;
    gboolean       ok;

    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    root = flow_build_root (self, NULL, PN_FLOW_FILE_FORMAT);

    gen = json_generator_new ();
    json_generator_set_root   (gen, root);
    json_generator_set_pretty (gen, TRUE);
    json_generator_set_indent (gen, 2);

    ok = json_generator_to_file (gen, path, error);

    g_object_unref (gen);
    json_node_free (root);

    /* Persisting back to disk reconciles in-memory and on-disk state:
     * flip the dirty flag so the host's Save UI can grey out again. */
    if (ok)
        pn_flow_set_modified (self, FALSE);

    return ok;
}

gchar *
pn_flow_serialize_nodes (
        PnFlow *self,
        GList  *subset)
{
    JsonGenerator *gen;
    JsonNode      *root;
    gchar         *out;

    g_return_val_if_fail (PN_IS_FLOW (self), NULL);

    root = flow_build_root (self, subset, PN_FLOW_CLIPBOARD_FORMAT);

    gen = json_generator_new ();
    json_generator_set_root   (gen, root);
    json_generator_set_pretty (gen, TRUE);
    json_generator_set_indent (gen, 2);

    out = json_generator_to_data (gen, NULL);

    g_object_unref (gen);
    json_node_free (root);

    return out;
}

GList *
pn_flow_paste_from_string (
        PnFlow      *self,
        const gchar *json,
        double       dx,
        double       dy,
        GError     **error)
{
    JsonParser  *parser;
    JsonNode    *root;
    JsonObject  *obj;
    JsonArray   *nodes_arr;
    JsonArray   *conns_arr;
    GHashTable  *by_old_name; /* old-name -> PnNode* (borrowed) */
    GHashTable  *by_old_uuid; /* clipboard-uuid -> PnNode* (borrowed) */
    GList       *added = NULL;
    guint        n;
    guint        i;

    g_return_val_if_fail (PN_IS_FLOW (self), NULL);
    g_return_val_if_fail (json != NULL, NULL);

    /* Same one-shot factory bootstrap as in pn_flow_load_from_file —
     * the registry is the authoritative source for "is this node
     * class available right now". */
    (void) pn_node_factory_get_default ();

    parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, json, -1, error))
    {
        g_object_unref (parser);
        return NULL;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_set_error_literal (error, PN_FLOW_ERROR, 0,
                             "clipboard payload root is not a JSON object");
        g_object_unref (parser);
        return NULL;
    }

    obj = json_node_get_object (root);

    by_old_name = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, NULL);
    by_old_uuid = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, NULL);

    nodes_arr = json_object_has_member (obj, "nodes")
        ? json_object_get_array_member (obj, "nodes")
        : NULL;
    n = nodes_arr != NULL ? json_array_get_length (nodes_arr) : 0;

    for (i = 0; i < n; i++)
    {
        JsonNode    *entry = json_array_get_element (nodes_arr, i);
        JsonObject  *node_obj;
        PnNode      *node;
        const PnPoint *p;
        PnPoint      offset_pos;
        const gchar *old_name;

        if (!JSON_NODE_HOLDS_OBJECT (entry))
            continue;

        node_obj = json_node_get_object (entry);
        node     = node_from_json (node_obj, error);
        if (node == NULL)
            goto fail;

        /* Capture the clipboard-supplied UUID *before* minting the
         * fresh one so wires in the same payload (which key on the
         * old UUID) can still resolve back to this node. */
        {
            const gchar *old_uuid = pn_node_get_uuid (node);
            if (old_uuid != NULL && *old_uuid != '\0' &&
                !g_hash_table_contains (by_old_uuid, old_uuid))
                g_hash_table_insert (by_old_uuid,
                                     g_strdup (old_uuid), node);
        }

        /* node_from_json restored the clipboard's UUID — but pasting
         * is a clone, not a move, so the new node must have its own
         * identity.  Mint a fresh UUID here so that a Copy/Paste
         * never produces two live nodes that claim the same `from_id`
         * in their outgoing messages. */
        pn_node_set_uuid (node, NULL);

        /* Apply paste offset on top of whatever node_from_json read. */
        p = pn_node_get_position (node);
        offset_pos.x = (p != NULL ? p->x : 0.0) + dx;
        offset_pos.y = (p != NULL ? p->y : 0.0) + dy;
        pn_node_set_position (node, &offset_pos);

        /* Index by the JSON-supplied name so wires in the same
         * payload that still use the legacy name-keyed shape resolve
         * to this node.  Names are no longer disambiguated on paste —
         * with wires keying on UUID the user is free to reuse a name
         * that already exists in the worksheet. */
        old_name = pn_node_get_name (node);
        if (old_name != NULL && *old_name != '\0' &&
            !g_hash_table_contains (by_old_name, old_name))
            g_hash_table_insert (by_old_name, g_strdup (old_name), node);

        /* Only fills in a default when the JSON had no name; verbatim
         * names (including ones that collide with existing nodes) are
         * preserved. */
        flow_assign_default_name (self, node);

        pn_node_store_add (self->nodes, node);
        added = g_list_prepend (added, node);
        g_object_unref (node); /* store holds the strong ref now */
    }

    conns_arr = json_object_has_member (obj, "connections")
        ? json_object_get_array_member (obj, "connections")
        : NULL;
    n = conns_arr != NULL ? json_array_get_length (conns_arr) : 0;

    for (i = 0; i < n; i++)
    {
        JsonNode    *entry = json_array_get_element (conns_arr, i);
        JsonObject  *cobj;
        const gchar *src_id;
        const gchar *dst_id;
        const gchar *src_name;
        const gchar *dst_name;
        gint         dst_input;
        PnNode      *src = NULL;
        PnNode      *dst = NULL;
        PnWire      *wire;

        if (!JSON_NODE_HOLDS_OBJECT (entry))
            continue;

        cobj     = json_node_get_object (entry);
        src_id   = json_object_has_member (cobj, "source_id")
            ? json_object_get_string_member (cobj, "source_id") : NULL;
        dst_id   = json_object_has_member (cobj, "target_id")
            ? json_object_get_string_member (cobj, "target_id") : NULL;
        src_name = json_object_has_member (cobj, "source")
            ? json_object_get_string_member (cobj, "source") : NULL;
        dst_name = json_object_has_member (cobj, "target")
            ? json_object_get_string_member (cobj, "target") : NULL;
        dst_input = json_object_has_member (cobj, "target_input")
            ? (gint) json_object_get_int_member (cobj, "target_input") : 0;

        /* Prefer UUID; fall back to old name for clipboard payloads
         * generated before the migration. */
        if (src_id != NULL && *src_id != '\0')
            src = g_hash_table_lookup (by_old_uuid, src_id);
        if (src == NULL && src_name != NULL)
            src = g_hash_table_lookup (by_old_name, src_name);

        if (dst_id != NULL && *dst_id != '\0')
            dst = g_hash_table_lookup (by_old_uuid, dst_id);
        if (dst == NULL && dst_name != NULL)
            dst = g_hash_table_lookup (by_old_name, dst_name);

        if (src == NULL || dst == NULL)
        {
            g_warning ("pn-flow: pasted connection refers to unknown "
                       "node (%s -> %s); skipping",
                       src_id != NULL ? src_id : (src_name ? src_name : "?"),
                       dst_id != NULL ? dst_id : (dst_name ? dst_name : "?"));
            continue;
        }

        wire = pn_wire_new_full (src, dst, dst_input);
        pn_wire_store_add (self->wires, wire);
        g_object_unref (wire);
    }

    g_hash_table_destroy (by_old_name);
    g_hash_table_destroy (by_old_uuid);
    g_object_unref (parser);

    return g_list_reverse (added);

fail:
    g_hash_table_destroy (by_old_name);
    g_hash_table_destroy (by_old_uuid);
    g_list_free (added);
    g_object_unref (parser);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  GObject boilerplate                                                */
/* ------------------------------------------------------------------ */

static void
pn_flow_finalize (GObject *object)
{
    PnFlow *self = PN_FLOW (object);

    g_clear_object (&self->nodes);
    g_clear_object (&self->wires);

    g_clear_pointer (&self->sheets, g_ptr_array_unref);
    g_clear_pointer (&self->active_sheet, g_free);

    G_OBJECT_CLASS (pn_flow_parent_class)->finalize (object);
}

static void
pn_flow_class_init (PnFlowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = pn_flow_finalize;

    /* Forwarded text from #PnDebug nodes whose target is the status
     * bar.  Hosts (the worksheet widget, or a CLI) subscribe to
     * surface the text. */
    signals[SIG_STATUS_MESSAGE] = g_signal_new (
            "status-message",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);

    /* Forwarded text from #PnDebug nodes whose target is the debug
     * view. */
    signals[SIG_DEBUG_MESSAGE] = g_signal_new (
            "debug-message",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0,
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);

    /* Sheet list mutation signals: hosts (PnWindow) keep their
     * notebook tabs in sync with the model. */
    signals[SIG_SHEET_ADDED] = g_signal_new (
            "sheet-added",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIG_SHEET_REMOVED] = g_signal_new (
            "sheet-removed",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_STRING);

    /* (from, to) — both strings carried by the signal so the host can
     * relabel the affected tab without scanning the sheet list. */
    signals[SIG_SHEET_RENAMED] = g_signal_new (
            "sheet-renamed",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_STRING);

    signals[SIG_SHEETS_REORDERED] = g_signal_new (
            "sheets-reordered",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 0);

    signals[SIG_ACTIVE_SHEET_CHANGED] = g_signal_new (
            "active-sheet-changed",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_STRING);

    signals[SIG_MODIFIED_CHANGED] = g_signal_new (
            "modified-changed",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
}

static void
pn_flow_init (PnFlow *self)
{
    self->nodes = pn_node_store_new ();
    self->wires = pn_wire_store_new ();

    self->sheets = g_ptr_array_new_with_free_func (g_free);
    sheet_list_reset_default (self);

    g_signal_connect (self->nodes, "node-added",
                      G_CALLBACK (on_node_added), self);
    g_signal_connect (self->nodes, "node-removed",
                      G_CALLBACK (on_node_removed), self);

    /* Modified-flag listeners on the stores themselves: every add or
     * remove flips the flag.  Per-node property changes are caught by
     * on_node_added wiring up notify on each node individually. */
    g_signal_connect (self->nodes, "node-added",
                      G_CALLBACK (on_node_store_node_added_mark), self);
    g_signal_connect (self->nodes, "node-removed",
                      G_CALLBACK (on_node_store_node_removed_mark), self);
    g_signal_connect (self->wires, "wire-added",
                      G_CALLBACK (on_wire_store_wire_added_mark), self);
    g_signal_connect (self->wires, "wire-removed",
                      G_CALLBACK (on_wire_store_wire_removed_mark), self);
}

PnFlow *
pn_flow_new (void)
{
    return g_object_new (PN_TYPE_FLOW, NULL);
}
