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

#ifndef PN_APPLICATION_H
#define PN_APPLICATION_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PN_TYPE_APPLICATION (pn_application_get_type ())

G_DECLARE_FINAL_TYPE (PnApplication,
                      pn_application,
                      PN, APPLICATION,
                      GtkApplication)

/**
 * pn_application_new:
 * @unique: %TRUE to register the well-known D-Bus name as a single
 *   instance (the background engine via --gapplication-service, or a
 *   test instance via --dbus-name); %FALSE for a normal launch, which
 *   runs as its own independent, non-unique process so opening a file
 *   never lands in the engine.
 */
PnApplication *pn_application_new (gboolean unique);

G_END_DECLS

#endif /* PN_APPLICATION_H */
