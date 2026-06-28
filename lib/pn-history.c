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

#include "pn-history.h"
#include "pn-flow.h"

/* Cap on how many undo snapshots we retain.  A document is a few KB of
 * JSON, so even the full stack is small; the cap only bounds a runaway
 * editing session. */
#define PN_HISTORY_LIMIT 100

/* ------------------------------------------------------------------ */
/*  PnHistory                                                          */
/* ------------------------------------------------------------------ */

struct _PnHistory
{
    GObject parent_instance;

    /* The tracked document.  Borrowed: #PnFlow owns its history, so the
     * flow always outlives us and we take no reference (which would be a
     * cycle anyway). */
    PnFlow    *flow;

    /* Last committed document snapshot — the state an undo would step
     * back *from*, and the yardstick a commit diffs against. */
    gchar     *baseline;

    /* The on-disk snapshot, refreshed on save: when @baseline equals it
     * the document is unmodified. */
    gchar     *saved;

    /* Snapshot stacks, top == last element.  Neither array owns its
     * strings (no free func): undo/redo move ownership in and out by
     * hand, which the GLib 2.40 baseline's GPtrArray can do without the
     * 2.58 steal helpers. */
    GPtrArray *undo;
    GPtrArray *redo;

    guint      freeze_depth;   /* >0 while a drag / dialog brackets edits */
    guint      idle_id;        /* pending coalesced commit, 0 if none     */
    gboolean   restoring;      /* TRUE while loading a snapshot back       */
};

G_DEFINE_TYPE (PnHistory, pn_history, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------ */

static void
pn_history_finalize (GObject *object)
{
    PnHistory *self = PN_HISTORY (object);

    if (self->idle_id != 0)
    {
        g_source_remove (self->idle_id);
        self->idle_id = 0;
    }

    g_ptr_array_set_free_func (self->undo, g_free);
    g_ptr_array_set_free_func (self->redo, g_free);
    g_clear_pointer (&self->undo, g_ptr_array_unref);
    g_clear_pointer (&self->redo, g_ptr_array_unref);

    g_clear_pointer (&self->baseline, g_free);
    g_clear_pointer (&self->saved, g_free);

    G_OBJECT_CLASS (pn_history_parent_class)->finalize (object);
}

static void
pn_history_class_init (PnHistoryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = pn_history_finalize;

    /* Emitted whenever the undo/redo availability or the saved state may
     * have changed: hosts re-evaluate menu/toolbar sensitivity and the
     * title-bar dirty marker. */
    signals[SIG_CHANGED] = g_signal_new (
            "changed",
            PN_TYPE_HISTORY,
            G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL,
            G_TYPE_NONE, 0);
}

static void
pn_history_init (PnHistory *self)
{
    self->undo = g_ptr_array_new ();
    self->redo = g_ptr_array_new ();
}

PnHistory *
pn_history_new (PnFlow *flow)
{
    PnHistory *self;

    g_return_val_if_fail (flow != NULL, NULL);

    self = g_object_new (PN_TYPE_HISTORY, NULL);
    self->flow     = flow;
    self->baseline = pn_flow_to_string (flow);
    self->saved    = g_strdup (self->baseline);
    return self;
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Pop and hand back the top string of @stack, or %NULL if empty.
 * Ownership transfers to the caller (the array has no free func). */
static gchar *
stack_pop (GPtrArray *stack)
{
    gchar *top;

    if (stack->len == 0)
        return NULL;

    top = g_ptr_array_index (stack, stack->len - 1);
    g_ptr_array_set_size (stack, stack->len - 1);
    return top;
}

/* Free every retained string and empty @stack. */
static void
stack_clear (GPtrArray *stack)
{
    guint i;

    for (i = 0; i < stack->len; i++)
        g_free (g_ptr_array_index (stack, i));
    g_ptr_array_set_size (stack, 0);
}

/* Drop the redo stack — any forward history is invalidated by a fresh
 * edit. */
static void
redo_clear (PnHistory *self)
{
    stack_clear (self->redo);
}

/* Reconcile the document's modified flag with the current baseline: the
 * document is dirty exactly when its committed state differs from what
 * is on disk. */
static void
sync_modified (PnHistory *self)
{
    pn_flow_set_modified (self->flow,
                          g_strcmp0 (self->baseline, self->saved) != 0);
}

/* ------------------------------------------------------------------ */
/*  Recording                                                          */
/* ------------------------------------------------------------------ */

static gboolean
commit_idle (gpointer user_data)
{
    PnHistory *self = PN_HISTORY (user_data);

    self->idle_id = 0;
    pn_history_commit_now (self);
    return G_SOURCE_REMOVE;
}

void
pn_history_record (PnHistory *self)
{
    g_return_if_fail (PN_IS_HISTORY (self));

    /* A frozen span commits once at thaw; a restore replays a snapshot
     * and must not record itself. */
    if (self->restoring || self->freeze_depth > 0)
        return;
    if (self->idle_id != 0)
        return;

    self->idle_id = g_idle_add (commit_idle, self);
}

void
pn_history_commit_now (PnHistory *self)
{
    gchar *current;

    g_return_if_fail (PN_IS_HISTORY (self));

    if (self->restoring || self->freeze_depth > 0)
        return;

    current = pn_flow_to_string (self->flow);
    if (current == NULL || g_strcmp0 (current, self->baseline) == 0)
    {
        /* No net change since the last commit (e.g. a drag that landed
         * where it started, or a dialog closed untouched). */
        g_free (current);
        return;
    }

    /* The old baseline becomes the step an undo returns to. */
    g_ptr_array_add (self->undo, self->baseline);
    if (self->undo->len > PN_HISTORY_LIMIT)
    {
        g_free (g_ptr_array_index (self->undo, 0));
        g_ptr_array_remove_index (self->undo, 0);
    }
    redo_clear (self);

    self->baseline = current; /* ownership moves to the baseline */

    sync_modified (self);
    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

/* ------------------------------------------------------------------ */
/*  Freeze / thaw                                                      */
/* ------------------------------------------------------------------ */

void
pn_history_freeze (PnHistory *self)
{
    g_return_if_fail (PN_IS_HISTORY (self));
    self->freeze_depth++;
}

void
pn_history_thaw (PnHistory *self)
{
    g_return_if_fail (PN_IS_HISTORY (self));

    if (self->freeze_depth == 0)
        return;

    self->freeze_depth--;
    if (self->freeze_depth == 0)
        pn_history_commit_now (self);
}

/* ------------------------------------------------------------------ */
/*  Undo / redo                                                        */
/* ------------------------------------------------------------------ */

gboolean
pn_history_can_undo (PnHistory *self)
{
    g_return_val_if_fail (PN_IS_HISTORY (self), FALSE);
    return self->undo->len > 0;
}

gboolean
pn_history_can_redo (PnHistory *self)
{
    g_return_val_if_fail (PN_IS_HISTORY (self), FALSE);
    return self->redo->len > 0;
}

/* Load @snapshot into the document with the restore guard held, so the
 * cascade of store signals it triggers does not record a new step. */
static void
restore_snapshot (PnHistory *self, gchar *snapshot)
{
    GError *error = NULL;

    self->restoring = TRUE;
    if (!pn_flow_load_from_data (self->flow, snapshot, &error))
    {
        g_warning ("pn-history: failed to restore snapshot: %s",
                   error != NULL ? error->message : "(unknown)");
        g_clear_error (&error);
    }
    self->restoring = FALSE;

    /* The restored text is now the committed baseline. */
    g_free (self->baseline);
    self->baseline = snapshot; /* ownership moves to the baseline */

    sync_modified (self);
    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

void
pn_history_undo (PnHistory *self)
{
    gchar *target;

    g_return_if_fail (PN_IS_HISTORY (self));

    /* Flush any pending coalesced edit so undo steps from the true
     * current state. */
    if (self->idle_id != 0)
    {
        g_source_remove (self->idle_id);
        self->idle_id = 0;
        pn_history_commit_now (self);
    }

    target = stack_pop (self->undo);
    if (target == NULL)
        return;

    /* Current state goes onto the redo stack before we overwrite it. */
    g_ptr_array_add (self->redo, self->baseline);
    self->baseline = NULL; /* restore_snapshot adopts @target as baseline */

    restore_snapshot (self, target);
}

void
pn_history_redo (PnHistory *self)
{
    gchar *target;

    g_return_if_fail (PN_IS_HISTORY (self));

    target = stack_pop (self->redo);
    if (target == NULL)
        return;

    g_ptr_array_add (self->undo, self->baseline);
    self->baseline = NULL;

    restore_snapshot (self, target);
}

/* ------------------------------------------------------------------ */
/*  Lifecycle                                                          */
/* ------------------------------------------------------------------ */

void
pn_history_clear (PnHistory *self)
{
    g_return_if_fail (PN_IS_HISTORY (self));

    if (self->idle_id != 0)
    {
        g_source_remove (self->idle_id);
        self->idle_id = 0;
    }

    stack_clear (self->undo);
    stack_clear (self->redo);

    g_free (self->baseline);
    self->baseline = pn_flow_to_string (self->flow);
    g_free (self->saved);
    self->saved = g_strdup (self->baseline);

    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

void
pn_history_mark_saved (PnHistory *self)
{
    g_return_if_fail (PN_IS_HISTORY (self));

    /* Flush any coalesced edit so the baseline reflects exactly what was
     * just written to disk; otherwise saved != baseline would leave the
     * freshly-saved document reported as modified. */
    if (self->idle_id != 0)
    {
        g_source_remove (self->idle_id);
        self->idle_id = 0;
        pn_history_commit_now (self);
    }

    g_free (self->saved);
    self->saved = g_strdup (self->baseline);

    sync_modified (self);
    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

gboolean
pn_history_is_restoring (PnHistory *self)
{
    g_return_val_if_fail (PN_IS_HISTORY (self), FALSE);
    return self->restoring;
}
