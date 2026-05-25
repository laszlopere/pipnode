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

#ifndef PN_NETWORK_PROFILES_H
#define PN_NETWORK_PROFILES_H

#include <glib.h>

#include "pn-node-factory.h"

G_BEGIN_DECLS

/* The host-provisioned profile types this plugin's nodes reference.  The
 * MQTT Source / Sink share "mqtt-broker"; the HTTPS Tunnel Sender / Receiver
 * share "http-basic". */
#define PN_NETWORK_PROFILE_MQTT_BROKER "mqtt-broker"
#define PN_NETWORK_PROFILE_HTTP_BASIC  "http-basic"

/**
 * pn_network_register_profile_types:
 * @factory: the process-wide factory
 *
 * Registers the "mqtt-broker" and "http-basic" profile types.  Called once
 * from the plugin's pn_plugin_init().
 */
void pn_network_register_profile_types (PnNodeFactory *factory);

/**
 * pn_network_import_profile:
 * @type_id:        the profile type to find or create an instance of
 * @suggested_name: a human name for a newly-created profile
 * @names:  (array length=n): field names to match / set
 * @values: (array length=n): the corresponding values (a %NULL entry is
 *          treated as the empty string)
 * @n:      number of fields
 *
 * Idempotently materialises a profile in the default vault for a legacy node
 * carrying inline connection values: if a profile of @type_id already holds
 * exactly these field values it is reused, otherwise a new one is created (and,
 * if it is the first of its type, becomes the primary).  This is the one-time
 * import that lets an old workflow's plaintext secret move into the 0600 vault
 * the first time the file is opened.
 *
 * Returns: (transfer full) (nullable): the id of the matching or new profile,
 *   or %NULL on failure.  Free with g_free().
 */
gchar *pn_network_import_profile (const gchar        *type_id,
                                  const gchar        *suggested_name,
                                  const gchar *const *names,
                                  const gchar *const *values,
                                  guint               n);

G_END_DECLS

#endif /* PN_NETWORK_PROFILES_H */
