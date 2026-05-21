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

#ifndef PN_DIAL_H
#define PN_DIAL_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnDial                                                             */
/*                                                                     */
/*  Sink node that draws a round analogue dial (the kind a pressure    */
/*  gauge or speedometer uses): metallic outer bezel, glassy face,     */
/*  thin red tick marks with engraved digits, a configurable           */
/*  green/yellow/red zone arc, a 3D-shaded needle, a metallic centre   */
/*  pivot, and a configurable label at the lower centre.  The needle  */
/*  reads the latest numeric value extracted from every incoming       */
/*  message via a JSON path key.                                       */
/* ------------------------------------------------------------------ */

#define PN_TYPE_DIAL (pn_dial_get_type ())

G_DECLARE_FINAL_TYPE (PnDial, pn_dial, PN, DIAL, PnNode)

PnDial *pn_dial_new (void);

G_END_DECLS

#endif /* PN_DIAL_H */
