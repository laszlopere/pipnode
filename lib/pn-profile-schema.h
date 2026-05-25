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

#ifndef PN_PROFILE_SCHEMA_H
#define PN_PROFILE_SCHEMA_H

#include <glib-object.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnProfileSchema                                                     */
/*                                                                     */
/*  A GTK-free, declarative description of a host-provisioned profile   */
/*  *type*: a stable id ("kodi-server", "mqtt-broker"), a display name, */
/*  and an ordered list of typed fields.  A plugin builds one in        */
/*  #pn_plugin_init and hands it to                                     */
/*  pn_node_factory_register_profile_type(); the host then lets the     */
/*  user create named *instances* (see #PnVault) and a node references  */
/*  one through a property tagged with pn_param_spec_set_profile_ref().  */
/*                                                                     */
/*  The schema is the data half of the credentials feature, exactly as  */
/*  #PnSettingsSchema is the data half of the node settings dialog: the */
/*  headless core only ever holds it, and the editor's credentials      */
/*  manager turns it into widgets when the user provisions a profile.   */
/*                                                                     */
/*  One field *kind* unifies the three scopes the user asked for —      */
/*  secrets, plain settings, and permission grants — so a single store  */
/*  and a single resolver cover all three.                             */
/* ------------------------------------------------------------------ */

#define PN_TYPE_PROFILE_SCHEMA (pn_profile_schema_get_type ())

typedef struct _PnProfileSchema PnProfileSchema;

/**
 * PnProfileFieldKind:
 * @PN_FIELD_STRING:     plain setting, free text (host, URL, client id).
 * @PN_FIELD_INT:        plain setting, integer (a port).
 * @PN_FIELD_BOOL:       plain setting, boolean toggle.
 * @PN_FIELD_ENUM:       plain setting chosen from an explicit list (see
 *                       pn_profile_schema_field_choices()).
 * @PN_FIELD_SECRET:     a credential — a password / token / API key.  The
 *                       manager masks it; it lives in the 0600 vault file
 *                       and never in a workflow file.
 * @PN_FIELD_PERMISSION: a capability grant.  Defaults to %FALSE and must be
 *                       explicitly enabled by the user (rendered as a warning-
 *                       styled checkbox); a node consults it at runtime before
 *                       taking the gated action.
 *
 * The kind drives both how the credentials manager presents a field and how
 * #PnVault resolves it (string / int / bool).
 */
typedef enum
{
    PN_FIELD_STRING = 0,
    PN_FIELD_INT,
    PN_FIELD_BOOL,
    PN_FIELD_ENUM,
    PN_FIELD_SECRET,
    PN_FIELD_PERMISSION
} PnProfileFieldKind;

GType pn_profile_schema_get_type (void) G_GNUC_CONST;

/* ---- lifecycle -------------------------------------------------- */

/**
 * pn_profile_schema_new:
 * @type_id:      stable machine id, e.g. "mqtt-broker" (copied).  This is the
 *                key a node's profile-ref tag and the vault file refer to.
 * @display_name: human label shown in the manager, e.g. "MQTT Broker".
 *
 * Returns: (transfer full): a new, empty schema; add fields with
 *   pn_profile_schema_field() then register it with the factory (which takes
 *   the ref), or release with pn_profile_schema_unref().
 */
PnProfileSchema *pn_profile_schema_new   (const gchar *type_id,
                                          const gchar *display_name);
PnProfileSchema *pn_profile_schema_ref   (PnProfileSchema *self);
void             pn_profile_schema_unref (PnProfileSchema *self);

/* ---- builder ---------------------------------------------------- */

/**
 * pn_profile_schema_field:
 * @self:  a schema
 * @name:  machine name of the field, e.g. "password" (copied)
 * @label: human label shown in the manager (copied)
 * @kind:  the field kind
 *
 * Append a field.  Fields render and serialize in call order.
 */
void pn_profile_schema_field (PnProfileSchema   *self,
                              const gchar       *name,
                              const gchar       *label,
                              PnProfileFieldKind kind);

/**
 * pn_profile_schema_field_default:
 * @self:          a schema
 * @name:          a field added by a previous pn_profile_schema_field()
 * @default_value: the value resolved when neither an env override nor a stored
 *                 value is present (copied)
 */
void pn_profile_schema_field_default (PnProfileSchema *self,
                                      const gchar     *name,
                                      const gchar     *default_value);

/**
 * pn_profile_schema_field_choices:
 * @self:    a schema
 * @name:    a field added by a previous pn_profile_schema_field()
 * @choices: (array zero-terminated=1): %NULL-terminated label list (copied)
 *
 * Attach the choice list for a %PN_FIELD_ENUM field.
 */
void pn_profile_schema_field_choices (PnProfileSchema    *self,
                                      const gchar        *name,
                                      const gchar *const *choices);

/* ---- read-back (manager, vault, tests) -------------------------- */

const gchar        *pn_profile_schema_get_type_id      (PnProfileSchema *self);
const gchar        *pn_profile_schema_get_display_name (PnProfileSchema *self);
guint               pn_profile_schema_get_n_fields     (PnProfileSchema *self);
const gchar        *pn_profile_schema_field_name       (PnProfileSchema *self,
                                                        guint            index);
const gchar        *pn_profile_schema_field_get_label  (PnProfileSchema *self,
                                                        guint            index);
PnProfileFieldKind  pn_profile_schema_field_get_kind   (PnProfileSchema *self,
                                                        guint            index);
const gchar        *pn_profile_schema_field_get_default (PnProfileSchema *self,
                                                         guint            index);
const gchar *const *pn_profile_schema_field_get_choices (PnProfileSchema *self,
                                                         guint            index);

/**
 * pn_profile_schema_find_field:
 * @self: a schema
 * @name: a field name
 *
 * Returns: the index of the field named @name, or -1 if absent.
 */
gint pn_profile_schema_find_field (PnProfileSchema *self, const gchar *name);

G_END_DECLS

#endif /* PN_PROFILE_SCHEMA_H */
