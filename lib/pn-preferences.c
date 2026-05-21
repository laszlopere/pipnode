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

#include "pn-preferences.h"

#include <json-glib/json-glib.h>

/* Process-wide singleton.  Constructed on first pn_preferences_get_default()
 * call; never freed (the application owns it for its whole lifetime, and a
 * static singleton keeps the painter's hot path from chasing weak refs). */
static PnPreferences *singleton_instance = NULL;

struct _PnPreferences
{
    GObject parent_instance;

    gboolean   snap_to_grid;
    PnGridType grid_type;
    GdkRGBA    grid_color;

    /* Restored UI geometry: debug-pane open state + width, and the
     * main window size.  See the header for how the window code keeps
     * these in sync. */
    gboolean   debug_view_open;
    gint       debug_view_width;
    gint       window_width;
    gint       window_height;

    /* Set of plugin .so basenames the user has opted out of.  The
     * factory's loader checks membership before each g_module_open()
     * so a disabled plugin is treated as if it were not on disk.
     * Keyed by g_strdup'd basenames; values unused (used as a set). */
    GHashTable *disabled_plugins;

    /* Set while the JSON loader is pushing values into the object so the
     * autosave hook can short-circuit -- we do not want the initial load
     * to immediately rewrite the same file.  Also pinned to %TRUE during
     * property defaults installation. */
    gboolean   loading;

    /* Idle handle that coalesces a burst of property changes into one
     * write to disk.  Zero when no save is pending. */
    guint      save_idle_id;

    /* Resolved absolute path of the on-disk JSON.  Cached at construction
     * so neither the loader nor the autosave hook has to re-derive it. */
    gchar     *config_path;
};

G_DEFINE_TYPE (PnPreferences, pn_preferences, G_TYPE_OBJECT)

enum {
    PROP_0,
    PROP_SNAP_TO_GRID,
    PROP_GRID_TYPE,
    PROP_GRID_COLOR,
    PROP_DEBUG_VIEW_OPEN,
    PROP_DEBUG_VIEW_WIDTH,
    PROP_WINDOW_WIDTH,
    PROP_WINDOW_HEIGHT,
    N_PROPS,
};

static GParamSpec *properties[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Enum boilerplate                                                   */
/* ------------------------------------------------------------------ */

GType
pn_grid_type_get_type (void)
{
    static gsize once = 0;
    static const GEnumValue values[] = {
        { PN_GRID_TYPE_LINES, "PN_GRID_TYPE_LINES", "lines" },
        { PN_GRID_TYPE_DOTS,  "PN_GRID_TYPE_DOTS",  "dots"  },
        { PN_GRID_TYPE_NONE,  "PN_GRID_TYPE_NONE",  "none"  },
        { 0, NULL, NULL }
    };

    if (g_once_init_enter (&once))
    {
        GType t = g_enum_register_static ("PnGridType", values);
        g_once_init_leave (&once, t);
    }
    return (GType) once;
}

/* ------------------------------------------------------------------ */
/*  JSON persistence                                                   */
/* ------------------------------------------------------------------ */

static gchar *
default_config_path (void)
{
    const gchar *cfg_root = g_get_user_config_dir ();
    return g_build_filename (cfg_root, "pipnode", "preferences.json", NULL);
}

/* Map our enum to/from its nick so the JSON stays human-readable. */
static const gchar *
grid_type_to_nick (PnGridType type)
{
    switch (type)
    {
    case PN_GRID_TYPE_LINES: return "lines";
    case PN_GRID_TYPE_DOTS:  return "dots";
    case PN_GRID_TYPE_NONE:  return "none";
    }
    return "lines";
}

static PnGridType
grid_type_from_nick (const gchar *nick, PnGridType fallback)
{
    if (nick == NULL) return fallback;
    if (g_strcmp0 (nick, "lines") == 0) return PN_GRID_TYPE_LINES;
    if (g_strcmp0 (nick, "dots")  == 0) return PN_GRID_TYPE_DOTS;
    if (g_strcmp0 (nick, "none")  == 0) return PN_GRID_TYPE_NONE;
    return fallback;
}

/** Push the current state to @self->config_path.  Atomic via
 *  g_file_set_contents, so a crash midway through cannot leave a
 *  truncated config behind.  Silently ignores write errors -- a
 *  read-only $XDG_CONFIG_HOME is the user's problem, and the in-memory
 *  state is still valid for this session. */
static void
pn_preferences_save_now (PnPreferences *self)
{
    JsonBuilder   *b;
    JsonGenerator *gen;
    JsonNode      *root;
    gchar         *text;
    gchar         *color_str;
    gchar         *dir;

    if (self->config_path == NULL)
        return;

    b = json_builder_new ();
    json_builder_begin_object (b);

    json_builder_set_member_name (b, "snap_to_grid");
    json_builder_add_boolean_value (b, self->snap_to_grid);

    json_builder_set_member_name (b, "grid_type");
    json_builder_add_string_value (b, grid_type_to_nick (self->grid_type));

    color_str = gdk_rgba_to_string (&self->grid_color);
    json_builder_set_member_name (b, "grid_color");
    json_builder_add_string_value (b, color_str);
    g_free (color_str);

    json_builder_set_member_name (b, "debug_view_open");
    json_builder_add_boolean_value (b, self->debug_view_open);

    json_builder_set_member_name (b, "debug_view_width");
    json_builder_add_int_value (b, self->debug_view_width);

    json_builder_set_member_name (b, "window_width");
    json_builder_add_int_value (b, self->window_width);

    json_builder_set_member_name (b, "window_height");
    json_builder_add_int_value (b, self->window_height);

    /* disabled_plugins: array of .so basenames the user has opted out
     * of.  Sorted so the on-disk file is stable across saves regardless
     * of the hash table's iteration order. */
    {
        GList *keys = g_hash_table_get_keys (self->disabled_plugins);
        GList *l;

        keys = g_list_sort (keys, (GCompareFunc) g_strcmp0);

        json_builder_set_member_name (b, "disabled_plugins");
        json_builder_begin_array (b);
        for (l = keys; l != NULL; l = l->next)
            json_builder_add_string_value (b, (const gchar *) l->data);
        json_builder_end_array (b);

        g_list_free (keys);
    }

    json_builder_end_object (b);

    root = json_builder_get_root (b);
    gen  = json_generator_new ();
    json_generator_set_root   (gen, root);
    json_generator_set_pretty (gen, TRUE);
    text = json_generator_to_data (gen, NULL);

    dir = g_path_get_dirname (self->config_path);
    g_mkdir_with_parents (dir, 0700);
    g_free (dir);

    g_file_set_contents (self->config_path, text, -1, NULL);

    g_free (text);
    json_node_free (root);
    g_object_unref (gen);
    g_object_unref (b);
}

static gboolean
pn_preferences_save_idle (gpointer data)
{
    PnPreferences *self = data;
    self->save_idle_id = 0;
    pn_preferences_save_now (self);
    return G_SOURCE_REMOVE;
}

/** Queue an autosave for the next idle.  Coalesces a burst of property
 *  changes (e.g. the user dragging a colour picker) into a single
 *  write, which is friendlier to the file system and the watcher tools
 *  that some users run against $XDG_CONFIG_HOME.  The dialog's "close"
 *  path also calls pn_preferences_save_now() directly so a quit before
 *  the idle fires is still persisted. */
static void
pn_preferences_schedule_save (PnPreferences *self)
{
    if (self->loading)
        return;
    if (self->save_idle_id != 0)
        return;
    self->save_idle_id = g_idle_add (pn_preferences_save_idle, self);
}

/** Read the JSON file (if any) and replace the in-memory values with
 *  the parsed ones.  Missing keys keep their defaults; malformed JSON
 *  leaves the object untouched and the file is rewritten cleanly on the
 *  next property change.  Holds @self->loading so the property setters
 *  invoked from here do not schedule an immediate write-back. */
static void
pn_preferences_load (PnPreferences *self)
{
    JsonParser *parser;
    JsonNode   *root;
    JsonObject *obj;

    if (self->config_path == NULL)
        return;
    if (!g_file_test (self->config_path, G_FILE_TEST_EXISTS))
        return;

    parser = json_parser_new ();
    if (!json_parser_load_from_file (parser, self->config_path, NULL))
    {
        g_object_unref (parser);
        return;
    }

    root = json_parser_get_root (parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT (root))
    {
        g_object_unref (parser);
        return;
    }
    obj = json_node_get_object (root);

    self->loading = TRUE;

    if (json_object_has_member (obj, "snap_to_grid"))
        pn_preferences_set_snap_to_grid (
                self,
                json_object_get_boolean_member (obj, "snap_to_grid"));

    if (json_object_has_member (obj, "grid_type"))
        pn_preferences_set_grid_type (
                self,
                grid_type_from_nick (
                        json_object_get_string_member (obj, "grid_type"),
                        self->grid_type));

    if (json_object_has_member (obj, "grid_color"))
    {
        const gchar *cstr =
                json_object_get_string_member (obj, "grid_color");
        GdkRGBA      rgba;
        if (cstr != NULL && gdk_rgba_parse (&rgba, cstr))
            pn_preferences_set_grid_color (self, &rgba);
    }

    if (json_object_has_member (obj, "debug_view_open"))
        pn_preferences_set_debug_view_open (
                self,
                json_object_get_boolean_member (obj, "debug_view_open"));

    if (json_object_has_member (obj, "debug_view_width"))
        pn_preferences_set_debug_view_width (
                self,
                (gint) json_object_get_int_member (obj, "debug_view_width"));

    if (json_object_has_member (obj, "window_width") &&
        json_object_has_member (obj, "window_height"))
        pn_preferences_set_window_size (
                self,
                (gint) json_object_get_int_member (obj, "window_width"),
                (gint) json_object_get_int_member (obj, "window_height"));

    if (json_object_has_member (obj, "disabled_plugins"))
    {
        JsonNode *arr_node =
                json_object_get_member (obj, "disabled_plugins");
        if (arr_node != NULL && JSON_NODE_HOLDS_ARRAY (arr_node))
        {
            JsonArray *arr = json_node_get_array (arr_node);
            guint      i, n = json_array_get_length (arr);

            g_hash_table_remove_all (self->disabled_plugins);
            for (i = 0; i < n; i++)
            {
                const gchar *name =
                        json_array_get_string_element (arr, i);
                if (name != NULL && *name != '\0')
                    g_hash_table_add (self->disabled_plugins,
                                      g_strdup (name));
            }
        }
    }

    self->loading = FALSE;

    g_object_unref (parser);
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_preferences_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnPreferences *self = PN_PREFERENCES (object);

    switch (prop_id)
    {
    case PROP_SNAP_TO_GRID:
        g_value_set_boolean (value, self->snap_to_grid);
        break;
    case PROP_GRID_TYPE:
        g_value_set_enum (value, self->grid_type);
        break;
    case PROP_GRID_COLOR:
        g_value_set_boxed (value, &self->grid_color);
        break;
    case PROP_DEBUG_VIEW_OPEN:
        g_value_set_boolean (value, self->debug_view_open);
        break;
    case PROP_DEBUG_VIEW_WIDTH:
        g_value_set_int (value, self->debug_view_width);
        break;
    case PROP_WINDOW_WIDTH:
        g_value_set_int (value, self->window_width);
        break;
    case PROP_WINDOW_HEIGHT:
        g_value_set_int (value, self->window_height);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_preferences_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnPreferences *self = PN_PREFERENCES (object);

    switch (prop_id)
    {
    case PROP_SNAP_TO_GRID:
        pn_preferences_set_snap_to_grid (self, g_value_get_boolean (value));
        break;
    case PROP_GRID_TYPE:
        pn_preferences_set_grid_type (self, g_value_get_enum (value));
        break;
    case PROP_GRID_COLOR:
        pn_preferences_set_grid_color (self, g_value_get_boxed (value));
        break;
    case PROP_DEBUG_VIEW_OPEN:
        pn_preferences_set_debug_view_open (self,
                                            g_value_get_boolean (value));
        break;
    case PROP_DEBUG_VIEW_WIDTH:
        pn_preferences_set_debug_view_width (self, g_value_get_int (value));
        break;
    case PROP_WINDOW_WIDTH:
        pn_preferences_set_window_size (self, g_value_get_int (value),
                                        self->window_height);
        break;
    case PROP_WINDOW_HEIGHT:
        pn_preferences_set_window_size (self, self->window_width,
                                        g_value_get_int (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_preferences_finalize (GObject *object)
{
    PnPreferences *self = PN_PREFERENCES (object);

    if (self->save_idle_id != 0)
    {
        g_source_remove (self->save_idle_id);
        self->save_idle_id = 0;
        pn_preferences_save_now (self);
    }
    g_clear_pointer (&self->config_path, g_free);
    g_clear_pointer (&self->disabled_plugins, g_hash_table_unref);

    G_OBJECT_CLASS (pn_preferences_parent_class)->finalize (object);
}

static void
pn_preferences_class_init (PnPreferencesClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->get_property = pn_preferences_get_property;
    object_class->set_property = pn_preferences_set_property;
    object_class->finalize     = pn_preferences_finalize;

    properties[PROP_SNAP_TO_GRID] = g_param_spec_boolean (
            "snap-to-grid", "Snap to Grid",
            "Whether node positions are quantised to the worksheet grid.",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_GRID_TYPE] = g_param_spec_enum (
            "grid-type", "Grid Type",
            "How the worksheet background grid is rendered.",
            PN_TYPE_GRID_TYPE,
            PN_GRID_TYPE_LINES,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_GRID_COLOR] = g_param_spec_boxed (
            "grid-color", "Grid Color",
            "RGBA colour used to paint the worksheet grid.",
            GDK_TYPE_RGBA,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_DEBUG_VIEW_OPEN] = g_param_spec_boolean (
            "debug-view-open", "Debug View Open",
            "Whether the debug pane is shown on launch.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_DEBUG_VIEW_WIDTH] = g_param_spec_int (
            "debug-view-width", "Debug View Width",
            "On-screen width of the debug pane in pixels; 0 until set.",
            0, G_MAXINT, 0,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_WINDOW_WIDTH] = g_param_spec_int (
            "window-width", "Window Width",
            "Main window width in pixels, restored on launch.",
            1, G_MAXINT, 1024,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    properties[PROP_WINDOW_HEIGHT] = g_param_spec_int (
            "window-height", "Window Height",
            "Main window height in pixels, restored on launch.",
            1, G_MAXINT, 720,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
pn_preferences_init (PnPreferences *self)
{
    /* Defaults match the painter's historical hard-coded values, so an
     * absent on-disk config produces a worksheet visually identical to
     * the pre-preferences build. */
    self->snap_to_grid    = TRUE;
    self->grid_type       = PN_GRID_TYPE_LINES;
    self->grid_color.red   = 0.85;
    self->grid_color.green = 0.85;
    self->grid_color.blue  = 0.88;
    self->grid_color.alpha = 1.0;
    self->debug_view_open  = FALSE;
    /* 0 = "no width recorded yet": the window then reveals the pane at
     * its historical 25 %-of-window default until the user sizes it. */
    self->debug_view_width = 0;
    self->window_width     = 1024;
    self->window_height    = 720;
    self->loading         = FALSE;
    self->save_idle_id    = 0;
    self->config_path     = default_config_path ();
    self->disabled_plugins = g_hash_table_new_full (
            g_str_hash, g_str_equal, g_free, NULL);
}

/* ------------------------------------------------------------------ */
/*  Public surface                                                     */
/* ------------------------------------------------------------------ */

PnPreferences *
pn_preferences_get_default (void)
{
    if (G_UNLIKELY (singleton_instance == NULL))
    {
        singleton_instance = g_object_new (PN_TYPE_PREFERENCES, NULL);
        pn_preferences_load (singleton_instance);
    }
    return singleton_instance;
}

gboolean
pn_preferences_get_snap_to_grid (PnPreferences *self)
{
    g_return_val_if_fail (PN_IS_PREFERENCES (self), TRUE);
    return self->snap_to_grid;
}

void
pn_preferences_set_snap_to_grid (PnPreferences *self, gboolean snap)
{
    g_return_if_fail (PN_IS_PREFERENCES (self));

    snap = snap ? TRUE : FALSE;
    if (self->snap_to_grid == snap)
        return;
    self->snap_to_grid = snap;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SNAP_TO_GRID]);
    pn_preferences_schedule_save (self);
}

PnGridType
pn_preferences_get_grid_type (PnPreferences *self)
{
    g_return_val_if_fail (PN_IS_PREFERENCES (self), PN_GRID_TYPE_LINES);
    return self->grid_type;
}

void
pn_preferences_set_grid_type (PnPreferences *self, PnGridType type)
{
    g_return_if_fail (PN_IS_PREFERENCES (self));

    if (self->grid_type == type)
        return;
    self->grid_type = type;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_GRID_TYPE]);
    pn_preferences_schedule_save (self);
}

void
pn_preferences_get_grid_color (PnPreferences *self, GdkRGBA *out_color)
{
    g_return_if_fail (PN_IS_PREFERENCES (self));
    g_return_if_fail (out_color != NULL);

    *out_color = self->grid_color;
}

void
pn_preferences_set_grid_color (PnPreferences *self, const GdkRGBA *color)
{
    g_return_if_fail (PN_IS_PREFERENCES (self));
    g_return_if_fail (color != NULL);

    if (gdk_rgba_equal (&self->grid_color, color))
        return;
    self->grid_color = *color;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_GRID_COLOR]);
    pn_preferences_schedule_save (self);
}

gboolean
pn_preferences_get_debug_view_open (PnPreferences *self)
{
    g_return_val_if_fail (PN_IS_PREFERENCES (self), FALSE);
    return self->debug_view_open;
}

void
pn_preferences_set_debug_view_open (PnPreferences *self, gboolean open)
{
    g_return_if_fail (PN_IS_PREFERENCES (self));

    open = open ? TRUE : FALSE;
    if (self->debug_view_open == open)
        return;
    self->debug_view_open = open;
    g_object_notify_by_pspec (G_OBJECT (self),
                              properties[PROP_DEBUG_VIEW_OPEN]);
    pn_preferences_schedule_save (self);
}

gint
pn_preferences_get_debug_view_width (PnPreferences *self)
{
    g_return_val_if_fail (PN_IS_PREFERENCES (self), 0);
    return self->debug_view_width;
}

void
pn_preferences_set_debug_view_width (PnPreferences *self, gint width)
{
    g_return_if_fail (PN_IS_PREFERENCES (self));

    if (width < 0)
        width = 0;
    if (self->debug_view_width == width)
        return;
    self->debug_view_width = width;
    g_object_notify_by_pspec (G_OBJECT (self),
                              properties[PROP_DEBUG_VIEW_WIDTH]);
    pn_preferences_schedule_save (self);
}

void
pn_preferences_get_window_size (PnPreferences *self,
                                gint          *out_width,
                                gint          *out_height)
{
    g_return_if_fail (PN_IS_PREFERENCES (self));

    if (out_width  != NULL) *out_width  = self->window_width;
    if (out_height != NULL) *out_height = self->window_height;
}

void
pn_preferences_set_window_size (PnPreferences *self,
                                gint           width,
                                gint           height)
{
    g_return_if_fail (PN_IS_PREFERENCES (self));

    if (width  < 1) width  = 1;
    if (height < 1) height = 1;
    if (self->window_width == width && self->window_height == height)
        return;
    self->window_width  = width;
    self->window_height = height;
    g_object_notify_by_pspec (G_OBJECT (self),
                              properties[PROP_WINDOW_WIDTH]);
    g_object_notify_by_pspec (G_OBJECT (self),
                              properties[PROP_WINDOW_HEIGHT]);
    pn_preferences_schedule_save (self);
}

gboolean
pn_preferences_is_plugin_disabled (PnPreferences *self,
                                   const gchar   *basename)
{
    g_return_val_if_fail (PN_IS_PREFERENCES (self), FALSE);
    g_return_val_if_fail (basename != NULL, FALSE);

    return g_hash_table_contains (self->disabled_plugins, basename);
}

void
pn_preferences_set_plugin_disabled (PnPreferences *self,
                                    const gchar   *basename,
                                    gboolean       disabled)
{
    gboolean changed;

    g_return_if_fail (PN_IS_PREFERENCES (self));
    g_return_if_fail (basename != NULL);

    if (disabled)
        changed = g_hash_table_add (self->disabled_plugins,
                                    g_strdup (basename));
    else
        changed = g_hash_table_remove (self->disabled_plugins, basename);

    if (changed)
        pn_preferences_schedule_save (self);
}
