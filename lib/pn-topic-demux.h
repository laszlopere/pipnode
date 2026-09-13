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

#ifndef PN_TOPIC_DEMUX_H
#define PN_TOPIC_DEMUX_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTopicDemux                                                       */
/*                                                                     */
/*  One input, N outputs (the "outputs" property, 2..16), one topic    */
/*  pattern per output (the "topics" property, a JSON array of         */
/*  strings).  Every arriving message is compared against the patterns */
/*  of outputs 1..N in order and leaves, unchanged, by the FIRST       */
/*  output whose pattern matches its topic.  A message no pattern      */
/*  matches is dropped; an empty pattern matches nothing.              */
/*                                                                     */
/*  A pattern is one of:                                               */
/*    - a glob, when it contains '*' or '?' (g_pattern_match_simple,   */
/*      '*' crosses '/' like in the Filter node);                      */
/*    - an MQTT topic filter otherwise: '+' matches exactly one level, */
/*      a trailing '#' matches the rest (including none), anything     */
/*      else must match byte for byte.                                 */
/*                                                                     */
/*  First match wins, so a catch-all ("#" or "*") on the last output   */
/*  collects everything the earlier outputs did not claim.             */
/*                                                                     */
/*  "topics" may hold more entries than there are outputs: lowering    */
/*  the output count keeps the patterns of the removed outputs, so     */
/*  raising it again restores them.  Only the first "outputs" entries  */
/*  take part in routing.  Each output is labelled on the worksheet    */
/*  with (the tail of) its pattern.                                    */
/* ------------------------------------------------------------------ */

#define PN_TOPIC_DEMUX_MIN_OUTPUTS 2
#define PN_TOPIC_DEMUX_MAX_OUTPUTS 16
#define PN_TOPIC_DEMUX_DEF_OUTPUTS 4

#define PN_TYPE_TOPIC_DEMUX (pn_topic_demux_get_type ())

G_DECLARE_FINAL_TYPE (PnTopicDemux, pn_topic_demux,
                      PN, TOPIC_DEMUX, PnNode)

PnTopicDemux *pn_topic_demux_new (void);

/**
 * pn_topic_demux_get_topic:
 * @index: 0-based output index
 *
 * The pattern of output @index, "" when none is set.
 *
 * Returns: (transfer none): owned by @self, valid until the topics change.
 */
const gchar  *pn_topic_demux_get_topic (PnTopicDemux *self, gint index);

/**
 * pn_topic_demux_set_topic:
 * @index:   0-based output index, 0 .. PN_TOPIC_DEMUX_MAX_OUTPUTS-1
 * @pattern: (nullable): the pattern; %NULL or "" clears it
 *
 * Sets one output's pattern; notifies "topics".
 */
void          pn_topic_demux_set_topic (PnTopicDemux *self,
                                        gint          index,
                                        const gchar  *pattern);

/**
 * pn_topic_demux_topic_matches:
 * @pattern: (nullable): a pattern as described above
 * @topic:   (nullable): a message topic; %NULL is treated as ""
 *
 * Pure matching seam, for tests.  An empty or %NULL pattern never
 * matches.
 */
gboolean      pn_topic_demux_topic_matches (const gchar *pattern,
                                            const gchar *topic);

/**
 * pn_topic_demux_route:
 * @topic: (nullable): a message topic
 *
 * Index of the output a message with @topic would leave by, or -1 when
 * it would be dropped.
 */
gint          pn_topic_demux_route (PnTopicDemux *self, const gchar *topic);

G_END_DECLS

#endif /* PN_TOPIC_DEMUX_H */
