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
#include "pn-jump.h"
#include "pn-jump-in.h"
#include "pn-jump-out.h"
#include "pn-debug.h"
#include "pn-desktop-geometry.h"

#include <json-glib/json-glib.h>

/* Headless split (TODO #23): the serializer is GTK-free.  Every colour
 * property in the tree -- the base node "color" and all the dual-nature
 * visual nodes' own colours -- is PN_TYPE_COLOR since the Phase 1/4/5
 * migration, and (de)serialises via pn_color_*().  The transitional
 * "resolve GdkRGBA by name and read it as a PnColor" clause that bridged
 * the not-yet-migrated nodes is gone now that none remain. */

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

    /* Document-wide user globals: name (gchar*, owned) -> value
     * (GValue*, owned).  Shared by every sheet and saved into the file.
     * Holds only the four basic scalar types (boolean/int64/double/
     * string).  Guarded by @globals_mutex because substitution reads
     * them from the auto-trigger worker thread while the document
     * settings dialog edits them from the main thread. */
    GHashTable  *globals;
    GMutex       globals_mutex;

    /* Panel-applet layout: node UUID (gchar*, owned) -> placement
     * (PnLayoutPos*, owned) on the #PnLayoutEditor canvas.  Main-thread
     * only (the layout editor and the save/load path), so unlike
     * @globals it needs no lock. */
    GHashTable  *panel_layout;

    /* Whether the panel-applet GUI layout editor tab is open.  A
     * view-state flag (not a sheet and not user data), saved into the
     * document so reopening a file restores the editor the way the user
     * left it.  See [[pn-layout-editor]]. */
    gboolean     panel_editor_open;

    /* Desktop-application layout: the same shape as @panel_layout, but
     * the coordinates are relative to the desktop window's content area
     * rather than to the editor canvas, and the map is entirely separate
     * so a node can sit in a different spot on each surface.  Main-thread
     * only, like @panel_layout. */
    GHashTable  *desktop_layout;

    /* Size and title of the window the desktop layout is arranged for —
     * document data the (still to be written) desktop application reads
     * to build its window.  Sizes are clamped to the PN_DE_WINDOW_*
     * range; @desktop_title is never NULL (empty means "no title set",
     * and the application falls back to the document's own name). */
    gint         desktop_width;
    gint         desktop_height;
    gchar       *desktop_title;

    /* Whether the desktop layout editor tab is open — the desktop twin of
     * @panel_editor_open, saved and restored the same way. */
    gboolean     desktop_editor_open;

    /* Undo/redo for this document.  Created with the flow and owned by
     * it; the funnel below feeds it edits and the clear/load paths reset
     * it (skipping the work while it is replaying a snapshot). */
    PnHistory   *history;
};

/* A stored layout-editor placement; see @panel_layout / @desktop_layout
 * above.  One type serves both maps — they differ in what the
 * coordinates are relative to, not in their shape. */
typedef struct { gdouble x, y; } PnLayoutPos;

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
    SIG_PANEL_EDITOR_VISIBLE,
    SIG_PANEL_LAYOUT_CHANGED,
    SIG_DESKTOP_EDITOR_VISIBLE,
    SIG_DESKTOP_LAYOUT_CHANGED,
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

PnHistory *
pn_flow_get_history (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), NULL);
    return self->history;
}

void
pn_flow_history_freeze (PnFlow *self)
{
    g_return_if_fail (PN_IS_FLOW (self));
    if (self->history != NULL)
        pn_history_freeze (self->history);
}

void
pn_flow_history_thaw (PnFlow *self)
{
    g_return_if_fail (PN_IS_FLOW (self));
    if (self->history != NULL)
        pn_history_thaw (self->history);
}

static void
on_store_changed_mark_modified (gpointer user_data)
{
    PnFlow *self = PN_FLOW (user_data);

    pn_flow_set_modified (self, TRUE);

    /* Feed the undo history.  Skip while a file is being applied (the
     * load cascade is not a user edit); pn_history_record itself also
     * ignores the call while frozen or replaying a snapshot. */
    if (!self->loading && self->history != NULL)
        pn_history_record (self->history);
}

/* ------------------------------------------------------------------ */
/*  Document globals                                                   */
/* ------------------------------------------------------------------ */

/** Hash-table value destructor for the globals table. */
static void
free_gvalue (gpointer data)
{
    GValue *v = data;
    g_value_unset (v);
    g_free (v);
}

/** TRUE when @type is one of the four scalar types a global may hold. */
static gboolean
global_type_supported (GType type)
{
    return type == G_TYPE_BOOLEAN || type == G_TYPE_INT64
        || type == G_TYPE_DOUBLE  || type == G_TYPE_STRING;
}

/** Value equality across the supported scalar types, used so a set that
 *  does not change anything does not dirty the document. */
static gboolean
gvalue_equal_basic (const GValue *a, const GValue *b)
{
    GType type = G_VALUE_TYPE (a);

    if (G_VALUE_TYPE (b) != type)
        return FALSE;

    switch (type)
    {
    case G_TYPE_BOOLEAN:
        return g_value_get_boolean (a) == g_value_get_boolean (b);
    case G_TYPE_INT64:
        return g_value_get_int64 (a) == g_value_get_int64 (b);
    case G_TYPE_DOUBLE:
        return g_value_get_double (a) == g_value_get_double (b);
    case G_TYPE_STRING:
        return g_strcmp0 (g_value_get_string (a),
                          g_value_get_string (b)) == 0;
    default:
        return FALSE;
    }
}

/** Stable on-disk nick for a global's value type. */
static const gchar *
global_type_nick (GType type)
{
    switch (type)
    {
    case G_TYPE_BOOLEAN: return "boolean";
    case G_TYPE_INT64:   return "integer";
    case G_TYPE_DOUBLE:  return "double";
    case G_TYPE_STRING:  return "string";
    default:             return NULL;
    }
}

/** Inverse of global_type_nick; %G_TYPE_INVALID for an unknown nick. */
static GType
gtype_from_global_nick (const gchar *nick)
{
    if (g_strcmp0 (nick, "boolean") == 0) return G_TYPE_BOOLEAN;
    if (g_strcmp0 (nick, "integer") == 0) return G_TYPE_INT64;
    if (g_strcmp0 (nick, "double")  == 0) return G_TYPE_DOUBLE;
    if (g_strcmp0 (nick, "string")  == 0) return G_TYPE_STRING;
    return G_TYPE_INVALID;
}

GList *
pn_flow_list_globals (PnFlow *self)
{
    GList *names;
    GList *l;

    g_return_val_if_fail (PN_IS_FLOW (self), NULL);

    g_mutex_lock (&self->globals_mutex);
    names = g_hash_table_get_keys (self->globals);
    names = g_list_sort (names, (GCompareFunc) g_strcmp0);
    /* Copy the keys while we still hold the lock; the table owns them. */
    for (l = names; l != NULL; l = l->next)
        l->data = g_strdup (l->data);
    g_mutex_unlock (&self->globals_mutex);

    return names;
}

gboolean
pn_flow_get_global (PnFlow *self, const gchar *name, GValue *out_value)
{
    GValue   *stored;
    gboolean  found = FALSE;

    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    g_return_val_if_fail (name != NULL, FALSE);
    g_return_val_if_fail (out_value != NULL, FALSE);

    g_mutex_lock (&self->globals_mutex);
    stored = g_hash_table_lookup (self->globals, name);
    if (stored != NULL)
    {
        g_value_init (out_value, G_VALUE_TYPE (stored));
        g_value_copy (stored, out_value);
        found = TRUE;
    }
    g_mutex_unlock (&self->globals_mutex);

    return found;
}

void
pn_flow_set_global (PnFlow *self, const gchar *name, const GValue *value)
{
    GValue   *stored;
    GValue   *copy;
    gboolean  changed = FALSE;

    g_return_if_fail (PN_IS_FLOW (self));
    g_return_if_fail (name != NULL && *name != '\0');
    g_return_if_fail (value != NULL);
    g_return_if_fail (global_type_supported (G_VALUE_TYPE (value)));

    g_mutex_lock (&self->globals_mutex);
    stored = g_hash_table_lookup (self->globals, name);
    if (stored == NULL || !gvalue_equal_basic (stored, value))
    {
        copy = g_new0 (GValue, 1);
        g_value_init (copy, G_VALUE_TYPE (value));
        g_value_copy (value, copy);
        g_hash_table_insert (self->globals, g_strdup (name), copy);
        changed = TRUE;
    }
    g_mutex_unlock (&self->globals_mutex);

    /* Emit outside the lock: pn_flow_set_modified runs handlers (the
     * window's Save UI) that have no business contending for it. */
    if (changed)
        pn_flow_set_modified (self, TRUE);
}

void
pn_flow_remove_global (PnFlow *self, const gchar *name)
{
    gboolean changed;

    g_return_if_fail (PN_IS_FLOW (self));
    g_return_if_fail (name != NULL);

    g_mutex_lock (&self->globals_mutex);
    changed = g_hash_table_remove (self->globals, name);
    g_mutex_unlock (&self->globals_mutex);

    if (changed)
        pn_flow_set_modified (self, TRUE);
}

/* ------------------------------------------------------------------ */
/*  GUI layout maps (panel applet, desktop window)                     */
/*                                                                     */
/*  Both surfaces store the same thing — a node UUID -> (x, y) map —    */
/*  and differ only in what the coordinates mean and which signal a     */
/*  change raises, so the three primitives below are shared and the      */
/*  public per-surface entry points are thin wrappers over them.         */
/* ------------------------------------------------------------------ */

static gboolean
layout_get_position (GHashTable  *layout,
                     const gchar *uuid,
                     gdouble     *out_x,
                     gdouble     *out_y)
{
    PnLayoutPos *pos = g_hash_table_lookup (layout, uuid);

    if (pos == NULL)
        return FALSE;

    if (out_x != NULL)
        *out_x = pos->x;
    if (out_y != NULL)
        *out_y = pos->y;
    return TRUE;
}

/* Store @uuid at (@x, @y).  Returns whether that actually changed
 * anything, so the caller only signals and dirties the document on a real
 * move (re-clicking a widget without moving it leaves the file clean). */
static gboolean
layout_set_position (GHashTable  *layout,
                     const gchar *uuid,
                     gdouble      x,
                     gdouble      y)
{
    PnLayoutPos *pos = g_hash_table_lookup (layout, uuid);

    if (pos != NULL && pos->x == x && pos->y == y)
        return FALSE;

    if (pos == NULL)
    {
        pos = g_new0 (PnLayoutPos, 1);
        g_hash_table_insert (layout, g_strdup (uuid), pos);
    }
    pos->x = x;
    pos->y = y;
    return TRUE;
}

/* Every placed UUID, as freshly-allocated strings (transfer full).  Order
 * is unspecified, so callers that need a particular one sort themselves. */
static GList *
layout_list_positions (GHashTable *layout)
{
    GList         *out = NULL;
    GHashTableIter iter;
    gpointer       key;

    g_hash_table_iter_init (&iter, layout);
    while (g_hash_table_iter_next (&iter, &key, NULL))
        out = g_list_prepend (out, g_strdup ((const gchar *) key));

    return out;
}

/* Read the UUID -> { x, y } object stored under @member of @obj into
 * @layout (which the caller has already emptied).  Malformed entries are
 * skipped so a hand-edited file degrades gracefully rather than failing
 * the whole load. */
static void
layout_load_json (GHashTable  *layout,
                  JsonObject  *obj,
                  const gchar *member)
{
    JsonObject *lay;
    GList      *members;
    GList      *l;

    if (!json_object_has_member (obj, member) ||
        !JSON_NODE_HOLDS_OBJECT (json_object_get_member (obj, member)))
        return;

    lay     = json_object_get_object_member (obj, member);
    members = json_object_get_members (lay);

    for (l = members; l != NULL; l = l->next)
    {
        const gchar *uid   = l->data;
        JsonNode    *enode = json_object_get_member (lay, uid);
        JsonObject  *entry;
        PnLayoutPos *pos;

        if (uid == NULL || *uid == '\0' || !JSON_NODE_HOLDS_OBJECT (enode))
            continue;

        entry = json_node_get_object (enode);
        if (!json_object_has_member (entry, "x")
            || !json_object_has_member (entry, "y"))
            continue;

        pos    = g_new0 (PnLayoutPos, 1);
        pos->x = json_object_get_double_member (entry, "x");
        pos->y = json_object_get_double_member (entry, "y");
        g_hash_table_insert (layout, g_strdup (uid), pos);
    }
    g_list_free (members);
}

/* Write @layout into @obj under @member as a UUID-keyed object of
 * { x, y } placements.  Entries for nodes no longer in the flow are
 * pruned so deleting a node cleans up its placement, and the keys are
 * sorted for stable diffs.  The member is omitted entirely when nothing
 * placed survives the pruning. */
static void
layout_save_json (PnFlow      *self,
                  GHashTable  *layout,
                  JsonObject  *obj,
                  const gchar *member)
{
    GHashTable *live;
    JsonObject *lay;
    GList      *keys;
    GList      *l;
    guint       k;

    if (g_hash_table_size (layout) == 0)
        return;

    live = g_hash_table_new (g_str_hash, g_str_equal);
    lay  = json_object_new ();
    keys = g_hash_table_get_keys (layout);

    for (k = 0; k < pn_node_store_get_length (self->nodes); k++)
    {
        const gchar *uid = pn_node_get_uuid (
                pn_node_store_get_node (self->nodes, k));
        if (uid != NULL && *uid != '\0')
            g_hash_table_add (live, (gpointer) uid);
    }

    keys = g_list_sort (keys, (GCompareFunc) g_strcmp0);
    for (l = keys; l != NULL; l = l->next)
    {
        const gchar       *uid = l->data;
        const PnLayoutPos *pos;
        JsonObject        *entry;

        if (!g_hash_table_contains (live, uid))
            continue;

        pos   = g_hash_table_lookup (layout, uid);
        entry = json_object_new ();
        json_object_set_double_member (entry, "x", pos->x);
        json_object_set_double_member (entry, "y", pos->y);
        json_object_set_object_member (lay, uid, entry);
    }
    g_list_free (keys);
    g_hash_table_destroy (live);

    if (json_object_get_size (lay) > 0)
        json_object_set_object_member (obj, member, lay);
    else
        json_object_unref (lay);
}

/* An editor-tab open/closed view-state flag, defaulting to closed when
 * the member is absent or not a plain value. */
static gboolean
load_open_flag (JsonObject *obj, const gchar *member)
{
    JsonNode *bn;

    if (!json_object_has_member (obj, member))
        return FALSE;

    bn = json_object_get_member (obj, member);
    return JSON_NODE_HOLDS_VALUE (bn) ? json_node_get_boolean (bn) : FALSE;
}

/* ------------------------------------------------------------------ */
/*  Panel-applet layout                                                */
/* ------------------------------------------------------------------ */

gboolean
pn_flow_get_panel_position (PnFlow      *self,
                            const gchar *uuid,
                            gdouble     *out_x,
                            gdouble     *out_y)
{
    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    g_return_val_if_fail (uuid != NULL, FALSE);

    return layout_get_position (self->panel_layout, uuid, out_x, out_y);
}

void
pn_flow_set_panel_position (PnFlow      *self,
                            const gchar *uuid,
                            gdouble      x,
                            gdouble      y)
{
    g_return_if_fail (PN_IS_FLOW (self));
    g_return_if_fail (uuid != NULL && *uuid != '\0');

    if (!layout_set_position (self->panel_layout, uuid, x, y))
        return;                 /* unchanged — keep the document clean */

    g_signal_emit (self, signals[SIG_PANEL_LAYOUT_CHANGED], 0);
    pn_flow_set_modified (self, TRUE);
}

/* Every UUID with a stored panel placement, as freshly-allocated strings
 * (transfer full: free with g_list_free_full(list, g_free)).  The headless
 * engine uses this to learn which nodes the layout editor placed without
 * reaching into the private @panel_layout table; order is unspecified, so
 * callers that need left-to-right band order sort by x themselves. */
GList *
pn_flow_list_panel_positions (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), NULL);

    return layout_list_positions (self->panel_layout);
}

gboolean
pn_flow_get_panel_editor_open (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    return self->panel_editor_open;
}

void
pn_flow_set_panel_editor_open (PnFlow *self, gboolean open)
{
    g_return_if_fail (PN_IS_FLOW (self));

    open = !!open;
    if (self->panel_editor_open == open)
        return;

    self->panel_editor_open = open;

    /* The placements outlive the editor tab: they are document data that
     * drives the real panel applet (the engine mirrors every band-snapped
     * Countdown/LED widget), so closing the editor no longer discards the
     * layout.  pn_flow_clear still empties it when the whole document goes. */

    g_signal_emit (self, signals[SIG_PANEL_EDITOR_VISIBLE], 0, open);
    pn_flow_set_modified (self, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Desktop-application layout                                         */
/* ------------------------------------------------------------------ */

gboolean
pn_flow_get_desktop_position (PnFlow      *self,
                              const gchar *uuid,
                              gdouble     *out_x,
                              gdouble     *out_y)
{
    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    g_return_val_if_fail (uuid != NULL, FALSE);

    return layout_get_position (self->desktop_layout, uuid, out_x, out_y);
}

void
pn_flow_set_desktop_position (PnFlow      *self,
                              const gchar *uuid,
                              gdouble      x,
                              gdouble      y)
{
    g_return_if_fail (PN_IS_FLOW (self));
    g_return_if_fail (uuid != NULL && *uuid != '\0');

    if (!layout_set_position (self->desktop_layout, uuid, x, y))
        return;                 /* unchanged — keep the document clean */

    g_signal_emit (self, signals[SIG_DESKTOP_LAYOUT_CHANGED], 0);
    pn_flow_set_modified (self, TRUE);
}

GList *
pn_flow_list_desktop_positions (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), NULL);

    return layout_list_positions (self->desktop_layout);
}

gboolean
pn_flow_get_desktop_editor_open (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    return self->desktop_editor_open;
}

void
pn_flow_set_desktop_editor_open (PnFlow *self, gboolean open)
{
    g_return_if_fail (PN_IS_FLOW (self));

    open = !!open;
    if (self->desktop_editor_open == open)
        return;

    self->desktop_editor_open = open;

    /* As on the panel: the placements are document data for the desktop
     * application and outlive the editor tab; only pn_flow_clear drops
     * them. */

    g_signal_emit (self, signals[SIG_DESKTOP_EDITOR_VISIBLE], 0, open);
    pn_flow_set_modified (self, TRUE);
}

void
pn_flow_get_desktop_window (PnFlow *self,
                            gint   *out_width,
                            gint   *out_height)
{
    g_return_if_fail (PN_IS_FLOW (self));

    if (out_width != NULL)
        *out_width = self->desktop_width;
    if (out_height != NULL)
        *out_height = self->desktop_height;
}

void
pn_flow_set_desktop_window (PnFlow *self,
                            gint    width,
                            gint    height)
{
    g_return_if_fail (PN_IS_FLOW (self));

    width  = CLAMP (width,  PN_DE_WINDOW_MIN_WIDTH,  PN_DE_WINDOW_MAX_WIDTH);
    height = CLAMP (height, PN_DE_WINDOW_MIN_HEIGHT, PN_DE_WINDOW_MAX_HEIGHT);

    if (self->desktop_width == width && self->desktop_height == height)
        return;

    self->desktop_width  = width;
    self->desktop_height = height;

    g_signal_emit (self, signals[SIG_DESKTOP_LAYOUT_CHANGED], 0);
    pn_flow_set_modified (self, TRUE);
}

const gchar *
pn_flow_get_desktop_title (PnFlow *self)
{
    g_return_val_if_fail (PN_IS_FLOW (self), "");

    return self->desktop_title != NULL ? self->desktop_title : "";
}

void
pn_flow_set_desktop_title (PnFlow *self, const gchar *title)
{
    g_return_if_fail (PN_IS_FLOW (self));

    if (title == NULL)
        title = "";
    if (g_strcmp0 (self->desktop_title, title) == 0)
        return;

    g_free (self->desktop_title);
    self->desktop_title = g_strdup (title);

    g_signal_emit (self, signals[SIG_DESKTOP_LAYOUT_CHANGED], 0);
    pn_flow_set_modified (self, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Globals as a ${...} substitution source                           */
/* ------------------------------------------------------------------ */

static JsonNode *
globals_resolve (PnSubstResolver *self, const gchar *key, gboolean *owned)
{
    PnFlow   *flow = self->user_data;
    GValue    v    = G_VALUE_INIT;
    JsonNode *node;

    *owned = FALSE;
    if (flow == NULL || !pn_flow_get_global (flow, key, &v))
        return NULL;

    node = json_node_new (JSON_NODE_VALUE);
    switch (G_VALUE_TYPE (&v))
    {
    case G_TYPE_BOOLEAN:
        json_node_set_boolean (node, g_value_get_boolean (&v));
        break;
    case G_TYPE_INT64:
        json_node_set_int (node, g_value_get_int64 (&v));
        break;
    case G_TYPE_DOUBLE:
        json_node_set_double (node, g_value_get_double (&v));
        break;
    case G_TYPE_STRING:
        json_node_set_string (node, g_value_get_string (&v));
        break;
    default:
        json_node_free (node);
        g_value_unset (&v);
        return NULL;
    }

    g_value_unset (&v);
    *owned = TRUE;
    return node;
}

void
pn_flow_subst_resolver_globals (PnSubstResolver *r, PnFlow *flow)
{
    r->resolve   = globals_resolve;
    r->user_data = flow;
}

static void
on_node_store_node_added_mark (
        PnNodeStore *store,
        PnNode      *node,
        guint        index,
        gpointer     user_data)
{
    (void) store; (void) node; (void) index;
    pn_jump_refresh_errors (PN_FLOW (user_data));
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
    /* Deleting one half of a pair strands the other: re-check after the
     * node is out of the store, so the survivor lights up. */
    pn_jump_refresh_errors (PN_FLOW (user_data));
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
    /* Retagging a jump flag re-forms connections document-wide: the old
     * partner may be stranded and a new one adopted.  Gate on the flag
     * types so an unrelated node's "tag" property cannot trigger the
     * sweep, and on the property name so the has-error notification this
     * very sweep emits does not recurse back into it. */
    if (g_strcmp0 (pspec->name, "tag") == 0 &&
        (PN_IS_JUMP_IN (object) || PN_IS_JUMP_OUT (object)))
        pn_jump_refresh_errors (PN_FLOW (user_data));

    /* Ignore purely cosmetic / live-status notifications so a node
     * settling into its runtime appearance after load does not flag the
     * document dirty. */
    if (!property_affects_document (pspec))
        return;

    on_store_changed_mark_modified (user_data);
}

/* "input-names-changed" handler: a per-input rename is a document edit. */
static void
on_node_input_names_changed (
        PnNode  *node,
        gpointer user_data)
{
    (void) node;
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

    /* Give the node a back-pointer to us so substitution can reach the
     * document globals. */
    pn_node_set_flow (node, self);

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

    /* Per-input display names round-trip to disk but live outside the
     * GObject property system, so they raise no "notify"; a rename
     * marks the document modified through this dedicated signal. */
    g_signal_connect (node, "input-names-changed",
                      G_CALLBACK (on_node_input_names_changed), self);
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

    pn_node_set_flow (node, NULL);

    g_signal_handlers_disconnect_by_func (
            node, G_CALLBACK (on_node_any_notify_mark), self);
    g_signal_handlers_disconnect_by_func (
            node, G_CALLBACK (on_node_input_names_changed), self);
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

    if (type == PN_TYPE_COLOR)
    {
        const PnColor *c = g_value_get_boxed (value);
        out = json_node_new (JSON_NODE_VALUE);
        if (c != NULL)
        {
            gchar *s = pn_color_to_string (c);
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

    if (type == PN_TYPE_COLOR)
    {
        PnColor c;

        if (!JSON_NODE_HOLDS_VALUE (json) ||
            json_node_get_value_type (json) != G_TYPE_STRING)
            return FALSE;

        if (!pn_color_parse (&c, json_node_get_string (json)))
            return FALSE;

        g_value_set_boxed (out_value, &c);
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

        /* Secrets never touch the workflow file (plugin ABI v5): credentials
         * live in the host vault, reached through a profile reference.  This
         * is a SAVE-only skip — the load path below still reads a legacy
         * inline secret so a pre-v5 file's plaintext can be imported into a
         * profile (and then dropped from the file on the next save). */
        if (pn_param_spec_get_secret (pspec))
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

    /* Per-input display names for multi-input nodes.  These live in a
     * bespoke PnNode store (not a GObject property), so the generic
     * property loop above never sees them — they need their own field.
     * Only names the user actually customised are written: a name still
     * equal to its "valueN" default round-trips by being regenerated on
     * load, so single-input and untouched multi-input nodes leave the
     * on-disk format unchanged.  Keyed by 0-based input index so a
     * sparse set of renames stays compact. */
    {
        gint        n     = pn_node_get_n_inputs (node);
        JsonObject *names = NULL;
        gint        i;

        for (i = 0; i < n; i++)
        {
            const gchar *nm    = pn_node_get_input_name (node, i);
            gchar       *deflt = g_strdup_printf ("value%d", i + 1);

            if (nm != NULL && g_strcmp0 (nm, deflt) != 0)
            {
                gchar *idx = g_strdup_printf ("%d", i);

                if (names == NULL)
                    names = json_object_new ();
                json_object_set_string_member (names, idx, nm);
                g_free (idx);
            }
            g_free (deflt);
        }

        if (names != NULL)
            json_object_set_object_member (obj, "input-names", names);
    }

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

    /* Restore per-input display names (see node_to_json).  Applied after
     * the properties bag so the input count — carried by a subclass
     * property such as Calculator's "inputs" — is already in place.
     * Missing entries keep their lazily-generated "valueN" default. */
    if (json_object_has_member (obj, "input-names"))
    {
        JsonObject *names = json_object_get_object_member (obj, "input-names");

        if (names != NULL)
        {
            GList *members = json_object_get_members (names);
            GList *iter;

            for (iter = members; iter != NULL; iter = iter->next)
            {
                const gchar *idxs = iter->data;
                JsonNode    *vn   = json_object_get_member (names, idxs);
                gint64       idx  = g_ascii_strtoll (idxs, NULL, 10);

                if (idx >= 0 && JSON_NODE_HOLDS_VALUE (vn)
                    && json_node_get_value_type (vn) == G_TYPE_STRING)
                    pn_node_set_input_name (node, (gint) idx,
                                            json_node_get_string (vn));
            }
            g_list_free (members);
        }
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
    g_mutex_lock (&self->globals_mutex);
    g_hash_table_remove_all (self->globals);
    g_mutex_unlock (&self->globals_mutex);

    g_hash_table_remove_all (self->panel_layout);
    g_hash_table_remove_all (self->desktop_layout);

    /* The desktop window reverts to a default, untitled one. */
    self->desktop_width  = PN_DE_WINDOW_DEFAULT_WIDTH;
    self->desktop_height = PN_DE_WINDOW_DEFAULT_HEIGHT;
    g_free (self->desktop_title);
    self->desktop_title = g_strdup ("");

    /* Close both GUI layout editors so a cleared (or about-to-be-loaded)
     * document starts without a stale editor tab. */
    if (self->panel_editor_open)
    {
        self->panel_editor_open = FALSE;
        g_signal_emit (self, signals[SIG_PANEL_EDITOR_VISIBLE], 0, FALSE);
    }
    if (self->desktop_editor_open)
    {
        self->desktop_editor_open = FALSE;
        g_signal_emit (self, signals[SIG_DESKTOP_EDITOR_VISIBLE], 0, FALSE);
    }

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

    /* Reset the undo history to this empty slate (File→New).  A load
     * runs pn_flow_clear as its first step then repopulates; that path
     * re-baselines again at the end, so resetting here is harmless for
     * it.  A snapshot replay must keep its history, hence the guard. */
    if (self->history != NULL && !pn_history_is_restoring (self->history))
        pn_history_clear (self->history);
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

/* Shared back end for pn_flow_load_from_file and pn_flow_load_from_data:
 * rebuilds the entire model (nodes, wires, sheets, globals, panel layout)
 * from the already-parsed document root @obj, committing atomically — a
 * structural failure along the way leaves @self untouched.  The caller
 * owns the #JsonParser / #JsonNode that produced @obj. */
static gboolean
flow_load_from_object (
        PnFlow     *self,
        JsonObject *obj,
        GError    **error)
{
    JsonArray  *nodes_arr;
    JsonArray  *conns_arr;
    GPtrArray  *new_nodes;
    GPtrArray  *new_wires;
    GHashTable *by_name;
    GHashTable *by_uuid;
    guint       n;
    guint       i;

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

    /* Both GUI layouts must be in place BEFORE the node-add cascade
     * below: a layout editor reads each node's saved position as its
     * node-added handler fires.  pn_flow_clear above emptied the tables.
     * Malformed entries are skipped so a hand-edited file degrades
     * gracefully. */
    layout_load_json (self->panel_layout,   obj, "panel_layout");
    layout_load_json (self->desktop_layout, obj, "desktop_layout");

    /* The desktop window the layout above is arranged for.  Absent (or
     * malformed) members leave the defaults pn_flow_clear just restored. */
    if (json_object_has_member (obj, "desktop_window") &&
        JSON_NODE_HOLDS_OBJECT (json_object_get_member (obj,
                                                        "desktop_window")))
    {
        JsonObject *win = json_object_get_object_member (obj,
                                                         "desktop_window");

        if (json_object_has_member (win, "width"))
            self->desktop_width =
                    CLAMP ((gint) json_object_get_int_member (win, "width"),
                           PN_DE_WINDOW_MIN_WIDTH, PN_DE_WINDOW_MAX_WIDTH);
        if (json_object_has_member (win, "height"))
            self->desktop_height =
                    CLAMP ((gint) json_object_get_int_member (win, "height"),
                           PN_DE_WINDOW_MIN_HEIGHT, PN_DE_WINDOW_MAX_HEIGHT);
        if (json_object_has_member (win, "title"))
        {
            const gchar *t = json_object_get_string_member (win, "title");
            g_free (self->desktop_title);
            self->desktop_title = g_strdup (t != NULL ? t : "");
        }
    }

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

    /* Document globals.  pn_flow_clear above emptied the table; repopulate
     * from the file's name-keyed "globals" object, trusting each entry's
     * explicit type nick.  Malformed or unknown-typed entries are skipped
     * so a hand-edited file degrades gracefully. */
    if (json_object_has_member (obj, "globals") &&
        JSON_NODE_HOLDS_OBJECT (json_object_get_member (obj, "globals")))
    {
        JsonObject *globals_obj = json_object_get_object_member (obj,
                                                                 "globals");
        GList      *members = json_object_get_members (globals_obj);
        GList      *l;

        g_mutex_lock (&self->globals_mutex);
        for (l = members; l != NULL; l = l->next)
        {
            const gchar *name  = l->data;
            JsonNode    *enode = json_object_get_member (globals_obj, name);
            JsonObject  *entry;
            GType        type;
            JsonNode    *vnode;
            GValue      *val;

            if (name == NULL || *name == '\0'
                || !JSON_NODE_HOLDS_OBJECT (enode))
                continue;

            entry = json_node_get_object (enode);
            if (!json_object_has_member (entry, "type")
                || !json_object_has_member (entry, "value"))
                continue;

            type = gtype_from_global_nick (
                    json_object_get_string_member (entry, "type"));
            vnode = json_object_get_member (entry, "value");
            if (type == G_TYPE_INVALID || !JSON_NODE_HOLDS_VALUE (vnode))
                continue;

            val = g_new0 (GValue, 1);
            g_value_init (val, type);
            switch (type)
            {
            case G_TYPE_BOOLEAN:
                g_value_set_boolean (val, json_node_get_boolean (vnode));
                break;
            case G_TYPE_INT64:
                g_value_set_int64 (val, json_node_get_int (vnode));
                break;
            case G_TYPE_DOUBLE:
                g_value_set_double (val, json_node_get_double (vnode));
                break;
            case G_TYPE_STRING:
                g_value_set_string (val, json_node_get_string (vnode));
                break;
            default:
                break;
            }
            g_hash_table_insert (self->globals, g_strdup (name), val);
        }
        g_mutex_unlock (&self->globals_mutex);
        g_list_free (members);
    }

    /* Layout-editor open/closed view-state, one flag per surface
     * (default closed). */
    self->panel_editor_open   = load_open_flag (obj, "panel_editor_open");
    self->desktop_editor_open = load_open_flag (obj, "desktop_editor_open");

    self->loading = FALSE;
    if (self->modified)
    {
        self->modified = FALSE;
        g_signal_emit (self, signals[SIG_MODIFIED_CHANGED], 0, FALSE);
    }

    /* Now that the model is fully settled, tell the host to match the
     * editor tabs to the loaded state.  Emitted unconditionally so a load
     * into a window that still shows a stale editor also closes it. */
    g_signal_emit (self, signals[SIG_PANEL_EDITOR_VISIBLE], 0,
                   self->panel_editor_open);
    g_signal_emit (self, signals[SIG_DESKTOP_EDITOR_VISIBLE], 0,
                   self->desktop_editor_open);

    g_ptr_array_unref  (new_wires);
    g_ptr_array_unref  (new_nodes);
    g_hash_table_destroy (by_name);
    g_hash_table_destroy (by_uuid);

    /* A genuine load (File→Open, or a whole-document replace over D-Bus)
     * starts a clean undo history baselined on the just-loaded content.
     * A snapshot replay (undo/redo) reuses this same path but manages
     * its own baseline, so leave the history alone while restoring. */
    if (self->history != NULL && !pn_history_is_restoring (self->history))
        pn_history_clear (self->history);

    return TRUE;

fail:
    g_ptr_array_unref  (new_wires);
    g_ptr_array_unref  (new_nodes);
    g_hash_table_destroy (by_name);
    g_hash_table_destroy (by_uuid);
    return FALSE;
}

/* Parse @doc — a file path when @is_path, otherwise an in-memory JSON
 * string — into a document root object and hand it to
 * flow_load_from_object().  The two public entry points below differ
 * only in how the bytes reach the parser. */
static gboolean
flow_load_from_parsed (
        PnFlow      *self,
        const gchar *doc,
        gboolean     is_path,
        GError     **error)
{
    JsonParser *parser;
    JsonNode   *root;
    gboolean    ok;

    /* Touch the factory once up front so every built-in (and any
     * already-loaded plugin) class is registered before node_from_json
     * starts looking type names up.  The factory is a singleton — this
     * is a no-op after the first call. */
    (void) pn_node_factory_get_default ();

    parser = json_parser_new ();
    ok     = is_path
        ? json_parser_load_from_file (parser, doc, error)
        : json_parser_load_from_data (parser, doc, -1, error);
    if (!ok)
    {
        g_object_unref (parser);
        return FALSE;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_set_error_literal (error, PN_FLOW_ERROR, 0,
                             "worksheet document root is not a JSON object");
        g_object_unref (parser);
        return FALSE;
    }

    ok = flow_load_from_object (self, json_node_get_object (root), error);
    g_object_unref (parser);
    return ok;
}

gboolean
pn_flow_load_from_file (
        PnFlow      *self,
        const gchar *path,
        GError     **error)
{
    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    g_return_val_if_fail (path != NULL, FALSE);

    return flow_load_from_parsed (self, path, TRUE, error);
}

gboolean
pn_flow_load_from_data (
        PnFlow      *self,
        const gchar *json,
        GError     **error)
{
    g_return_val_if_fail (PN_IS_FLOW (self), FALSE);
    g_return_val_if_fail (json != NULL, FALSE);

    return flow_load_from_parsed (self, json, FALSE, error);
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

        /* Document globals: a name-keyed object, each entry carrying an
         * explicit type nick alongside its value so loading never has to
         * guess (e.g. integer vs double).  Names sorted for stable
         * diffs; omitted entirely when there are none. */
        g_mutex_lock (&self->globals_mutex);
        if (g_hash_table_size (self->globals) > 0)
        {
            JsonObject *globals_obj = json_object_new ();
            GList      *names = g_hash_table_get_keys (self->globals);
            GList      *l;

            names = g_list_sort (names, (GCompareFunc) g_strcmp0);
            for (l = names; l != NULL; l = l->next)
            {
                const gchar *name  = l->data;
                const GValue *val  = g_hash_table_lookup (self->globals,
                                                          name);
                const gchar *nick  = global_type_nick (G_VALUE_TYPE (val));
                JsonObject  *entry;
                JsonNode    *vnode;

                if (nick == NULL)
                    continue;
                vnode = gvalue_to_json (val);
                if (vnode == NULL)
                    continue;

                entry = json_object_new ();
                json_object_set_string_member (entry, "type", nick);
                json_object_set_member         (entry, "value", vnode);
                json_object_set_object_member  (globals_obj, name, entry);
            }
            g_list_free (names);

            json_object_set_object_member (obj, "globals", globals_obj);
        }
        g_mutex_unlock (&self->globals_mutex);

        /* Each GUI layout editor's tab open/closed is a view-state flag,
         * saved only while open so reopening the file restores the tab. */
        if (self->panel_editor_open)
            json_object_set_boolean_member (obj, "panel_editor_open", TRUE);
        if (self->desktop_editor_open)
            json_object_set_boolean_member (obj, "desktop_editor_open", TRUE);

        /* The layouts persist regardless of whether their editor tab is
         * open: they are real document data (the panel one drives the
         * panel applet, each band-snapped Countdown/LED mirrored to a real
         * panel widget; the desktop one is what the desktop application
         * builds its window from). */
        layout_save_json (self, self->panel_layout,   obj, "panel_layout");
        layout_save_json (self, self->desktop_layout, obj, "desktop_layout");

        /* The desktop window itself, saved only when it differs from a
         * fresh default one, so documents that never opened the desktop
         * layout stay byte-identical to before. */
        if (self->desktop_width  != PN_DE_WINDOW_DEFAULT_WIDTH  ||
            self->desktop_height != PN_DE_WINDOW_DEFAULT_HEIGHT ||
            (self->desktop_title != NULL && *self->desktop_title != '\0'))
        {
            JsonObject *win = json_object_new ();

            json_object_set_int_member (win, "width",  self->desktop_width);
            json_object_set_int_member (win, "height", self->desktop_height);
            json_object_set_string_member (win, "title",
                                           pn_flow_get_desktop_title (self));
            json_object_set_object_member (obj, "desktop_window", win);
        }
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
     * flip the dirty flag so the host's Save UI can grey out again, and
     * record this as the history's saved point so undoing back to it
     * reports the document clean. */
    if (ok)
    {
        pn_flow_set_modified (self, FALSE);
        if (self->history != NULL)
            pn_history_mark_saved (self->history);
    }

    return ok;
}

gchar *
pn_flow_to_string (PnFlow *self)
{
    JsonGenerator *gen;
    JsonNode      *root;
    gchar         *out;

    g_return_val_if_fail (PN_IS_FLOW (self), NULL);

    /* Same whole-document serialiser that backs pn_flow_save_to_file(),
     * but rendered to a string instead of a file — the on-disk format,
     * tagged %PN_FLOW_FILE_FORMAT, carrying every node, wire, sheet,
     * global and the panel layout.  Reading the flow is non-destructive,
     * so the modified flag is left untouched (unlike a real save). */
    root = flow_build_root (self, NULL, PN_FLOW_FILE_FORMAT);

    gen = json_generator_new ();
    json_generator_set_root   (gen, root);
    json_generator_set_pretty (gen, TRUE);
    json_generator_set_indent (gen, 2);

    out = json_generator_to_data (gen, NULL);

    g_object_unref (gen);
    json_node_free (root);

    return out;
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

    g_clear_object (&self->history);
    g_clear_object (&self->nodes);
    g_clear_object (&self->wires);

    g_clear_pointer (&self->sheets, g_ptr_array_unref);
    g_clear_pointer (&self->active_sheet, g_free);
    g_clear_pointer (&self->globals, g_hash_table_destroy);
    g_mutex_clear (&self->globals_mutex);
    g_clear_pointer (&self->panel_layout, g_hash_table_destroy);
    g_clear_pointer (&self->desktop_layout, g_hash_table_destroy);
    g_clear_pointer (&self->desktop_title, g_free);

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

    /* The panel-applet editor's open/closed state changed (carries the
     * new state): on load and on clear the host adds or removes the
     * editor tab to match the document. */
    signals[SIG_PANEL_EDITOR_VISIBLE] = g_signal_new (
            "panel-editor-visible-changed",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    /* A panel-applet widget placement changed (added, moved, or snapped in
     * or out of the band).  The headless engine mirrors band-snapped
     * widgets to the real panel, so it re-publishes the layout in response. */
    signals[SIG_PANEL_LAYOUT_CHANGED] = g_signal_new (
            "panel-layout-changed",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 0);

    /* The desktop layout editor's open/closed state changed (carries the
     * new state): the panel signal's twin, driving the desktop tab. */
    signals[SIG_DESKTOP_EDITOR_VISIBLE] = g_signal_new (
            "desktop-editor-visible-changed",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_BOOLEAN);

    /* Something about the desktop window changed — a widget placement, the
     * window size, or its title.  The desktop application (once it exists)
     * re-reads the layout in response. */
    signals[SIG_DESKTOP_LAYOUT_CHANGED] = g_signal_new (
            "desktop-layout-changed",
            PN_TYPE_FLOW,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 0);
}

static void
pn_flow_init (PnFlow *self)
{
    self->nodes = pn_node_store_new ();
    self->wires = pn_wire_store_new ();

    self->sheets = g_ptr_array_new_with_free_func (g_free);
    sheet_list_reset_default (self);

    self->globals = g_hash_table_new_full (g_str_hash, g_str_equal,
                                           g_free, free_gvalue);
    g_mutex_init (&self->globals_mutex);

    self->panel_layout = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);

    self->desktop_layout = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, g_free);
    self->desktop_width  = PN_DE_WINDOW_DEFAULT_WIDTH;
    self->desktop_height = PN_DE_WINDOW_DEFAULT_HEIGHT;
    self->desktop_title  = g_strdup ("");

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

    /* Undo/redo, seeded with this empty document as its baseline.  Built
     * last so the stores and sheet list it serialises already exist. */
    self->history = pn_history_new (self);
}

PnFlow *
pn_flow_new (void)
{
    return g_object_new (PN_TYPE_FLOW, NULL);
}
