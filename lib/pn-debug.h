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

#ifndef PN_DEBUG_H
#define PN_DEBUG_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDebugTarget                                                      */
/*                                                                     */
/*  Where a #PnDebug delivers its rendered output.  Stderr/stdout go   */
/*  to the matching standard stream; #PN_DEBUG_TARGET_STATUS_BAR       */
/*  emits the "status-message" signal, which the worksheet forwards    */
/*  up to the main window's status bar.  #PN_DEBUG_TARGET_DEBUG_VIEW   */
/*  emits "debug-message", surfaced by the main window's collapsible   */
/*  debug pane.                                                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DEBUG_TARGET (pn_debug_target_get_type ())

typedef enum
{
    PN_DEBUG_TARGET_STDERR,
    PN_DEBUG_TARGET_STDOUT,
    PN_DEBUG_TARGET_STATUS_BAR,
    PN_DEBUG_TARGET_DEBUG_VIEW,
} PnDebugTarget;

GType pn_debug_target_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnDebugFormat                                                      */
/*                                                                     */
/*  How a #PnDebug renders each message.  Oneliner is the historical  */
/*  human-readable trace; JSON wraps source/topic/id/payload into a   */
/*  single JSON object on its own line.                                */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DEBUG_FORMAT (pn_debug_format_get_type ())

typedef enum
{
    PN_DEBUG_FORMAT_TEXT,
    PN_DEBUG_FORMAT_ONELINER,
    PN_DEBUG_FORMAT_JSON,
} PnDebugFormat;

GType pn_debug_format_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnDebug                                                            */
/*                                                                     */
/*  Sink node that prints every received #PnMessage.  The destination */
/*  and rendering are configurable through the #PnDebug:target and    */
/*  #PnDebug:format properties.                                        */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DEBUG (pn_debug_get_type ())

G_DECLARE_FINAL_TYPE (PnDebug, pn_debug, PN, DEBUG, PnNode)

PnDebug *pn_debug_new (void);

G_END_DECLS

#endif /* PN_DEBUG_H */
