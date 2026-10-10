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

#include "pn-pipe-common.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

GType
pn_pipe_format_get_type (void)
{
    static gsize id = 0;

    if (g_once_init_enter (&id))
    {
        /* The nicks become the combo labels; the numeric values stay
           fixed so saved projects keep working. */
        static const GEnumValue values[] = {
            { PN_PIPE_FORMAT_OUTPUT, "PN_PIPE_FORMAT_OUTPUT", "Output text"  },
            { PN_PIPE_FORMAT_JSON,   "PN_PIPE_FORMAT_JSON",   "JSON message" },
            { 0, NULL, NULL }
        };

        GType type = g_enum_register_static ("PnPipeFormat", values);
        g_once_init_leave (&id, type);
    }

    return id;
}

/* Create or validate the FIFO at @path; *@created tells whether this
 * call made it, @st receives its stat. */
static gboolean
ensure_fifo (const gchar *path, gboolean *created, GStatBuf *st,
             GError **error)
{
    *created = FALSE;

    if (path == NULL || *path == '\0')
    {
        g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                             "no pipe path set");
        return FALSE;
    }

    if (g_stat (path, st) != 0)
    {
        gint err = errno;

        if (err != ENOENT)
        {
            g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                         "cannot access '%s': %s", path, g_strerror (err));
            return FALSE;
        }

        if (mkfifo (path, 0666) == 0)
            *created = TRUE;
        else if (errno != EEXIST)
        {
            err = errno;
            g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                         "cannot create pipe '%s': %s", path,
                         g_strerror (err));
            return FALSE;
        }

        /* Re-check: EEXIST may mean someone raced us with a plain file. */
        if (g_stat (path, st) != 0)
        {
            err = errno;
            g_set_error (error, G_IO_ERROR, g_io_error_from_errno (err),
                         "cannot access '%s': %s", path, g_strerror (err));
            return FALSE;
        }
    }

    if (!S_ISFIFO (st->st_mode))
    {
        *created = FALSE;
        g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_REGULAR_FILE,
                     "'%s' exists but is not a named pipe", path);
        return FALSE;
    }

    return TRUE;
}

gboolean
pn_pipe_ensure_fifo (const gchar *path, GError **error)
{
    GStatBuf st;
    gboolean created;

    return ensure_fifo (path, &created, &st, error);
}

/* ------------------------------------------------------------------ */
/*  Claims: which FIFOs this process uses, and which it created        */
/* ------------------------------------------------------------------ */

typedef struct
{
    guint    users;    /* claims held on the path                     */
    gboolean created;  /* this process made the FIFO now at the path  */
    dev_t    dev;      /* identity of that FIFO, so a replacement     */
    ino_t    ino;      /* someone else put there is never unlinked    */
} PipeUse;

G_LOCK_DEFINE_STATIC (claims);
static GHashTable *claims;   /* path -> PipeUse */

gboolean
pn_pipe_claim (gchar **claimed, const gchar *path, GError **error)
{
    GStatBuf st;
    gboolean created;
    PipeUse *use;

    g_return_val_if_fail (claimed != NULL, FALSE);

    if (*claimed != NULL && g_strcmp0 (*claimed, path) != 0)
        pn_pipe_release (claimed, TRUE);

    if (!ensure_fifo (path, &created, &st, error))
    {
        pn_pipe_release (claimed, FALSE);
        return FALSE;
    }

    G_LOCK (claims);
    if (claims == NULL)
        claims = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, g_free);
    use = g_hash_table_lookup (claims, path);
    if (use == NULL)
    {
        use = g_new0 (PipeUse, 1);
        g_hash_table_insert (claims, g_strdup (path), use);
    }
    if (created)
    {
        use->created = TRUE;
        use->dev     = st.st_dev;
        use->ino     = st.st_ino;
    }
    if (*claimed == NULL)
    {
        use->users++;
        *claimed = g_strdup (path);
    }
    G_UNLOCK (claims);

    return TRUE;
}

void
pn_pipe_release (gchar **claimed, gboolean remove)
{
    PipeUse *use;

    g_return_if_fail (claimed != NULL);

    if (*claimed == NULL)
        return;

    G_LOCK (claims);
    use = claims != NULL ? g_hash_table_lookup (claims, *claimed) : NULL;
    if (use != NULL && --use->users == 0)
    {
        GStatBuf st;

        if (remove && use->created &&
            g_lstat (*claimed, &st) == 0 && S_ISFIFO (st.st_mode) &&
            st.st_dev == use->dev && st.st_ino == use->ino)
            g_unlink (*claimed);
        g_hash_table_remove (claims, *claimed);
    }
    G_UNLOCK (claims);

    g_clear_pointer (claimed, g_free);
}

gssize
pn_pipe_write (gint fd, const gchar *buf, gsize len)
{
    sigset_t        pipe_set;
    sigset_t        old_set;
    sigset_t        pending;
    gboolean        was_pending;
    gssize          n;
    gint            err;

    /* Block SIGPIPE on this thread for the duration of the write.  If the
     * write raises one (EPIPE) and it was not already pending before, eat
     * it with a zero-timeout sigtimedwait so unblocking does not deliver
     * it.  Process-wide dispositions are left alone. */
    sigemptyset (&pipe_set);
    sigaddset (&pipe_set, SIGPIPE);
    pthread_sigmask (SIG_BLOCK, &pipe_set, &old_set);

    sigpending (&pending);
    was_pending = sigismember (&pending, SIGPIPE);

    do
        n = write (fd, buf, len);
    while (n < 0 && errno == EINTR);
    err = errno;

    if (n < 0 && err == EPIPE && !was_pending)
    {
        const struct timespec zero = { 0, 0 };

        while (sigtimedwait (&pipe_set, NULL, &zero) < 0 && errno == EINTR)
            ;
    }

    pthread_sigmask (SIG_SETMASK, &old_set, NULL);
    errno = err;
    return n;
}
