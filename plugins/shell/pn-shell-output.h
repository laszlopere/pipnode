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

#ifndef PN_SHELL_OUTPUT_H
#define PN_SHELL_OUTPUT_H

#include <glib-object.h>

#include "pn-message.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  Shared output-shaping for the Shell Command / Shell Script nodes.  */
/*                                                                     */
/*  How a captured shell run is turned into the outgoing message's     */
/*  data bag.  Both runner nodes carry an `output-format` property of   */
/*  this enum type and hand their captured streams to                  */
/*  pn_shell_output_apply().                                           */
/* ------------------------------------------------------------------ */

/**
 * PnShellOutputFormat:
 * @PN_SHELL_OUTPUT_TEXT: the historical behaviour — the combined
 *   stdout+stderr lands verbatim in `data.output`, nothing in
 *   `data.value`.
 * @PN_SHELL_OUTPUT_JSON: the script's stdout is parsed as JSON.  A JSON
 *   object is merged member-by-member into the `data` bag (so the script
 *   defines the payload, Node-RED style); a bare scalar/array/null lands
 *   in `data.value` (a boolean coerced to 1.0/0.0 per the bag contract).
 */
typedef enum
{
    PN_SHELL_OUTPUT_TEXT = 0,
    PN_SHELL_OUTPUT_JSON,
} PnShellOutputFormat;

#define PN_TYPE_SHELL_OUTPUT_FORMAT (pn_shell_output_format_get_type ())

GType pn_shell_output_format_get_type (void);

/**
 * pn_shell_output_apply:
 * @msg:         the message whose data bag to fill (its source already set)
 * @format:      %PN_SHELL_OUTPUT_TEXT or %PN_SHELL_OUTPUT_JSON
 * @success:     whether the command exited with status 0
 * @stdout_text: (nullable): captured stdout (only stdout is parsed as JSON)
 * @stderr_text: (nullable): captured stderr (diagnostics; never parsed)
 *
 * Populate @msg from a finished shell run.  In %PN_SHELL_OUTPUT_TEXT mode
 * this sets `data.success` = @success and `data.output` = stdout+stderr,
 * exactly as the runner nodes always have.
 *
 * In %PN_SHELL_OUTPUT_JSON mode @stdout_text is parsed as JSON:
 *  - a JSON **object** is deep-merged into `data.*` (its own `success` /
 *    `output` members, if any, override the baseline) on top of a baseline
 *    `data.success` = @success and `data.output` = the trimmed stdout;
 *  - a non-object **scalar/array/null** is stored in `data.value` (a
 *    boolean as 1.0/0.0), with the same baseline success/output;
 *  - on a parse failure (including empty stdout) `data.success` is forced
 *    %FALSE and `data.output` carries the raw text (or the parser error),
 *    surfacing the problem through the message rather than stderr.
 */
void pn_shell_output_apply (PnMessage           *msg,
                            PnShellOutputFormat  format,
                            gboolean             success,
                            const gchar         *stdout_text,
                            const gchar         *stderr_text);

G_END_DECLS

#endif /* PN_SHELL_OUTPUT_H */
