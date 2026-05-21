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

/* ------------------------------------------------------------------ */
/*  pntimer                                                            */
/*                                                                     */
/*  Helpers for unit-testing the #PnAutoTrigger family (AutoInjector,  */
/*  AutoRandom, Clock, Watchdog).  Unlike the plain message nodes,     */
/*  these fire on a dedicated worker thread and hand their output to   */
/*  the main loop via pn_auto_trigger_emit_on_main(), so a test must   */
/*  drive a #GMainLoop to observe an emission.  Rather than wait out   */
/*  the real timer, a test forces a tick with pn_auto_trigger_kick()   */
/*  and pumps the default context until the message lands.             */
/*                                                                     */
/*  Teardown matters here: the worker holds a borrowed pointer to the  */
/*  node and is joined only in dispose (refcount 0).  Any emission     */
/*  still queued on the main context holds a strong ref, so the helper */
/*  disables the node (stopping further triggers), drains the context, */
/*  then drops the last ref — at which point dispose joins the worker. */
/* ------------------------------------------------------------------ */

#ifndef PNTIMER_H
#define PNTIMER_H

#include <glib.h>

#include "pntest.h"
#include "pn-node.h"
#include "pn-auto-trigger.h"

G_BEGIN_DECLS

typedef struct
{
    GMainLoop *loop;        /* live only during pn_timer_kick_wait()   */
    guint      count;       /* messages seen since pn_timer_attach()   */
    PnMessage *last;        /* most recent emission (strong ref)       */
    gulong     handler;     /* the "message" signal connection         */
    gboolean   timed_out;   /* set when the safety timeout fired       */
} PnTimerDriver;

static inline void
pn_timer_on_message (PnNode *node, PnMessage *message, gpointer user_data)
{
    PnTimerDriver *d = user_data;

    (void) node;
    d->count++;
    g_clear_object (&d->last);
    d->last = g_object_ref (message);

    if (d->loop != NULL)
        g_main_loop_quit (d->loop);
}

static inline gboolean
pn_timer_on_timeout (gpointer user_data)
{
    PnTimerDriver *d = user_data;

    d->timed_out = TRUE;
    if (d->loop != NULL)
        g_main_loop_quit (d->loop);

    return G_SOURCE_REMOVE;
}

/* Wire @d to @node's "message" signal and push the auto-trigger period
 * out of the way so the only ticks are the ones a test kicks. */
static inline void
pn_timer_attach (PnNode *node, PnTimerDriver *d)
{
    d->loop      = NULL;
    d->count     = 0;
    d->last      = NULL;
    d->timed_out = FALSE;
    d->handler   = g_signal_connect (node, "message",
                                     G_CALLBACK (pn_timer_on_message), d);

    /* A long period stops the worker's own schedule from racing the
     * test; kick() supplies the ticks we actually assert on. */
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (node), 3600u);
}

/* Force one trigger and pump the default context until the resulting
 * message arrives, or a safety timeout trips (so a regression cannot
 * hang the suite). */
static inline void
pn_timer_kick_wait (PnNode *node, PnTimerDriver *d)
{
    guint to;

    d->loop      = g_main_loop_new (NULL, FALSE);
    d->timed_out = FALSE;

    to = g_timeout_add (3000, pn_timer_on_timeout, d);
    pn_auto_trigger_kick (PN_AUTO_TRIGGER (node));
    g_main_loop_run (d->loop);

    if (!d->timed_out)
        g_source_remove (to);

    g_main_loop_unref (d->loop);
    d->loop = NULL;
}

/* Run the default context for @ms milliseconds — used both to let a
 * silent trigger complete and to confirm that no message arrives. */
static inline void
pn_timer_pump (guint ms)
{
    GMainContext *ctx = g_main_context_default ();
    gint64        end = g_get_monotonic_time () + (gint64) ms * 1000;

    while (g_get_monotonic_time () < end)
    {
        g_main_context_iteration (ctx, FALSE);
        g_usleep (1000);   /* 1 ms, so the loop does not spin hot */
    }
}

/* Stop the worker, drain queued emissions, and drop the node. */
static inline void
pn_timer_destroy (PnNode *node, PnTimerDriver *d)
{
    if (d->handler != 0)
        g_signal_handler_disconnect (node, d->handler);

    /* Disabling makes the worker skip further triggers, so nothing new
     * is queued past this point; the drain then releases the refs held
     * by any already-queued emission. */
    pn_node_set_disabled (node, TRUE);
    while (g_main_context_iteration (g_main_context_default (), FALSE))
        ;

    g_clear_object (&d->last);
    g_object_unref (node);
}

G_END_DECLS

#endif /* PNTIMER_H */
