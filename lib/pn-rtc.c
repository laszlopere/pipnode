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

#include "pn-rtc.h"
#include "pn-message.h"

struct _PnRtc
{
    PnAutoTrigger parent_instance;
};

G_DEFINE_TYPE (PnRtc, pn_rtc, PN_TYPE_AUTO_TRIGGER)

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger.  We      */
/*  build the message here and hand it off to                         */
/*  pn_auto_trigger_emit_on_main() so the signal listeners (which     */
/*  may touch GTK widgets) run on the main thread.                    */
/* ------------------------------------------------------------------ */

static void
pn_rtc_trigger (PnAutoTrigger *self)
{
    PnNode    *node = PN_NODE (self);
    PnMessage *msg;
    GDateTime *now  = g_date_time_new_now_local ();
    gchar     *spoken;

    msg = pn_message_new (node, NULL);
    pn_message_set_int64 (msg, "epoch",  g_date_time_to_unix          (now));
    pn_message_set_int   (msg, "year",   g_date_time_get_year         (now));
    pn_message_set_int   (msg, "month",  g_date_time_get_month        (now));
    pn_message_set_int   (msg, "dom",    g_date_time_get_day_of_month (now));
    pn_message_set_int   (msg, "hour",   g_date_time_get_hour         (now));
    pn_message_set_int   (msg, "minute", g_date_time_get_minute       (now));
    pn_message_set_int   (msg, "second", g_date_time_get_second       (now));

    /* Convenience members for downstream consumers: "success" lets a
     * #PnEdge filter chain off the tick stream, "output" gives sinks
     * (notably #PnTts) a ready-to-render sentence. */
    pn_message_set_boolean (msg, "success", TRUE);
    spoken = g_strdup_printf ("The time is %02d:%02d:%02d.",
                              g_date_time_get_hour   (now),
                              g_date_time_get_minute (now),
                              g_date_time_get_second (now));
    pn_message_set_string (msg, "output", spoken);
    g_free (spoken);

    pn_auto_trigger_emit_on_main (self, msg);

    g_date_time_unref (now);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_rtc_class_init (PnRtcClass *klass)
{
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    trigger_class->trigger = pn_rtc_trigger;

    node_class->class_name = "Clock";
    node_class->icon       = "\xef\x80\x97";  /* fa-clock-o U+F017 */
    node_class->color      = (PnColor){ 0.95, 0.78, 0.40, 1.0 };
    node_class->category   = "Sources";
    node_class->has_input  = FALSE;
    node_class->has_output = TRUE;
}

static void
pn_rtc_init (PnRtc *self)
{
    PnNode *node = PN_NODE (self);

    /* Defaults that identify this node visually.  Amber colour, alarm
     * clock glyph, source-only ports.  Period inherits from
     * #PnAutoTrigger (default 1 second). */
    PnColor amber = { 0.95, 0.78, 0.40, 1.0 };

    pn_node_set_class_name (node, "Clock");
    pn_node_set_icon       (node, "\xef\x80\x97");  /* fa-clock-o U+F017 */
    pn_node_set_color      (node, &amber);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnRtc *
pn_rtc_new (void)
{
    return g_object_new (PN_TYPE_RTC, NULL);
}
