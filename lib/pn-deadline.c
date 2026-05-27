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

#include <stdio.h>      /* sscanf */
#include <json-glib/json-glib.h>

#include "pn-deadline.h"
#include "pn-message.h"
#include "pn-settings-schema.h"
#include "pn-tz-table.h"

struct _PnDeadline
{
    PnNode parent_instance;

    gchar *target;    /* "yyyy-mm-dd hh:MM:ss" wall-clock string */
    gchar *timezone;  /* combo label, e.g. "CET — Central European Time" */
};

G_DEFINE_TYPE (PnDeadline, pn_deadline, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_TARGET,
    PROP_TIMEZONE,
    N_PROPS,
};
static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Timezones                                                          */
/*                                                                     */
/*  The combo choices and the label→offset lookup come from the shared */
/*  pn-tz-table module (see pn-tz-table.h).  The Digital Clock node    */
/*  draws from the same list, so the two stay in sync.                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Read the incoming "now" instant from data.value.  Accepts a double
 * or an int64 (the Clock emits a double; both decode the same epoch). */
static gboolean
read_value (PnMessage *message, gdouble *out)
{
    JsonNode *node = pn_message_get_member (message, "value");
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt != G_TYPE_DOUBLE && vt != G_TYPE_INT64)
        return FALSE;

    *out = json_node_get_double (node);
    return TRUE;
}

/* Parse a "yyyy-mm-dd hh:MM:ss" wall-clock string interpreted at the
 * given fixed UTC offset and resolve it to a Unix epoch.  We build the
 * wall clock as if it were UTC and then subtract the offset, which
 * avoids g_time_zone_new_identifier() (GLib 2.68) and keeps us on the
 * 2.40 baseline.  Returns FALSE on a malformed string or an impossible
 * calendar date. */
static gboolean
parse_target_epoch (const gchar *target, gint off_min, gint64 *out)
{
    gint       y, mo, d, h, mi, s;
    GDateTime *utc;

    if (target == NULL)
        return FALSE;
    if (sscanf (target, "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
        return FALSE;

    utc = g_date_time_new_utc (y, mo, d, h, mi, (gdouble) s);
    if (utc == NULL)
        return FALSE;

    *out = g_date_time_to_unix (utc) - (gint64) off_min * 60;
    g_date_time_unref (utc);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_deadline_receive (PnNode *node, PnMessage *message)
{
    PnDeadline *self = PN_DEADLINE (node);
    gdouble     now_epoch;
    gint        off_min;
    gint64      target_epoch;

    if (!read_value (message, &now_epoch))
        return;   /* no numeric instant to measure against — stay silent */

    off_min = pn_tz_table_offset_minutes (self->timezone);
    if (!parse_target_epoch (self->target, off_min, &target_epoch))
        return;   /* unparseable target — stay silent */

    /* Seconds remaining: counts down to 0 at the target, then negative. */
    pn_message_set_double (message, "value",
                           (gdouble) (target_epoch - (gint64) now_epoch));
    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  GObject properties                                                 */
/* ------------------------------------------------------------------ */

static void
pn_deadline_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
    PnDeadline *self = PN_DEADLINE (object);

    switch (prop_id)
    {
    case PROP_TARGET:    g_value_set_string (value, self->target);   break;
    case PROP_TIMEZONE:  g_value_set_string (value, self->timezone); break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_deadline_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
    PnDeadline *self = PN_DEADLINE (object);

    switch (prop_id)
    {
    case PROP_TARGET:
        g_free (self->target);
        self->target = g_value_dup_string (value);
        g_object_notify_by_pspec (object, props[PROP_TARGET]);
        break;
    case PROP_TIMEZONE:
        g_free (self->timezone);
        self->timezone = g_value_dup_string (value);
        g_object_notify_by_pspec (object, props[PROP_TIMEZONE]);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_deadline_finalize (GObject *object)
{
    PnDeadline *self = PN_DEADLINE (object);

    g_free (self->target);
    g_free (self->timezone);

    G_OBJECT_CLASS (pn_deadline_parent_class)->finalize (object);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_deadline_class_init (PnDeadlineClass *klass)
{
    GObjectClass     *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass      *node_class   = PN_NODE_CLASS (klass);
    PnSettingsSchema *schema;

    object_class->get_property = pn_deadline_get_property;
    object_class->set_property = pn_deadline_set_property;
    object_class->finalize     = pn_deadline_finalize;
    node_class->receive        = pn_deadline_receive;

    node_class->class_name = "Deadline";
    node_class->icon       = "\xef\x89\x92";  /* fa-hourglass-half U+F252 */
    node_class->color      = (PnColor){ 0.40, 0.62, 0.85, 1.0 };
    node_class->category   = "Filters";
    node_class->has_input  = TRUE;
    node_class->has_output = TRUE;

    props[PROP_TARGET] = g_param_spec_string (
            "target", "Target",
            "Target instant as a \"yyyy-mm-dd hh:MM:ss\" wall-clock string",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TIMEZONE] = g_param_spec_string (
            "timezone", "Timezone",
            "Timezone the target is expressed in; empty means GMT",
            "GMT — Greenwich Mean Time",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Settings dialog: a free-text timestamp entry and a timezone combo
     * built from the shared fixed-offset table.  Declarative schema only,
     * so no -gui.so companion is needed. */
    schema = pn_settings_schema_new ();
    pn_settings_schema_row     (schema, "target",   PN_EDITOR_ENTRY);
    pn_settings_schema_row     (schema, "timezone", PN_EDITOR_COMBO);
    pn_settings_schema_choices (schema, "timezone", pn_tz_table_choices ());
    pn_node_class_set_settings_schema (node_class, schema);
}

static void
pn_deadline_init (PnDeadline *self)
{
    PnNode    *node = PN_NODE (self);
    PnColor    blue = { 0.40, 0.62, 0.85, 1.0 };
    GDateTime *now  = g_date_time_new_now_local ();

    /* Pre-fill the target with the current local date/time so the dialog
     * opens on a sensible, editable value. */
    self->target   = g_date_time_format (now, "%Y-%m-%d %H:%M:%S");
    self->timezone = g_strdup ("GMT — Greenwich Mean Time");
    g_date_time_unref (now);

    pn_node_set_class_name (node, "Deadline");
    pn_node_set_icon       (node, "\xef\x89\x92");  /* fa-hourglass-half */
    pn_node_set_color      (node, &blue);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnDeadline *
pn_deadline_new (void)
{
    return g_object_new (PN_TYPE_DEADLINE, NULL);
}
