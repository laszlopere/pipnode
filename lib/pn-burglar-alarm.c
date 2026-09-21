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

#include "pn-burglar-alarm.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* fa-bell U+F0F3 — the bell box on the wall. */
#define PN_BURGLAR_ALARM_ICON "\xef\x83\xb3"

/* Typed-code buffer.  Twelve digits is already a longer code than any
 * panel asks for; the buffer has room for the terminator. */
#define PN_BURGLAR_ALARM_CODE_MAX   12
#define PN_BURGLAR_ALARM_ENTRY_SIZE (PN_BURGLAR_ALARM_CODE_MAX + 1)

/* Delay bounds, in seconds: up to ten minutes, which is far longer than
 * any real panel allows but costs nothing to permit. */
#define PN_BURGLAR_ALARM_DELAY_MAX   600u
#define PN_BURGLAR_ALARM_EXIT_DEF     30u
#define PN_BURGLAR_ALARM_ENTRY_DEF    30u

/* Siren cut-off, in seconds: up to an hour, zero meaning "until someone
 * disarms it".  Three minutes is the usual legal limit for a bell box. */
#define PN_BURGLAR_ALARM_SIREN_MAX  3600u
#define PN_BURGLAR_ALARM_SIREN_DEF   180u

#define PN_BURGLAR_ALARM_LINES_MIN 2
#define PN_BURGLAR_ALARM_LINES_MAX 4
#define PN_BURGLAR_ALARM_LINES_DEF 2

struct _PnBurglarAlarm
{
    PnNode parent_instance;

    /* Settings. */
    gchar   *code;
    guint    exit_delay;
    guint    entry_delay;
    guint    siren_time;
    gint     display_lines;
    gboolean sms_on_arming;

    /* Where the panel is.  Never serialized: a reloaded worksheet comes
     * up disarmed, the only safe assumption about a house nobody was
     * watching. */
    PnBurglarAlarmState state;

    /* Seconds left in the running exit or entry delay; 0 when neither
     * is running. */
    guint    remaining;

    /* The siren, and the seconds left before its cut-off.  @siren_left
     * is meaningless while siren-time is 0 (no cut-off at all). */
    gboolean siren;
    guint    siren_left;

    /* Held state of each zone's contact — a door switch left open, not
     * a PIR's momentary trip. */
    gboolean zone_open[PN_BURGLAR_ALARM_N_ZONES];

    /* Name of the zone that started the entry delay or the alarm, or
     * NULL when nothing has. */
    gchar   *cause;

    /* The code as it is being typed, and the one-shot complaint shown
     * in its place on the readout ("WRONG CODE", "NOT READY").  The
     * complaint is cleared by the next keystroke or state change, so it
     * reads like a panel's beep rather than a latched error. */
    gchar    entry[PN_BURGLAR_ALARM_ENTRY_SIZE];
    gchar   *flash;

    /* Last value put on the alarm and armed outputs, as a tri-state:
     * -1 means "nothing yet", so the startup announce always speaks. */
    gint     last_siren;
    gint     last_armed;

    /* The 1 Hz countdown timer, live only while something counts, and
     * the one-shot startup announce.  Both are plain main-loop sources
     * holding no reference on @self; dispose() pulls them. */
    guint    tick_id;
    guint    startup_emit_id;
};

G_DEFINE_TYPE (PnBurglarAlarm, pn_burglar_alarm, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_CODE,
    PROP_EXIT_DELAY,
    PROP_ENTRY_DELAY,
    PROP_SIREN_TIME,
    PROP_DISPLAY_LINES,
    PROP_SMS_ON_ARMING,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

static void sync_timer (PnBurglarAlarm *self);

/* ------------------------------------------------------------------ */
/*  Small helpers                                                      */
/* ------------------------------------------------------------------ */

const gchar *
pn_burglar_alarm_state_to_string (PnBurglarAlarmState state)
{
    switch (state)
    {
    case PN_BURGLAR_ALARM_DISARMED: return "disarmed";
    case PN_BURGLAR_ALARM_EXIT:     return "exit";
    case PN_BURGLAR_ALARM_ARMED:    return "armed";
    case PN_BURGLAR_ALARM_ENTRY:    return "entry";
    case PN_BURGLAR_ALARM_ALARM:    return "alarm";
    default:                        return "disarmed";
    }
}

/** The zone's display name — the name of the input it arrives on, so a
 *  rename on the node's Inputs tab renames the zone everywhere it is
 *  announced. */
static const gchar *
zone_name (PnBurglarAlarm *self, gint zone)
{
    return pn_node_get_input_name (PN_NODE (self), zone + 1);
}

/** Armed in the "the system is on" sense: entry delay and alarm count,
 *  the exit delay does not (nothing is being watched yet). */
static gboolean
is_armed (PnBurglarAlarm *self)
{
    return self->state == PN_BURGLAR_ALARM_ARMED ||
           self->state == PN_BURGLAR_ALARM_ENTRY ||
           self->state == PN_BURGLAR_ALARM_ALARM;
}

gboolean
pn_burglar_alarm_get_ready (PnBurglarAlarm *self)
{
    gint z;

    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self), FALSE);

    /* The delayed zone is deliberately not consulted: an open front
     * door is the door being left through. */
    for (z = 1; z < PN_BURGLAR_ALARM_N_ZONES; z++)
        if (self->zone_open[z])
            return FALSE;

    return TRUE;
}

/** Comma-separated names of the instant zones held open, or NULL when
 *  none are.  Used for the NOT READY complaint. */
static gchar *
open_zone_list (PnBurglarAlarm *self)
{
    GString *g = NULL;
    gint     z;

    for (z = 1; z < PN_BURGLAR_ALARM_N_ZONES; z++)
    {
        if (!self->zone_open[z])
            continue;
        if (g == NULL)
            g = g_string_new (NULL);
        else
            g_string_append (g, ", ");
        g_string_append (g, zone_name (self, z));
    }

    return g != NULL ? g_string_free (g, FALSE) : NULL;
}

static void
set_flash (PnBurglarAlarm *self, const gchar *text)
{
    g_free (self->flash);
    self->flash = g_strdup (text);
}

/* Store the zone name that tripped the panel.  @text may point into
 * @cause itself (the entry delay hands its own cause to the alarm), so
 * it is duplicated before the old one is freed; an empty name is stored
 * as "no cause" so the readout never shows a blank reason. */
static void
set_cause (PnBurglarAlarm *self, const gchar *text)
{
    gchar *dup = (text != NULL && *text != '\0') ? g_strdup (text) : NULL;

    g_free (self->cause);
    self->cause = dup;
}

const gchar *
pn_burglar_alarm_get_cause (PnBurglarAlarm *self)
{
    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self), "");
    return self->cause != NULL ? self->cause : "";
}

PnBurglarAlarmState
pn_burglar_alarm_get_state (PnBurglarAlarm *self)
{
    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self),
                          PN_BURGLAR_ALARM_DISARMED);
    return self->state;
}

gboolean
pn_burglar_alarm_get_siren (PnBurglarAlarm *self)
{
    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self), FALSE);
    return self->siren;
}

guint
pn_burglar_alarm_get_remaining (PnBurglarAlarm *self)
{
    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self), 0);
    return self->remaining;
}

gboolean
pn_burglar_alarm_get_zone_open (PnBurglarAlarm *self, gint zone)
{
    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self), FALSE);
    g_return_val_if_fail (zone >= 0 && zone < PN_BURGLAR_ALARM_N_ZONES,
                          FALSE);
    return self->zone_open[zone];
}

/* ------------------------------------------------------------------ */
/*  The readout                                                        */
/*                                                                     */
/*  Four lines, of which the first #display-lines are emitted:         */
/*                                                                     */
/*    1  the banner — the state, with the count when one is running    */
/*    2  the detail — why, or what to do next                          */
/*    3  the zone map — one character per zone, its marker when open   */
/*    4  the code being typed, masked                                  */
/*                                                                     */
/*  On a two-line panel there is no room for lines 3 and 4, so the     */
/*  masked code takes the detail line while it is being typed — which  */
/*  is what a two-line panel does.                                     */
/* ------------------------------------------------------------------ */

/* Markers painted on the zone map, one per zone, in zone order. */
static const gchar PN_BURGLAR_ALARM_ZONE_MARKS[PN_BURGLAR_ALARM_N_ZONES] =
    { 'D', '1', '2', '3' };

static gchar *
banner_line (PnBurglarAlarm *self)
{
    switch (self->state)
    {
    case PN_BURGLAR_ALARM_EXIT:
        return g_strdup_printf ("EXIT %u", self->remaining);
    case PN_BURGLAR_ALARM_ARMED:
        return g_strdup ("ARMED");
    case PN_BURGLAR_ALARM_ENTRY:
        return g_strdup_printf ("ENTRY %u", self->remaining);
    case PN_BURGLAR_ALARM_ALARM:
        return g_strdup ("*** ALARM ***");
    case PN_BURGLAR_ALARM_DISARMED:
    default:
        return g_strdup (pn_burglar_alarm_get_ready (self)
                         ? "READY" : "NOT READY");
    }
}

static gchar *
detail_line (PnBurglarAlarm *self)
{
    if (self->flash != NULL)
        return g_strdup (self->flash);

    switch (self->state)
    {
    case PN_BURGLAR_ALARM_EXIT:
        return g_strdup ("Leave now");
    case PN_BURGLAR_ALARM_ARMED:
        return g_strdup ("All secure");
    case PN_BURGLAR_ALARM_ENTRY:
        return g_strdup ("Enter code");
    case PN_BURGLAR_ALARM_ALARM:
        return g_strdup (self->cause != NULL ? self->cause : "Intruder");
    case PN_BURGLAR_ALARM_DISARMED:
    default:
    {
        gchar *open = open_zone_list (self);
        gchar *line;

        if (open == NULL)
            return g_strdup ("All zones closed");

        line = g_strconcat ("Open: ", open, NULL);
        g_free (open);
        return line;
    }
    }
}

/* "Zones ...." with an open zone showing its marker instead of a dot. */
static gchar *
zone_line (PnBurglarAlarm *self)
{
    gchar map[PN_BURGLAR_ALARM_N_ZONES + 1];
    gint  z;

    for (z = 0; z < PN_BURGLAR_ALARM_N_ZONES; z++)
        map[z] = self->zone_open[z] ? PN_BURGLAR_ALARM_ZONE_MARKS[z] : '.';
    map[PN_BURGLAR_ALARM_N_ZONES] = '\0';

    return g_strconcat ("Zones ", map, NULL);
}

/* "Code: ****", or "" when nothing is typed. */
static gchar *
code_line (PnBurglarAlarm *self)
{
    gsize  n = strlen (self->entry);
    gchar *stars;
    gchar *line;

    if (n == 0)
        return g_strdup ("");

    stars = g_strnfill (n, '*');
    line  = g_strconcat ("Code: ", stars, NULL);
    g_free (stars);
    return line;
}

gchar *
pn_burglar_alarm_get_display_text (PnBurglarAlarm *self)
{
    GString *out;
    gint     lines;

    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self), g_strdup (""));

    lines = CLAMP (self->display_lines,
                   PN_BURGLAR_ALARM_LINES_MIN, PN_BURGLAR_ALARM_LINES_MAX);
    out   = g_string_new (NULL);

    {
        gchar *s = banner_line (self);
        g_string_append (out, s);
        g_free (s);
    }

    {
        /* On a two-line panel the code being typed displaces the
         * detail; from three lines up it has a line of its own. */
        gchar *s = (lines == 2 && self->entry[0] != '\0')
                   ? code_line (self) : detail_line (self);
        g_string_append_c (out, '\n');
        g_string_append   (out, s);
        g_free (s);
    }

    if (lines >= 3)
    {
        gchar *s = zone_line (self);
        g_string_append_c (out, '\n');
        g_string_append   (out, s);
        g_free (s);
    }

    if (lines >= 4)
    {
        gchar *s = code_line (self);
        g_string_append_c (out, '\n');
        g_string_append   (out, s);
        g_free (s);
    }

    return g_string_free (out, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Emission                                                           */
/*                                                                     */
/*  Five outputs, each written only when it has something new to say:  */
/*  the two booleans on a change, the readout on every event, and the  */
/*  two text outputs only when an event has words worth saying.        */
/* ------------------------------------------------------------------ */

/** Stamp the members every output shares and send @out on @output. */
static void
emit_on (PnBurglarAlarm *self, gint output, PnMessage *out)
{
    pn_message_set_string  (out, "state",
                            pn_burglar_alarm_state_to_string (self->state));
    pn_message_set_boolean (out, "success", TRUE);

    pn_node_emit_message_on_output (PN_NODE (self), out, output);
    g_object_unref (out);
}

static void
emit_siren (PnBurglarAlarm *self)
{
    PnMessage *out;
    gchar     *text;

    if (self->last_siren == (self->siren ? 1 : 0))
        return;

    self->last_siren = self->siren ? 1 : 0;

    out  = pn_message_new (PN_NODE (self), NULL);
    text = self->siren && self->cause != NULL
           ? g_strconcat ("ALARM: ", self->cause, NULL)
           : g_strdup (self->siren ? "ALARM" : "OK");

    pn_message_set_double (out, "value", self->siren ? 1.0 : 0.0);
    pn_message_set_string (out, "output", text);
    pn_message_set_string (out, "cause",
                           self->cause != NULL ? self->cause : "");
    g_free (text);

    emit_on (self, PN_BURGLAR_ALARM_OUT_ALARM, out);
}

static void
emit_armed (PnBurglarAlarm *self)
{
    gboolean   armed = is_armed (self);
    PnMessage *out;

    if (self->last_armed == (armed ? 1 : 0))
        return;

    self->last_armed = armed ? 1 : 0;

    out = pn_message_new (PN_NODE (self), NULL);
    pn_message_set_double (out, "value",  armed ? 1.0 : 0.0);
    pn_message_set_string (out, "output", armed ? "ARMED" : "DISARMED");

    emit_on (self, PN_BURGLAR_ALARM_OUT_ARMED, out);
}

static void
emit_display (PnBurglarAlarm *self)
{
    PnMessage *out  = pn_message_new (PN_NODE (self), NULL);
    gchar     *text = pn_burglar_alarm_get_display_text (self);

    /* The countdown rides along as the number, so a Numeric or a
     * Countdown wired to the same output shows the seconds. */
    pn_message_set_double (out, "value",  (gdouble) self->remaining);
    pn_message_set_string (out, "output", text);
    g_free (text);

    emit_on (self, PN_BURGLAR_ALARM_OUT_DISPLAY, out);
}

/** One sentence on the speech or sms output.  @text is what gets said;
 *  the state's number rides in data.value so a Filter can split alarms
 *  from arming without reading the prose. */
static void
emit_text (PnBurglarAlarm *self, gint output, const gchar *text)
{
    PnMessage *out = pn_message_new (PN_NODE (self), NULL);

    pn_message_set_double (out, "value",  (gdouble) self->state);
    pn_message_set_string (out, "output", text);
    if (self->cause != NULL)
        pn_message_set_string (out, "cause", self->cause);

    emit_on (self, output, out);
}

static void
emit_speech (PnBurglarAlarm *self, const gchar *text)
{
    emit_text (self, PN_BURGLAR_ALARM_OUT_SPEECH, text);
}

/** The sms output, gated on #PnBurglarAlarm:sms-on-arming for the
 *  everyday arming traffic.  @important marks the messages that always
 *  go out — an alarm, and its clearing. */
static void
emit_sms (PnBurglarAlarm *self, gboolean important, const gchar *text)
{
    if (!important && !self->sms_on_arming)
        return;

    emit_text (self, PN_BURGLAR_ALARM_OUT_SMS, text);
}

/* ------------------------------------------------------------------ */
/*  The state machine                                                  */
/* ------------------------------------------------------------------ */

static void enter_alarm (PnBurglarAlarm *self, const gchar *cause);
static void enter_entry (PnBurglarAlarm *self, gint zone);

void
pn_burglar_alarm_reset (PnBurglarAlarm *self)
{
    gint z;

    g_return_if_fail (PN_IS_BURGLAR_ALARM (self));

    self->state      = PN_BURGLAR_ALARM_DISARMED;
    self->remaining  = 0;
    self->siren      = FALSE;
    self->siren_left = 0;
    self->entry[0]   = '\0';

    for (z = 0; z < PN_BURGLAR_ALARM_N_ZONES; z++)
        self->zone_open[z] = FALSE;

    set_cause (self, NULL);
    set_flash (self, NULL);

    pn_node_set_has_error (PN_NODE (self), FALSE);
}

/** Now armed and watching.  Any zone that was left open while the exit
 *  delay ran is re-read here, at the moment it starts to matter. */
static void
enter_armed (PnBurglarAlarm *self)
{
    gint z;

    self->state     = PN_BURGLAR_ALARM_ARMED;
    self->remaining = 0;
    set_flash (self, NULL);

    emit_armed   (self);
    emit_display (self);
    emit_speech  (self, "System armed.");
    emit_sms     (self, FALSE, "Burglar alarm: system armed.");

    /* An instant zone opened during the exit delay wins over the front
     * door still standing open: it is the more serious of the two. */
    for (z = 1; z < PN_BURGLAR_ALARM_N_ZONES; z++)
        if (self->zone_open[z])
        {
            enter_alarm (self, zone_name (self, z));
            return;
        }

    if (self->zone_open[PN_BURGLAR_ALARM_ZONE_DELAYED])
        enter_entry (self, PN_BURGLAR_ALARM_ZONE_DELAYED);
}

/** Start the exit delay, or arm at once when there is none. */
static void
enter_exit (PnBurglarAlarm *self)
{
    self->state     = PN_BURGLAR_ALARM_EXIT;
    self->remaining = self->exit_delay;
    set_cause (self, NULL);
    set_flash (self, NULL);

    if (self->remaining == 0)
    {
        enter_armed (self);
        return;
    }

    emit_display (self);
    {
        gchar *say = g_strdup_printf ("Exit now. %u seconds to leave.",
                                      self->remaining);
        emit_speech (self, say);
        g_free (say);
    }
}

/** The delayed zone tripped while armed: count down to the siren. */
static void
enter_entry (PnBurglarAlarm *self, gint zone)
{
    set_cause (self, zone_name (self, zone));

    if (self->entry_delay == 0)
    {
        enter_alarm (self, self->cause);
        return;
    }

    self->state     = PN_BURGLAR_ALARM_ENTRY;
    self->remaining = self->entry_delay;
    set_flash (self, NULL);

    emit_display (self);
    emit_speech  (self, "Entry delay. Enter your code.");
}

/** Sound the siren, and remember what tripped it.  Re-entered when a
 *  second zone goes while the panel is already in alarm: the cause is
 *  updated and the siren restarts, but nobody is texted twice. */
static void
enter_alarm (PnBurglarAlarm *self, const gchar *cause)
{
    gboolean already = (self->state == PN_BURGLAR_ALARM_ALARM);

    set_cause (self, cause);
    set_flash (self, NULL);

    self->state      = PN_BURGLAR_ALARM_ALARM;
    self->remaining  = 0;
    self->siren      = TRUE;
    self->siren_left = self->siren_time;

    /* Paint the node red on the worksheet for as long as the panel is
     * tripped: the generic node error state says "this one wants you",
     * which is exactly what an alarm is. */
    pn_node_set_has_error (PN_NODE (self), TRUE);

    /* The alarm output carries the cause, so a fresh zone tripping
     * during an alarm is worth re-emitting even when the siren was
     * already sounding. */
    self->last_siren = -1;
    emit_siren   (self);
    emit_armed   (self);
    emit_display (self);

    if (already)
        return;

    {
        gchar *say = g_strdup_printf ("Alarm! %s.", pn_burglar_alarm_get_cause (self));
        gchar *sms = g_strdup_printf ("Burglar alarm: intruder on %s.",
                                      pn_burglar_alarm_get_cause (self));

        emit_speech (self, say);
        emit_sms    (self, TRUE, sms);
        g_free (say);
        g_free (sms);
    }
}

/** A valid code was typed: back to disarmed from wherever we were. */
static void
enter_disarmed (PnBurglarAlarm *self)
{
    gboolean  was_alarm = (self->state == PN_BURGLAR_ALARM_ALARM);
    gchar    *sms       = NULL;

    if (was_alarm)
        sms = g_strdup_printf ("Burglar alarm: cleared, %s.",
                               pn_burglar_alarm_get_cause (self));

    self->state      = PN_BURGLAR_ALARM_DISARMED;
    self->remaining  = 0;
    self->siren      = FALSE;
    self->siren_left = 0;
    set_cause (self, NULL);
    set_flash (self, NULL);
    pn_node_set_has_error (PN_NODE (self), FALSE);

    emit_siren   (self);
    emit_armed   (self);
    emit_display (self);
    emit_speech  (self, "System disarmed.");

    if (was_alarm)
        emit_sms (self, TRUE, sms);
    else
        emit_sms (self, FALSE, "Burglar alarm: system disarmed.");

    g_free (sms);
}

/** Arming refused because an instant zone is held open. */
static void
refuse_arming (PnBurglarAlarm *self)
{
    gchar *open = open_zone_list (self);
    gchar *say;

    set_flash (self, "NOT READY");
    emit_display (self);

    say = open != NULL
          ? g_strdup_printf ("Not ready. %s is open.", open)
          : g_strdup ("Not ready.");
    emit_speech (self, say);

    g_free (say);
    g_free (open);
}

/* ------------------------------------------------------------------ */
/*  Keypad                                                             */
/* ------------------------------------------------------------------ */

/** Whether @typed is the panel's code.  An empty configured code means
 *  the panel has no code at all: bare "#" then arms and disarms it. */
static gboolean
code_matches (PnBurglarAlarm *self, const gchar *typed)
{
    if (self->code == NULL || self->code[0] == '\0')
        return TRUE;

    return g_strcmp0 (self->code, typed) == 0;
}

/** A code was submitted, by "#" or as a whole string.  Returns whether
 *  it was the right one. */
static gboolean
submit_code (PnBurglarAlarm *self, const gchar *typed)
{
    gboolean ok = code_matches (self, typed);

    self->entry[0] = '\0';

    if (!ok)
    {
        set_flash (self, "WRONG CODE");
        emit_display (self);
        emit_speech  (self, "Wrong code.");
        return FALSE;
    }

    if (self->state == PN_BURGLAR_ALARM_DISARMED)
    {
        if (pn_burglar_alarm_get_ready (self))
            enter_exit (self);
        else
            refuse_arming (self);
    }
    else
    {
        enter_disarmed (self);
    }

    return TRUE;
}

gboolean
pn_burglar_alarm_submit (PnBurglarAlarm *self, const gchar *code)
{
    gboolean ok;

    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self), FALSE);
    g_return_val_if_fail (code != NULL, FALSE);

    ok = submit_code (self, code);
    sync_timer (self);
    return ok;
}

gboolean
pn_burglar_alarm_press (PnBurglarAlarm *self, const gchar *key)
{
    gsize len;

    g_return_val_if_fail (PN_IS_BURGLAR_ALARM (self), FALSE);
    g_return_val_if_fail (key != NULL, FALSE);

    /* A complaint lives until the next keystroke — the panel's beep,
     * not a latched error. */
    set_flash (self, NULL);

    if (key[0] != '\0' && key[1] == '\0' && g_ascii_isdigit (key[0]))
    {
        len = strlen (self->entry);

        /* A code longer than the buffer is a mis-key, not a code: drop
         * the digit rather than silently truncating what is compared. */
        if (len + 1 < PN_BURGLAR_ALARM_ENTRY_SIZE)
        {
            self->entry[len]     = key[0];
            self->entry[len + 1] = '\0';
        }

        emit_display (self);
        sync_timer (self);
        return TRUE;
    }

    /* "#" is the decimal pad's enter key; "=" is the calculator pad's,
     * so either keypad layout drives the panel. */
    if (g_strcmp0 (key, "#") == 0 || g_strcmp0 (key, "=") == 0)
    {
        submit_code (self, self->entry);
        sync_timer (self);
        return TRUE;
    }

    /* Likewise "*" on the decimal pad and "C"/"CE" on the calculator
     * one: rub out what has been typed, change nothing else. */
    if (g_strcmp0 (key, "*")  == 0 || g_strcmp0 (key, "C") == 0 ||
        g_strcmp0 (key, "CE") == 0)
    {
        self->entry[0] = '\0';
        emit_display (self);
        sync_timer (self);
        return TRUE;
    }

    /* Not a key this panel uses. */
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Zones                                                              */
/* ------------------------------------------------------------------ */

/** A zone went active.  What that means depends entirely on where the
 *  panel is: nothing at all while it is disarmed or letting the user
 *  out, the entry delay for the front door, the siren for the rest. */
static void
zone_tripped (PnBurglarAlarm *self, gint zone)
{
    switch (self->state)
    {
    case PN_BURGLAR_ALARM_ARMED:
        if (zone == PN_BURGLAR_ALARM_ZONE_DELAYED)
            enter_entry (self, zone);
        else
            enter_alarm (self, zone_name (self, zone));
        break;

    case PN_BURGLAR_ALARM_ENTRY:
        /* The front door tripping again is the same person still coming
         * in; an inner zone means they did not stop at the keypad. */
        if (zone != PN_BURGLAR_ALARM_ZONE_DELAYED)
            enter_alarm (self, zone_name (self, zone));
        else
            emit_display (self);
        break;

    case PN_BURGLAR_ALARM_ALARM:
        /* Already ringing: remember the newest zone and restart the
         * siren, in case its cut-off had already run out. */
        enter_alarm (self, zone_name (self, zone));
        break;

    case PN_BURGLAR_ALARM_DISARMED:
    case PN_BURGLAR_ALARM_EXIT:
    default:
        /* Nothing is being watched.  The readout still tracks what is
         * held open — that is how the user learns why arming was
         * refused — but a momentary trip changes nothing visible and a
         * PIR firing every few seconds should not flood the wire. */
        if (self->zone_open[zone])
            emit_display (self);
        break;
    }
}

void
pn_burglar_alarm_set_zone (PnBurglarAlarm *self, gint zone, gboolean open)
{
    g_return_if_fail (PN_IS_BURGLAR_ALARM (self));
    g_return_if_fail (zone >= 0 && zone < PN_BURGLAR_ALARM_N_ZONES);

    if (self->zone_open[zone] == open)
    {
        /* A contact repeating itself is not news; a repeat while the
         * panel is armed still is, because it may be a door being
         * worked at.  Keep it simple: nothing changed, say nothing. */
        return;
    }

    self->zone_open[zone] = open;

    if (open)
        zone_tripped (self, zone);
    else
        emit_display (self);      /* the zone map and READY change */

    sync_timer (self);
}

void
pn_burglar_alarm_trip_zone (PnBurglarAlarm *self, gint zone)
{
    g_return_if_fail (PN_IS_BURGLAR_ALARM (self));
    g_return_if_fail (zone >= 0 && zone < PN_BURGLAR_ALARM_N_ZONES);

    /* Momentary: the zone does not stay open, so it never keeps the
     * panel from arming. */
    zone_tripped (self, zone);
    sync_timer (self);
}

/* ------------------------------------------------------------------ */
/*  The clock                                                          */
/* ------------------------------------------------------------------ */

void
pn_burglar_alarm_tick (PnBurglarAlarm *self)
{
    g_return_if_fail (PN_IS_BURGLAR_ALARM (self));

    switch (self->state)
    {
    case PN_BURGLAR_ALARM_EXIT:
        if (self->remaining > 0)
            self->remaining--;
        if (self->remaining == 0)
            enter_armed (self);
        else
            emit_display (self);
        break;

    case PN_BURGLAR_ALARM_ENTRY:
        if (self->remaining > 0)
            self->remaining--;
        if (self->remaining == 0)
            enter_alarm (self, pn_burglar_alarm_get_cause (self));
        else
            emit_display (self);
        break;

    case PN_BURGLAR_ALARM_ALARM:
        /* siren-time 0 means no cut-off: ring until disarmed. */
        if (!self->siren || self->siren_time == 0)
            break;
        if (self->siren_left > 0)
            self->siren_left--;
        if (self->siren_left == 0)
        {
            /* The bell box cuts off, but the panel stays in alarm and
             * keeps showing what tripped it until someone disarms. */
            self->siren = FALSE;
            emit_siren   (self);
            emit_display (self);
        }
        break;

    case PN_BURGLAR_ALARM_DISARMED:
    case PN_BURGLAR_ALARM_ARMED:
    default:
        break;
    }
}

/** Whether anything is still counting and so wants the 1 Hz timer. */
static gboolean
needs_tick (PnBurglarAlarm *self)
{
    if (self->state == PN_BURGLAR_ALARM_EXIT ||
        self->state == PN_BURGLAR_ALARM_ENTRY)
        return TRUE;

    return self->state == PN_BURGLAR_ALARM_ALARM &&
           self->siren && self->siren_time > 0;
}

static gboolean
on_tick (gpointer data)
{
    PnBurglarAlarm *self = PN_BURGLAR_ALARM (data);

    pn_burglar_alarm_tick (self);

    /* tick() deliberately does not touch the timer — a headless test
     * drives it directly and must not arm main-loop sources — so the
     * source retires itself here when the count has run out. */
    if (!needs_tick (self))
    {
        self->tick_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

/** Start or stop the 1 Hz timer to match what the panel is doing.
 *  Called from the message-driven entry points only. */
static void
sync_timer (PnBurglarAlarm *self)
{
    gboolean want = needs_tick (self);

    if (want && self->tick_id == 0)
        self->tick_id = g_timeout_add_seconds (1, on_tick, self);
    else if (!want && self->tick_id != 0)
    {
        g_source_remove (self->tick_id);
        self->tick_id = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** A JSON string member, or NULL when it is missing or another type. */
static const gchar *
read_string (PnMessage *message, const gchar *name)
{
    JsonNode *node = pn_message_get_member (message, name);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return NULL;
    if (json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string (node);
}

/** A numeric "value" member.  Returns %FALSE — leaving @out alone —
 *  when the message carries none, which is what tells a momentary trip
 *  from a held contact. */
static gboolean
read_value (PnMessage *message, gdouble *out)
{
    JsonNode *node = pn_message_get_member (message, "value");
    GType     vt;

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return FALSE;

    vt = json_node_get_value_type (node);
    if (vt == G_TYPE_DOUBLE)
        *out = json_node_get_double (node);
    else if (vt == G_TYPE_INT64)
        *out = (gdouble) json_node_get_int (node);
    else
        return FALSE;

    return TRUE;
}

static void
pn_burglar_alarm_receive (PnNode *node, PnMessage *message)
{
    PnBurglarAlarm *self  = PN_BURGLAR_ALARM (node);
    gint            input = pn_node_current_input ();
    gdouble         value;

    if (input == PN_BURGLAR_ALARM_IN_KEYPAD)
    {
        const gchar *key = read_string (message, "key");

        if (key != NULL)
        {
            pn_burglar_alarm_press (self, key);
            return;
        }

        /* No keystroke, but a string: a whole code submitted at once by
         * an MQTT feed or a Text node.  Anything else on this input is
         * not a key press and is ignored in silence, so a stray reading
         * cannot rub out a half-typed code. */
        key = read_string (message, "output");
        if (key != NULL)
            pn_burglar_alarm_submit (self, key);

        return;
    }

    if (input < PN_BURGLAR_ALARM_IN_DELAYED ||
        input > PN_BURGLAR_ALARM_IN_ZONE3)
        return;

    if (read_value (message, &value))
        pn_burglar_alarm_set_zone (self, input - 1, value > 0.5);
    else
        pn_burglar_alarm_trip_zone (self, input - 1);
}

/* ------------------------------------------------------------------ */
/*  Startup announce                                                   */
/*                                                                     */
/*  A panel that has just been loaded is disarmed, but nothing         */
/*  downstream knows that: the LCD would stay blank and the siren      */
/*  would never be told to be quiet.  Announce the resting state once, */
/*  on the three outputs that carry state rather than words — the      */
/*  speech and sms outputs stay silent, or opening a worksheet would   */
/*  talk to the room.                                                  */
/* ------------------------------------------------------------------ */

static gboolean
emit_startup_state (gpointer data)
{
    PnBurglarAlarm *self = PN_BURGLAR_ALARM (data);

    self->startup_emit_id = 0;

    emit_siren   (self);
    emit_armed   (self);
    emit_display (self);

    return G_SOURCE_REMOVE;
}

/* ------------------------------------------------------------------ */
/*  GObject boilerplate                                                */
/* ------------------------------------------------------------------ */

static void
pn_burglar_alarm_constructed (GObject *object)
{
    PnBurglarAlarm *self = PN_BURGLAR_ALARM (object);

    G_OBJECT_CLASS (pn_burglar_alarm_parent_class)->constructed (object);

    /* Deferred to an idle for the reason every startup announce is: at
     * construction time the load path has built this node but not yet
     * attached its wires, so an emit now would reach nobody. */
    self->startup_emit_id = g_idle_add (emit_startup_state, self);
}

static void
pn_burglar_alarm_dispose (GObject *object)
{
    PnBurglarAlarm *self = PN_BURGLAR_ALARM (object);

    if (self->startup_emit_id != 0)
    {
        g_source_remove (self->startup_emit_id);
        self->startup_emit_id = 0;
    }
    if (self->tick_id != 0)
    {
        g_source_remove (self->tick_id);
        self->tick_id = 0;
    }

    g_clear_pointer (&self->code,  g_free);
    g_clear_pointer (&self->cause, g_free);
    g_clear_pointer (&self->flash, g_free);

    G_OBJECT_CLASS (pn_burglar_alarm_parent_class)->dispose (object);
}

static void
pn_burglar_alarm_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnBurglarAlarm *self = PN_BURGLAR_ALARM (object);

    switch (prop_id)
    {
    case PROP_CODE:
        g_value_set_string (value, self->code);
        break;
    case PROP_EXIT_DELAY:
        g_value_set_uint (value, self->exit_delay);
        break;
    case PROP_ENTRY_DELAY:
        g_value_set_uint (value, self->entry_delay);
        break;
    case PROP_SIREN_TIME:
        g_value_set_uint (value, self->siren_time);
        break;
    case PROP_DISPLAY_LINES:
        g_value_set_int (value, self->display_lines);
        break;
    case PROP_SMS_ON_ARMING:
        g_value_set_boolean (value, self->sms_on_arming);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_burglar_alarm_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnBurglarAlarm *self = PN_BURGLAR_ALARM (object);

    switch (prop_id)
    {
    case PROP_CODE:
        g_free (self->code);
        self->code = g_value_dup_string (value);
        break;
    case PROP_EXIT_DELAY:
        self->exit_delay = g_value_get_uint (value);
        break;
    case PROP_ENTRY_DELAY:
        self->entry_delay = g_value_get_uint (value);
        break;
    case PROP_SIREN_TIME:
        self->siren_time = g_value_get_uint (value);
        break;
    case PROP_DISPLAY_LINES:
        self->display_lines = g_value_get_int (value);
        break;
    case PROP_SMS_ON_ARMING:
        self->sms_on_arming = g_value_get_boolean (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_burglar_alarm_class_init (PnBurglarAlarmClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->constructed  = pn_burglar_alarm_constructed;
    object_class->dispose      = pn_burglar_alarm_dispose;
    object_class->get_property = pn_burglar_alarm_get_property;
    object_class->set_property = pn_burglar_alarm_set_property;

    node_class->receive = pn_burglar_alarm_receive;

    node_class->palette_icon = PN_BURGLAR_ALARM_ICON;
    node_class->class_name   = "Burglar Alarm";
    node_class->icon         = PN_BURGLAR_ALARM_ICON;
    /* Bell-box amber: loud enough to find on a worksheet, and not the
     * red the worksheet paints a node that has errored. */
    node_class->color        = (PnColor){ 0.85, 0.55, 0.15, 1.0 };
    node_class->category     = "Controllers";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_CODE] = g_param_spec_string (
            "code", "Code",
            "The code that arms and disarms the panel, typed on the "
            "keypad and submitted with \"#\".  Leave it empty for a "
            "panel with no code at all, where a bare \"#\" arms and "
            "disarms.  Stored in the worksheet as plain text — this is "
            "a demonstration panel, not a certified one.",
            "1234",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_EXIT_DELAY] = g_param_spec_uint (
            "exit-delay", "Exit delay",
            "Seconds between a valid code being accepted and the system "
            "actually arming — the time to walk out of the door.  Zero "
            "arms immediately.",
            0u, PN_BURGLAR_ALARM_DELAY_MAX, PN_BURGLAR_ALARM_EXIT_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ENTRY_DELAY] = g_param_spec_uint (
            "entry-delay", "Entry delay",
            "Seconds between the delayed zone tripping and the siren — "
            "the time to reach the keypad and type the code.  Zero "
            "makes the delayed zone behave like an instant one.",
            0u, PN_BURGLAR_ALARM_DELAY_MAX, PN_BURGLAR_ALARM_ENTRY_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SIREN_TIME] = g_param_spec_uint (
            "siren-time", "Siren time",
            "Seconds the siren sounds before it cuts off, the way a bell "
            "box must.  The panel stays in alarm, remembering what "
            "tripped it, until a valid code is typed.  Zero means no "
            "cut-off: it rings until disarmed.",
            0u, PN_BURGLAR_ALARM_SIREN_MAX, PN_BURGLAR_ALARM_SIREN_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DISPLAY_LINES] = g_param_spec_int (
            "display-lines", "Display lines",
            "How many lines the display output carries — set it to the "
            "line count of the character LCD it feeds.  Two gives the "
            "state and one line of detail; three adds the zone map; four "
            "adds the code being typed on a line of its own.",
            PN_BURGLAR_ALARM_LINES_MIN, PN_BURGLAR_ALARM_LINES_MAX,
            PN_BURGLAR_ALARM_LINES_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SMS_ON_ARMING] = g_param_spec_boolean (
            "sms-on-arming", "SMS on arming",
            "Also send an sms message every time the system is armed or "
            "disarmed.  Off by default: alarms and their clearing always "
            "go out, but a radio message per arming is a lot of radio.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_burglar_alarm_init (PnBurglarAlarm *self)
{
    PnNode *node = PN_NODE (self);

    self->code          = g_strdup ("1234");
    self->exit_delay    = PN_BURGLAR_ALARM_EXIT_DEF;
    self->entry_delay   = PN_BURGLAR_ALARM_ENTRY_DEF;
    self->siren_time    = PN_BURGLAR_ALARM_SIREN_DEF;
    self->display_lines = PN_BURGLAR_ALARM_LINES_DEF;
    self->last_siren    = -1;
    self->last_armed    = -1;

    pn_burglar_alarm_reset (self);

    pn_node_set_class_name (node, "Burglar Alarm");
    pn_node_set_icon       (node, PN_BURGLAR_ALARM_ICON);

    pn_node_set_n_inputs   (node, PN_BURGLAR_ALARM_N_INPUTS);
    pn_node_set_input_name (node, PN_BURGLAR_ALARM_IN_KEYPAD,  "keypad");
    pn_node_set_input_name (node, PN_BURGLAR_ALARM_IN_DELAYED, "delayed");
    pn_node_set_input_name (node, PN_BURGLAR_ALARM_IN_ZONE1,   "zone 1");
    pn_node_set_input_name (node, PN_BURGLAR_ALARM_IN_ZONE2,   "zone 2");
    pn_node_set_input_name (node, PN_BURGLAR_ALARM_IN_ZONE3,   "zone 3");

    pn_node_set_n_outputs   (node, PN_BURGLAR_ALARM_N_OUTPUTS);
    pn_node_set_output_name (node, PN_BURGLAR_ALARM_OUT_ALARM,   "alarm");
    pn_node_set_output_name (node, PN_BURGLAR_ALARM_OUT_DISPLAY, "display");
    pn_node_set_output_name (node, PN_BURGLAR_ALARM_OUT_ARMED,   "armed");
    pn_node_set_output_name (node, PN_BURGLAR_ALARM_OUT_SPEECH,  "speech");
    pn_node_set_output_name (node, PN_BURGLAR_ALARM_OUT_SMS,     "sms");

    {
        PnColor amber = { 0.85, 0.55, 0.15, 1.0 };
        pn_node_set_color (node, &amber);
    }
}

PnBurglarAlarm *
pn_burglar_alarm_new (void)
{
    return g_object_new (PN_TYPE_BURGLAR_ALARM, NULL);
}
