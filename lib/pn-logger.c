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

#include "pn-logger.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <string.h>

struct _PnLogger
{
    PnNode parent_instance;

    gchar          *file_path;
    PnLoggerFormat  format;
    gboolean        logrotate;
    gint            max_size_mb;   /* rotate when the file reaches this many MB */
    gint            max_files;     /* number of rotated archives to keep */
    gboolean        flush;         /* fflush() after every line when TRUE */

    /* Live stream state.  The file is opened lazily on the first write
     * and held open across messages so the OS-level buffering the
     * "buffered" mode relies on actually takes effect. */
    FILE     *fp;
    gchar    *open_path;   /* the path @fp was opened for; tracks file_path */
    goffset   cur_size;    /* bytes written to @fp so far (seeded from disk) */
    gboolean  warned;      /* open failure already reported for @open_path */
};

G_DEFINE_TYPE (PnLogger, pn_logger, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_FILE_PATH,
    PROP_FORMAT,
    PROP_LOGROTATE,
    PROP_MAX_SIZE_MB,
    PROP_MAX_FILES,
    PROP_FLUSH,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Enum boxed type                                                    */
/* ------------------------------------------------------------------ */

GType
pn_logger_format_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        /* The nicks become the combo labels; the numeric values stay
           fixed so saved projects keep working. */
        static const GEnumValue values[] = {
            { PN_LOGGER_FORMAT_LINES, "PN_LOGGER_FORMAT_LINES", "Lines" },
            { PN_LOGGER_FORMAT_JSON,  "PN_LOGGER_FORMAT_JSON",  "JSON"  },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnLoggerFormat", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Message → text helpers                                             */
/* ------------------------------------------------------------------ */

/** Pick the user-facing label for a node: prefer the assigned
 *  #PnNode:name and fall back to its class name when unset. */
static const gchar *
display_name (PnNode *node)
{
    const gchar *n;

    if (node == NULL)
        return NULL;

    n = pn_node_get_name (node);
    if (n != NULL && *n != '\0')
        return n;

    return pn_node_get_class_name (node);
}

/** Render just the message's "output" string from its data bag;
 *  returns an empty string when "output" is absent or not a string so
 *  the surrounding line break still marks the event. */
static gchar *
read_output (PnMessage *message)
{
    JsonObject *data = pn_message_get_data (message);
    JsonNode   *node;

    if (data == NULL || !json_object_has_member (data, "output"))
        return g_strdup ("");

    node = json_object_get_member (data, "output");
    if (!JSON_NODE_HOLDS_VALUE (node) ||
        json_node_get_value_type (node) != G_TYPE_STRING)
        return g_strdup ("");

    return g_strdup (json_node_get_string (node));
}

/** Read the message's SUCCESS/FAILURE verdict from its data bag.
 *  Prefers the contract's boolean data.success; falls back to the
 *  numeric data.value (> 0.5) when success is absent; defaults to TRUE
 *  so an unannotated message still logs as a (neutral) success. */
static gboolean
read_success (PnMessage *message)
{
    JsonObject *data = pn_message_get_data (message);
    JsonNode   *node;

    if (data == NULL)
        return TRUE;

    if (json_object_has_member (data, "success"))
    {
        node = json_object_get_member (data, "success");
        if (JSON_NODE_HOLDS_VALUE (node) &&
            json_node_get_value_type (node) == G_TYPE_BOOLEAN)
            return json_node_get_boolean (node);
    }

    if (json_object_has_member (data, "value"))
    {
        node = json_object_get_member (data, "value");
        if (JSON_NODE_HOLDS_VALUE (node))
            return json_node_get_double (node) > 0.5;
    }

    return TRUE;
}

/** Build a #JsonObject describing @message: source/topic/id metadata
 *  alongside the message's data bag (by reference).  Owned by caller. */
static JsonObject *
message_to_json_object (PnMessage *message)
{
    PnNode      *source    = pn_message_get_source  (message);
    const gchar *src_name  = display_name (source);
    const gchar *src_uuid  = source != NULL
                             ? pn_node_get_uuid (source) : NULL;
    const gchar *id        = pn_message_get_id      (message);
    const gchar *topic     = pn_message_get_topic   (message);
    const gchar *created   = pn_message_get_created (message);
    JsonObject  *data      = pn_message_get_data    (message);
    JsonObject  *obj       = json_object_new ();
    JsonNode    *data_node = json_node_new (JSON_NODE_OBJECT);

    json_object_set_string_member (obj, "type",
                                   G_OBJECT_TYPE_NAME (message));
    json_object_set_string_member (obj, "from",
                                   src_name  ? src_name  : "");
    json_object_set_string_member (obj, "from_id",
                                   src_uuid  ? src_uuid  : "");
    json_object_set_string_member (obj, "topic",
                                   topic     ? topic     : "");
    json_object_set_string_member (obj, "id",
                                   id        ? id        : "");
    json_object_set_string_member (obj, "created",
                                   created   ? created   : "");

    /* Reference, do not copy: the generator only reads it. */
    json_node_set_object   (data_node, data);
    json_object_set_member (obj, "data", data_node);

    return obj;
}

/** Render the whole envelope of @message as compact (single-line) JSON. */
static gchar *
read_compact_json (PnMessage *message)
{
    JsonObject    *obj  = message_to_json_object (message);
    JsonNode      *root = json_node_new (JSON_NODE_OBJECT);
    JsonGenerator *gen  = json_generator_new ();
    gchar         *out;

    json_node_set_object    (root, obj);
    json_generator_set_root (gen, root);
    out = json_generator_to_data (gen, NULL);

    json_node_free   (root);
    g_object_unref   (gen);
    json_object_unref (obj);

    return out;
}

/* ------------------------------------------------------------------ */
/*  File handling + rotation                                           */
/* ------------------------------------------------------------------ */

/** Close the live stream if open, leaving @fp / @open_path cleared. */
static void
logger_close (PnLogger *self)
{
    if (self->fp != NULL)
    {
        fclose (self->fp);
        self->fp = NULL;
    }
    g_clear_pointer (&self->open_path, g_free);
    self->cur_size = 0;
    self->warned   = FALSE;
}

/** Ensure @fp is open for the current file_path, reopening when the
 *  path changed.  Seeds cur_size from the file already on disk so a
 *  restarted process rotates relative to the real size, not zero.
 *  Returns FALSE (and leaves @fp NULL) when the path is empty or the
 *  file cannot be opened. */
static gboolean
logger_ensure_open (PnLogger *self)
{
    GStatBuf st;

    if (self->fp != NULL &&
        g_strcmp0 (self->open_path, self->file_path) == 0)
        return TRUE;

    /* Path changed or not yet open: drop any previous stream. */
    logger_close (self);

    if (self->file_path == NULL || self->file_path[0] == '\0')
        return FALSE;

    self->fp = g_fopen (self->file_path, "a");
    if (self->fp == NULL)
    {
        if (!self->warned)
        {
            g_warning ("Logger: cannot open '%s' for append: %s",
                       self->file_path, g_strerror (errno));
            self->warned = TRUE;
        }
        return FALSE;
    }

    self->open_path = g_strdup (self->file_path);
    self->cur_size  = (g_stat (self->open_path, &st) == 0)
                      ? (goffset) st.st_size : 0;
    self->warned    = FALSE;
    return TRUE;
}

/** Rotate the current log: the live file becomes "<path>.1", existing
 *  archives shift up by one, and anything beyond max_files is dropped.
 *  With max_files == 0 the file is simply truncated (no archives kept).
 *  Reopens a fresh, empty log afterwards. */
static void
logger_rotate (PnLogger *self)
{
    gchar *path = g_strdup (self->open_path);
    gint   i;

    if (self->fp != NULL)
    {
        fclose (self->fp);
        self->fp = NULL;
    }

    if (self->max_files <= 0)
    {
        /* Keep no history: just remove the current file. */
        g_unlink (path);
    }
    else
    {
        gchar *oldest = g_strdup_printf ("%s.%d", path, self->max_files);
        g_unlink (oldest);
        g_free (oldest);

        for (i = self->max_files - 1; i >= 1; i--)
        {
            gchar *src = g_strdup_printf ("%s.%d", path, i);
            gchar *dst = g_strdup_printf ("%s.%d", path, i + 1);

            if (g_file_test (src, G_FILE_TEST_EXISTS))
                g_rename (src, dst);

            g_free (src);
            g_free (dst);
        }

        {
            gchar *first = g_strdup_printf ("%s.1", path);
            g_rename (path, first);
            g_free (first);
        }
    }

    /* Reopen a fresh log under the original path. */
    self->fp = g_fopen (path, "a");
    self->cur_size = 0;
    if (self->fp == NULL && !self->warned)
    {
        g_warning ("Logger: cannot reopen '%s' after rotation: %s",
                   path, g_strerror (errno));
        self->warned = TRUE;
    }

    g_free (path);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_logger_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnLogger  *self = PN_LOGGER (node);
    GDateTime *dt;
    gchar     *ts;
    gchar     *payload;
    gchar     *line;

    if (!logger_ensure_open (self))
        return;

    dt = g_date_time_new_now_local ();
    ts = g_date_time_format (dt, "%Y-%m-%dT%H:%M:%S");
    g_date_time_unref (dt);

    payload = (self->format == PN_LOGGER_FORMAT_JSON)
              ? read_compact_json (message)
              : read_output (message);

    line = g_strdup_printf ("%s %s %s\n",
                            ts,
                            read_success (message) ? "SUCCESS" : "FAILURE",
                            payload);

    fputs (line, self->fp);
    self->cur_size += strlen (line);
    if (self->flush)
        fflush (self->fp);

    if (self->logrotate &&
        self->cur_size >= (goffset) self->max_size_mb * 1024 * 1024)
        logger_rotate (self);

    g_free (line);
    g_free (payload);
    g_free (ts);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_logger_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnLogger *self = PN_LOGGER (object);

    switch (prop_id)
    {
    case PROP_FILE_PATH:
        g_value_set_string (value, self->file_path);
        break;
    case PROP_FORMAT:
        g_value_set_enum (value, self->format);
        break;
    case PROP_LOGROTATE:
        g_value_set_boolean (value, self->logrotate);
        break;
    case PROP_MAX_SIZE_MB:
        g_value_set_int (value, self->max_size_mb);
        break;
    case PROP_MAX_FILES:
        g_value_set_int (value, self->max_files);
        break;
    case PROP_FLUSH:
        g_value_set_boolean (value, self->flush);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_logger_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnLogger *self = PN_LOGGER (object);

    switch (prop_id)
    {
    case PROP_FILE_PATH:
        {
            const gchar *path = g_value_get_string (value);
            if (g_strcmp0 (self->file_path, path) != 0)
            {
                g_free (self->file_path);
                self->file_path = g_strdup (path != NULL ? path : "");
                /* Reopen against the new path on the next write. */
                logger_close (self);
                /* No file to write to is a configuration error: flag it
                 * so the worksheet paints the node red until set. */
                pn_node_set_has_error (PN_NODE (self),
                                       self->file_path[0] == '\0');
                g_object_notify_by_pspec (object, props[PROP_FILE_PATH]);
            }
        }
        break;
    case PROP_FORMAT:
        {
            PnLoggerFormat f = g_value_get_enum (value);
            if (self->format != f)
            {
                self->format = f;
                g_object_notify_by_pspec (object, props[PROP_FORMAT]);
            }
        }
        break;
    case PROP_LOGROTATE:
        {
            gboolean b = g_value_get_boolean (value);
            if (self->logrotate != b)
            {
                self->logrotate = b;
                g_object_notify_by_pspec (object, props[PROP_LOGROTATE]);
            }
        }
        break;
    case PROP_MAX_SIZE_MB:
        {
            gint v = g_value_get_int (value);
            if (self->max_size_mb != v)
            {
                self->max_size_mb = v;
                g_object_notify_by_pspec (object, props[PROP_MAX_SIZE_MB]);
            }
        }
        break;
    case PROP_MAX_FILES:
        {
            gint v = g_value_get_int (value);
            if (self->max_files != v)
            {
                self->max_files = v;
                g_object_notify_by_pspec (object, props[PROP_MAX_FILES]);
            }
        }
        break;
    case PROP_FLUSH:
        {
            gboolean b = g_value_get_boolean (value);
            if (self->flush != b)
            {
                self->flush = b;
                g_object_notify_by_pspec (object, props[PROP_FLUSH]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_logger_dispose (GObject *object)
{
    PnLogger *self = PN_LOGGER (object);

    logger_close (self);

    G_OBJECT_CLASS (pn_logger_parent_class)->dispose (object);
}

static void
pn_logger_finalize (GObject *object)
{
    PnLogger *self = PN_LOGGER (object);

    g_clear_pointer (&self->file_path, g_free);

    G_OBJECT_CLASS (pn_logger_parent_class)->finalize (object);
}

static void
pn_logger_class_init (PnLoggerClass *klass)
{
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    node_class->receive        = pn_logger_receive;
    object_class->get_property = pn_logger_get_property;
    object_class->set_property = pn_logger_set_property;
    object_class->dispose      = pn_logger_dispose;
    object_class->finalize     = pn_logger_finalize;

    node_class->class_name     = "Logger";
    node_class->icon           = "\xef\x83\xb6";  /* fa-file-text-o U+F0F6 */
    node_class->color          = (PnColor){ 0.62, 0.45, 0.20, 1.0 };
    node_class->category       = "Sinks";
    node_class->has_input      = TRUE;
    node_class->has_output     = FALSE;

    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        pn_settings_schema_row (schema, "file-path",   PN_EDITOR_FILE);
        pn_settings_schema_row (schema, "format",      PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "logrotate",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "max-size-mb", PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "max-files",   PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "flush",       PN_EDITOR_AUTO);
        /* The rotation knobs only matter while rotation is enabled. */
        pn_settings_schema_enable_when_truthy (schema, "max-size-mb",
                                               "logrotate");
        pn_settings_schema_enable_when_truthy (schema, "max-files",
                                               "logrotate");
        pn_node_class_set_settings_schema (node_class, schema);
    }

    props[PROP_FILE_PATH] = g_param_spec_string (
            "file-path", "Log file",
            "Path of the file log lines are appended to.  Empty disables "
            "logging.  Parent directories must already exist",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FORMAT] = g_param_spec_enum (
            "format", "Format",
            "How each message is written: \"Lines\" logs the message's "
            "output text, \"JSON\" logs the whole envelope as one-line JSON. "
            "Both are prefixed with an ISO-8601 timestamp and SUCCESS/FAILURE",
            PN_TYPE_LOGGER_FORMAT,
            PN_LOGGER_FORMAT_LINES,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LOGROTATE] = g_param_spec_boolean (
            "logrotate", "Rotate logs",
            "Rotate the file internally once it reaches the maximum size, "
            "keeping a bounded number of numbered archives (no external "
            "logrotate tool is invoked)",
            TRUE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAX_SIZE_MB] = g_param_spec_int (
            "max-size-mb", "Max file size (MB)",
            "Rotate once the live log reaches this many megabytes.  Only "
            "used when rotation is enabled",
            1, G_MAXINT, 10,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_MAX_FILES] = g_param_spec_int (
            "max-files", "Max old files",
            "How many rotated archives (file.1 … file.N) to keep.  0 keeps "
            "none — the log is simply truncated on rotation.  Only used when "
            "rotation is enabled",
            0, 1000, 5,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FLUSH] = g_param_spec_boolean (
            "flush", "Flush after each write",
            "Flush every line straight to disk.  Off (the default) leaves "
            "writes buffered, which is faster but may lose the last lines on "
            "a crash",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_logger_init (PnLogger *self)
{
    PnNode  *node  = PN_NODE (self);
    PnColor  amber = { 0.62, 0.45, 0.20, 1.0 };

    self->file_path   = g_strdup ("");
    self->format      = PN_LOGGER_FORMAT_LINES;
    self->logrotate   = TRUE;
    self->max_size_mb = 10;
    self->max_files   = 5;
    self->flush       = FALSE;

    self->fp          = NULL;
    self->open_path   = NULL;
    self->cur_size    = 0;
    self->warned      = FALSE;

    pn_node_set_class_name (node, "Logger");
    pn_node_set_icon       (node, "\xef\x83\xb6");   /* fa-file-text-o U+F0F6 */
    pn_node_set_color      (node, &amber);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    /* Starts unconfigured: no log file yet, so flag the error state.
     * Setting file-path clears it (see pn_logger_set_property). */
    pn_node_set_has_error  (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnLogger *
pn_logger_new (void)
{
    return g_object_new (PN_TYPE_LOGGER, NULL);
}
