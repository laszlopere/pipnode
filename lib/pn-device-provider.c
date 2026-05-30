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

#include "pn-device-provider.h"

/* One registered provider.  Owned by the registry list below. */
typedef struct
{
    gchar                       *id;
    gchar                       *display_name;
    gchar                       *icon_name;     /* may be NULL */
    PnDeviceProviderPresentFunc  present;
    gpointer                     user_data;
    GDestroyNotify               destroy;
} Provider;

/* Registration-ordered list of Provider*.  Process-global, main-thread
 * only; never freed (providers live for the process lifetime). */
static GList *providers = NULL;

static Provider *
provider_find (const gchar *id)
{
    GList *l;

    for (l = providers; l != NULL; l = l->next)
    {
        Provider *p = l->data;
        if (g_strcmp0 (p->id, id) == 0)
            return p;
    }
    return NULL;
}

void
pn_device_provider_register (const gchar                 *id,
                             const gchar                 *display_name,
                             const gchar                 *icon_name,
                             PnDeviceProviderPresentFunc  present,
                             gpointer                     user_data,
                             GDestroyNotify               destroy)
{
    Provider *p;

    g_return_if_fail (id != NULL && *id != '\0');
    g_return_if_fail (display_name != NULL);
    g_return_if_fail (present != NULL);

    /* Re-registering an id replaces the prior entry in place: free its
     * payload (running the old destroy) and overwrite, keeping its list
     * position so the menu order is stable across a companion reload. */
    p = provider_find (id);
    if (p != NULL)
    {
        if (p->destroy != NULL)
            p->destroy (p->user_data);
        g_free (p->display_name);
        g_free (p->icon_name);
    }
    else
    {
        p = g_slice_new0 (Provider);
        p->id = g_strdup (id);
        providers = g_list_append (providers, p);
    }

    p->display_name = g_strdup (display_name);
    p->icon_name    = g_strdup (icon_name);
    p->present      = present;
    p->user_data    = user_data;
    p->destroy      = destroy;
}

void
pn_device_provider_info_free (PnDeviceProviderInfo *info)
{
    if (info == NULL)
        return;
    g_free (info->id);
    g_free (info->display_name);
    g_free (info->icon_name);
    g_slice_free (PnDeviceProviderInfo, info);
}

GList *
pn_device_provider_list (void)
{
    GList *out = NULL;
    GList *l;

    for (l = providers; l != NULL; l = l->next)
    {
        Provider             *p    = l->data;
        PnDeviceProviderInfo *info = g_slice_new0 (PnDeviceProviderInfo);

        info->id           = g_strdup (p->id);
        info->display_name = g_strdup (p->display_name);
        info->icon_name    = g_strdup (p->icon_name);
        out = g_list_prepend (out, info);
    }
    return g_list_reverse (out);
}

gboolean
pn_device_provider_present (const gchar *id, GtkWindow *parent)
{
    Provider *p;

    g_return_val_if_fail (id != NULL, FALSE);
    g_return_val_if_fail (parent == NULL || GTK_IS_WINDOW (parent), FALSE);

    p = provider_find (id);
    if (p == NULL)
        return FALSE;

    p->present (parent, p->user_data);
    return TRUE;
}
