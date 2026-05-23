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

#ifndef PN_SETTINGS_SCHEMA_H
#define PN_SETTINGS_SCHEMA_H

#include <glib-object.h>

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSettingsSchema                                                    */
/*                                                                     */
/*  A GTK-free, declarative description of a node type's settings      */
/*  dialog, built imperatively in `_class_init` and attached to the    */
/*  class.  The headless core only ever holds the *data*; the GUI tier */
/*  (lib/pn-settings-renderer.c) turns it into GtkWidgets when the      */
/*  editor opens a node dialog.  This is the "default path" of decision */
/*  D2 in the headless split (see PLUGINS section 16): a node           */
/*  or plugin that only needs the common dialog customisations — tab    */
/*  grouping, an explicit editor kind, a choice list, a row greyed out  */
/*  by another property — needs no GTK and no `-gui.so` companion at    */
/*  all.  Anything the schema cannot express still falls through to the */
/*  imperative `build_*` vfuncs on #PnNodeClass, which always win.      */
/*                                                                     */
/*  The schema is stored as per-#GType qdata (like the                 */
/*  pn_param_spec_set_multiline() hint), so attaching one does NOT      */
/*  change the #PnNodeClass struct layout or the plugin ABI.           */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SETTINGS_SCHEMA (pn_settings_schema_get_type ())

typedef struct _PnSettingsSchema PnSettingsSchema;

/**
 * PnEditorKind:
 * @PN_EDITOR_AUTO:      pick the editor from the property's #GType, exactly
 *                       as the introspection-driven default tab does today
 *                       (the right choice for plain tab grouping).
 * @PN_EDITOR_ENTRY:     single-line #GtkEntry (force, even for non-strings).
 * @PN_EDITOR_MULTILINE: tall #GtkTextView in a scroller (string only).
 * @PN_EDITOR_SPIN:      #GtkSpinButton (numeric).
 * @PN_EDITOR_CHECK:     #GtkCheckButton (boolean).
 * @PN_EDITOR_COMBO:     #GtkComboBoxText populated from an explicit, non-enum
 *                       choice list (see pn_settings_schema_choices()).
 * @PN_EDITOR_COLOR:     #GtkColorButton (#PnColor / #GdkRGBA).
 * @PN_EDITOR_FILE:      file-chooser button bound to a path string.
 * @PN_EDITOR_CODE:      #GtkSourceView code editor (added in Phase 7.5 for
 *                       JSON / expression bodies); renders as a plain
 *                       multiline editor if GtkSourceView is unavailable.
 * @PN_EDITOR_LABEL:     read-only selectable #GtkLabel.
 *
 * How the renderer should present a property's value.  @PN_EDITOR_AUTO
 * defers to the host default; every other value overrides it.
 */
typedef enum
{
    PN_EDITOR_AUTO = 0,
    PN_EDITOR_ENTRY,
    PN_EDITOR_MULTILINE,
    PN_EDITOR_SPIN,
    PN_EDITOR_CHECK,
    PN_EDITOR_COMBO,
    PN_EDITOR_COLOR,
    PN_EDITOR_FILE,
    PN_EDITOR_CODE,
    PN_EDITOR_LABEL
} PnEditorKind;

/**
 * PnRowFlags:
 * @PN_ROW_FLAG_NONE:       no special layout.
 * @PN_ROW_FLAG_FULL_WIDTH: span the editor across both grid columns with no
 *                          label cell — for big editors (expression bodies)
 *                          that read better full width.
 * @PN_ROW_FLAG_READONLY:   present the editor disabled regardless of the
 *                          property's writability.
 *
 * Per-row layout tweaks, OR-able.
 */
typedef enum
{
    PN_ROW_FLAG_NONE       = 0,
    PN_ROW_FLAG_FULL_WIDTH = 1 << 0,
    PN_ROW_FLAG_READONLY   = 1 << 1
} PnRowFlags;

GType pn_settings_schema_get_type (void) G_GNUC_CONST;

/* ---- lifecycle -------------------------------------------------- */

/**
 * pn_settings_schema_new:
 *
 * Create an empty schema.  Build it up with the functions below from a
 * type's `_class_init`, then hand it to pn_node_class_set_settings_schema().
 *
 * Returns: (transfer full): a new schema; release with
 *   pn_settings_schema_unref() (or hand to the class, which takes the ref).
 */
PnSettingsSchema *pn_settings_schema_new   (void);
PnSettingsSchema *pn_settings_schema_ref   (PnSettingsSchema *self);
void              pn_settings_schema_unref (PnSettingsSchema *self);

/* ---- builder ---------------------------------------------------- */

/**
 * pn_settings_schema_tab:
 * @self:  a schema
 * @title: tab label (copied)
 *
 * Open a new named tab; subsequent pn_settings_schema_row() calls attach to
 * it.  A schema that names at least one tab REPLACES this class's single
 * auto-generated property tab with one page per named tab (the declarative
 * counterpart of #PnNodeClass.build_class_tabs).  A schema with no named tab
 * keeps the single auto tab and only customises individual rows.
 */
void pn_settings_schema_tab (PnSettingsSchema *self, const gchar *title);

/**
 * pn_settings_schema_row:
 * @self: a schema
 * @prop: the GObject property name this row edits (copied)
 * @kind: the editor to use, or %PN_EDITOR_AUTO to infer from the type
 *
 * Append a row for @prop to the current tab (or the implicit default tab if
 * no pn_settings_schema_tab() has been opened).  Rows render top-to-bottom
 * in call order.  A property whose name resolves to nothing writable on the
 * class is silently skipped at render time, so lists can be authored plainly.
 */
void pn_settings_schema_row (PnSettingsSchema *self,
                             const gchar      *prop,
                             PnEditorKind      kind);

/**
 * pn_settings_schema_choices:
 * @self:    a schema
 * @prop:    the property a previous pn_settings_schema_row() added
 * @choices: (array zero-terminated=1): %NULL-terminated label list (copied)
 *
 * Attach an explicit choice list to @prop's row and force it to a combo.
 * For string properties each choice is both the visible label and the stored
 * value; this is the non-enum analogue of the automatic enum combo.
 */
void pn_settings_schema_choices (PnSettingsSchema   *self,
                                 const gchar        *prop,
                                 const gchar *const *choices);

/**
 * pn_settings_schema_row_flags:
 * @self:  a schema
 * @prop:  the property a previous pn_settings_schema_row() added
 * @flags: layout flags to set on the row
 */
void pn_settings_schema_row_flags (PnSettingsSchema *self,
                                   const gchar      *prop,
                                   PnRowFlags        flags);

/**
 * pn_settings_schema_enable_when_eq:
 * @self:      a schema
 * @prop:      the dependent property (a previous row)
 * @when_prop: the controller property on the same node
 * @value:     the controller value, as a string, that enables @prop
 *
 * Make @prop's editor sensitive only while @when_prop equals @value.  The
 * string is parsed against @when_prop's type at render time: an enum matches
 * by nick or name, a boolean by `true`/`false`/`1`/`0`, a number by value.
 * (Example: Led's `hold-ms` is enabled only when `mode` == `flash`.)
 */
void pn_settings_schema_enable_when_eq (PnSettingsSchema *self,
                                        const gchar      *prop,
                                        const gchar      *when_prop,
                                        const gchar      *value);

/**
 * pn_settings_schema_enable_when_truthy:
 * @self:      a schema
 * @prop:      the dependent property (a previous row)
 * @when_prop: the controller property on the same node
 *
 * Make @prop's editor sensitive only while @when_prop is "truthy" — a
 * nonzero number, %TRUE boolean, or non-empty string.
 */
void pn_settings_schema_enable_when_truthy (PnSettingsSchema *self,
                                            const gchar      *prop,
                                            const gchar      *when_prop);

/* ---- per-class attachment --------------------------------------- */

/**
 * pn_node_class_set_settings_schema:
 * @klass:  a #PnNodeClass (typically `self` in `_class_init`)
 * @schema: (transfer full): the schema to attach
 *
 * Attach @schema to @klass, taking ownership of the caller's ref.  Stored as
 * qdata on the class's #GType — classes live for the whole process, so the
 * schema is never freed and this changes neither the class struct nor the
 * plugin ABI.  Calling twice replaces (and unrefs) the previous schema.
 */
void              pn_node_class_set_settings_schema (PnNodeClass      *klass,
                                                     PnSettingsSchema *schema);

/**
 * pn_node_class_get_settings_schema:
 * @klass: a #PnNodeClass
 *
 * Returns: (transfer none) (nullable): the schema attached to @klass's own
 *   #GType (not inherited), or %NULL.
 */
PnSettingsSchema *pn_node_class_get_settings_schema (PnNodeClass      *klass);

/* ---- read-back (used by the GUI renderer and the unit test) ----- */

gboolean            pn_settings_schema_has_tabs        (PnSettingsSchema *self);
guint               pn_settings_schema_get_n_tabs      (PnSettingsSchema *self);
const gchar        *pn_settings_schema_get_tab_title   (PnSettingsSchema *self,
                                                        guint             tab);
guint               pn_settings_schema_get_n_rows      (PnSettingsSchema *self,
                                                        guint             tab);
const gchar        *pn_settings_schema_row_prop        (PnSettingsSchema *self,
                                                        guint             tab,
                                                        guint             row);
PnEditorKind        pn_settings_schema_row_kind        (PnSettingsSchema *self,
                                                        guint             tab,
                                                        guint             row);
PnRowFlags          pn_settings_schema_row_get_flags   (PnSettingsSchema *self,
                                                        guint             tab,
                                                        guint             row);
const gchar *const *pn_settings_schema_row_choices     (PnSettingsSchema *self,
                                                        guint             tab,
                                                        guint             row);
const gchar        *pn_settings_schema_row_when_prop   (PnSettingsSchema *self,
                                                        guint             tab,
                                                        guint             row);
/* Returns the equality target, or %NULL for a truthy test / no condition;
 * distinguish the two with pn_settings_schema_row_when_prop() != NULL. */
const gchar        *pn_settings_schema_row_when_value  (PnSettingsSchema *self,
                                                        guint             tab,
                                                        guint             row);

G_END_DECLS

#endif /* PN_SETTINGS_SCHEMA_H */
