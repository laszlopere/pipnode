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

#ifndef PN_IMAGE_THRESHOLD_H
#define PN_IMAGE_THRESHOLD_H

#include "pn-node.h"

G_BEGIN_DECLS

#define PN_TYPE_IMAGE_THRESHOLD (pn_image_threshold_get_type ())

G_DECLARE_FINAL_TYPE (PnImageThreshold, pn_image_threshold,
                      PN, IMAGE_THRESHOLD, PnNode)

G_END_DECLS

#endif /* PN_IMAGE_THRESHOLD_H */
