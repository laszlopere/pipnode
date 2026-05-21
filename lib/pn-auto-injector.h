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

#ifndef PN_AUTO_INJECTOR_H
#define PN_AUTO_INJECTOR_H

#include "pn-auto-trigger.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnAutoInjector                                                     */
/*                                                                     */
/*  Auto-trigger source node that emits a fixed, user-configured       */
/*  message once per #PnAutoTrigger:period seconds.  Behaves like      */
/*  #PnInject without the manual fire button: the same envelope and    */
/*  data-bag fields users wire downstream filters / sinks against      */
/*  (data.value, data.output, data.success) come straight off the      */
/*  node's properties, so a freshly-dropped AutoInjector starts        */
/*  delivering a stable synthetic message every tick without any       */
/*  external input.                                                    */
/*                                                                     */
/*  Useful as a heartbeat for downstream nodes that need a steady      */
/*  signal — driving a Graph with a constant baseline, feeding a       */
/*  Watchdog with the "alive" pulse it expects, or wiring up a stub    */
/*  source while a real probe is still being built.                    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_AUTO_INJECTOR (pn_auto_injector_get_type ())

G_DECLARE_FINAL_TYPE (PnAutoInjector, pn_auto_injector,
                      PN, AUTO_INJECTOR, PnAutoTrigger)

PnAutoInjector *pn_auto_injector_new (void);

G_END_DECLS

#endif /* PN_AUTO_INJECTOR_H */
