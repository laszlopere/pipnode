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

#ifndef PN_HISTORY_H
#define PN_HISTORY_H

#include <glib-object.h>

G_BEGIN_DECLS

/* PnFlow is the document this history snapshots; forward-declared to
 * avoid an include cycle (pn-flow.h includes us for its accessor). */
typedef struct _PnFlow PnFlow;

/* ------------------------------------------------------------------ */
/*  PnHistory                                                          */
/*                                                                     */
/*  Undo/redo for one #PnFlow document, implemented as a stack of      */
/*  whole-document JSON snapshots (the same text pn_flow_to_string()   */
/*  produces).  Restore is the in-place pn_flow_load_from_data() load  */
/*  the worksheet already reacts to, so a single load path covers      */
/*  every kind of edit.                                                */
/*                                                                      */
/*  Edits are coalesced two ways.  Discrete mutations (add/delete/wire */
/*  /paste/globals, even D-Bus) call pn_history_record(), which        */
/*  schedules one idle commit so a burst becomes a single step.        */
/*  Long-lived interactions (a node drag, a property-dialog session)   */
/*  bracket themselves with pn_history_freeze()/pn_history_thaw() so    */
/*  every intermediate change collapses into one step at thaw time.    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_HISTORY (pn_history_get_type ())

G_DECLARE_FINAL_TYPE (PnHistory, pn_history, PN, HISTORY, GObject)

/**
 * pn_history_new:
 * @flow: (transfer none): the document to track; must outlive the history
 *
 * Creates a history seeded with @flow's current state as both the
 * baseline and the saved (on-disk) snapshot.  The history holds no
 * reference to @flow — #PnFlow owns its history, not the other way
 * round.
 */
PnHistory *pn_history_new          (PnFlow *flow);

/**
 * pn_history_record:
 * @self: the history
 *
 * Notes that the document just changed and schedules a coalesced commit
 * on the main loop.  A no-op while frozen, while restoring, or when the
 * document is unchanged versus the baseline.
 */
void       pn_history_record       (PnHistory *self);

/**
 * pn_history_freeze:
 * @self: the history
 *
 * Suspends commits.  Nestable; balance every freeze with a thaw.
 */
void       pn_history_freeze       (PnHistory *self);

/**
 * pn_history_thaw:
 * @self: the history
 *
 * Resumes commits; the outermost thaw commits the net change accrued
 * while frozen (one undo step for the whole frozen span).
 */
void       pn_history_thaw         (PnHistory *self);

/**
 * pn_history_commit_now:
 * @self: the history
 *
 * Immediately records the change since the baseline, if any: pushes the
 * old baseline onto the undo stack, clears the redo stack, and adopts
 * the current document as the new baseline.  Does nothing if frozen,
 * restoring, or unchanged.
 */
void       pn_history_commit_now   (PnHistory *self);

gboolean   pn_history_can_undo     (PnHistory *self);
gboolean   pn_history_can_redo     (PnHistory *self);

/**
 * pn_history_undo:
 * @self: the history
 *
 * Restores the previous snapshot into the document, pushing the current
 * state onto the redo stack.  No-op when there is nothing to undo.
 */
void       pn_history_undo         (PnHistory *self);

/**
 * pn_history_redo:
 * @self: the history
 *
 * Re-applies the next snapshot, the inverse of pn_history_undo().
 */
void       pn_history_redo         (PnHistory *self);

/**
 * pn_history_clear:
 * @self: the history
 *
 * Drops both stacks and re-seeds the baseline and saved snapshot from
 * the document's current state — the clean slate for File→New / Open.
 */
void       pn_history_clear        (PnHistory *self);

/**
 * pn_history_mark_saved:
 * @self: the history
 *
 * Records the current document as the on-disk state, so undoing or
 * redoing back to it reports the document unmodified.
 */
void       pn_history_mark_saved   (PnHistory *self);

/**
 * pn_history_is_restoring:
 * @self: the history
 *
 * %TRUE while an undo/redo is loading a snapshot into the document.
 * #PnFlow consults this so its clear/load paths leave the history
 * untouched during a restore.
 */
gboolean   pn_history_is_restoring (PnHistory *self);

G_END_DECLS

#endif /* PN_HISTORY_H */
