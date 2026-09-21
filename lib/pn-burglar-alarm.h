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

#ifndef PN_BURGLAR_ALARM_H
#define PN_BURGLAR_ALARM_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnBurglarAlarm                                                     */
/*                                                                     */
/*  The panel of an intruder alarm: the box on the hall wall that      */
/*  watches four zones, counts the exit and entry delays, and decides  */
/*  when the siren goes off.  It is to a #PnKeypad what #PnCalcEngine  */
/*  is — the state machine behind the keys — only with five inputs and */
/*  five outputs instead of one each, because an alarm panel has more  */
/*  than one thing to say.                                             */
/*                                                                     */
/*  Inputs                                                             */
/*                                                                     */
/*    keypad    one keystroke per message, read from `data.key` —      */
/*              exactly what #PnKeypad's Decimal pad emits.  Digits    */
/*              build the code, "#" (or "=") submits it, "*" (or "C" / */
/*              "CE") clears what has been typed.  A message with no   */
/*              `data.key` but a `data.output` string is taken as a    */
/*              WHOLE code submitted at once, so an MQTT feed or a     */
/*              Text node can arm and disarm the panel remotely.       */
/*    delayed   the entry/exit route — the front door.  Tripping it    */
/*              while armed starts the entry delay instead of the      */
/*              siren, giving whoever came in time to type the code.   */
/*    zone 1-3  instant zones.  Tripping one while armed sounds the    */
/*              siren at once.                                         */
/*                                                                     */
/*  A zone message carrying a numeric `data.value` sets that zone's    */
/*  held state the usual way (> 0.5 is open, the boolean encoding      */
/*  every other node uses) — a door contact.  A message with no        */
/*  numeric value is a momentary trip that leaves the zone closed —    */
/*  a PIR that only ever fires on motion.  The zones are named by      */
/*  their input names, so renaming an input on the node's Inputs tab   */
/*  renames the zone everywhere it is announced ("Alarm! Kitchen").    */
/*                                                                     */
/*  Outputs                                                            */
/*                                                                     */
/*    alarm     the siren: `data.value` 1.0 while it sounds, 0.0 when  */
/*              it does not.  Wire it to an LED, a Tasmota relay, or   */
/*              anything that reads the boolean encoding.              */
/*    display   the panel's own readout as text in `data.output`, two  */
/*              to four newline-separated lines sized by               */
/*              #PnBurglarAlarm:display-lines — meant for a #PnMatrix57 */
/*              character LCD set to the same line count.              */
/*    armed     `data.value` 1.0 once the system is armed (including   */
/*              while it is in entry delay or in alarm), 0.0 while it  */
/*              is disarmed or still counting the exit delay.          */
/*    speech    one short spoken sentence per event in `data.output`,  */
/*              for a #PnTts — "System armed.", "Alarm! Front door."   */
/*    sms       one longer written sentence per *important* event, for */
/*              a Meshtastic node or any other text sink.  Alarms and  */
/*              their clearing always go out; arming and disarming do  */
/*              too only when #PnBurglarAlarm:sms-on-arming is set,    */
/*              because a radio message per arming is a lot of radio.  */
/*                                                                     */
/*  Every emitted message also carries `data.state` — "disarmed",      */
/*  "exit", "armed", "entry" or "alarm" — so a Value Router or Filter  */
/*  downstream can split the stream without parsing the text.          */
/*                                                                     */
/*  The state machine                                                  */
/*                                                                     */
/*    DISARMED  a valid code arms the panel, but only when no instant  */
/*              zone is held open; otherwise the readout says NOT      */
/*              READY and names what is open.  The delayed zone may be */
/*              open — that is the door being walked out of.           */
/*    EXIT      counting down to armed.  Zones are ignored; a valid    */
/*              code cancels.                                          */
/*    ARMED     the delayed zone starts the entry delay; any instant   */
/*              zone goes straight to alarm.                           */
/*    ENTRY     counting down to the siren.  A valid code disarms; an  */
/*              instant zone does not wait for the count.              */
/*    ALARM     the siren sounds for #PnBurglarAlarm:siren-time        */
/*              seconds and then cuts off, the way a bell box must by  */
/*              law; the panel stays in alarm, remembering what tripped */
/*              it, until a valid code disarms it.                     */
/*                                                                     */
/*  The state is *not* serialized: a reloaded worksheet comes up       */
/*  disarmed, which is the only safe assumption about a house nobody   */
/*  was watching.  On load the node announces that once — display,     */
/*  armed = 0 and alarm = 0 — so a wired LCD is not left blank and a   */
/*  wired siren is explicitly told to be quiet.  It stays silent on    */
/*  the speech and sms outputs, which would otherwise talk to the room */
/*  every time the worksheet is opened.                                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_BURGLAR_ALARM (pn_burglar_alarm_get_type ())

G_DECLARE_FINAL_TYPE (PnBurglarAlarm, pn_burglar_alarm,
                      PN, BURGLAR_ALARM, PnNode)

/* Input port indices.  Zone z (see below) arrives on input z + 1. */
#define PN_BURGLAR_ALARM_IN_KEYPAD   0
#define PN_BURGLAR_ALARM_IN_DELAYED  1
#define PN_BURGLAR_ALARM_IN_ZONE1    2
#define PN_BURGLAR_ALARM_IN_ZONE2    3
#define PN_BURGLAR_ALARM_IN_ZONE3    4
#define PN_BURGLAR_ALARM_N_INPUTS    5

/* Zone indices.  Zone 0 is the delayed one; 1..3 are instant. */
#define PN_BURGLAR_ALARM_ZONE_DELAYED 0
#define PN_BURGLAR_ALARM_N_ZONES      4

/* Output port indices. */
#define PN_BURGLAR_ALARM_OUT_ALARM    0
#define PN_BURGLAR_ALARM_OUT_DISPLAY  1
#define PN_BURGLAR_ALARM_OUT_ARMED    2
#define PN_BURGLAR_ALARM_OUT_SPEECH   3
#define PN_BURGLAR_ALARM_OUT_SMS      4
#define PN_BURGLAR_ALARM_N_OUTPUTS    5

/**
 * PnBurglarAlarmState:
 * @PN_BURGLAR_ALARM_DISARMED: off; zones are watched only to report
 *   whether the system is ready to arm
 * @PN_BURGLAR_ALARM_EXIT:     counting the exit delay down to armed
 * @PN_BURGLAR_ALARM_ARMED:    watching
 * @PN_BURGLAR_ALARM_ENTRY:    the delayed zone tripped; counting the
 *   entry delay down to the siren
 * @PN_BURGLAR_ALARM_ALARM:    tripped, and staying tripped until a
 *   valid code is typed (the siren itself may already have cut off)
 *
 * Which of the five states the panel is in.  Travels on every emitted
 * message as the `data.state` nick (see
 * pn_burglar_alarm_state_to_string()) and, as a number, as the
 * `data.value` of the speech and sms outputs.
 */
typedef enum
{
    PN_BURGLAR_ALARM_DISARMED = 0,
    PN_BURGLAR_ALARM_EXIT     = 1,
    PN_BURGLAR_ALARM_ARMED    = 2,
    PN_BURGLAR_ALARM_ENTRY    = 3,
    PN_BURGLAR_ALARM_ALARM    = 4,
} PnBurglarAlarmState;

PnBurglarAlarm *pn_burglar_alarm_new (void);

/**
 * pn_burglar_alarm_state_to_string:
 * @state: a panel state
 *
 * The state's machine-readable nick — "disarmed", "exit", "armed",
 * "entry" or "alarm" — as it travels in `data.state`.  Not localised:
 * a downstream Filter matching on it must not depend on the user's
 * locale.
 *
 * Returns: (transfer none): the nick, never %NULL.
 */
const gchar *pn_burglar_alarm_state_to_string (PnBurglarAlarmState state);

/* ------------------------------------------------------------------ */
/*  Logic seam (no messages, no timers)                                */
/*                                                                     */
/*  receive() is a thin wrapper that pulls a keystroke or a zone       */
/*  reading out of a message and calls one of these; the 1 Hz timer is */
/*  a thin wrapper that calls pn_burglar_alarm_tick().  Exposed so     */
/*  headless tests — and the D-Bus automation surface — can walk the   */
/*  panel through a whole burglary without building messages or        */
/*  waiting out a real delay.  Each of them emits exactly what the     */
/*  node would have emitted.                                           */
/* ------------------------------------------------------------------ */

/**
 * pn_burglar_alarm_press:
 * @self: the panel
 * @key:  a key code — "0".."9" to type, "#" or "=" to submit, "*",
 *        "C" or "CE" to clear, as #PnKeypad emits them
 *
 * Feeds one keystroke into the panel.
 *
 * Returns: %FALSE, changing and emitting nothing, when @key names no
 *   key the panel uses.
 */
gboolean pn_burglar_alarm_press (PnBurglarAlarm *self,
                                 const gchar    *key);

/**
 * pn_burglar_alarm_submit:
 * @self: the panel
 * @code: a whole code typed elsewhere
 *
 * Submits @code as if it had been typed key by key and followed by
 * "#".  What an MQTT feed or a Text node arriving on the keypad input
 * with only a `data.output` string does.
 *
 * Returns: whether @code was the right one.
 */
gboolean pn_burglar_alarm_submit (PnBurglarAlarm *self,
                                  const gchar    *code);

/**
 * pn_burglar_alarm_set_zone:
 * @self: the panel
 * @zone: 0 for the delayed zone, 1..3 for the instant ones
 * @open: whether the contact is now open
 *
 * Reports a *held* zone contact — a door or window switch.  Opening it
 * is a trip; closing it restores the zone and may make the system
 * ready to arm again.
 */
void pn_burglar_alarm_set_zone (PnBurglarAlarm *self,
                                gint            zone,
                                gboolean        open);

/**
 * pn_burglar_alarm_trip_zone:
 * @self: the panel
 * @zone: 0 for the delayed zone, 1..3 for the instant ones
 *
 * Reports a *momentary* trip — a PIR that fires on motion and has no
 * resting "open" state.  Trips the zone without leaving it held open,
 * so a movement in the hall does not keep the panel from arming.
 */
void pn_burglar_alarm_trip_zone (PnBurglarAlarm *self,
                                 gint            zone);

/**
 * pn_burglar_alarm_tick:
 * @self: the panel
 *
 * Advances every running count by one second: the exit delay, the
 * entry delay and the siren's cut-off timer.  Called once a second by
 * the node's own timer while anything is counting; a test calls it
 * directly and walks a thirty-second exit delay in thirty cheap steps.
 */
void pn_burglar_alarm_tick (PnBurglarAlarm *self);

/**
 * pn_burglar_alarm_reset:
 * @self: the panel
 *
 * Back to disarmed, zones closed, nothing typed — what the node comes
 * up as.  Silent: nothing is emitted.
 */
void pn_burglar_alarm_reset (PnBurglarAlarm *self);

/* ------------------------------------------------------------------ */
/*  Read seam                                                          */
/* ------------------------------------------------------------------ */

/**
 * pn_burglar_alarm_get_state:
 * @self: the panel
 *
 * Returns: which of the five states the panel is in.
 */
PnBurglarAlarmState pn_burglar_alarm_get_state (PnBurglarAlarm *self);

/**
 * pn_burglar_alarm_get_siren:
 * @self: the panel
 *
 * Whether the siren is sounding — what the alarm output last carried.
 * %FALSE in every state but alarm, and %FALSE in alarm too once the
 * siren's cut-off time has run out.
 *
 * Returns: whether the siren sounds.
 */
gboolean pn_burglar_alarm_get_siren (PnBurglarAlarm *self);

/**
 * pn_burglar_alarm_get_remaining:
 * @self: the panel
 *
 * Seconds left in the running exit or entry delay, 0 when neither is
 * running.  What the display output carries as `data.value`.
 *
 * Returns: the remaining seconds.
 */
guint pn_burglar_alarm_get_remaining (PnBurglarAlarm *self);

/**
 * pn_burglar_alarm_get_ready:
 * @self: the panel
 *
 * Whether the system could be armed right now — that is, whether every
 * *instant* zone is closed.  The delayed zone is allowed to be open:
 * that is the door being left through.
 *
 * Returns: whether arming would be accepted.
 */
gboolean pn_burglar_alarm_get_ready (PnBurglarAlarm *self);

/**
 * pn_burglar_alarm_get_zone_open:
 * @self: the panel
 * @zone: 0 for the delayed zone, 1..3 for the instant ones
 *
 * Returns: whether that zone's contact is held open.
 */
gboolean pn_burglar_alarm_get_zone_open (PnBurglarAlarm *self,
                                         gint            zone);

/**
 * pn_burglar_alarm_get_cause:
 * @self: the panel
 *
 * Name of the zone that started the entry delay or the alarm, or "" if
 * nothing has.  What the alarm output carries as `data.cause`.
 *
 * Returns: (transfer none): the zone name, never %NULL.
 */
const gchar *pn_burglar_alarm_get_cause (PnBurglarAlarm *self);

/**
 * pn_burglar_alarm_get_display_text:
 * @self: the panel
 *
 * Builds the readout as the display output would carry it:
 * #PnBurglarAlarm:display-lines newline-separated lines, top line
 * first.  A fresh string the caller owns.
 *
 * Returns: (transfer full): the readout.
 */
gchar *pn_burglar_alarm_get_display_text (PnBurglarAlarm *self);

G_END_DECLS

#endif /* PN_BURGLAR_ALARM_H */
