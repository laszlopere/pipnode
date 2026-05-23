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

#ifndef PN_CHAT_GUI_H
#define PN_CHAT_GUI_H

#include <glib.h>

G_BEGIN_DECLS

/* pn_chat_gui_install:
 *
 * Install the gui-tier vfunc slot — the cairo/Pango chat painter (the
 * scrolling bubble list with per-sender pastel hues and the inline
 * entry strip + Send button with its blinking caret) — onto the
 * already-registered #PnChat class.  Called once at editor startup (via
 * pn_gui_install_builtin_nodes()), after the factory has registered the
 * built-in node types.  The headless runtime never calls this, so the
 * Chat logic (receive with self-loop suppression, bubble accumulation +
 * #limit trimming, the focus flag, the draft-editing primitives, submit)
 * runs without GTK while the editor still draws the chat.  The scroll
 * vfunc and the focus/submit/draft-editing API stay in the GTK-free
 * core; only the GdkEventKey key-press translator lives here.  */
void pn_chat_gui_install (void);

G_END_DECLS

#endif /* PN_CHAT_GUI_H */
