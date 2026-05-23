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

#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <libsoup/soup.h>

#include "pn-node-dialog-helpers.h"
#include "pn-sci-fi-sound.h"

/* fa-rocket U+F135 — a recognisable sci-fi glyph for the palette.  */
#define PN_SCI_FI_SOUND_NORMAL_ICON  "\xef\x84\xb5"
#define PN_SCI_FI_SOUND_WARNING_ICON "\xe2\x9d\x97"      /* ❗ U+2757 */

#define PN_SCI_FI_SOUND_DEAD_PERIOD_MIN 0u
#define PN_SCI_FI_SOUND_DEAD_PERIOD_MAX 3600u
#define PN_SCI_FI_SOUND_DEAD_PERIOD_DEF 0u

#define PN_SCI_FI_INDEX_URL "https://www.trekcore.com/audio/"

/* ------------------------------------------------------------------ */
/*  Category table                                                     */
/*                                                                     */
/*  Trekcore's audio/ index links a thousand-ish clips organised into  */
/*  top-level directories (aliensounds, doors, weapons, …) with a      */
/*  few buckets that carry a meaningful filename prefix in addition    */
/*  to the dir name (aliensounds/borg_…, computer/tng_…).  This table  */
/*  routes a (dir, basename) pair to one of a handful of user-facing   */
/*  "sound packs": Romulan, Xindi, TOS Computer, TNG Ambience, etc.    */
/*  — that the settings dialog exposes as a per-pack download /        */
/*  delete checkbox grid.                                              */
/*                                                                     */
/*  Order matters: the first matching row wins, so the more-specific   */
/*  prefix rules ("aliensounds/borg_*") come before the catch-all      */
/*  dir rule for the same parent dir (aliensounds → Other Aliens).     */
/*  A URL that doesn't match anything is silently skipped at download  */
/*  time — every clip in the trekcore catalogue is currently covered   */
/*  by either a specific row or one of the three "Other …" buckets.   */
/* ------------------------------------------------------------------ */

typedef struct
{
    const gchar *display;   /* user-facing pack name + on-disk subdir */
    const gchar *dir;       /* trekcore top-level dir, NULL = wildcard */
    const gchar *prefix;    /* required basename prefix, NULL = any   */
} PnSciFiCategory;

static const PnSciFiCategory categories[] = {
    /* Alien races */
    { "Borg",         "aliensounds", "borg_" },
    { "Klingon",      "aliensounds", "klingon_" },
    { "Romulan",      "aliensounds", "romulan_" },
    { "Xindi",        "aliensounds", "xindi_" },
    { "Cardassian",   "aliensounds", "cardassian_" },
    { "Andorian",     "aliensounds", "andor" },
    { "Other Aliens", "aliensounds", NULL },

    /* Computer voice / beeps / alerts */
    { "TOS Computer", "toscomputer", NULL },
    { "TNG Computer", "computer",    NULL },

    /* Per-series engine room / bridge ambience */
    { "TOS Ambience", "background", "tos_" },
    { "TNG Ambience", "background", "tng_" },
    { "DS9 Ambience", "background", "ds9_" },
    { "VOY Ambience", "background", "voy_" },
    { "Ambient",      "background", "ambient_" },
    { "Other Background", "background", NULL },

    /* Ship sounds */
    { "Red Alert",     "redalertandklaxons", NULL },
    { "Doors",         "doors",        NULL },
    { "Transporter",   "transporters", NULL },
    { "Warp",          "warp",         NULL },
    { "Communicator",  "communicator", NULL },
    { "Weapons",       "weapons",      NULL },
    { "Explosions",    "explosions",   NULL },
    { "Viewscreen",    "viewscreen",   NULL },
    { "Turbolift",     "turbolift",    NULL },
    { "Holodeck",      "holodeck",     NULL },
    { "Brig",          "brig",         NULL },
    { "Replicator",    "replicator",   NULL },
    { "Cloaking",      "cloaking",     NULL },
    { "Tricorder",     "tricorder",    NULL },
    { "Medical",       "medicaldevices", NULL },

    /* Catch-all bucket trekcore itself labels "other/". */
    { "Other",        "other", NULL },
};

#define PN_SCI_FI_N_CATEGORIES G_N_ELEMENTS (categories)

/** Find the first matching category for a (top-dir, basename) pair.
 *  Returns -1 if nothing in the table matches, which means the URL is
 *  silently dropped at download time. */
static gssize
sci_fi_classify (const gchar *dir, const gchar *basename)
{
    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
    {
        const PnSciFiCategory *c = &categories[i];

        if (c->dir != NULL && g_strcmp0 (c->dir, dir) != 0)
            continue;
        if (c->prefix != NULL
            && !g_str_has_prefix (basename, c->prefix))
            continue;
        return (gssize) i;
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnSciFiSound
{
    PnNode parent_instance;

    /* Clip identifier of the form "<Category>/<basename>", e.g.
     * "Romulan/romulan_disruptor.mp3".  Stored as a relative path so a
     * worksheet saved on one machine still resolves when reopened on
     * another whose cache happens to live under a different $HOME. */
    gchar *clip;

    guint    dead_period;
    gboolean playing;
    gint64   ready_at_us;

    /* Lazily-constructed; shared by background downloads and reaped
     * on dispose so an in-flight fetch does not outlive the node. */
    SoupSession  *session;
    GCancellable *cancellable;
};

G_DEFINE_TYPE (PnSciFiSound, pn_sci_fi_sound, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_CLIP,
    PROP_DEAD_PERIOD,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Storage layout                                                     */
/* ------------------------------------------------------------------ */

static gchar *
sci_fi_sound_cache_dir (void)
{
    return g_build_filename (g_get_user_data_dir (),
                             "pipnode",
                             "sound-effects",
                             "startrek",
                             NULL);
}

/** Resolve a "<Category>/<basename>" clip identifier to its absolute
 *  cache path.  Rejects path-traversal forms ("..", leading slash,
 *  backslash) so a hand-edited worksheet can't aim the player at an
 *  arbitrary file. */
static gchar *
sci_fi_sound_resolve_path (const gchar *clip)
{
    gchar *dir;
    gchar *path;

    if (clip == NULL || *clip == '\0')
        return NULL;
    if (clip[0] == '/' || strchr (clip, '\\') != NULL)
        return NULL;
    if (strstr (clip, "..") != NULL)
        return NULL;

    dir  = sci_fi_sound_cache_dir ();
    path = g_build_filename (dir, clip, NULL);
    g_free (dir);
    return path;
}

static int
sci_fi_basename_compare (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (*(const gchar * const *) a,
                      *(const gchar * const *) b);
}

/** Count the number of regular files inside one category directory.
 *  Used by the per-pack indicator in the Sound packs tab so the user
 *  can see at a glance which packs are already on disk. */
static gsize
sci_fi_sound_count_in_category (const gchar *category)
{
    gchar *root = sci_fi_sound_cache_dir ();
    gchar *dir  = g_build_filename (root, category, NULL);
    GDir  *d    = g_dir_open (dir, 0, NULL);
    gsize  n    = 0;

    if (d != NULL)
    {
        const gchar *entry;
        while ((entry = g_dir_read_name (d)) != NULL)
        {
            gchar *full = g_build_filename (dir, entry, NULL);
            if (g_file_test (full, G_FILE_TEST_IS_REGULAR))
                n++;
            g_free (full);
        }
        g_dir_close (d);
    }
    g_free (dir);
    g_free (root);
    return n;
}

/** Sorted list of "<Category>/<basename>" identifiers for every clip
 *  currently on disk.  Used to populate the clip combo on the
 *  Playback tab. */
static gchar **
sci_fi_sound_list_local (void)
{
    GPtrArray *names = g_ptr_array_new ();
    gchar     *root  = sci_fi_sound_cache_dir ();

    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
    {
        const gchar *category = categories[i].display;
        gchar *dir = g_build_filename (root, category, NULL);
        GDir  *d   = g_dir_open (dir, 0, NULL);

        if (d != NULL)
        {
            const gchar *entry;
            while ((entry = g_dir_read_name (d)) != NULL)
            {
                gchar *full = g_build_filename (dir, entry, NULL);
                if (g_file_test (full, G_FILE_TEST_IS_REGULAR))
                {
                    g_ptr_array_add (names,
                            g_strdup_printf ("%s/%s", category, entry));
                }
                g_free (full);
            }
            g_dir_close (d);
        }
        g_free (dir);
    }
    g_free (root);

    g_ptr_array_sort (names, sci_fi_basename_compare);
    g_ptr_array_add  (names, NULL);
    return (gchar **) g_ptr_array_free (names, FALSE);
}

/** Recursive `rm -rf` over a single category subdir.  Used by the
 *  "Delete selected" button.  Returns the number of files removed. */
static gsize
sci_fi_sound_delete_category (const gchar *category)
{
    gchar *root = sci_fi_sound_cache_dir ();
    gchar *dir  = g_build_filename (root, category, NULL);
    GDir  *d    = g_dir_open (dir, 0, NULL);
    gsize  removed = 0;

    if (d != NULL)
    {
        const gchar *entry;
        while ((entry = g_dir_read_name (d)) != NULL)
        {
            gchar *full = g_build_filename (dir, entry, NULL);
            if (g_unlink (full) == 0)
                removed++;
            g_free (full);
        }
        g_dir_close (d);
        g_rmdir (dir);
    }

    g_free (dir);
    g_free (root);
    return removed;
}

/* ------------------------------------------------------------------ */
/*  Visual state                                                       */
/* ------------------------------------------------------------------ */

static void
apply_visual_state (PnSciFiSound *self, gboolean configured)
{
    PnNode *node = PN_NODE (self);

    if (configured)
    {
        PnColor warp = { 0.20, 0.55, 0.85, 1.0 };
        pn_node_set_color (node, &warp);
        pn_node_set_icon  (node, PN_SCI_FI_SOUND_NORMAL_ICON);
    }
    else
    {
        PnColor red = { 0.86, 0.30, 0.28, 1.0 };
        pn_node_set_color (node, &red);
        pn_node_set_icon  (node, PN_SCI_FI_SOUND_WARNING_ICON);
    }
}

/* ------------------------------------------------------------------ */
/*  Playback                                                           */
/* ------------------------------------------------------------------ */

static gchar *
sci_fi_sound_find_player (void)
{
    static const gchar *candidates[] = {
        "mpv", "ffplay", "gst-play-1.0", "mpg123", "paplay", NULL
    };

    for (gsize i = 0; candidates[i] != NULL; i++)
    {
        gchar *path = g_find_program_in_path (candidates[i]);
        if (path != NULL)
            return path;
    }
    return NULL;
}

static gchar **
sci_fi_sound_build_argv (const gchar *player, const gchar *file)
{
    const gchar *basename = strrchr (player, '/');
    GPtrArray   *argv     = g_ptr_array_new ();

    basename = (basename != NULL) ? basename + 1 : player;

    g_ptr_array_add (argv, g_strdup (player));

    if (g_strcmp0 (basename, "mpv") == 0)
    {
        g_ptr_array_add (argv, g_strdup ("--no-video"));
        g_ptr_array_add (argv, g_strdup ("--really-quiet"));
        g_ptr_array_add (argv, g_strdup ("--no-config"));
    }
    else if (g_strcmp0 (basename, "ffplay") == 0)
    {
        g_ptr_array_add (argv, g_strdup ("-nodisp"));
        g_ptr_array_add (argv, g_strdup ("-autoexit"));
        g_ptr_array_add (argv, g_strdup ("-loglevel"));
        g_ptr_array_add (argv, g_strdup ("quiet"));
    }
    else if (g_strcmp0 (basename, "mpg123") == 0)
    {
        g_ptr_array_add (argv, g_strdup ("-q"));
    }

    g_ptr_array_add (argv, g_strdup (file));
    g_ptr_array_add (argv, NULL);

    return (gchar **) g_ptr_array_free (argv, FALSE);
}

static void
on_play_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    GSubprocess  *sub   = G_SUBPROCESS (source);
    PnSciFiSound *self  = PN_SCI_FI_SOUND (user_data);
    GError       *error = NULL;

    g_subprocess_wait_finish (sub, result, &error);
    if (error != NULL)
    {
        g_warning ("pn-sci-fi-sound: playback failed: %s", error->message);
        g_error_free (error);
    }

    self->playing     = FALSE;
    self->ready_at_us = g_get_monotonic_time ()
                      + (gint64) self->dead_period * G_TIME_SPAN_SECOND;

    g_object_unref (sub);
    g_object_unref (self);
}

static gboolean
sci_fi_sound_play_file (PnSciFiSound *self, const gchar *path)
{
    gchar       *player;
    gchar      **argv;
    GSubprocess *sub;
    GError      *error = NULL;

    player = sci_fi_sound_find_player ();
    if (player == NULL)
    {
        g_warning ("pn-sci-fi-sound: no media player found "
                   "(install mpv, ffplay, or gst-play-1.0)");
        return FALSE;
    }

    argv = sci_fi_sound_build_argv (player, path);
    g_free (player);

    sub  = g_subprocess_newv ((const gchar * const *) argv,
                              G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                              | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                              &error);
    g_strfreev (argv);

    if (sub == NULL)
    {
        g_warning ("pn-sci-fi-sound: failed to spawn player: %s",
                   error ? error->message : "(unknown)");
        g_clear_error (&error);
        return FALSE;
    }

    self->playing = TRUE;
    g_subprocess_wait_async (sub, NULL,
                             on_play_done, g_object_ref (self));
    return TRUE;
}

static void
sci_fi_sound_preview (PnSciFiSound *self)
{
    gchar *path;

    if (self->playing)
        return;
    if (self->clip == NULL || *self->clip == '\0')
        return;

    path = sci_fi_sound_resolve_path (self->clip);
    if (path == NULL)
        return;

    if (!g_file_test (path, G_FILE_TEST_IS_REGULAR))
    {
        g_warning ("pn-sci-fi-sound: clip not found on disk: %s", path);
        g_free (path);
        return;
    }

    sci_fi_sound_play_file (self, path);
    g_free (path);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_sci_fi_sound_receive (PnNode *node, PnMessage *message)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (node);
    gchar        *path;

    (void) message;

    if (self->playing)
        return;
    if (self->ready_at_us != 0
        && g_get_monotonic_time () < self->ready_at_us)
        return;
    if (self->clip == NULL || *self->clip == '\0')
        return;

    path = sci_fi_sound_resolve_path (self->clip);
    if (path == NULL)
        return;

    if (g_file_test (path, G_FILE_TEST_IS_REGULAR))
        sci_fi_sound_play_file (self, path);

    g_free (path);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_sci_fi_sound_set_clip (PnSciFiSound *self, const gchar *clip)
{
    if (g_strcmp0 (self->clip, clip) == 0)
        return;

    g_free (self->clip);
    self->clip = (clip != NULL && *clip != '\0') ? g_strdup (clip) : NULL;

    apply_visual_state (self, self->clip != NULL);
    g_object_notify_by_pspec (G_OBJECT (self), props[PROP_CLIP]);
}

static void
pn_sci_fi_sound_get_property (GObject    *object,
                              guint       prop_id,
                              GValue     *value,
                              GParamSpec *pspec)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (object);

    switch (prop_id)
    {
    case PROP_CLIP:
        g_value_set_string (value, self->clip);
        break;
    case PROP_DEAD_PERIOD:
        g_value_set_uint (value, self->dead_period);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_sci_fi_sound_set_property (GObject      *object,
                              guint         prop_id,
                              const GValue *value,
                              GParamSpec   *pspec)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (object);

    switch (prop_id)
    {
    case PROP_CLIP:
        pn_sci_fi_sound_set_clip (self, g_value_get_string (value));
        break;
    case PROP_DEAD_PERIOD:
        {
            guint v = g_value_get_uint (value);
            if (self->dead_period != v)
            {
                self->dead_period = v;
                g_object_notify_by_pspec (object,
                                          props[PROP_DEAD_PERIOD]);
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
pn_sci_fi_sound_dispose (GObject *object)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (object);

    if (self->cancellable != NULL)
    {
        g_cancellable_cancel (self->cancellable);
        g_clear_object (&self->cancellable);
    }
    g_clear_object (&self->session);

    G_OBJECT_CLASS (pn_sci_fi_sound_parent_class)->dispose (object);
}

static void
pn_sci_fi_sound_finalize (GObject *object)
{
    PnSciFiSound *self = PN_SCI_FI_SOUND (object);

    g_clear_pointer (&self->clip, g_free);

    G_OBJECT_CLASS (pn_sci_fi_sound_parent_class)->finalize (object);
}

/* ------------------------------------------------------------------ */
/*  Sound-packs tab state                                              */
/*                                                                     */
/*  The Playback tab and the Sound-packs tab share a couple of widgets */
/*  (the count label gets refreshed after a download or delete, the    */
/*  clip combo gets repopulated, the playback Category combo too).     */
/*  Rather than thread N pointers through every callback we attach     */
/*  a single PnSciFiTabState struct to the Sound-packs page widget and */
/*  pull it out as needed.                                             */
/* ------------------------------------------------------------------ */

typedef struct
{
    PnSciFiSound *node;            /* borrowed */
    GtkWidget    *category_combo;  /* Playback tab */
    GtkWidget    *clip_combo;      /* Playback tab */
    GtkWidget    *pack_check[PN_SCI_FI_N_CATEGORIES];
    GtkWidget    *pack_count[PN_SCI_FI_N_CATEGORIES];
    GtkWidget    *download_button;
    GtkWidget    *delete_button;
    GtkWidget    *progress;
} PnSciFiTabState;

static void sci_fi_refresh_pack_counts (PnSciFiTabState *st);
static void sci_fi_refresh_playback    (PnSciFiTabState *st);

/* ------------------------------------------------------------------ */
/*  Download                                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    gchar  *category;     /* matched pack name */
    gchar  *basename;     /* trekcore filename (last URL component)  */
    gchar  *url;          /* fully-qualified URL */
} PnSciFiClipEntry;

static void
pn_sci_fi_clip_entry_free (gpointer data)
{
    PnSciFiClipEntry *e = data;
    g_free (e->category);
    g_free (e->basename);
    g_free (e->url);
    g_free (e);
}

typedef struct
{
    PnSciFiSound    *node;        /* strong ref */
    PnSciFiTabState *state;       /* borrowed */
    gboolean         selected[PN_SCI_FI_N_CATEGORIES];
    GPtrArray       *entries;     /* of PnSciFiClipEntry*, owned */
    gsize            index;
    gsize            ok;
    gsize            skipped;
    gsize            failed;
} PnSciFiDownload;

typedef struct
{
    PnSciFiDownload *dl;
    SoupMessage     *msg;
    gsize            entry;
} PnSciFiAttempt;

static void sci_fi_download_next         (PnSciFiDownload *dl);
static void sci_fi_download_start_index  (PnSciFiDownload *dl);

static void
sci_fi_download_free (PnSciFiDownload *dl)
{
    if (dl->entries != NULL)
        g_ptr_array_unref (dl->entries);
    g_object_unref (dl->node);
    g_free (dl);
}

static void
sci_fi_progress (PnSciFiTabState *st, const gchar *line)
{
    if (st == NULL || st->progress == NULL)
        return;
    gtk_label_set_text (GTK_LABEL (st->progress), line);
}

static void
sci_fi_set_buttons_sensitive (PnSciFiTabState *st, gboolean on)
{
    if (st == NULL)
        return;
    if (st->download_button != NULL)
        gtk_widget_set_sensitive (st->download_button, on);
    if (st->delete_button != NULL)
        gtk_widget_set_sensitive (st->delete_button, on);
}

static void
on_download_read_finished (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
    SoupSession      *session = SOUP_SESSION (source);
    PnSciFiAttempt   *att     = user_data;
    PnSciFiDownload  *dl      = att->dl;
    PnSciFiClipEntry *entry   = g_ptr_array_index (dl->entries, att->entry);
    GBytes  *bytes;
    GError  *error  = NULL;
    guint    status;

    bytes = soup_session_send_and_read_finish (session, result, &error);

    if (bytes == NULL)
    {
        if (error != NULL && !g_error_matches (error, G_IO_ERROR,
                                               G_IO_ERROR_CANCELLED))
        {
            gchar *line = g_strdup_printf ("%s: %s",
                                           entry->basename, error->message);
            sci_fi_progress (dl->state, line);
            g_free (line);
        }
        g_clear_error (&error);
        dl->failed++;
        goto out;
    }

    status = soup_message_get_status (att->msg);
    if (status < 200 || status >= 300)
    {
        gchar *line = g_strdup_printf ("%s: HTTP %u",
                                       entry->basename, status);
        sci_fi_progress (dl->state, line);
        g_free (line);
        dl->failed++;
        g_bytes_unref (bytes);
        goto out;
    }

    {
        gchar *root  = sci_fi_sound_cache_dir ();
        gchar *cdir  = g_build_filename (root, entry->category, NULL);
        gchar *path  = g_build_filename (cdir, entry->basename, NULL);
        gsize  len   = 0;
        gconstpointer data = g_bytes_get_data (bytes, &len);

        g_mkdir_with_parents (cdir, 0755);

        if (!g_file_set_contents (path, data, (gssize) len, &error))
        {
            gchar *line = g_strdup_printf ("%s: write failed: %s",
                                           entry->basename,
                                           error ? error->message : "(?)");
            sci_fi_progress (dl->state, line);
            g_free (line);
            g_clear_error (&error);
            dl->failed++;
        }
        else
        {
            dl->ok++;
        }
        g_free (path);
        g_free (cdir);
        g_free (root);
    }
    g_bytes_unref (bytes);

out:
    g_object_unref (att->msg);
    g_free (att);
    sci_fi_download_next (dl);
}

/** Already-on-disk test for an entry.  Skip-on-second-run depends on
 *  this returning %TRUE for any file the previous run dropped into
 *  the category dir under its canonical basename. */
static gboolean
sci_fi_entry_is_on_disk (const PnSciFiClipEntry *e)
{
    gchar    *root = sci_fi_sound_cache_dir ();
    gchar    *path = g_build_filename (root, e->category, e->basename, NULL);
    gboolean  on   = g_file_test (path, G_FILE_TEST_IS_REGULAR);

    g_free (path);
    g_free (root);
    return on;
}

static void
sci_fi_download_next (PnSciFiDownload *dl)
{
    PnSciFiClipEntry *entry;
    PnSciFiAttempt   *att;
    gchar            *line;

    while (dl->index < dl->entries->len)
    {
        entry = g_ptr_array_index (dl->entries, dl->index);
        if (sci_fi_entry_is_on_disk (entry))
        {
            dl->skipped++;
            dl->index++;
            continue;
        }
        break;
    }

    if (dl->index >= dl->entries->len)
    {
        line = g_strdup_printf (
                "Done — %zu downloaded, %zu skipped, %zu failed.",
                dl->ok, dl->skipped, dl->failed);
        sci_fi_progress (dl->state, line);
        g_free (line);

        sci_fi_set_buttons_sensitive (dl->state, TRUE);
        sci_fi_refresh_pack_counts (dl->state);
        sci_fi_refresh_playback    (dl->state);

        sci_fi_download_free (dl);
        return;
    }

    entry = g_ptr_array_index (dl->entries, dl->index);

    line = g_strdup_printf ("Downloading %s/%s (%zu/%u)…",
                            entry->category, entry->basename,
                            dl->index + 1, dl->entries->len);
    sci_fi_progress (dl->state, line);
    g_free (line);

    att        = g_new0 (PnSciFiAttempt, 1);
    att->dl    = dl;
    att->entry = dl->index;
    att->msg   = soup_message_new (SOUP_METHOD_GET, entry->url);

    dl->index++;

    if (att->msg == NULL)
    {
        gchar *err = g_strdup_printf ("%s: invalid URL", entry->basename);
        sci_fi_progress (dl->state, err);
        g_free (err);
        g_free (att);
        dl->failed++;
        sci_fi_download_next (dl);
        return;
    }

    soup_session_send_and_read_async (dl->node->session,
                                      att->msg, G_PRIORITY_DEFAULT,
                                      dl->node->cancellable,
                                      on_download_read_finished, att);
}

/** Parse the trekcore landing page into an array of clip entries
 *  whose category passes the (caller-provided) selection filter.
 *  Unmatched URLs are silently dropped.  Duplicates across the
 *  page's "preview" and "download" cells are deduped on the
 *  (category, basename) pair. */
static GPtrArray *
sci_fi_parse_index (const gchar    *html,
                    gsize           len,
                    const gboolean  selected[PN_SCI_FI_N_CATEGORIES])
{
    GPtrArray  *entries = g_ptr_array_new_with_free_func (
            pn_sci_fi_clip_entry_free);
    GHashTable *seen    = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                 g_free, NULL);
    GError     *error   = NULL;
    GRegex     *re;
    GMatchInfo *info;

    re = g_regex_new ("href=\"([^\"]+\\.mp3)\"",
                      G_REGEX_CASELESS, 0, &error);
    if (re == NULL)
    {
        g_warning ("pn-sci-fi-sound: regex compile failed: %s",
                   error ? error->message : "(unknown)");
        g_clear_error (&error);
        g_hash_table_destroy (seen);
        return entries;
    }

    g_regex_match_full (re, html, (gssize) len, 0, 0, &info, NULL);
    while (g_match_info_matches (info))
    {
        gchar *rel = g_match_info_fetch (info, 1);

        if (rel != NULL && strstr (rel, "://") == NULL)
        {
            gchar *slash = strchr (rel, '/');

            if (slash != NULL && slash != rel)
            {
                *slash = '\0';
                const gchar *dir      = rel;
                const gchar *basename = slash + 1;
                gssize       cat_idx;

                cat_idx = sci_fi_classify (dir, basename);
                if (cat_idx >= 0 && selected[cat_idx])
                {
                    PnSciFiClipEntry *e;
                    gchar *key = g_strdup_printf (
                            "%s/%s",
                            categories[cat_idx].display, basename);

                    if (!g_hash_table_contains (seen, key))
                    {
                        e = g_new0 (PnSciFiClipEntry, 1);
                        e->category = g_strdup (categories[cat_idx].display);
                        e->basename = g_strdup (basename);
                        e->url      = g_strconcat (
                                PN_SCI_FI_INDEX_URL, dir, "/", basename, NULL);

                        g_hash_table_add (seen, key);  /* takes key */
                        g_ptr_array_add (entries, e);
                    }
                    else
                    {
                        g_free (key);
                    }
                }
                *slash = '/';
            }
        }
        g_free (rel);
        g_match_info_next (info, NULL);
    }
    g_match_info_free (info);
    g_regex_unref (re);
    g_hash_table_destroy (seen);

    return entries;
}

static void
on_index_finished (GObject      *source,
                   GAsyncResult *result,
                   gpointer      user_data)
{
    SoupSession      *session = SOUP_SESSION (source);
    PnSciFiAttempt   *att     = user_data;
    PnSciFiDownload  *dl      = att->dl;
    GBytes           *bytes;
    GError           *error   = NULL;
    guint             status;
    gchar            *line;

    bytes = soup_session_send_and_read_finish (session, result, &error);

    if (bytes == NULL)
    {
        line = g_strdup_printf ("Index fetch failed: %s",
                                error ? error->message : "(unknown)");
        sci_fi_progress (dl->state, line);
        g_free (line);
        g_clear_error (&error);
        goto fail;
    }

    status = soup_message_get_status (att->msg);
    if (status < 200 || status >= 300)
    {
        line = g_strdup_printf ("Index fetch returned HTTP %u", status);
        sci_fi_progress (dl->state, line);
        g_free (line);
        g_bytes_unref (bytes);
        goto fail;
    }

    {
        gsize len = 0;
        const gchar *html = g_bytes_get_data (bytes, &len);
        dl->entries = sci_fi_parse_index (html, len, dl->selected);
    }
    g_bytes_unref (bytes);

    if (dl->entries->len == 0)
    {
        sci_fi_progress (dl->state,
                "No clips matched the selected packs.");
        goto fail;
    }

    line = g_strdup_printf ("Found %u clips, starting download…",
                            dl->entries->len);
    sci_fi_progress (dl->state, line);
    g_free (line);

    g_object_unref (att->msg);
    g_free (att);

    sci_fi_download_next (dl);
    return;

fail:
    sci_fi_set_buttons_sensitive (dl->state, TRUE);
    g_object_unref (att->msg);
    g_free (att);
    sci_fi_download_free (dl);
}

static void
sci_fi_download_start_index (PnSciFiDownload *dl)
{
    PnSciFiAttempt *att;

    sci_fi_progress (dl->state, "Fetching index…");

    att      = g_new0 (PnSciFiAttempt, 1);
    att->dl  = dl;
    att->msg = soup_message_new (SOUP_METHOD_GET, PN_SCI_FI_INDEX_URL);

    if (att->msg == NULL)
    {
        sci_fi_progress (dl->state, "Invalid index URL.");
        sci_fi_set_buttons_sensitive (dl->state, TRUE);
        g_free (att);
        sci_fi_download_free (dl);
        return;
    }

    soup_session_send_and_read_async (dl->node->session,
                                      att->msg, G_PRIORITY_DEFAULT,
                                      dl->node->cancellable,
                                      on_index_finished, att);
}

/* ------------------------------------------------------------------ */
/*  Tab callbacks                                                      */
/* ------------------------------------------------------------------ */

/** Pull the currently checked categories out of the dialog state and
 *  return them as a fixed-size bool array indexed by category index. */
static void
sci_fi_collect_selection (PnSciFiTabState *st,
                          gboolean         out[PN_SCI_FI_N_CATEGORIES])
{
    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
        out[i] = gtk_toggle_button_get_active (
                GTK_TOGGLE_BUTTON (st->pack_check[i]));
}

static gboolean
sci_fi_selection_empty (const gboolean sel[PN_SCI_FI_N_CATEGORIES])
{
    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
        if (sel[i])
            return FALSE;
    return TRUE;
}

static void
on_download_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    PnSciFiTabState *st = user_data;
    PnSciFiSound    *self = st->node;
    PnSciFiDownload *dl;
    gboolean         sel[PN_SCI_FI_N_CATEGORIES];

    sci_fi_collect_selection (st, sel);
    if (sci_fi_selection_empty (sel))
    {
        sci_fi_progress (st, "Select at least one pack first.");
        return;
    }

    if (self->session == NULL)
        self->session = soup_session_new ();
    if (self->cancellable == NULL)
        self->cancellable = g_cancellable_new ();

    dl        = g_new0 (PnSciFiDownload, 1);
    dl->node  = g_object_ref (self);
    dl->state = st;
    memcpy (dl->selected, sel, sizeof sel);

    sci_fi_set_buttons_sensitive (st, FALSE);
    sci_fi_download_start_index (dl);
}

static void
on_delete_clicked (GtkButton *button, gpointer user_data)
{
    PnSciFiTabState *st = user_data;
    gboolean         sel[PN_SCI_FI_N_CATEGORIES];
    GtkWidget       *dialog;
    GtkWindow       *parent;
    gint             response;
    gsize            removed = 0;
    gsize            n_packs = 0;

    sci_fi_collect_selection (st, sel);
    if (sci_fi_selection_empty (sel))
    {
        sci_fi_progress (st, "Select at least one pack first.");
        return;
    }

    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
        if (sel[i])
            n_packs++;

    parent = GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (button)));
    dialog = gtk_message_dialog_new (
            parent,
            GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
            GTK_MESSAGE_QUESTION,
            GTK_BUTTONS_OK_CANCEL,
            "Delete every downloaded clip in %zu pack%s?",
            n_packs, n_packs == 1 ? "" : "s");
    response = gtk_dialog_run (GTK_DIALOG (dialog));
    gtk_widget_destroy (dialog);

    if (response != GTK_RESPONSE_OK)
        return;

    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
        if (sel[i])
            removed += sci_fi_sound_delete_category (categories[i].display);

    {
        gchar *line = g_strdup_printf ("Removed %zu file%s.",
                                       removed, removed == 1 ? "" : "s");
        sci_fi_progress (st, line);
        g_free (line);
    }

    sci_fi_refresh_pack_counts (st);
    sci_fi_refresh_playback    (st);
}

static void
on_select_all_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    PnSciFiTabState *st = user_data;
    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
        gtk_toggle_button_set_active (
                GTK_TOGGLE_BUTTON (st->pack_check[i]), TRUE);
}

static void
on_clear_selection_clicked (GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
    PnSciFiTabState *st = user_data;
    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
        gtk_toggle_button_set_active (
                GTK_TOGGLE_BUTTON (st->pack_check[i]), FALSE);
}

static void
sci_fi_refresh_pack_counts (PnSciFiTabState *st)
{
    if (st == NULL)
        return;

    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
    {
        if (st->pack_count[i] == NULL)
            continue;

        gsize n = sci_fi_sound_count_in_category (categories[i].display);
        gchar *line;

        if (n == 0)
            line = g_strdup ("—");
        else
            line = g_strdup_printf ("%zu on disk", n);

        gtk_label_set_text (GTK_LABEL (st->pack_count[i]), line);
        g_free (line);
    }
}

/* ------------------------------------------------------------------ */
/*  Playback tab plumbing                                              */
/* ------------------------------------------------------------------ */

/** Set the clip property without triggering the combo's `changed`
 *  signal recursion that would otherwise audition the clip on every
 *  rebuild. */
typedef struct
{
    PnSciFiSound *node;
    GtkWidget    *category_combo;
    GtkWidget    *clip_combo;
    gboolean      updating;
} PnSciFiPlaybackBinding;

static void sci_fi_rebuild_clip_combo (PnSciFiPlaybackBinding *pb,
                                       const gchar            *category);

static void
on_category_combo_changed (GtkComboBox *combo, gpointer user_data)
{
    PnSciFiPlaybackBinding *pb  = user_data;
    const gchar            *cat;

    if (pb->updating)
        return;

    cat = gtk_combo_box_get_active_id (combo);
    sci_fi_rebuild_clip_combo (pb, cat);
}

static void
on_clip_combo_changed (GtkComboBox *combo, gpointer user_data)
{
    PnSciFiPlaybackBinding *pb  = user_data;
    const gchar            *cat;
    const gchar            *id;
    gchar                  *clip;

    if (pb->updating)
        return;

    id  = gtk_combo_box_get_active_id (combo);
    cat = gtk_combo_box_get_active_id (
            GTK_COMBO_BOX (pb->category_combo));

    if (id == NULL || *id == '\0' || cat == NULL || *cat == '\0')
    {
        g_object_set (pb->node, "clip", NULL, NULL);
        return;
    }

    clip = g_strdup_printf ("%s/%s", cat, id);
    g_object_set (pb->node, "clip", clip, NULL);
    g_free (clip);

    sci_fi_sound_preview (pb->node);
}

static void
sci_fi_rebuild_clip_combo (PnSciFiPlaybackBinding *pb,
                           const gchar            *category)
{
    GtkComboBoxText *clip   = GTK_COMBO_BOX_TEXT (pb->clip_combo);
    GDir            *d      = NULL;
    GPtrArray       *names  = g_ptr_array_new ();
    gchar           *root;
    gchar           *dir;
    gchar           *current;

    pb->updating = TRUE;

    g_object_get (pb->node, "clip", &current, NULL);
    gtk_combo_box_text_remove_all (clip);
    gtk_combo_box_text_append (clip, "", "(none)");

    if (category != NULL && *category != '\0')
    {
        root = sci_fi_sound_cache_dir ();
        dir  = g_build_filename (root, category, NULL);
        d    = g_dir_open (dir, 0, NULL);

        if (d != NULL)
        {
            const gchar *entry;
            while ((entry = g_dir_read_name (d)) != NULL)
            {
                gchar *full = g_build_filename (dir, entry, NULL);
                if (g_file_test (full, G_FILE_TEST_IS_REGULAR))
                    g_ptr_array_add (names, g_strdup (entry));
                g_free (full);
            }
            g_dir_close (d);
        }

        g_free (dir);
        g_free (root);
    }

    g_ptr_array_sort (names, sci_fi_basename_compare);
    for (guint i = 0; i < names->len; i++)
    {
        const gchar *n = g_ptr_array_index (names, i);
        gtk_combo_box_text_append (clip, n, n);
    }
    g_ptr_array_free (names, TRUE);

    /* Try to restore the current clip if its category matches. */
    if (current != NULL)
    {
        gchar *slash = strchr (current, '/');
        if (slash != NULL && category != NULL
            && strncmp (current, category, slash - current) == 0
            && category[slash - current] == '\0')
        {
            gtk_combo_box_set_active_id (GTK_COMBO_BOX (clip), slash + 1);
        }
        else
        {
            gtk_combo_box_set_active_id (GTK_COMBO_BOX (clip), "");
        }
    }
    else
    {
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (clip), "");
    }
    g_free (current);

    pb->updating = FALSE;
}

static void
sci_fi_refresh_playback (PnSciFiTabState *st)
{
    GtkComboBoxText        *cat;
    PnSciFiPlaybackBinding *pb;
    gchar                  *current;
    const gchar            *active;

    if (st == NULL || st->category_combo == NULL)
        return;

    pb  = g_object_get_data (G_OBJECT (st->category_combo),
                             "pn-sci-fi-playback");
    if (pb == NULL)
        return;

    cat = GTK_COMBO_BOX_TEXT (st->category_combo);
    pb->updating = TRUE;

    /* Rebuild the category combo from disk. */
    g_object_get (st->node, "clip", &current, NULL);
    gtk_combo_box_text_remove_all (cat);
    gtk_combo_box_text_append (cat, "", "(pick a pack)");

    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
    {
        const gchar *name = categories[i].display;
        if (sci_fi_sound_count_in_category (name) > 0)
            gtk_combo_box_text_append (cat, name, name);
    }

    /* Re-select either the current clip's category or empty. */
    if (current != NULL)
    {
        gchar *slash = strchr (current, '/');
        if (slash != NULL)
        {
            gchar *c = g_strndup (current, slash - current);
            gtk_combo_box_set_active_id (GTK_COMBO_BOX (cat), c);
            g_free (c);
        }
        else
        {
            gtk_combo_box_set_active_id (GTK_COMBO_BOX (cat), "");
        }
    }
    else
    {
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (cat), "");
    }
    g_free (current);

    pb->updating = FALSE;

    active = gtk_combo_box_get_active_id (GTK_COMBO_BOX (cat));
    sci_fi_rebuild_clip_combo (pb, active);
}

/* ------------------------------------------------------------------ */
/*  Build the two settings-dialog tabs                                 */
/* ------------------------------------------------------------------ */

static GtkWidget *
sci_fi_build_playback_tab (PnSciFiTabState *st)
{
    GtkWidget *grid    = pn_node_dialog_new_property_grid ();
    GtkWidget *cat_row;
    GtkWidget *cat_combo;
    GtkWidget *clip_row;
    GtkWidget *clip_combo;
    GtkWidget *preview;
    GtkWidget *spin;
    GParamSpec *pspec;
    PnSciFiPlaybackBinding *pb;

    /* Row 0: Category combo. */
    cat_row   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    cat_combo = gtk_combo_box_text_new ();
    gtk_widget_set_hexpand (cat_combo, TRUE);
    gtk_box_pack_start (GTK_BOX (cat_row), cat_combo, TRUE, TRUE, 0);
    pn_node_dialog_attach_row (GTK_GRID (grid), 0, "Pack", cat_row);

    /* Row 1: Clip combo + Preview button. */
    clip_row   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    clip_combo = gtk_combo_box_text_new ();
    preview    = gtk_button_new_with_label ("Preview");
    gtk_widget_set_hexpand (clip_combo, TRUE);
    gtk_box_pack_start (GTK_BOX (clip_row), clip_combo, TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX (clip_row), preview,    FALSE, FALSE, 0);
    pn_node_dialog_attach_row (GTK_GRID (grid), 1, "Clip", clip_row);

    /* Row 2: Dead period (delegated to the host default). */
    pspec = g_object_class_find_property (
            G_OBJECT_GET_CLASS (st->node), "dead-period");
    spin  = pn_node_dialog_default_editor (G_OBJECT (st->node), pspec);
    pn_node_dialog_attach_row (GTK_GRID (grid), 2, "Dead period", spin);

    pb = g_new0 (PnSciFiPlaybackBinding, 1);
    pb->node           = st->node;
    pb->category_combo = cat_combo;
    pb->clip_combo     = clip_combo;
    pb->updating       = FALSE;

    g_object_set_data_full (G_OBJECT (cat_combo),
                            "pn-sci-fi-playback",
                            pb, g_free);

    g_signal_connect (cat_combo, "changed",
                      G_CALLBACK (on_category_combo_changed), pb);
    g_signal_connect (clip_combo, "changed",
                      G_CALLBACK (on_clip_combo_changed), pb);
    g_signal_connect_swapped (preview, "clicked",
                              G_CALLBACK (sci_fi_sound_preview), st->node);

    st->category_combo = cat_combo;
    st->clip_combo     = clip_combo;

    sci_fi_refresh_playback (st);
    return grid;
}

static GtkWidget *
sci_fi_build_packs_tab (PnSciFiTabState *st)
{
    GtkWidget *outer    = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scrolled = gtk_scrolled_window_new (NULL, NULL);
    GtkWidget *grid     = gtk_grid_new ();
    GtkWidget *button_row;
    GtkWidget *select_all;
    GtkWidget *clear;
    GtkWidget *download;
    GtkWidget *delete_btn;
    GtkWidget *progress;

    gtk_container_set_border_width (GTK_CONTAINER (outer), 6);
    gtk_grid_set_row_spacing    (GTK_GRID (grid), 4);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);

    for (gsize i = 0; i < PN_SCI_FI_N_CATEGORIES; i++)
    {
        GtkWidget *check = gtk_check_button_new_with_label (
                categories[i].display);
        GtkWidget *count = gtk_label_new ("—");

        gtk_widget_set_hexpand (check, TRUE);
        gtk_label_set_xalign  (GTK_LABEL (count), 1.0);
        gtk_widget_set_halign (count, GTK_ALIGN_END);

        gtk_grid_attach (GTK_GRID (grid), check, 0, (gint) i, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), count, 1, (gint) i, 1, 1);

        st->pack_check[i] = check;
        st->pack_count[i] = count;
    }

    sci_fi_refresh_pack_counts (st);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    gtk_widget_set_size_request (scrolled, -1, 240);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), grid);
    gtk_box_pack_start (GTK_BOX (outer), scrolled, TRUE, TRUE, 0);

    /* Selection helpers. */
    button_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    select_all = gtk_button_new_with_label ("Select all");
    clear      = gtk_button_new_with_label ("Clear");
    download   = gtk_button_new_with_label ("Download selected");
    delete_btn = gtk_button_new_with_label ("Delete selected");

    gtk_box_pack_start (GTK_BOX (button_row), select_all, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (button_row), clear,      FALSE, FALSE, 0);
    gtk_box_pack_end   (GTK_BOX (button_row), download,   FALSE, FALSE, 0);
    gtk_box_pack_end   (GTK_BOX (button_row), delete_btn, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (outer), button_row, FALSE, FALSE, 0);

    progress = gtk_label_new ("");
    gtk_label_set_xalign    (GTK_LABEL (progress), 0.0);
    gtk_label_set_line_wrap (GTK_LABEL (progress), TRUE);
    gtk_widget_set_halign   (progress, GTK_ALIGN_START);
    gtk_box_pack_start (GTK_BOX (outer), progress, FALSE, FALSE, 0);

    st->download_button = download;
    st->delete_button   = delete_btn;
    st->progress        = progress;

    g_signal_connect (select_all, "clicked",
                      G_CALLBACK (on_select_all_clicked), st);
    g_signal_connect (clear, "clicked",
                      G_CALLBACK (on_clear_selection_clicked), st);
    g_signal_connect (download, "clicked",
                      G_CALLBACK (on_download_clicked), st);
    g_signal_connect (delete_btn, "clicked",
                      G_CALLBACK (on_delete_clicked), st);

    return outer;
}

static void
pn_sci_fi_sound_build_class_tabs (PnNode      *self,
                                  GtkNotebook *notebook,
                                  GtkWindow   *parent G_GNUC_UNUSED)
{
    PnSciFiTabState *st = g_new0 (PnSciFiTabState, 1);
    GtkWidget       *playback;
    GtkWidget       *packs;

    st->node = PN_SCI_FI_SOUND (self);

    playback = sci_fi_build_playback_tab (st);
    packs    = sci_fi_build_packs_tab    (st);

    /* Lifetime: the state struct outlives the two pages it serves
     * because both pages reference its widget pointers in their
     * callbacks.  Attach it to the packs page so the host frees it
     * when the dialog tears down (the playback page's binding lives
     * on its category combo and frees itself the same way). */
    g_object_set_data_full (G_OBJECT (packs),
                            "pn-sci-fi-tab-state", st, g_free);

    pn_node_dialog_append_page (notebook, playback,  "Playback");
    pn_node_dialog_append_page (notebook, packs,     "Sound packs");
}

/* ------------------------------------------------------------------ */
/*  GType                                                              */
/* ------------------------------------------------------------------ */

static void
pn_sci_fi_sound_class_init (PnSciFiSoundClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_sci_fi_sound_get_property;
    object_class->set_property = pn_sci_fi_sound_set_property;
    object_class->dispose      = pn_sci_fi_sound_dispose;
    object_class->finalize     = pn_sci_fi_sound_finalize;

    node_class->receive           = pn_sci_fi_sound_receive;
    node_class->build_class_tabs  = pn_sci_fi_sound_build_class_tabs;

    node_class->palette_icon = PN_SCI_FI_SOUND_NORMAL_ICON;
    node_class->class_name   = "SciFi Sound";
    node_class->icon         = PN_SCI_FI_SOUND_NORMAL_ICON;
    node_class->color        = (PnColor){ 0.20, 0.55, 0.85, 1.0 };
    node_class->category     = "Sinks";
    node_class->has_input    = TRUE;
    node_class->has_output   = FALSE;

    props[PROP_CLIP] = g_param_spec_string (
            "clip", "Clip",
            "Relative path of the clip in the per-user sound-effects "
            "cache, of the form \"<Pack>/<basename>\", e.g. "
            "\"Romulan/romulan_disruptor.mp3\"",
            NULL,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_DEAD_PERIOD] = g_param_spec_uint (
            "dead-period", "Dead period",
            "Seconds of silence enforced after each playback during "
            "which incoming messages are dropped",
            PN_SCI_FI_SOUND_DEAD_PERIOD_MIN,
            PN_SCI_FI_SOUND_DEAD_PERIOD_MAX,
            PN_SCI_FI_SOUND_DEAD_PERIOD_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_sci_fi_sound_init (PnSciFiSound *self)
{
    PnNode *node = PN_NODE (self);

    self->clip        = NULL;
    self->dead_period = PN_SCI_FI_SOUND_DEAD_PERIOD_DEF;
    self->playing     = FALSE;
    self->ready_at_us = 0;
    self->session     = NULL;
    self->cancellable = NULL;

    pn_node_set_class_name (node, "SciFi Sound");
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);

    apply_visual_state (self, FALSE);
}

PnSciFiSound *
pn_sci_fi_sound_new (void)
{
    return g_object_new (PN_TYPE_SCI_FI_SOUND, NULL);
}
