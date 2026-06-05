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

#ifndef PN_LOGGER_H
#define PN_LOGGER_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnLoggerFormat                                                     */
/*                                                                     */
/*  How a #PnLogger renders each received #PnMessage into the log.     */
/*  Both formats share the same line shape — an ISO-8601 local         */
/*  timestamp, then SUCCESS or FAILURE (from the message's             */
/*  data.success), then a payload:                                     */
/*    LINES -> the message's data.output string                        */
/*    JSON  -> the whole envelope as one-line compact JSON             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LOGGER_FORMAT (pn_logger_format_get_type ())

typedef enum
{
    PN_LOGGER_FORMAT_LINES,
    PN_LOGGER_FORMAT_JSON,
} PnLoggerFormat;

GType pn_logger_format_get_type (void) G_GNUC_CONST;

/* ------------------------------------------------------------------ */
/*  PnLogger                                                           */
/*                                                                     */
/*  Sink node that appends every received #PnMessage to a log file.    */
/*  Offers an internal (no external tool) size-based log rotation and  */
/*  a buffered/flushed write mode.  Input only — like #PnDebug it has  */
/*  no output port and is tapped off a wire.                           */
/* ------------------------------------------------------------------ */

#define PN_TYPE_LOGGER (pn_logger_get_type ())

G_DECLARE_FINAL_TYPE (PnLogger, pn_logger, PN, LOGGER, PnNode)

PnLogger *pn_logger_new (void);

G_END_DECLS

#endif /* PN_LOGGER_H */
