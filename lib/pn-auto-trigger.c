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

#include "pn-auto-trigger.h"

/* ------------------------------------------------------------------ */
/*  PnAutoTrigger                                                      */
/* ------------------------------------------------------------------ */

#define PN_AUTO_TRIGGER_PERIOD_MIN  1u
#define PN_AUTO_TRIGGER_PERIOD_MAX  3600u
#define PN_AUTO_TRIGGER_PERIOD_DEF  1u

/* Seconds to wait before the *first* trigger fires, regardless of how
 * long the configured period is.  The point is that a 30s / 60s / hourly
 * node should still produce output shortly after the worksheet loads
 * instead of leaving the user staring at a blank widget until the first
 * full period elapses.  Capped at one second so a worksheet that loads
 * many such nodes does not produce a visible thundering herd, but long
 * enough that g_object_new's construct-time property setters have
 * normally finished applying before the worker reads them. */
#define PN_AUTO_TRIGGER_FIRST_DELAY 1u

typedef struct
{
    /* Tick period in seconds.  Read on the worker thread to compute
     * the next deadline; written from any thread via
     * pn_auto_trigger_set_period(). */
    guint    period;

    /* Worker thread driving the timer.  Joined in dispose. */
    GThread *thread;

    /* Mutex/cond pair: protects @period, @stop, @due, @first,
     * @deliver_sync and lets the setter wake a sleeping worker so a
     * shorter period (or the stop signal, or a one-shot kick) takes
     * effect immediately. */
    GMutex   mutex;
    GCond    cond;
    gboolean stop;
    gboolean due;          /* one-shot trigger requested via kick() */
    gboolean first;        /* TRUE until the first deadline is set */

    /* Construct-only: when FALSE the worker thread is never spawned, so
     * the instance stays quiescent and a caller drives a single tick
     * by hand via pn_auto_trigger_run_once_sync().  Always TRUE in
     * normal use; FALSE is the test seam. */
    gboolean autostart;

    /* Set only for the duration of pn_auto_trigger_run_once_sync():
     * makes pn_auto_trigger_emit_on_main() deliver on the calling
     * thread instead of bouncing through the main loop, so the emitted
     * #PnMessage can be observed synchronously. */
    gboolean deliver_sync;
} PnAutoTriggerPrivate;

G_DEFINE_TYPE_WITH_PRIVATE (PnAutoTrigger, pn_auto_trigger, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_PERIOD,
    PROP_AUTOSTART,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Worker thread                                                      */
/* ------------------------------------------------------------------ */

/** Worker loop.  Runs on a dedicated thread and fires the class
 *  `trigger` vfunc once per period until the owning object signals
 *  shutdown via the `stop` flag. */
static gpointer
pn_auto_trigger_worker (gpointer data)
{
    PnAutoTrigger        *self = PN_AUTO_TRIGGER (data);
    PnAutoTriggerPrivate *priv = pn_auto_trigger_get_instance_private (self);

    for (;;)
    {
        gint64   deadline;
        gboolean should_stop;

        /* Compute the next deadline from the current period.  Reads
         * happen under the mutex so a concurrent set_period sees a
         * coherent view of the loop state.
         *
         * The first deadline is clamped to PN_AUTO_TRIGGER_FIRST_DELAY
         * so a long-period node (poll every 30s, every hour, …) still
         * produces its first message shortly after the worksheet has
         * finished loading instead of leaving downstream widgets blank
         * for a full period.  Subsequent deadlines honour the full
         * configured period. */
        g_mutex_lock (&priv->mutex);
        if (priv->stop)
        {
            g_mutex_unlock (&priv->mutex);
            break;
        }
        {
            guint sleep_secs = priv->period;
            if (priv->first)
            {
                if (sleep_secs > PN_AUTO_TRIGGER_FIRST_DELAY)
                    sleep_secs = PN_AUTO_TRIGGER_FIRST_DELAY;
                priv->first = FALSE;
            }
            deadline = g_get_monotonic_time ()
                       + (gint64) sleep_secs * G_TIME_SPAN_SECOND;
        }

        /* Sleep until either the deadline elapses or someone
         * broadcasts the cond (period change, shutdown, or a
         * one-shot kick).  The cond_wait_until contract: returns
         * FALSE on timeout.  The `due` flag is honoured here as a
         * way to fire an extra trigger off-schedule without
         * disturbing the periodic deadline. */
        while (!priv->stop && !priv->due)
        {
            if (!g_cond_wait_until (&priv->cond, &priv->mutex, deadline))
                break;  /* timer expired */
        }
        should_stop = priv->stop;
        priv->due   = FALSE;     /* consume the kick request */
        g_mutex_unlock (&priv->mutex);

        if (should_stop)
            break;

        {
            PnAutoTriggerClass *klass = PN_AUTO_TRIGGER_GET_CLASS (self);
            /* Skip the periodic action while the node is disabled.
             * pn_node_emit_message would already drop any output, but
             * gating here avoids the side-effects of the work itself
             * (HTTP requests, ICMP probes, watchdog evaluation, …). */
            if (klass->trigger != NULL && !pn_node_get_disabled (PN_NODE (self)))
                klass->trigger (self);
        }
    }

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  emit_on_main trampoline                                            */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnNode    *node;     /* strong ref */
    PnMessage *message;  /* strong ref */
} PnEmitClosure;

static gboolean
emit_on_main_trampoline (gpointer data)
{
    PnEmitClosure *c = data;

    pn_node_emit_message (c->node, c->message);

    return G_SOURCE_REMOVE;
}

static void
emit_closure_free (gpointer data)
{
    PnEmitClosure *c = data;

    g_clear_object (&c->message);
    g_clear_object (&c->node);
    g_free (c);
}

void
pn_auto_trigger_emit_on_main (
        PnAutoTrigger *self,
        PnMessage     *message)
{
    PnAutoTriggerPrivate *priv;
    PnEmitClosure        *c;
    gboolean              sync;

    g_return_if_fail (PN_IS_AUTO_TRIGGER (self));
    g_return_if_fail (PN_IS_MESSAGE (message));

    priv = pn_auto_trigger_get_instance_private (self);

    g_mutex_lock (&priv->mutex);
    sync = priv->deliver_sync;
    g_mutex_unlock (&priv->mutex);

    /* Synchronous one-shot path (pn_auto_trigger_run_once_sync): emit
     * straight away on the calling thread so a test sees the "message"
     * signal without running a main loop.  We hold transfer-full
     * ownership, and pn_node_emit_message only borrows, so unref after. */
    if (sync)
    {
        pn_node_emit_message (PN_NODE (self), message);
        g_object_unref (message);
        return;
    }

    c = g_new0 (PnEmitClosure, 1);
    c->node    = g_object_ref (PN_NODE (self));
    c->message = message;  /* transfer */

    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                emit_on_main_trampoline,
                                c,
                                emit_closure_free);
}

/* ------------------------------------------------------------------ */
/*  log_on_main trampoline                                             */
/*                                                                     */
/*  pn_node_log() must run on the main thread, but subclasses do       */
/*  their blocking work (and discover their failures) on the worker.   */
/*  This marshals the formatted line across, mirroring                 */
/*  emit_on_main above.                                                */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnNode     *node;  /* strong ref */
    PnLogLevel  level;
    gchar      *text;  /* owned */
} PnLogClosure;

static gboolean
log_on_main_trampoline (gpointer data)
{
    PnLogClosure *c = data;

    pn_node_log (c->node, c->level, "%s", c->text);

    return G_SOURCE_REMOVE;
}

static void
log_closure_free (gpointer data)
{
    PnLogClosure *c = data;

    g_free (c->text);
    g_clear_object (&c->node);
    g_free (c);
}

void
pn_auto_trigger_log_on_main (
        PnAutoTrigger *self,
        PnLogLevel     level,
        const gchar   *format,
        ...)
{
    PnAutoTriggerPrivate *priv;
    PnLogClosure         *c;
    gchar                *text;
    va_list               args;
    gboolean              sync;

    g_return_if_fail (PN_IS_AUTO_TRIGGER (self));
    g_return_if_fail (format != NULL);

    va_start (args, format);
    text = g_strdup_vprintf (format, args);
    va_end (args);

    priv = pn_auto_trigger_get_instance_private (self);

    g_mutex_lock (&priv->mutex);
    sync = priv->deliver_sync;
    g_mutex_unlock (&priv->mutex);

    /* Synchronous one-shot path (pn_auto_trigger_run_once_sync): the
     * trigger runs on the calling thread with no main loop, so append
     * straight away. */
    if (sync)
    {
        pn_node_log (PN_NODE (self), level, "%s", text);
        g_free (text);
        return;
    }

    c = g_new0 (PnLogClosure, 1);
    c->node  = g_object_ref (PN_NODE (self));
    c->level = level;
    c->text  = text;  /* transfer */

    g_main_context_invoke_full (NULL,
                                G_PRIORITY_DEFAULT,
                                log_on_main_trampoline,
                                c,
                                log_closure_free);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_auto_trigger_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnAutoTrigger        *self = PN_AUTO_TRIGGER (object);
    PnAutoTriggerPrivate *priv = pn_auto_trigger_get_instance_private (self);

    switch (prop_id)
    {
    case PROP_PERIOD:
        g_value_set_uint (value, pn_auto_trigger_get_period (self));
        break;
    case PROP_AUTOSTART:
        g_value_set_boolean (value, priv->autostart);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_auto_trigger_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnAutoTrigger        *self = PN_AUTO_TRIGGER (object);
    PnAutoTriggerPrivate *priv = pn_auto_trigger_get_instance_private (self);

    switch (prop_id)
    {
    case PROP_PERIOD:
        pn_auto_trigger_set_period (self, g_value_get_uint (value));
        break;
    case PROP_AUTOSTART:
        /* Construct-only: stored now, acted on in constructed(). */
        priv->autostart = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_auto_trigger_constructed (GObject *object)
{
    PnAutoTrigger        *self = PN_AUTO_TRIGGER (object);
    PnAutoTriggerPrivate *priv = pn_auto_trigger_get_instance_private (self);

    G_OBJECT_CLASS (pn_auto_trigger_parent_class)->constructed (object);

    /* Spawn the worker here rather than in init() so the construct-only
     * :autostart property has already been applied: a test creating the
     * node with "autostart" = FALSE gets a quiescent instance with no
     * background thread and drives a single tick via
     * pn_auto_trigger_run_once_sync().  The default (TRUE) reproduces the
     * historical behaviour exactly.  The thread holds a borrowed pointer
     * to @self and is joined in dispose before chain-up. */
    if (priv->autostart)
        priv->thread = g_thread_new ("pn-auto-trigger",
                                     pn_auto_trigger_worker,
                                     self);
}

static void
pn_auto_trigger_dispose (GObject *object)
{
    PnAutoTrigger        *self = PN_AUTO_TRIGGER (object);
    PnAutoTriggerPrivate *priv = pn_auto_trigger_get_instance_private (self);

    /* Stop the worker thread.  The cond broadcast wakes it
     * immediately so we do not block for the rest of the current
     * period.  Any messages the worker has already queued via
     * pn_auto_trigger_emit_on_main hold strong refs that keep the
     * object alive until they drain — so by the time dispose runs
     * (refcount dropped to zero), the queue is empty. */
    if (priv->thread != NULL)
    {
        g_mutex_lock (&priv->mutex);
        priv->stop = TRUE;
        g_cond_broadcast (&priv->cond);
        g_mutex_unlock (&priv->mutex);

        g_thread_join (priv->thread);
        priv->thread = NULL;
    }

    G_OBJECT_CLASS (pn_auto_trigger_parent_class)->dispose (object);
}

static void
pn_auto_trigger_finalize (GObject *object)
{
    PnAutoTrigger        *self = PN_AUTO_TRIGGER (object);
    PnAutoTriggerPrivate *priv = pn_auto_trigger_get_instance_private (self);

    g_mutex_clear (&priv->mutex);
    g_cond_clear  (&priv->cond);

    G_OBJECT_CLASS (pn_auto_trigger_parent_class)->finalize (object);
}

static void
pn_auto_trigger_class_init (PnAutoTriggerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = pn_auto_trigger_get_property;
    object_class->set_property = pn_auto_trigger_set_property;
    object_class->constructed  = pn_auto_trigger_constructed;
    object_class->dispose      = pn_auto_trigger_dispose;
    object_class->finalize     = pn_auto_trigger_finalize;

    props[PROP_PERIOD] = g_param_spec_uint (
            "period", "Period",
            "Seconds between automatic triggers",
            PN_AUTO_TRIGGER_PERIOD_MIN,
            PN_AUTO_TRIGGER_PERIOD_MAX,
            PN_AUTO_TRIGGER_PERIOD_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_AUTOSTART] = g_param_spec_boolean (
            "autostart", "Autostart",
            "Whether the periodic worker thread is spawned at "
            "construction.  Always TRUE in normal use; tests pass FALSE "
            "to obtain a quiescent instance and drive a single tick via "
            "pn_auto_trigger_run_once_sync().",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
            G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_auto_trigger_init (PnAutoTrigger *self)
{
    PnAutoTriggerPrivate *priv = pn_auto_trigger_get_instance_private (self);

    priv->period       = PN_AUTO_TRIGGER_PERIOD_DEF;
    priv->stop         = FALSE;
    priv->first        = TRUE;
    priv->autostart    = TRUE;   /* overridden by the construct property */
    priv->deliver_sync = FALSE;
    g_mutex_init (&priv->mutex);
    g_cond_init  (&priv->cond);

    /* The worker is spawned in constructed(), once :autostart is known. */
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

guint
pn_auto_trigger_get_period (PnAutoTrigger *self)
{
    PnAutoTriggerPrivate *priv;
    guint                 period;

    g_return_val_if_fail (PN_IS_AUTO_TRIGGER (self), 0);

    priv = pn_auto_trigger_get_instance_private (self);
    g_mutex_lock   (&priv->mutex);
    period = priv->period;
    g_mutex_unlock (&priv->mutex);

    return period;
}

void
pn_auto_trigger_set_period (
        PnAutoTrigger *self,
        guint          seconds)
{
    PnAutoTriggerPrivate *priv;
    gboolean              changed;

    g_return_if_fail (PN_IS_AUTO_TRIGGER (self));

    if (seconds < PN_AUTO_TRIGGER_PERIOD_MIN)
        seconds = PN_AUTO_TRIGGER_PERIOD_MIN;
    else if (seconds > PN_AUTO_TRIGGER_PERIOD_MAX)
        seconds = PN_AUTO_TRIGGER_PERIOD_MAX;

    priv = pn_auto_trigger_get_instance_private (self);

    g_mutex_lock (&priv->mutex);
    changed = (priv->period != seconds);
    if (changed)
    {
        priv->period = seconds;
        /* Wake the worker so the new period takes effect on the
         * very next tick rather than after the prior deadline. */
        g_cond_broadcast (&priv->cond);
    }
    g_mutex_unlock (&priv->mutex);

    if (changed)
        g_object_notify_by_pspec (G_OBJECT (self), props[PROP_PERIOD]);
}

void
pn_auto_trigger_kick (PnAutoTrigger *self)
{
    PnAutoTriggerPrivate *priv;

    g_return_if_fail (PN_IS_AUTO_TRIGGER (self));

    priv = pn_auto_trigger_get_instance_private (self);

    g_mutex_lock (&priv->mutex);
    priv->due = TRUE;
    g_cond_broadcast (&priv->cond);
    g_mutex_unlock (&priv->mutex);
}

void
pn_auto_trigger_run_once_sync (PnAutoTrigger *self)
{
    PnAutoTriggerPrivate *priv;
    PnAutoTriggerClass   *klass;

    g_return_if_fail (PN_IS_AUTO_TRIGGER (self));

    priv  = pn_auto_trigger_get_instance_private (self);
    klass = PN_AUTO_TRIGGER_GET_CLASS (self);

    /* Flip emit_on_main into synchronous delivery, run one trigger on
     * this thread, then restore.  Intended for instances created with
     * "autostart" = FALSE so no worker thread runs concurrently. */
    g_mutex_lock (&priv->mutex);
    priv->deliver_sync = TRUE;
    g_mutex_unlock (&priv->mutex);

    if (klass->trigger != NULL)
        klass->trigger (self);

    g_mutex_lock (&priv->mutex);
    priv->deliver_sync = FALSE;
    g_mutex_unlock (&priv->mutex);
}
