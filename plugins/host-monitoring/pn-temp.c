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

#include "pn-temp.h"
#include "pn-message.h"
#include "pn-ssh-profile.h"
#include "pn-vault.h"

#include <string.h>

/* fa-thermometer-full U+F2C7 -- the "full mercury" thermometer glyph,
 * added in FA-4.7 (same FA-4 ceiling caveat as the other host-
 * monitoring icons). */
#define PN_TEMP_ICON           "\xef\x8b\x87"

#define PN_TEMP_DEFAULT_PERIOD 5u
/* See PN_LOAD_DEFAULT_HOST -- empty is the canonical "this machine"
 * marker; the dialog shows g_get_host_name() as a gray placeholder
 * hint and the trigger emits that same name on data.host. */
#define PN_TEMP_DEFAULT_HOST   ""

#define PN_TEMP_HWMON_ROOT     "/sys/class/hwmon"
#define PN_TEMP_THERMAL_ROOT   "/sys/class/thermal"

struct _PnTemp
{
    PnAutoTrigger parent_instance;

    /* @hostname and @aggregation are touched from both the worker
     * thread (in the trigger) and the main thread (via property
     * setters), so the mutex serialises access.  The string itself is
     * owned by the node and freed in finalize. */
    GMutex             mutex;
    gchar             *hostname;
    PnTempAggregation  aggregation;

    /* Optional SSH Login credential (item 47.4): @auth_profile is the
     * picked profile id (main thread only); @ssh_login is the value
     * snapshot the worker reads under @mutex, refreshed on the main
     * thread when the property or the vault changes. */
    gchar             *auth_profile;
    PnSshLogin         ssh_login;
};

G_DEFINE_TYPE (PnTemp, pn_temp, PN_TYPE_AUTO_TRIGGER)

enum {
    PROP_0,
    PROP_AUTH_PROFILE,
    PROP_HOSTNAME,
    PROP_AGGREGATION,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Aggregation enum boxed type                                        */
/* ------------------------------------------------------------------ */

GType
pn_temp_aggregation_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        static const GEnumValue values[] = {
            { PN_TEMP_AVERAGE, "PN_TEMP_AVERAGE", "average" },
            { PN_TEMP_MAXIMUM, "PN_TEMP_MAXIMUM", "maximum" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnTempAggregation", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* ------------------------------------------------------------------ */
/*  Property accessors (thread-safe)                                   */
/* ------------------------------------------------------------------ */

static gchar *
temp_dup_hostname_locked (PnTemp *self)
{
    gchar *copy;

    g_mutex_lock (&self->mutex);
    copy = g_strdup (self->hostname);
    g_mutex_unlock (&self->mutex);

    return copy;
}

static PnTempAggregation
temp_get_aggregation_locked (PnTemp *self)
{
    PnTempAggregation a;

    g_mutex_lock (&self->mutex);
    a = self->aggregation;
    g_mutex_unlock (&self->mutex);

    return a;
}

static void
temp_set_hostname (
        PnTemp      *self,
        const gchar *hostname)
{
    gchar *old;
    gchar *replacement;

    /* NULL collapses to the default "localhost" so a freshly-loaded
     * node missing the property still behaves sensibly. */
    replacement = g_strdup ((hostname != NULL) ? hostname
                                               : PN_TEMP_DEFAULT_HOST);

    g_mutex_lock (&self->mutex);
    old = self->hostname;
    self->hostname = replacement;
    g_mutex_unlock (&self->mutex);

    g_free (old);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_HOSTNAME]);
}

static void
temp_set_aggregation (
        PnTemp            *self,
        PnTempAggregation  aggregation)
{
    g_mutex_lock (&self->mutex);
    self->aggregation = aggregation;
    g_mutex_unlock (&self->mutex);

    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_AGGREGATION]);
}

/* True when @hostname selects the local-filesystem path: NULL,
 * empty string, and the literal "localhost" all map there. */
static gboolean
hostname_is_local (const gchar *hostname)
{
    if (hostname == NULL || *hostname == '\0')
        return TRUE;
    return g_ascii_strcasecmp (hostname, "localhost") == 0;
}

/* ------------------------------------------------------------------ */
/*  Reading buffer                                                     */
/*                                                                     */
/*  PnTempReading and the two pure helpers below                       */
/*  (pn_temp_parse_remote_lines / pn_temp_aggregate) are declared in   */
/*  pn-temp.h so the headless unit tests can drive the parse + collapse */
/*  logic on canned text without sampling real hardware.               */
/* ------------------------------------------------------------------ */

static void
temp_reading_clear (gpointer data)
{
    PnTempReading *r = data;
    g_free (r->label);
}

/* The hwmon `name` files we treat as CPU temperature drivers.  The
 * list covers the common cases (Intel coretemp, AMD k8/k10/zenpower,
 * ARM cpu_thermal) and is intentionally short -- pulling in unrelated
 * sensors (board fans, chipset, GPU) would skew the aggregation away
 * from "CPU temperature".  Anything not on the list is ignored. */
static gboolean
is_cpu_hwmon_name (const gchar *name)
{
    if (name == NULL || *name == '\0')
        return FALSE;
    return g_strcmp0 (name, "coretemp")    == 0
        || g_strcmp0 (name, "k10temp")     == 0
        || g_strcmp0 (name, "k8temp")      == 0
        || g_strcmp0 (name, "zenpower")    == 0
        || g_strcmp0 (name, "cpu_thermal") == 0
        || g_strcmp0 (name, "cpu-thermal") == 0;
}

/* Thermal-zone `type` values that look like a CPU thermal sensor;
 * used as the fallback when no hwmon source matched. */
static gboolean
is_cpu_thermal_type (const gchar *type)
{
    if (type == NULL || *type == '\0')
        return FALSE;
    return g_strcmp0 (type, "x86_pkg_temp") == 0
        || g_strcmp0 (type, "cpu-thermal")  == 0
        || g_strcmp0 (type, "cpu_thermal")  == 0
        || g_strcmp0 (type, "coretemp")     == 0
        || g_strcmp0 (type, "k10temp")      == 0;
}

/* ------------------------------------------------------------------ */
/*  Local sampling (pure C, no helper subprocesses)                    */
/* ------------------------------------------------------------------ */

/* Parse `tempN_input` filenames and return N (NULL on miss).  The
 * returned slice is the digits substring inside @entry; the caller
 * owns nothing, the bounds are returned via @out_len. */
static const gchar *
extract_temp_index (
        const gchar *entry,
        gsize       *out_len)
{
    const gchar *p;
    const gchar *q;

    if (!g_str_has_prefix (entry, "temp"))
        return NULL;
    if (!g_str_has_suffix (entry, "_input"))
        return NULL;

    p = entry + 4;
    q = strchr (p, '_');
    if (q == NULL || q == p)
        return NULL;

    *out_len = (gsize) (q - p);
    return p;
}

static void
read_hwmon_dir (
        const gchar *dir,
        const gchar *source_name,
        GArray      *readings)
{
    GDir        *sub;
    const gchar *entry;

    sub = g_dir_open (dir, 0, NULL);
    if (sub == NULL)
        return;

    while ((entry = g_dir_read_name (sub)) != NULL)
    {
        gsize        idx_len = 0;
        const gchar *idx;
        gchar       *idx_str;
        gchar       *input_path;
        gchar       *label_path;
        gchar       *label_name;
        gchar       *value_str = NULL;
        gchar       *label_str = NULL;
        gchar       *label;
        gdouble      milli;
        PnTempReading  r;

        idx = extract_temp_index (entry, &idx_len);
        if (idx == NULL)
            continue;

        idx_str    = g_strndup (idx, idx_len);
        input_path = g_build_filename (dir, entry, NULL);

        if (g_file_get_contents (input_path, &value_str, NULL, NULL))
        {
            milli = g_ascii_strtod (value_str, NULL);

            label_name = g_strdup_printf ("temp%s_label", idx_str);
            label_path = g_build_filename (dir, label_name, NULL);

            if (g_file_get_contents (label_path, &label_str, NULL, NULL))
            {
                g_strchomp (label_str);
                label = g_strdup_printf ("%s/%s", source_name, label_str);
            }
            else
            {
                label = g_strdup_printf ("%s/temp%s", source_name, idx_str);
            }

            r.label   = label;
            r.celsius = milli / 1000.0;
            g_array_append_val (readings, r);

            g_free (label_name);
            g_free (label_path);
            g_free (label_str);
        }

        g_free (value_str);
        g_free (input_path);
        g_free (idx_str);
    }

    g_dir_close (sub);
}

static void
collect_local_hwmon (GArray *readings)
{
    GDir        *root;
    const gchar *child;

    root = g_dir_open (PN_TEMP_HWMON_ROOT, 0, NULL);
    if (root == NULL)
        return;

    while ((child = g_dir_read_name (root)) != NULL)
    {
        gchar *dir;
        gchar *name_path;
        gchar *name = NULL;

        dir       = g_build_filename (PN_TEMP_HWMON_ROOT, child, NULL);
        name_path = g_build_filename (dir, "name", NULL);

        if (g_file_get_contents (name_path, &name, NULL, NULL))
            g_strchomp (name);

        if (is_cpu_hwmon_name (name))
            read_hwmon_dir (dir, name, readings);

        g_free (name);
        g_free (name_path);
        g_free (dir);
    }

    g_dir_close (root);
}

static void
collect_local_thermal (GArray *readings)
{
    GDir        *root;
    const gchar *child;

    root = g_dir_open (PN_TEMP_THERMAL_ROOT, 0, NULL);
    if (root == NULL)
        return;

    while ((child = g_dir_read_name (root)) != NULL)
    {
        gchar *dir;
        gchar *type_path;
        gchar *temp_path;
        gchar *type_str  = NULL;
        gchar *value_str = NULL;

        if (!g_str_has_prefix (child, "thermal_zone"))
            continue;

        dir       = g_build_filename (PN_TEMP_THERMAL_ROOT, child, NULL);
        type_path = g_build_filename (dir, "type", NULL);
        temp_path = g_build_filename (dir, "temp", NULL);

        if (g_file_get_contents (type_path, &type_str, NULL, NULL))
            g_strchomp (type_str);

        if (is_cpu_thermal_type (type_str)
            && g_file_get_contents (temp_path, &value_str, NULL, NULL))
        {
            PnTempReading r;
            r.label   = g_strdup_printf ("%s/%s", type_str, child);
            r.celsius = g_ascii_strtod (value_str, NULL) / 1000.0;
            g_array_append_val (readings, r);
        }

        g_free (value_str);
        g_free (type_str);
        g_free (temp_path);
        g_free (type_path);
        g_free (dir);
    }

    g_dir_close (root);
}

static gboolean
sample_local (
        GArray  *readings,
        GError **error)
{
    (void) error;

    collect_local_hwmon (readings);
    /* Thermal-zone entries on x86 typically mirror coretemp/k10temp,
     * so we only fall back to them when hwmon found nothing -- avoids
     * the same package being counted twice through two interfaces. */
    if (readings->len == 0)
        collect_local_thermal (readings);

    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  Remote sampling (ssh transport, parse server-side text)            */
/*                                                                     */
/*  The remote shell snippet emits one `name|label|millidegrees` line  */
/*  per matched sensor.  `|` is the field separator because hwmon      */
/*  labels never contain it (they are short ASCII strings like         */
/*  "Package id 0" / "Tctl"); the snippet also strips any newlines    */
/*  out of @label so a multi-line label cannot desync the parser.      */
/* ------------------------------------------------------------------ */

#define PN_TEMP_REMOTE_SH                                                \
    "set -e; "                                                           \
    "emit() { "                                                          \
    "  for d in /sys/class/hwmon/hwmon*/; do "                           \
    "    [ -r \"${d}name\" ] || continue; "                              \
    "    n=$(cat \"${d}name\"); "                                        \
    "    case \"$n\" in "                                                \
    "      coretemp|k10temp|k8temp|zenpower|cpu_thermal|cpu-thermal) ;; "\
    "      *) continue ;; "                                              \
    "    esac; "                                                         \
    "    for f in \"$d\"temp*_input; do "                                \
    "      [ -r \"$f\" ] || continue; "                                  \
    "      base=$(basename \"$f\" _input); "                             \
    "      lf=\"$d${base}_label\"; "                                     \
    "      if [ -r \"$lf\" ]; then l=$(tr -d '|\\n' <\"$lf\"); "         \
    "      else l=\"$base\"; fi; "                                       \
    "      v=$(cat \"$f\"); "                                            \
    "      printf '%s|%s|%s\\n' \"$n\" \"$l\" \"$v\"; "                  \
    "    done; "                                                         \
    "  done; "                                                           \
    "}; "                                                                \
    "out=$(emit); "                                                      \
    "if [ -z \"$out\" ]; then "                                          \
    "  for d in /sys/class/thermal/thermal_zone*/; do "                  \
    "    [ -r \"${d}type\" ] || continue; "                              \
    "    t=$(cat \"${d}type\"); "                                        \
    "    case \"$t\" in "                                                \
    "      x86_pkg_temp|cpu-thermal|cpu_thermal|coretemp|k10temp) ;; "   \
    "      *) continue ;; "                                              \
    "    esac; "                                                         \
    "    [ -r \"${d}temp\" ] || continue; "                              \
    "    v=$(cat \"${d}temp\"); "                                        \
    "    z=$(basename \"$d\"); "                                         \
    "    printf '%s|%s|%s\\n' \"$t\" \"$z\" \"$v\"; "                    \
    "  done; "                                                           \
    "else printf '%s' \"$out\"; fi"

static gboolean
sample_ssh (
        const gchar      *hostname,
        const PnSshLogin *login,
        guint             timeout,
        gchar           **out_text,
        gchar           **out_stderr,
        GError          **error)
{
    /* The remote command is `sh -c <script>` as three argv words; ssh
     * joins them with spaces on the wire and the remote shell un-quotes
     * them back, so the script is passed verbatim with no local shell
     * re-parse (the old command-line path g_shell_quote'd it only to
     * survive GLib's parser, which g_spawn_sync no longer runs). */
    const gchar *base_argv[] = { "sh", "-c", PN_TEMP_REMOTE_SH, NULL };
    gchar      **argv;
    gint         exit_status = 0;
    gboolean     spawned;
    gboolean     ok = FALSE;

    /* Route through the one shared SSH argv builder (TODO #47.3); no
     * login profile yet (item 47.4), so pass NULL for today's argv. */
    argv = pn_ssh_build_argv (hostname, login, timeout, base_argv);

    spawned = g_spawn_sync (NULL, argv, NULL,
                            G_SPAWN_SEARCH_PATH,
                            NULL, NULL,
                            out_text, out_stderr,
                            &exit_status, error);

    if (spawned)
        ok = g_spawn_check_wait_status (exit_status, error);

    g_strfreev (argv);

    return ok;
}

void
pn_temp_parse_remote_lines (
        const gchar *blob,
        GArray      *readings)
{
    const gchar *cursor = blob;

    if (blob == NULL)
        return;

    while (cursor != NULL && *cursor != '\0')
    {
        const gchar *line_end;
        const gchar *bar1;
        const gchar *bar2;
        const gchar *line_stop;
        gsize        line_len;
        gchar       *line;
        PnTempReading  r;
        gdouble      milli;

        line_end = strchr (cursor, '\n');
        line_stop = (line_end != NULL) ? line_end : cursor + strlen (cursor);
        line_len = (gsize) (line_stop - cursor);

        if (line_len == 0)
        {
            cursor = (line_end != NULL) ? line_end + 1 : NULL;
            continue;
        }

        line = g_strndup (cursor, line_len);
        cursor = (line_end != NULL) ? line_end + 1 : NULL;

        bar1 = strchr (line, '|');
        bar2 = (bar1 != NULL) ? strchr (bar1 + 1, '|') : NULL;
        if (bar1 == NULL || bar2 == NULL)
        {
            g_free (line);
            continue;
        }

        milli = g_ascii_strtod (bar2 + 1, NULL);

        r.label   = g_strdup_printf ("%.*s/%.*s",
                                     (int) (bar1 - line),       line,
                                     (int) (bar2 - bar1 - 1),   bar1 + 1);
        r.celsius = milli / 1000.0;
        g_array_append_val (readings, r);

        g_free (line);
    }
}

/* ------------------------------------------------------------------ */
/*  Aggregation                                                        */
/* ------------------------------------------------------------------ */

static const gchar *
aggregation_nick (PnTempAggregation a)
{
    return (a == PN_TEMP_MAXIMUM) ? "maximum" : "average";
}

gdouble
pn_temp_aggregate (
        GArray             *readings,
        PnTempAggregation   mode,
        gdouble            *out_min,
        gdouble            *out_max,
        gdouble            *out_avg,
        const gchar       **out_hot_label)
{
    gdouble sum = 0.0;
    gdouble lo  = 0.0;
    gdouble hi  = 0.0;
    const gchar *hot = NULL;
    guint i;

    if (readings->len == 0)
    {
        *out_min = 0.0;
        *out_max = 0.0;
        *out_avg = 0.0;
        *out_hot_label = NULL;
        return 0.0;
    }

    for (i = 0; i < readings->len; ++i)
    {
        PnTempReading *r = &g_array_index (readings, PnTempReading, i);
        if (i == 0)
        {
            lo  = r->celsius;
            hi  = r->celsius;
            hot = r->label;
        }
        else
        {
            if (r->celsius < lo) lo = r->celsius;
            if (r->celsius > hi) { hi = r->celsius; hot = r->label; }
        }
        sum += r->celsius;
    }

    *out_min = lo;
    *out_max = hi;
    *out_avg = sum / (gdouble) readings->len;
    *out_hot_label = hot;

    return (mode == PN_TEMP_MAXIMUM) ? hi : (sum / (gdouble) readings->len);
}

/* Build a JSON array of {label, value} objects so a downstream node
 * (Debug Print, Table, Graph fed by a Query) can see every sensor
 * the aggregation was computed from rather than just the rolled-up
 * number on data.value. */
static JsonNode *
build_readings_json (GArray *readings)
{
    JsonArray *arr = json_array_new ();
    JsonNode  *node;
    guint i;

    for (i = 0; i < readings->len; ++i)
    {
        PnTempReading *r = &g_array_index (readings, PnTempReading, i);
        JsonObject  *obj = json_object_new ();
        json_object_set_string_member (obj, "label", r->label);
        json_object_set_double_member (obj, "value", r->celsius);
        json_array_add_object_element (arr, obj);
    }

    node = json_node_new (JSON_NODE_ARRAY);
    json_node_take_array (node, arr);
    return node;
}

/* ------------------------------------------------------------------ */
/*  Trigger                                                            */
/*                                                                     */
/*  Runs on the worker thread inherited from #PnAutoTrigger, so the    */
/*  potentially blocking SSH call cannot freeze the GUI.               */
/* ------------------------------------------------------------------ */

static void
pn_temp_trigger (PnAutoTrigger *trigger)
{
    PnTemp            *self     = PN_TEMP (trigger);
    PnNode            *node     = PN_NODE (self);
    gchar             *hostname = temp_dup_hostname_locked (self);
    PnTempAggregation  mode     = temp_get_aggregation_locked (self);
    gchar             *raw      = NULL;
    gchar             *errbuf   = NULL;
    GError            *error    = NULL;
    PnMessage         *msg;
    GArray            *readings;
    gboolean           success  = FALSE;
    const gchar       *effective;
    gboolean           local;
    /* Empty / "localhost" both resolve to the actual machine name on
     * the wire and in the summary -- same convention as #PnLoad. */
    const gchar       *display;
    guint              period;
    guint              timeout;
    PnSshLogin         login    = { 0 };

    /* Resolve the SSH-Login snapshot up front: a profile carrying its own
     * host pins WHERE we sample (mirroring pn_ssh_build_argv), so a node
     * left blank but pointing at such a credential reaches that host rather
     * than falling through to the local file. */
    pn_ssh_login_dup_locked (&self->mutex, &self->ssh_login, &login);
    effective = pn_ssh_login_effective_host (&login, hostname);
    local     = hostname_is_local (effective);
    display   = local ? g_get_host_name () : effective;

    period  = pn_auto_trigger_get_period (trigger);
    timeout = (period > 1u) ? period - 1u : 1u;

    readings = g_array_new (FALSE, FALSE, sizeof (PnTempReading));
    g_array_set_clear_func (readings, temp_reading_clear);

    if (local)
    {
        success = sample_local (readings, &error);
    }
    else
    {
        success = sample_ssh (effective, &login, timeout, &raw, &errbuf, &error);
        if (success)
            pn_temp_parse_remote_lines (raw, readings);
    }

    /* A successful sample with zero sensors is treated as a failure
     * downstream -- there is nothing meaningful to put on data.value,
     * and the diagnostic ("no CPU temperature sensors found") is more
     * useful than silently emitting 0.0. */
    if (success && readings->len == 0)
        success = FALSE;

    msg = pn_message_new (node, NULL);
    pn_message_set_string  (msg, "host",
                            display);
    pn_message_set_string  (msg, "aggregation", aggregation_nick (mode));
    pn_message_set_boolean (msg, "success", success);

    if (success)
    {
        gdouble      value;
        gdouble      lo = 0.0, hi = 0.0, avg = 0.0;
        const gchar *hot = NULL;
        gchar       *summary;

        value = pn_temp_aggregate (readings, mode, &lo, &hi, &avg, &hot);

        pn_message_set_double (msg, "value",   value);
        pn_message_set_double (msg, "average", avg);
        pn_message_set_double (msg, "min",     lo);
        pn_message_set_double (msg, "max",     hi);
        pn_message_set_int    (msg, "count",   (gint) readings->len);
        pn_message_set_string (msg, "unit",    "\xc2\xb0""C");
        if (hot != NULL)
            pn_message_set_string (msg, "hot", hot);

        pn_message_set_member (msg, "readings",
                               build_readings_json (readings));

        if (mode == PN_TEMP_MAXIMUM)
        {
            summary = g_strdup_printf (
                    "CPU temperature on %s: max %.1f \xc2\xb0""C "
                    "(%s) across %u sensor%s; avg %.1f \xc2\xb0""C.",
                    display,
                    hi, hot != NULL ? hot : "?",
                    readings->len,
                    readings->len == 1 ? "" : "s",
                    avg);
        }
        else
        {
            summary = g_strdup_printf (
                    "CPU temperature on %s: avg %.1f \xc2\xb0""C "
                    "across %u sensor%s (min %.1f, max %.1f).",
                    display,
                    avg,
                    readings->len,
                    readings->len == 1 ? "" : "s",
                    lo, hi);
        }
        pn_message_set_string (msg, "output", summary);
        g_free (summary);
    }
    else
    {
        /* Surface a single-line reason so a downstream Debug Print
         * tells the user what went wrong without them having to
         * spelunk in the journal.  Prefer remote stderr over GError
         * because the GError for a non-zero exit is the generic
         * "Child process exited with code N", whereas stderr
         * typically carries the actual cause. */
        const gchar *reason = NULL;
        gchar       *summary;

        if (errbuf != NULL && *errbuf != '\0')
            reason = g_strchomp (errbuf);
        else if (error != NULL)
            reason = error->message;
        else if (readings->len == 0)
            reason = "no CPU temperature sensors found";

        if (reason != NULL)
            summary = g_strdup_printf (
                    "Failed to read CPU temperature from %s: %s",
                    display, reason);
        else
            summary = g_strdup_printf (
                    "Failed to read CPU temperature from %s.",
                    display);

        pn_message_set_string (msg, "output", summary);
        g_free (summary);
    }

    pn_auto_trigger_emit_on_main (trigger, msg);

    g_array_free (readings, TRUE);
    g_clear_error (&error);
    g_free (raw);
    g_free (errbuf);
    g_free (hostname);
    pn_ssh_login_clear (&login);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_temp_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnTemp *self = PN_TEMP (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        g_value_take_string (value, temp_dup_hostname_locked (self));
        break;
    case PROP_AGGREGATION:
        g_value_set_enum (value, temp_get_aggregation_locked (self));
        break;
    case PROP_AUTH_PROFILE:
        g_value_set_string (value, self->auth_profile);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_temp_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnTemp *self = PN_TEMP (object);

    switch (prop_id)
    {
    case PROP_HOSTNAME:
        temp_set_hostname (self, g_value_get_string (value));
        break;
    case PROP_AGGREGATION:
        temp_set_aggregation (self, g_value_get_enum (value));
        break;
    case PROP_AUTH_PROFILE:
        g_free (self->auth_profile);
        self->auth_profile = g_value_dup_string (value);
        pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                              &self->mutex, &self->ssh_login);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

/* A profile changed in the vault: re-resolve our login snapshot. */
static void
on_vault_changed (PnTemp *self)
{
    pn_ssh_login_refresh (PN_NODE (self), "auth-profile",
                          &self->mutex, &self->ssh_login);
}

static void
pn_temp_finalize (GObject *object)
{
    PnTemp *self = PN_TEMP (object);

    g_clear_pointer (&self->hostname,     g_free);
    g_clear_pointer (&self->auth_profile, g_free);
    pn_ssh_login_clear (&self->ssh_login);
    g_mutex_clear (&self->mutex);

    G_OBJECT_CLASS (pn_temp_parent_class)->finalize (object);
}

static void
pn_temp_class_init (PnTempClass *klass)
{
    GObjectClass       *object_class  = G_OBJECT_CLASS (klass);
    PnNodeClass        *node_class    = PN_NODE_CLASS (klass);
    PnAutoTriggerClass *trigger_class = PN_AUTO_TRIGGER_CLASS (klass);

    object_class->get_property = pn_temp_get_property;
    object_class->set_property = pn_temp_set_property;
    object_class->finalize     = pn_temp_finalize;
    trigger_class->trigger     = pn_temp_trigger;

    node_class->palette_icon   = PN_TEMP_ICON;
    node_class->class_name     = "CPU Temperature";
    node_class->icon           = PN_TEMP_ICON;
    node_class->color          = (PnColor){ 0.62, 0.50, 0.78, 1.0 };
    node_class->category       = "Host monitoring";
    node_class->has_input      = FALSE;
    node_class->has_output     = TRUE;

    props[PROP_HOSTNAME] = g_param_spec_string (
            "hostname", "Hostname",
            "Host whose CPU temperature sensors should be sampled.  "
            "An empty string or \"localhost\" reads /sys/class/hwmon "
            "(falling back to /sys/class/thermal) directly; any other "
            "value is treated as an SSH target and the sample is "
            "fetched via passwordless ssh.",
            PN_TEMP_DEFAULT_HOST,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
    pn_param_spec_set_hostname_hint (props[PROP_HOSTNAME]);

    props[PROP_AGGREGATION] = g_param_spec_enum (
            "aggregation", "Aggregation",
            "How to collapse the per-CPU per-core sensor readings "
            "into the single number emitted as data.value: "
            "\"average\" reports the arithmetic mean (good for a "
            "whole-package feel), \"maximum\" reports the hottest "
            "single sensor (better for spotting a runaway core).",
            PN_TYPE_TEMP_AGGREGATION,
            PN_TEMP_AVERAGE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Optional SSH Login credential picker (item 47.4). */
    props[PROP_AUTH_PROFILE] = pn_ssh_auth_profile_param_spec ();

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_temp_init (PnTemp *self)
{
    PnNode  *node   = PN_NODE (self);
    PnColor  violet = { 0.62, 0.50, 0.78, 1.0 };

    g_mutex_init (&self->mutex);
    self->hostname     = g_strdup (PN_TEMP_DEFAULT_HOST);
    self->aggregation  = PN_TEMP_AVERAGE;
    self->auth_profile = g_strdup ("");
    self->ssh_login    = (PnSshLogin){ 0 };

    g_signal_connect_object (pn_vault_get_default (), "changed",
                             G_CALLBACK (on_vault_changed), self,
                             G_CONNECT_SWAPPED);
    pn_ssh_login_refresh (node, "auth-profile", &self->mutex, &self->ssh_login);

    pn_node_set_class_name (node, "CPU Temperature");
    pn_node_set_icon       (node, PN_TEMP_ICON);
    pn_node_set_color      (node, &violet);
    pn_node_set_has_input  (node, FALSE);
    pn_node_set_has_output (node, TRUE);

    /* CPU temperatures move on the order of seconds (thermal mass of
     * the silicon + heatsink); 5 s mirrors the System Load default
     * and keeps a freshly-created node from hammering an SSH target
     * on first paint. */
    pn_auto_trigger_set_period (PN_AUTO_TRIGGER (self),
                                PN_TEMP_DEFAULT_PERIOD);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnTemp *
pn_temp_new (void)
{
    return g_object_new (PN_TYPE_TEMP, NULL);
}
