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

#ifndef PN_NOTIFY_H
#define PN_NOTIFY_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnNotifyUrgency                                                    */
/*                                                                     */
/*  Mirrors the freedesktop notification urgency hint (BYTE 0/1/2)     */
/*  that the org.freedesktop.Notifications spec defines.  The values   */
/*  match the wire-format integers so the receive path can pass them   */
/*  straight to the hint dictionary without translation.               */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NOTIFY_URGENCY (pn_notify_urgency_get_type ())

typedef enum
{
    PN_NOTIFY_URGENCY_LOW      = 0,
    PN_NOTIFY_URGENCY_NORMAL   = 1,
    PN_NOTIFY_URGENCY_CRITICAL = 2,
} PnNotifyUrgency;

GType pn_notify_urgency_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnNotify                                                           */
/*                                                                     */
/*  Sink node that shows a desktop notification whenever a message     */
/*  arrives, via the org.freedesktop.Notifications D-Bus service on    */
/*  the session bus (the same protocol libnotify / notify-send /       */
/*  GNotification all use).  Summary and body are template strings     */
/*  with ${path/to/field} placeholders resolved against the incoming   */
/*  message the same way #PnFormat does.                               */
/*                                                                     */
/*  Subsequent notifications from the same node replace the previous   */
/*  one in place when #PnNotify:replace is %TRUE (the default), so a   */
/*  fast-firing source does not stack up dozens of bubbles.            */
/* ------------------------------------------------------------------ */

#define PN_TYPE_NOTIFY (pn_notify_get_type ())

G_DECLARE_FINAL_TYPE (PnNotify, pn_notify, PN, NOTIFY, PnNode)

PnNotify *pn_notify_new (void);

G_END_DECLS

#endif /* PN_NOTIFY_H */
