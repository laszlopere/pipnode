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

#ifndef PN_IMAGE_ROTATE_H
#define PN_IMAGE_ROTATE_H

#include "pn-node.h"

G_BEGIN_DECLS

#define PN_TYPE_IMAGE_ROTATE (pn_image_rotate_get_type ())

G_DECLARE_FINAL_TYPE (PnImageRotate, pn_image_rotate,
                      PN, IMAGE_ROTATE, PnNode)

G_END_DECLS

#endif /* PN_IMAGE_ROTATE_H */
