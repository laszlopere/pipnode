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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-gui.h"

#include "pn-analog-meter-gui.h"
#include "pn-chat-gui.h"
#include "pn-dial-gui.h"
#include "pn-graph-gui.h"
#include "pn-knob-gui.h"
#include "pn-led-gui.h"
#include "pn-switch-gui.h"
#include "pn-table-gui.h"
#include "pn-text-view-gui.h"
#include "pn-weather-report-gui.h"

void
pn_gui_install_builtin_nodes (void)
{
    /* Dual-nature built-ins whose drawing + dialog have been split out
     * to the gui tier (TODO #23, Phase 4).  Each entry installs the
     * gui-only vfunc slots onto the core-registered class. */
    pn_analog_meter_gui_install ();
    pn_chat_gui_install ();
    pn_dial_gui_install ();
    pn_graph_gui_install ();
    pn_knob_gui_install ();
    pn_led_gui_install ();
    pn_switch_gui_install ();
    pn_table_gui_install ();
    pn_text_view_gui_install ();
    pn_weather_report_gui_install ();
}
