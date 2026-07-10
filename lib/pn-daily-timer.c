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
/*  PnDailyTimer — logic tier (headless core).                         */
/*                                                                     */
/*  This file holds the GTK-free half of the Daily Timer node: the     */
/*  GType, the canonical "schedule" string property, the compiled      */
/*  interval cache, the pure containment test and the poll-and-emit-   */
/*  on-edge trigger.  The per-interval row editor (day combo + four    */
/*  spin buttons + Add/Remove) lives in the companion gui-tier file    */
/*  pn-daily-timer-gui.c, which installs the build_class_tab vfunc     */
/*  onto this class at editor startup.  The headless runtime runs the  */
/*  schedule without ever pulling GTK.                                 */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-daily-timer.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>

/* fa-calendar U+F073.  FontAwesome 4.7 -- the only set the palette
 * font ships; a 5.x-only glyph would render as tofu. */
#define PN_DAILY_TIMER_ICON  "\xef\x81\xb3"

/* Sentinel :day value for a row that fires on all seven days.  Real
 * days are 1..7 (Mon..Sun) so they can be compared straight against
 * g_date_time_get_day_of_week() without a lookup table. */
#define PN_DAILY_TIMER_EVERY_DAY  (-1)

/* A new node ships with one worked example rather than an empty
 * schedule: dropping a Daily Timer and opening it should show what an
 * interval looks like, not a blank grid.  Clearing every row leaves
 * "[]", which is always off. */
#define PN_DAILY_TIMER_DEFAULT_SCHEDULE \
    "[{\"day\":-1,\"on_hour\":7,\"on_minute\":0," \
    "\"off_hour\":9,\"off_minute\":0}]"

#define MINUTES_PER_DAY  1440

/* ------------------------------------------------------------------ */
/*  Compiled interval                                                  */
/*                                                                     */
/*  Times collapse to a minute-of-day once, at compile time, so the    */
/*  once-a-second containment test is three integer comparisons and    */
/*  never re-reads the JSON.                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    gint day;      /* 1..7 (Mon..Sun) or PN_DAILY_TIMER_EVERY_DAY */
    gint on_min;   /* minute-of-day, 0..1439 */
    gint off_min;  /* minute-of-day, 0..1439 */
} CompiledInterval;

struct _PnDailyTimer
{
    PnAutoTrigger parent_instance;

    /* Guards every field below.  The trigger reads them on the worker
     * thread inherited from #PnAutoTrigger while the settings dialog
     * (and the loader, and D-Bus) write :schedule from the main thread,
     * so the compiled cache would otherwise be swapped out from under a
     * poll in progress. */
    GMutex   lock;

    gchar   *schedule_json;  /* canonical string property */
    GArray  *compiled;       /* CompiledInterval, parsed cache */

    /* State carried by the last emitted message, as a tri-state: -1
     * until the first poll, then 0 / 1.  The unknown state is what
     * makes the first tick after construction announce unconditionally
     * instead of comparing against a fabricated "off"; it is also
     * restored whenever :schedule changes, so editing the schedule
     * re-announces the state the new schedule implies rather than
     * leaving downstream nodes on the old one until the next edge. */
    gint     last_state;
};

G_DEFINE_TYPE (PnDailyTimer, pn_daily_timer, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_SCHEDULE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Compilation                                                        */
/* ------------------------------------------------------------------ */

/** Read a 0-based integer member, defaulting to 0 when absent or of the
 *  wrong shape, and clamp it into [0, @max].  The editor never writes
 *  out-of-range values; a hand-edited file might. */
static gint
read_clamped_int (
        JsonObject  *obj,
        const gchar *member,
        gint         max)
{
    JsonNode *n;
    gint64    v;

    if (!json_object_has_member (obj, member))
        return 0;

    n = json_object_get_member (obj, member);
    if (n == NULL || !JSON_NODE_HOLDS_VALUE (n) ||
        json_node_get_value_type (n) != G_TYPE_INT64)
        return 0;

    v = json_node_get_int (n);
    if (v < 0)   return 0;
    if (v > max) return max;
    return (gint) v;
}

static GArray *
compile_schedule (const gchar *json_string)
{
    GArray     *out    = g_array_new (FALSE, FALSE, sizeof (CompiledInterval));
    JsonParser *parser = json_parser_new ();
    JsonNode   *root;
    JsonArray  *array;
    GError     *error  = NULL;
    guint       i, n;

    if (json_string == NULL || *json_string == '\0')
        json_string = "[]";

    if (!json_parser_load_from_data (parser, json_string, -1, &error))
    {
        g_warning ("pn-daily-timer: invalid schedule JSON: %s", error->message);
        g_error_free (error);
        g_object_unref (parser);
        return out;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_ARRAY (root))
    {
        g_object_unref (parser);
        return out;
    }

    array = json_node_get_array (root);
    n     = json_array_get_length (array);
    for (i = 0; i < n; i++)
    {
        JsonNode         *item = json_array_get_element (array, i);
        JsonObject       *obj;
        CompiledInterval  entry;
        gint              day;

        if (item == NULL || !JSON_NODE_HOLDS_OBJECT (item))
            continue;

        obj = json_node_get_object (item);

        /* A missing / unparseable day means "every day" rather than a
         * dropped row: the user asked for an interval, and firing it on
         * all seven days is the reading that keeps their intent visible
         * in the editor instead of silently discarding it. */
        day = json_object_has_member (obj, "day")
                  ? (gint) json_object_get_int_member (obj, "day")
                  : PN_DAILY_TIMER_EVERY_DAY;
        if (day < 1 || day > 7)
            day = PN_DAILY_TIMER_EVERY_DAY;

        entry.day     = day;
        entry.on_min  = read_clamped_int (obj, "on_hour",   23) * 60
                      + read_clamped_int (obj, "on_minute", 59);
        entry.off_min = read_clamped_int (obj, "off_hour",   23) * 60
                      + read_clamped_int (obj, "off_minute", 59);

        /* Equal on/off is degenerate -- a zero-length interval and a
         * full-day interval are equally defensible readings of it, so
         * neither is guessed at. */
        if (entry.on_min == entry.off_min)
            continue;

        g_array_append_val (out, entry);
    }

    g_object_unref (parser);
    return out;
}

/* ------------------------------------------------------------------ */
/*  Containment                                                        */
/*                                                                     */
/*  Half-open [on, off): the timer switches on during the "on" minute  */
/*  and off during the "off" minute, so back-to-back intervals         */
/*  (06:00-08:00, 08:00-10:00) neither overlap nor leave a one-minute  */
/*  gap where the relay drops out.                                     */
/*                                                                     */
/*  An interval with off <= on wraps through midnight.  It is then in  */
/*  force twice as seen from a single day: from its on time to the end */
/*  of its own day, and from the start of the *next* day to its off    */
/*  time.  Checking the previous day's rows for the tail is what lets  */
/*  a Friday 22:00-06:00 row still be on at 02:00 on Saturday.         */
/* ------------------------------------------------------------------ */

static gboolean
day_matches (const CompiledInterval *e, gint day_of_week)
{
    return e->day == PN_DAILY_TIMER_EVERY_DAY || e->day == day_of_week;
}

static gboolean
schedule_contains (
        GArray *compiled,
        gint    day_of_week,
        gint    minute_of_day)
{
    gint  prev_day = (day_of_week == 1) ? 7 : day_of_week - 1;
    guint i;

    if (compiled == NULL)
        return FALSE;

    for (i = 0; i < compiled->len; i++)
    {
        const CompiledInterval *e =
            &g_array_index (compiled, CompiledInterval, i);

        if (e->off_min > e->on_min)
        {
            if (day_matches (e, day_of_week) &&
                minute_of_day >= e->on_min && minute_of_day < e->off_min)
                return TRUE;
        }
        else
        {
            if (day_matches (e, day_of_week) && minute_of_day >= e->on_min)
                return TRUE;
            if (day_matches (e, prev_day) && minute_of_day < e->off_min)
                return TRUE;
        }
    }

    return FALSE;
}

gboolean
pn_daily_timer_state_at (
        PnDailyTimer *self,
        gint          day_of_week,
        gint          hour,
        gint          minute)
{
    gint     minute_of_day;
    gboolean on;

    g_return_val_if_fail (PN_IS_DAILY_TIMER (self), FALSE);
    g_return_val_if_fail (day_of_week >= 1 && day_of_week <= 7, FALSE);

    minute_of_day = hour * 60 + minute;
    if (minute_of_day < 0 || minute_of_day >= MINUTES_PER_DAY)
        return FALSE;

    g_mutex_lock (&self->lock);
    on = schedule_contains (self->compiled, day_of_week, minute_of_day);
    g_mutex_unlock (&self->lock);

    return on;
}

gboolean
pn_daily_timer_get_active (PnDailyTimer *self)
{
    gboolean active;

    g_return_val_if_fail (PN_IS_DAILY_TIMER (self), FALSE);

    g_mutex_lock (&self->lock);
    active = (self->last_state == 1);
    g_mutex_unlock (&self->lock);

    return active;
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger, once per  */
/*  :period second.  The state is recomputed from the wall clock every */
/*  tick rather than tracked incrementally, so a suspend/resume, an    */
/*  NTP step or a DST changeover lands the node on the state its       */
/*  schedule implies for the new time instead of on whatever the       */
/*  missed edges would have left behind.                               */
/* ------------------------------------------------------------------ */

static void
pn_daily_timer_trigger (PnAutoTrigger *trigger)
{
    PnDailyTimer *self = PN_DAILY_TIMER (trigger);
    GDateTime    *now  = g_date_time_new_now_local ();
    PnMessage    *msg;
    gboolean      on;
    gboolean      changed;

    g_mutex_lock (&self->lock);
    on = schedule_contains (self->compiled,
                            g_date_time_get_day_of_week (now),
                            g_date_time_get_hour   (now) * 60
                          + g_date_time_get_minute (now));

    /* last_state == -1 on the first poll: emit whatever we found.  That
     * is the startup announce -- the worksheet has just loaded and the
     * downstream relay has no idea which side of the schedule "now"
     * falls on. */
    changed          = (self->last_state != (gint) on);
    self->last_state = on ? 1 : 0;
    g_mutex_unlock (&self->lock);

    g_date_time_unref (now);

    if (!changed)
        return;

    msg = pn_message_new (PN_NODE (self), NULL);
    pn_message_set_double  (msg, "value",   on ? 1.0 : 0.0);
    pn_message_set_boolean (msg, "success", on);
    pn_message_set_string  (msg, "output",  on ? "on" : "off");

    pn_auto_trigger_emit_on_main (trigger, msg);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_daily_timer_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnDailyTimer *self = PN_DAILY_TIMER (object);

    switch (prop_id)
    {
    case PROP_SCHEDULE:
        g_mutex_lock (&self->lock);
        g_value_set_string (value,
                            self->schedule_json != NULL ? self->schedule_json
                                                        : "[]");
        g_mutex_unlock (&self->lock);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_daily_timer_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnDailyTimer *self = PN_DAILY_TIMER (object);

    switch (prop_id)
    {
    case PROP_SCHEDULE:
        {
            const gchar *new_str = g_value_get_string (value);
            gboolean     changed;

            if (new_str == NULL || *new_str == '\0')
                new_str = "[]";

            g_mutex_lock (&self->lock);
            changed = (g_strcmp0 (self->schedule_json, new_str) != 0);
            if (changed)
            {
                g_free (self->schedule_json);
                self->schedule_json = g_strdup (new_str);

                g_array_unref (self->compiled);
                self->compiled = compile_schedule (self->schedule_json);

                /* Force the next poll to announce: the edited schedule
                 * may put "now" on the other side of an interval, and
                 * an unchanged-looking state under the old schedule
                 * must not suppress that. */
                self->last_state = -1;
            }
            g_mutex_unlock (&self->lock);

            if (changed)
                g_object_notify_by_pspec (object, props[PROP_SCHEDULE]);
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_daily_timer_finalize (GObject *object)
{
    PnDailyTimer *self = PN_DAILY_TIMER (object);

    /* The worker thread that reads these was joined by
     * PnAutoTrigger.dispose before we got here. */
    g_clear_pointer (&self->compiled,      g_array_unref);
    g_clear_pointer (&self->schedule_json, g_free);
    g_mutex_clear   (&self->lock);

    G_OBJECT_CLASS (pn_daily_timer_parent_class)->finalize (object);
}

static void
pn_daily_timer_class_init (PnDailyTimerClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_daily_timer_get_property;
    object_class->set_property = pn_daily_timer_set_property;
    object_class->finalize     = pn_daily_timer_finalize;

    trigger_class->trigger = pn_daily_timer_trigger;

    /* build_class_tab installed by the gui tier (pn_daily_timer_gui_install). */

    node_class->class_name = "Daily Timer";
    node_class->icon       = PN_DAILY_TIMER_ICON;
    node_class->color      = (PnColor){ 0.90, 0.60, 0.30, 1.0 };
    node_class->category   = "Sources";
    node_class->has_input  = FALSE;
    node_class->has_output = TRUE;

    props[PROP_SCHEDULE] = g_param_spec_string (
            "schedule", "Schedule",
            "JSON array of {day, on_hour, on_minute, off_hour, off_minute} "
            "intervals.  \"day\" is 1 (Monday) to 7 (Sunday), or -1 for "
            "every day.  The node emits data.value = 1.0 on entering any "
            "interval and 0.0 on leaving the last one.  Defaults to one "
            "example interval so a freshly dropped node has something to "
            "read and edit; an empty array \"[]\" is always off.",
            PN_DAILY_TIMER_DEFAULT_SCHEDULE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_daily_timer_init (PnDailyTimer *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  orange = { 0.90, 0.60, 0.30, 1.0 };

    g_mutex_init (&self->lock);

    /* GObject never calls set_property for a non-construct property's
     * default, so the pspec default has to be mirrored here -- and
     * compiled, or a node that is never assigned a schedule would read
     * as having one while behaving as if it had none. */
    self->schedule_json = g_strdup (PN_DAILY_TIMER_DEFAULT_SCHEDULE);
    self->compiled      = compile_schedule (self->schedule_json);
    self->last_state    = -1;

    pn_node_set_class_name (node, "Daily Timer");
    pn_node_set_icon       (node, PN_DAILY_TIMER_ICON);
    pn_node_set_color      (node, &orange);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDailyTimer *
pn_daily_timer_new (void)
{
    return g_object_new (PN_TYPE_DAILY_TIMER, NULL);
}
