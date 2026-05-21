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

#ifndef PN_AUTO_TRIGGER_H
#define PN_AUTO_TRIGGER_H

#include "pn-node.h"
#include "pn-message.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnAutoTrigger                                                      */
/*                                                                     */
/*  Derivable base class for nodes that fire periodically without an   */
/*  upstream input.  Each instance owns a worker thread that sleeps    */
/*  for #PnAutoTrigger:period seconds between ticks; on every tick    */
/*  the class virtual method #PnAutoTriggerClass.trigger is invoked   */
/*  on that worker thread, so subclasses can do blocking work without */
/*  freezing the UI.                                                   */
/*                                                                     */
/*  Because GTK and the message graph are single-threaded, the base   */
/*  class provides pn_auto_trigger_emit_on_main() so the worker can   */
/*  hand a finished #PnMessage back to the main loop for emission.    */
/*                                                                     */
/*  Setting #PnAutoTrigger:period wakes a sleeping worker so the new   */
/*  period takes effect on the next tick rather than only after the   */
/*  old deadline elapses.                                              */
/*                                                                     */
/*  The first tick after construction fires after at most one second,  */
/*  even when the configured period is much longer (30s, 1h, …), so   */
/*  downstream widgets get an initial value shortly after the worksheet*/
/*  loads instead of staying blank for a full period.  After that the */
/*  schedule honours the configured period exactly.                   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_AUTO_TRIGGER (pn_auto_trigger_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnAutoTrigger, pn_auto_trigger,
                          PN, AUTO_TRIGGER, PnNode)

struct _PnAutoTriggerClass
{
    PnNodeClass parent_class;

    /**
     * PnAutoTriggerClass::trigger:
     * @self: the auto-trigger instance
     *
     * Called once per period **on the worker thread**.  Subclasses
     * override this to do work and typically end by calling
     * pn_auto_trigger_emit_on_main() so the resulting #PnMessage is
     * delivered through the signal graph on the main thread.
     *
     * The default implementation is a no-op.
     */
    void (*trigger) (PnAutoTrigger *self);
};

/* Period in whole seconds; range 1..3600, default 1. */
guint pn_auto_trigger_get_period (PnAutoTrigger *self);
void  pn_auto_trigger_set_period (PnAutoTrigger *self, guint seconds);

/**
 * pn_auto_trigger_kick:
 * @self: the auto-trigger instance
 *
 * Wake the worker thread and request an extra trigger as soon as
 * possible (without resetting the periodic schedule).  Useful when
 * the node has just been constructed or loaded and the caller knows
 * the cached state is stale: the regular schedule still kicks in
 * #PnAutoTrigger:period seconds later.  Safe to call from any
 * thread; if the worker is currently busy in #PnAutoTriggerClass.trigger
 * the request is honoured at the start of the next loop iteration.
 */
void  pn_auto_trigger_kick       (PnAutoTrigger *self);

/**
 * pn_auto_trigger_emit_on_main:
 * @self: the auto-trigger instance
 * @message: (transfer full): the message to deliver
 *
 * Schedules @message to be emitted from @self on the GLib main
 * thread via pn_node_emit_message().  Takes ownership of @message
 * and adds a temporary reference on @self that is released after
 * the emission runs.  Safe to call from the worker thread.
 */
void  pn_auto_trigger_emit_on_main (PnAutoTrigger *self, PnMessage *message);

G_END_DECLS

#endif /* PN_AUTO_TRIGGER_H */
