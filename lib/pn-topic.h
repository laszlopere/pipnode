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

#ifndef PN_TOPIC_H
#define PN_TOPIC_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnTopic                                                            */
/*                                                                     */
/*  Filter that rewrites a message's topic to this node's own topic    */
/*  template, then forwards the message untouched otherwise.  It adds  */
/*  no property of its own: the topic is the user-configurable "topic" */
/*  field every #PnNode already carries (edited in the node's settings */
/*  dialog), with the usual ${nodeclass} / ${nodename} / ${hostname}   */
/*  placeholders resolved at emit time.                                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_TOPIC (pn_topic_get_type ())

G_DECLARE_FINAL_TYPE (PnTopic, pn_topic, PN, TOPIC, PnNode)

PnTopic *pn_topic_new (void);

G_END_DECLS

#endif /* PN_TOPIC_H */
