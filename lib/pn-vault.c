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

#include "pn-vault.h"
#include "pn-node-factory.h"
#include "pn-profile-schema.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

/* On-disk format version of the credentials file. */
#define PN_VAULT_FILE_VERSION 1

/* ------------------------------------------------------------------ */
/*  PnProfile — one provisioned instance                               */
/* ------------------------------------------------------------------ */

struct _PnProfile
{
    gchar      *id;
    gchar      *type_id;
    gchar      *name;
    GHashTable *fields;   /* str -> str (both owned)                  */
    PnVault    *vault;    /* borrowed back-pointer for save scheduling */
};

static PnProfile *
profile_new (PnVault     *vault,
             const gchar *id,
             const gchar *type_id,
             const gchar *name)
{
    PnProfile *p = g_new0 (PnProfile, 1);

    p->id      = g_strdup (id);
    p->type_id = g_strdup (type_id);
    p->name    = g_strdup (name != NULL ? name : id);
    p->fields  = g_hash_table_new_full (g_str_hash, g_str_equal,
                                        g_free, g_free);
    p->vault   = vault;
    return p;
}

static void
profile_free (PnProfile *p)
{
    if (p == NULL)
        return;
    g_free (p->id);
    g_free (p->type_id);
    g_free (p->name);
    g_hash_table_unref (p->fields);
    g_free (p);
}

/* ------------------------------------------------------------------ */
/*  PnVault                                                            */
/* ------------------------------------------------------------------ */

struct _PnVault
{
    GObject parent_instance;

    gchar      *config_path;
    GHashTable *profiles;   /* id -> PnProfile* (owned)               */
    GHashTable *defaults;   /* type_id -> profile id (both owned str) */

    /* Held while load_from_file() populates the object so the mutators
     * it calls do not write the file back out under us. */
    gboolean    loading;
};

G_DEFINE_TYPE (PnVault, pn_vault, G_TYPE_OBJECT)

enum { SIG_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

/* ------------------------------------------------------------------ */
/*  Paths                                                              */
/* ------------------------------------------------------------------ */

static gchar *
default_config_path (void)
{
    const gchar *env = g_getenv ("PIPNODE_CREDENTIALS_FILE");
    if (env != NULL && *env != '\0')
        return g_strdup (env);

    return g_build_filename (g_get_user_config_dir (), "pipnode",
                             "credentials.json", NULL);
}

/* ------------------------------------------------------------------ */
/*  Persistence                                                        */
/* ------------------------------------------------------------------ */

/* Write the whole store to @self->config_path with the file forced to
 * mode 0600.  Atomic content via g_file_set_contents; the chmod right
 * after closes the permission window the temp-file default would leave.
 * Errors are swallowed — the in-memory state is still valid this run. */
static void
pn_vault_save_now (PnVault *self)
{
    JsonBuilder   *b;
    JsonGenerator *gen;
    JsonNode      *root;
    gchar         *text;
    gchar         *dir;
    GList         *ids, *l;

    if (self->config_path == NULL)
        return;

    b = json_builder_new ();
    json_builder_begin_object (b);

    json_builder_set_member_name (b, "version");
    json_builder_add_int_value (b, PN_VAULT_FILE_VERSION);

    /* defaults: type_id -> primary profile id, sorted for stable diffs. */
    json_builder_set_member_name (b, "defaults");
    json_builder_begin_object (b);
    {
        GList *types = g_hash_table_get_keys (self->defaults);
        types = g_list_sort (types, (GCompareFunc) g_strcmp0);
        for (l = types; l != NULL; l = l->next)
        {
            json_builder_set_member_name (b, (const gchar *) l->data);
            json_builder_add_string_value (
                    b, g_hash_table_lookup (self->defaults, l->data));
        }
        g_list_free (types);
    }
    json_builder_end_object (b);

    /* profiles: id -> { type, name, fields }, sorted by id. */
    json_builder_set_member_name (b, "profiles");
    json_builder_begin_object (b);

    ids = g_hash_table_get_keys (self->profiles);
    ids = g_list_sort (ids, (GCompareFunc) g_strcmp0);
    for (l = ids; l != NULL; l = l->next)
    {
        PnProfile *p = g_hash_table_lookup (self->profiles, l->data);
        GList     *fkeys, *fl;

        json_builder_set_member_name (b, p->id);
        json_builder_begin_object (b);

        json_builder_set_member_name (b, "type");
        json_builder_add_string_value (b, p->type_id);

        json_builder_set_member_name (b, "name");
        json_builder_add_string_value (b, p->name);

        json_builder_set_member_name (b, "fields");
        json_builder_begin_object (b);
        fkeys = g_hash_table_get_keys (p->fields);
        fkeys = g_list_sort (fkeys, (GCompareFunc) g_strcmp0);
        for (fl = fkeys; fl != NULL; fl = fl->next)
        {
            json_builder_set_member_name (b, (const gchar *) fl->data);
            json_builder_add_string_value (
                    b, g_hash_table_lookup (p->fields, fl->data));
        }
        g_list_free (fkeys);
        json_builder_end_object (b);

        json_builder_end_object (b);
    }
    g_list_free (ids);

    json_builder_end_object (b);   /* profiles */
    json_builder_end_object (b);   /* root     */

    root = json_builder_get_root (b);
    gen  = json_generator_new ();
    json_generator_set_root   (gen, root);
    json_generator_set_pretty (gen, TRUE);
    text = json_generator_to_data (gen, NULL);

    dir = g_path_get_dirname (self->config_path);
    g_mkdir_with_parents (dir, 0700);
    g_free (dir);

    if (g_file_set_contents (self->config_path, text, -1, NULL))
        g_chmod (self->config_path, 0600);

    g_free (text);
    json_node_free (root);
    g_object_unref (gen);
    g_object_unref (b);
}

static void
pn_vault_schedule_save (PnVault *self)
{
    /* Credential edits are infrequent (the manager dialog) so a direct,
     * synchronous write keeps the on-disk state correct without a running
     * main loop — which also makes the unit tests deterministic. */
    if (self->loading)
        return;
    pn_vault_save_now (self);

    /* Tell live consumers (a placed MQTT/HTTPS node following a profile) to
     * re-resolve, so creating or editing a profile takes effect immediately
     * rather than only after the node's own properties next change. */
    g_signal_emit (self, signals[SIG_CHANGED], 0);
}

static void
pn_vault_load_from_file (PnVault *self)
{
    JsonParser *parser;
    JsonNode   *root;
    JsonObject *obj;

    if (self->config_path == NULL)
        return;
    if (!g_file_test (self->config_path, G_FILE_TEST_EXISTS))
        return;

    /* Enforce 0600 on an existing file too: a store created by an older
     * build, restored from backup, or edited by hand may be too open. */
    g_chmod (self->config_path, 0600);

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

    if (json_object_has_member (obj, "profiles"))
    {
        JsonObject *profs = json_object_get_object_member (obj, "profiles");
        GList      *ids   = json_object_get_members (profs);
        GList      *l;

        for (l = ids; l != NULL; l = l->next)
        {
            const gchar *id   = l->data;
            JsonObject  *pobj = json_object_get_object_member (profs, id);
            const gchar *type;
            const gchar *name;
            PnProfile   *p;

            if (pobj == NULL)
                continue;

            type = json_object_has_member (pobj, "type")
                 ? json_object_get_string_member (pobj, "type") : NULL;
            name = json_object_has_member (pobj, "name")
                 ? json_object_get_string_member (pobj, "name") : id;

            p = profile_new (self, id, type, name);

            if (json_object_has_member (pobj, "fields"))
            {
                JsonObject *fobj =
                        json_object_get_object_member (pobj, "fields");
                GList      *fkeys = json_object_get_members (fobj);
                GList      *fl;

                for (fl = fkeys; fl != NULL; fl = fl->next)
                {
                    const gchar *fk = fl->data;
                    const gchar *fv =
                            json_object_get_string_member (fobj, fk);
                    g_hash_table_insert (p->fields, g_strdup (fk),
                                         g_strdup (fv != NULL ? fv : ""));
                }
                g_list_free (fkeys);
            }

            g_hash_table_insert (self->profiles, g_strdup (id), p);
        }
        g_list_free (ids);
    }

    if (json_object_has_member (obj, "defaults"))
    {
        JsonObject *defs  = json_object_get_object_member (obj, "defaults");
        GList      *types = json_object_get_members (defs);
        GList      *l;

        for (l = types; l != NULL; l = l->next)
        {
            const gchar *type_id = l->data;
            const gchar *pid =
                    json_object_get_string_member (defs, type_id);
            if (pid != NULL)
                g_hash_table_insert (self->defaults, g_strdup (type_id),
                                     g_strdup (pid));
        }
        g_list_free (types);
    }

    self->loading = FALSE;
    g_object_unref (parser);
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_vault_finalize (GObject *object)
{
    PnVault *self = PN_VAULT (object);

    g_clear_pointer (&self->config_path, g_free);
    g_clear_pointer (&self->profiles, g_hash_table_unref);
    g_clear_pointer (&self->defaults, g_hash_table_unref);

    G_OBJECT_CLASS (pn_vault_parent_class)->finalize (object);
}

static void
pn_vault_class_init (PnVaultClass *klass)
{
    G_OBJECT_CLASS (klass)->finalize = pn_vault_finalize;

    /**
     * PnVault::changed:
     *
     * Emitted after any change to the store (a profile created, deleted,
     * renamed, re-pointed as primary, or a field edited).  Consumers that
     * cache resolved values — a placed node holding a live broker
     * connection — reconnect from this rather than polling.
     */
    signals[SIG_CHANGED] = g_signal_new (
            "changed", PN_TYPE_VAULT, G_SIGNAL_RUN_LAST,
            0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
pn_vault_init (PnVault *self)
{
    self->profiles = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, (GDestroyNotify) profile_free);
    self->defaults = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, g_free);
    self->loading  = FALSE;
}

/* ------------------------------------------------------------------ */
/*  Construction                                                       */
/* ------------------------------------------------------------------ */

PnVault *
pn_vault_new_for_path (const gchar *path)
{
    PnVault *self = g_object_new (PN_TYPE_VAULT, NULL);

    self->config_path = g_strdup (path);
    pn_vault_load_from_file (self);
    return self;
}

PnVault *
pn_vault_get_default (void)
{
    static PnVault *singleton = NULL;

    if (G_UNLIKELY (singleton == NULL))
    {
        singleton = g_object_new (PN_TYPE_VAULT, NULL);
        singleton->config_path = default_config_path ();
        pn_vault_load_from_file (singleton);
    }
    return singleton;
}

/* ------------------------------------------------------------------ */
/*  Lookup / enumeration                                              */
/* ------------------------------------------------------------------ */

static gint
profile_cmp_by_id (gconstpointer a, gconstpointer b)
{
    return g_strcmp0 (pn_profile_get_id ((PnProfile *) a),
                      pn_profile_get_id ((PnProfile *) b));
}

GList *
pn_vault_list_profiles (PnVault *self, const gchar *type_id)
{
    GList         *out = NULL;
    GHashTableIter it;
    gpointer       v;

    g_return_val_if_fail (PN_IS_VAULT (self), NULL);

    g_hash_table_iter_init (&it, self->profiles);
    while (g_hash_table_iter_next (&it, NULL, &v))
    {
        PnProfile *p = v;
        if (type_id == NULL || g_strcmp0 (p->type_id, type_id) == 0)
            out = g_list_prepend (out, p);
    }
    return g_list_sort (out, profile_cmp_by_id);
}

PnProfile *
pn_vault_get_profile (PnVault *self, const gchar *id)
{
    g_return_val_if_fail (PN_IS_VAULT (self), NULL);
    if (id == NULL || *id == '\0')
        return NULL;
    return g_hash_table_lookup (self->profiles, id);
}

PnProfile *
pn_vault_get_default_profile (PnVault *self, const gchar *type_id)
{
    const gchar *pid;
    PnProfile   *only = NULL;
    guint        count = 0;
    GHashTableIter it;
    gpointer       v;

    g_return_val_if_fail (PN_IS_VAULT (self), NULL);
    g_return_val_if_fail (type_id != NULL, NULL);

    /* An explicit primary wins. */
    pid = g_hash_table_lookup (self->defaults, type_id);
    if (pid != NULL)
    {
        PnProfile *p = g_hash_table_lookup (self->profiles, pid);
        if (p != NULL)
            return p;
    }

    /* Otherwise, if the type has exactly one profile, treat it as primary. */
    g_hash_table_iter_init (&it, self->profiles);
    while (g_hash_table_iter_next (&it, NULL, &v))
    {
        PnProfile *p = v;
        if (g_strcmp0 (p->type_id, type_id) == 0)
        {
            only = p;
            count += 1;
        }
    }
    return count == 1 ? only : NULL;
}

void
pn_vault_set_default_profile (PnVault *self, const gchar *id)
{
    PnProfile *p;

    g_return_if_fail (PN_IS_VAULT (self));
    g_return_if_fail (id != NULL);

    p = g_hash_table_lookup (self->profiles, id);
    if (p == NULL)
    {
        g_warning ("pn_vault_set_default_profile: no profile '%s'", id);
        return;
    }

    g_hash_table_insert (self->defaults, g_strdup (p->type_id),
                         g_strdup (id));
    pn_vault_schedule_save (self);
}

/* ------------------------------------------------------------------ */
/*  Mutation                                                          */
/* ------------------------------------------------------------------ */

/* Turn a human name into a stable, file-friendly id (lowercase, dashes). */
static gchar *
slugify (const gchar *s)
{
    GString    *out = g_string_new (NULL);
    const gchar *p;
    gboolean     last_dash = TRUE;   /* trims a leading dash */

    for (p = s; p != NULL && *p != '\0'; p++)
    {
        gchar c = *p;
        if (g_ascii_isalnum (c))
        {
            g_string_append_c (out, g_ascii_tolower (c));
            last_dash = FALSE;
        }
        else if (!last_dash)
        {
            g_string_append_c (out, '-');
            last_dash = TRUE;
        }
    }
    /* trim trailing dash */
    while (out->len > 0 && out->str[out->len - 1] == '-')
        g_string_truncate (out, out->len - 1);

    if (out->len == 0)
        g_string_append (out, "profile");
    return g_string_free (out, FALSE);
}

PnProfile *
pn_vault_create_profile (PnVault     *self,
                         const gchar *type_id,
                         const gchar *name)
{
    gchar     *base;
    gchar     *id;
    guint      n = 1;
    PnProfile *p;
    gboolean   first_of_type;

    g_return_val_if_fail (PN_IS_VAULT (self), NULL);
    g_return_val_if_fail (type_id != NULL, NULL);

    base = slugify (name != NULL && *name != '\0' ? name : type_id);
    id   = g_strdup (base);
    while (g_hash_table_contains (self->profiles, id))
    {
        g_free (id);
        id = g_strdup_printf ("%s-%u", base, ++n);
    }
    g_free (base);

    first_of_type = (pn_vault_get_default_profile (self, type_id) == NULL) &&
                    (g_hash_table_lookup (self->defaults, type_id) == NULL);

    p = profile_new (self, id, type_id, name);
    g_hash_table_insert (self->profiles, g_strdup (id), p);

    /* The first profile of a type becomes its primary, so a node with an
     * empty reference immediately resolves to it. */
    if (first_of_type)
        g_hash_table_insert (self->defaults, g_strdup (type_id),
                             g_strdup (id));

    g_free (id);
    pn_vault_schedule_save (self);
    return p;
}

void
pn_vault_delete_profile (PnVault *self, const gchar *id)
{
    PnProfile *p;

    g_return_if_fail (PN_IS_VAULT (self));
    g_return_if_fail (id != NULL);

    p = g_hash_table_lookup (self->profiles, id);
    if (p == NULL)
        return;

    /* Drop a primary mapping that pointed here. */
    {
        const gchar *cur = g_hash_table_lookup (self->defaults, p->type_id);
        if (g_strcmp0 (cur, id) == 0)
            g_hash_table_remove (self->defaults, p->type_id);
    }

    g_hash_table_remove (self->profiles, id);
    pn_vault_schedule_save (self);
}

/* ------------------------------------------------------------------ */
/*  Profile identity                                                  */
/* ------------------------------------------------------------------ */

const gchar *
pn_profile_get_id (PnProfile *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->id;
}

const gchar *
pn_profile_get_type_id (PnProfile *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->type_id;
}

const gchar *
pn_profile_get_name (PnProfile *self)
{
    g_return_val_if_fail (self != NULL, NULL);
    return self->name;
}

void
pn_profile_set_name (PnProfile *self, const gchar *name)
{
    g_return_if_fail (self != NULL);

    if (g_strcmp0 (self->name, name) == 0)
        return;
    g_free (self->name);
    self->name = g_strdup (name != NULL ? name : self->id);
    if (self->vault != NULL)
        pn_vault_schedule_save (self->vault);
}

/* ------------------------------------------------------------------ */
/*  Field resolution                                                  */
/* ------------------------------------------------------------------ */

/* PIPNODE_PROFILE_<ID>_<FIELD>, with every non-alphanumeric byte folded
 * to '_' and the whole thing upper-cased. */
static gchar *
env_var_name (const gchar *id, const gchar *field)
{
    gchar *raw = g_strdup_printf ("PIPNODE_PROFILE_%s_%s", id, field);
    gchar *p;

    for (p = raw; *p != '\0'; p++)
    {
        if (g_ascii_isalnum (*p))
            *p = g_ascii_toupper (*p);
        else
            *p = '_';
    }
    /* keep the leading "PIPNODE_PROFILE_" intact (it already is alnum/_) */
    return raw;
}

static const gchar *
schema_default_for (const gchar *type_id, const gchar *field)
{
    PnNodeFactory   *factory = pn_node_factory_get_default ();
    PnProfileSchema *schema;
    gint             idx;

    schema = pn_node_factory_lookup_profile_type (factory, type_id);
    if (schema == NULL)
        return NULL;
    idx = pn_profile_schema_find_field (schema, field);
    if (idx < 0)
        return NULL;
    return pn_profile_schema_field_get_default (schema, (guint) idx);
}

/* Resolve env override > stored value > schema default.  Returns a newly
 * allocated string, or %NULL when none of the three is present. */
static gchar *
profile_resolve (PnProfile *self, const gchar *field)
{
    gchar       *env_name;
    const gchar *env_val;
    const gchar *stored;
    const gchar *def;

    env_name = env_var_name (self->id, field);
    env_val  = g_getenv (env_name);
    g_free (env_name);
    if (env_val != NULL)
        return g_strdup (env_val);

    stored = g_hash_table_lookup (self->fields, field);
    if (stored != NULL)
        return g_strdup (stored);

    def = schema_default_for (self->type_id, field);
    if (def != NULL)
        return g_strdup (def);

    return NULL;
}

static gboolean
parse_truthy (const gchar *s)
{
    if (s == NULL)
        return FALSE;
    return g_ascii_strcasecmp (s, "1") == 0 ||
           g_ascii_strcasecmp (s, "true") == 0 ||
           g_ascii_strcasecmp (s, "on") == 0 ||
           g_ascii_strcasecmp (s, "yes") == 0;
}

gchar *
pn_profile_get_string (PnProfile *self, const gchar *field)
{
    gchar *v;

    g_return_val_if_fail (self != NULL, g_strdup (""));
    g_return_val_if_fail (field != NULL, g_strdup (""));

    v = profile_resolve (self, field);
    return v != NULL ? v : g_strdup ("");
}

gchar *
pn_profile_get_secret (PnProfile *self, const gchar *field)
{
    return pn_profile_get_string (self, field);
}

gint64
pn_profile_get_int (PnProfile *self, const gchar *field)
{
    gchar  *v;
    gint64  out = 0;

    g_return_val_if_fail (self != NULL, 0);
    g_return_val_if_fail (field != NULL, 0);

    v = profile_resolve (self, field);
    if (v != NULL && *v != '\0')
        out = g_ascii_strtoll (v, NULL, 10);
    g_free (v);
    return out;
}

gboolean
pn_profile_get_bool (PnProfile *self, const gchar *field)
{
    gchar    *v;
    gboolean  out;

    g_return_val_if_fail (self != NULL, FALSE);
    g_return_val_if_fail (field != NULL, FALSE);

    v   = profile_resolve (self, field);
    out = parse_truthy (v);
    g_free (v);
    return out;
}

gboolean
pn_profile_get_permission (PnProfile *self, const gchar *field)
{
    /* A permission defaults to denied; same truthy parse as a bool. */
    return pn_profile_get_bool (self, field);
}

void
pn_profile_set_field (PnProfile   *self,
                      const gchar *field,
                      const gchar *value)
{
    g_return_if_fail (self != NULL);
    g_return_if_fail (field != NULL);

    if (value == NULL)
        g_hash_table_remove (self->fields, field);
    else
        g_hash_table_insert (self->fields, g_strdup (field),
                             g_strdup (value));

    if (self->vault != NULL)
        pn_vault_schedule_save (self->vault);
}

/* ------------------------------------------------------------------ */
/*  Node bridge                                                       */
/* ------------------------------------------------------------------ */

PnProfile *
pn_node_get_profile (PnNode *self, const gchar *ref_prop_name)
{
    GObjectClass *oc;
    GParamSpec   *pspec;
    gchar        *ref = NULL;
    PnVault      *vault;
    PnProfile    *profile = NULL;

    g_return_val_if_fail (PN_IS_NODE (self), NULL);
    g_return_val_if_fail (ref_prop_name != NULL, NULL);

    oc    = G_OBJECT_GET_CLASS (self);
    pspec = g_object_class_find_property (oc, ref_prop_name);
    if (pspec == NULL || !G_IS_PARAM_SPEC_STRING (pspec))
    {
        g_warning ("pn_node_get_profile: '%s' is not a string property on %s",
                   ref_prop_name, G_OBJECT_TYPE_NAME (self));
        return NULL;
    }

    g_object_get (self, ref_prop_name, &ref, NULL);
    vault = pn_vault_get_default ();

    if (ref != NULL && *ref != '\0')
        profile = pn_vault_get_profile (vault, ref);

    /* Empty reference — or a dangling one — follows the type's primary. */
    if (profile == NULL)
    {
        const gchar *type_id = pn_param_spec_get_profile_ref (pspec);
        if (type_id != NULL)
            profile = pn_vault_get_default_profile (vault, type_id);
    }

    g_free (ref);
    return profile;
}
