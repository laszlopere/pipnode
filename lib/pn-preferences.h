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

#ifndef PN_PREFERENCES_H
#define PN_PREFERENCES_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define PN_TYPE_PREFERENCES (pn_preferences_get_type ())

G_DECLARE_FINAL_TYPE (PnPreferences,
                      pn_preferences,
                      PN, PREFERENCES,
                      GObject)

/**
 * PnGridType:
 * @PN_GRID_TYPE_LINES: cross-hatched lines at every grid intersection
 *   (the historical and default rendering).
 * @PN_GRID_TYPE_DOTS:  one dot per grid intersection.
 * @PN_GRID_TYPE_NONE:  no grid rendered at all.
 *
 * How the worksheet background grid is visualised.  Snapping is
 * controlled independently by the "snap-to-grid" property — a user
 * who wants a clean-looking canvas with the snap still on can pick
 * #PN_GRID_TYPE_NONE without losing alignment behaviour.
 */
typedef enum {
    PN_GRID_TYPE_LINES = 0,
    PN_GRID_TYPE_DOTS  = 1,
    PN_GRID_TYPE_NONE  = 2,
} PnGridType;

GType pn_grid_type_get_type (void) G_GNUC_CONST;
#define PN_TYPE_GRID_TYPE (pn_grid_type_get_type ())

/**
 * pn_preferences_get_default:
 *
 * Returns the process-wide preferences singleton.  The first call
 * loads the on-disk JSON from `$XDG_CONFIG_HOME/pipnode/preferences.json`
 * (falling back to the property defaults when the file is missing or
 * unreadable).  Subsequent calls return the same instance — every
 * setter on the returned object writes back to disk and emits the
 * appropriate `notify::` signal so live subscribers (the worksheet
 * painter, the preferences dialog) stay in sync.
 *
 * Returns: (transfer none): the singleton; not to be unrefed.
 */
PnPreferences *pn_preferences_get_default (void);

/* Convenience accessors — the properties are also reachable via
 * g_object_get / g_object_set, but the typed wrappers are nicer to
 * use from the painter's hot loop and keep the GdkRGBA copy semantics
 * uniform across call sites. */
gboolean    pn_preferences_get_snap_to_grid (PnPreferences *self);
void        pn_preferences_set_snap_to_grid (PnPreferences *self,
                                             gboolean       snap);

PnGridType  pn_preferences_get_grid_type    (PnPreferences *self);
void        pn_preferences_set_grid_type    (PnPreferences *self,
                                             PnGridType     type);

void        pn_preferences_get_grid_color   (PnPreferences *self,
                                             GdkRGBA       *out_color);
void        pn_preferences_set_grid_color   (PnPreferences *self,
                                             const GdkRGBA *color);

/* --- Window / debug-pane geometry ----------------------------------
 *
 * Persisted UI geometry restored on the next launch: whether the
 * right-hand debug pane is open, its on-screen width in pixels, and
 * the main window's size.  The window code reads these at construction
 * and writes them back as the user toggles the pane, drags its
 * divider, or resizes the window.
 */

gboolean    pn_preferences_get_debug_view_open  (PnPreferences *self);
void        pn_preferences_set_debug_view_open  (PnPreferences *self,
                                                gboolean       open);

gint        pn_preferences_get_debug_view_width (PnPreferences *self);
void        pn_preferences_set_debug_view_width (PnPreferences *self,
                                                gint           width);

void        pn_preferences_get_window_size      (PnPreferences *self,
                                                gint          *out_width,
                                                gint          *out_height);
void        pn_preferences_set_window_size      (PnPreferences *self,
                                                gint           width,
                                                gint           height);

/* --- Plugin enable/disable -----------------------------------------
 *
 * The disabled-plugins set holds plugin .so basenames the user has
 * opted out of (e.g. "pipnode_network.so").  The factory's loader
 * consults this set before each g_module_open() and skips a matching
 * file silently — equivalent to removing the .so from the plugin
 * directory but reversible without filesystem changes.  Changes only
 * take effect on the next process start because GType registrations
 * cannot be undone once a plugin's pn_plugin_init has run.
 */

gboolean    pn_preferences_is_plugin_disabled  (PnPreferences *self,
                                                const gchar   *basename);

void        pn_preferences_set_plugin_disabled (PnPreferences *self,
                                                const gchar   *basename,
                                                gboolean       disabled);

G_END_DECLS

#endif /* PN_PREFERENCES_H */
