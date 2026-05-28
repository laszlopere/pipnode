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

#ifndef PN_VAULT_H
#define PN_VAULT_H

#include <glib-object.h>

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnVault                                                             */
/*                                                                     */
/*  The host's store of provisioned profile *instances* — the values   */
/*  behind the types a plugin declared with                            */
/*  pn_node_factory_register_profile_type().  A profile is a named bag  */
/*  of field values (host, secret, permission grant, …) of one type.   */
/*  Nodes never store these themselves; they keep only a non-secret    */
/*  reference (a profile id, or empty to follow the type's primary) and */
/*  resolve the real values here at runtime through                    */
/*  pn_node_get_profile().                                             */
/*                                                                     */
/*  Storage is a single JSON file in the user's config dir, created and */
/*  kept at mode 0600, deliberately separate from both the workflow     */
/*  file (so workflows carry no secrets and stay shareable) and from    */
/*  preferences.json.  The vault is GTK-free and lives in the core, so  */
/*  headless #pipnode-run resolves secrets with no display.  Field      */
/*  resolution consults, in order: an environment override, then the    */
/*  stored value, then the schema default — so a server or CI can       */
/*  inject a secret with PIPNODE_PROFILE_<ID>_<FIELD> without writing   */
/*  the file at all.                                                    */
/* ------------------------------------------------------------------ */

#define PN_TYPE_VAULT (pn_vault_get_type ())

G_DECLARE_FINAL_TYPE (PnVault, pn_vault, PN, VAULT, GObject)

/**
 * PnProfile:
 *
 * One provisioned instance of a profile type.  Opaque and owned by the
 * #PnVault that vends it; valid until that profile is deleted.
 */
typedef struct _PnProfile PnProfile;

/**
 * pn_vault_get_default:
 *
 * Returns: (transfer none): the process-wide vault, constructed and loaded
 *   from the default path on first call.  Owned by the module; do not unref.
 */
PnVault *pn_vault_get_default (void);

/**
 * pn_vault_new_for_path:
 * @path: the JSON file to back this vault with
 *
 * Constructs a standalone vault bound to @path (loaded immediately if it
 * exists).  Intended for tests and for explicit non-default stores; the
 * caller owns the returned reference.
 *
 * Returns: (transfer full): a new vault.
 */
PnVault *pn_vault_new_for_path (const gchar *path);

/* ---- profile lookup / enumeration ------------------------------- */

/**
 * pn_vault_list_profiles:
 * @self:    a vault
 * @type_id: (nullable): restrict to this profile type, or %NULL for all
 *
 * Returns: (transfer container) (element-type PnProfile): the matching
 *   profiles.  The profiles are borrowed; free the list with g_list_free().
 */
GList *pn_vault_list_profiles (PnVault *self, const gchar *type_id);

/**
 * pn_vault_get_profile:
 * @self: a vault
 * @id:   a profile id
 *
 * Returns: (transfer none) (nullable): the profile with @id, or %NULL.
 */
PnProfile *pn_vault_get_profile (PnVault *self, const gchar *id);

/**
 * pn_vault_get_default_profile:
 * @self:    a vault
 * @type_id: a profile type id
 *
 * Returns the type's primary profile — the one a node with an empty
 * reference follows.  Falls back to the only profile of that type when none
 * is explicitly marked primary.
 *
 * Returns: (transfer none) (nullable): the primary profile, or %NULL.
 */
PnProfile *pn_vault_get_default_profile (PnVault *self, const gchar *type_id);

/**
 * pn_vault_set_default_profile:
 * @self: a vault
 * @id:   the profile to mark primary for its type
 */
void pn_vault_set_default_profile (PnVault *self, const gchar *id);

/* ---- mutation (editor side) ------------------------------------- */

/**
 * pn_vault_create_profile:
 * @self:    a vault
 * @type_id: the profile type
 * @name:    a human name for the instance
 *
 * Creates an empty profile of @type_id with a freshly-allocated unique id.
 * If it is the first profile of its type it also becomes the primary.
 *
 * Returns: (transfer none): the new profile, owned by the vault.
 */
PnProfile *pn_vault_create_profile (PnVault     *self,
                                    const gchar *type_id,
                                    const gchar *name);

/**
 * pn_vault_delete_profile:
 * @self: a vault
 * @id:   the profile to remove
 */
void pn_vault_delete_profile (PnVault *self, const gchar *id);

/**
 * pn_vault_import_inline_profile:
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
gchar *pn_vault_import_inline_profile (const gchar        *type_id,
                                       const gchar        *suggested_name,
                                       const gchar *const *names,
                                       const gchar *const *values,
                                       guint               n);

/* ---- profile identity ------------------------------------------- */

const gchar *pn_profile_get_id      (PnProfile *self);
const gchar *pn_profile_get_type_id (PnProfile *self);
const gchar *pn_profile_get_name    (PnProfile *self);
void         pn_profile_set_name    (PnProfile *self, const gchar *name);

/* ---- field resolution (env override > stored > schema default) --- */

/**
 * pn_profile_get_string:
 * Returns: (transfer full): the resolved value of @field, never %NULL (an
 *   empty string when nothing is set).  Free with g_free().
 */
gchar    *pn_profile_get_string     (PnProfile *self, const gchar *field);

/**
 * pn_profile_get_secret:
 *
 * Same resolution as pn_profile_get_string(); named apart so credential reads
 * read as such at the call site.
 *
 * Returns: (transfer full): the resolved secret, never %NULL.  Free with
 *   g_free() (consider g_free()-after-use for secrets).
 */
gchar    *pn_profile_get_secret     (PnProfile *self, const gchar *field);

gint64    pn_profile_get_int        (PnProfile *self, const gchar *field);
gboolean  pn_profile_get_bool       (PnProfile *self, const gchar *field);

/**
 * pn_profile_get_permission:
 *
 * Resolves a %PN_FIELD_PERMISSION field.  Defaults to %FALSE — a permission is
 * denied unless the user explicitly granted it.
 */
gboolean  pn_profile_get_permission (PnProfile *self, const gchar *field);

/**
 * pn_profile_set_field:
 * @self:  a profile
 * @field: the field name
 * @value: (nullable): the raw string value to store, or %NULL to clear it
 *
 * Stores @value as the field's value and schedules a save.  Booleans /
 * permissions are stored as "true"/"false"; integers as decimal text.
 */
void      pn_profile_set_field      (PnProfile   *self,
                                     const gchar *field,
                                     const gchar *value);

/* ---- node bridge ------------------------------------------------ */

/**
 * pn_node_get_profile:
 * @self:          a node
 * @ref_prop_name: the name of a string property tagged with
 *                 pn_param_spec_set_profile_ref()
 *
 * Reads @self's @ref_prop_name property: if it names a profile, returns that
 * one; if it is empty (or names a profile that no longer exists), falls back to
 * the primary profile of the type the property was tagged with.  This is the
 * single call a node makes at FIRE time to obtain its credentials / settings /
 * permissions from the host.
 *
 * Returns: (transfer none) (nullable): the resolved profile, or %NULL when
 *   none is configured.
 */
PnProfile *pn_node_get_profile (PnNode *self, const gchar *ref_prop_name);

G_END_DECLS

#endif /* PN_VAULT_H */
