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

#include "pn-settings-renderer.h"

#include "pn-node-dialog-helpers.h"

#include <gtksourceview/gtksource.h>

/* ------------------------------------------------------------------ */
/*  Multi-line string editor                                           */
/*                                                                     */
/*  GtkTextView in a scroller backing a string property, with the      */
/*  same re-entrancy-guarded binding the auto dialog uses for its       */
/*  pn_param_spec_set_multiline() editor (a naïve bind to              */
/*  GtkTextBuffer:text yanks the cursor to the end on every keystroke   */
/*  as the property re-emits notify).  Backs %PN_EDITOR_MULTILINE;      */
/*  %PN_EDITOR_CODE has its own GtkSourceView editor below, sharing     */
/*  this same #PnTextBinding.                                           */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject       *target;     /* borrowed                              */
    const gchar   *property;   /* static, from pspec->name              */
    GtkTextBuffer *buffer;
    gulong         notify_handler;
    gboolean       updating;
} PnTextBinding;

static void
text_binding_free (gpointer data)
{
    PnTextBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
text_binding_pull (PnTextBinding *bind)
{
    gchar       *value = NULL;
    GtkTextIter  start;
    GtkTextIter  end;
    gchar       *current;

    if (bind->updating)
        return;

    g_object_get (bind->target, bind->property, &value, NULL);

    gtk_text_buffer_get_bounds (bind->buffer, &start, &end);
    current = gtk_text_buffer_get_text (bind->buffer, &start, &end, FALSE);

    if (g_strcmp0 (current, value ? value : "") != 0)
    {
        bind->updating = TRUE;
        gtk_text_buffer_set_text (bind->buffer, value ? value : "", -1);
        bind->updating = FALSE;
    }

    g_free (current);
    g_free (value);
}

static void
text_binding_push (PnTextBinding *bind)
{
    GtkTextIter  start;
    GtkTextIter  end;
    gchar       *text;

    if (bind->updating)
        return;

    gtk_text_buffer_get_bounds (bind->buffer, &start, &end);
    text = gtk_text_buffer_get_text (bind->buffer, &start, &end, FALSE);

    bind->updating = TRUE;
    g_object_set (bind->target, bind->property, text, NULL);
    bind->updating = FALSE;

    g_free (text);
}

static void
on_text_target_notify (GObject    *object G_GNUC_UNUSED,
                       GParamSpec *pspec  G_GNUC_UNUSED,
                       gpointer    user_data)
{
    text_binding_pull (user_data);
}

static void
on_text_buffer_changed (GtkTextBuffer *buffer G_GNUC_UNUSED,
                        gpointer       user_data)
{
    text_binding_push (user_data);
}

static GtkWidget *
build_multiline_editor (GObject    *target,
                        GParamSpec *pspec)
{
    const gchar   *name     = pspec->name;
    gboolean       writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GtkWidget     *scrolled = gtk_scrolled_window_new (NULL, NULL);
    GtkWidget     *view     = gtk_text_view_new ();
    GtkTextBuffer *buffer   = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    PnTextBinding *bind;
    gchar         *signal_name;

    gtk_text_view_set_wrap_mode   (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace   (GTK_TEXT_VIEW (view), TRUE);
    gtk_text_view_set_accepts_tab (GTK_TEXT_VIEW (view), FALSE);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    gtk_widget_set_size_request (scrolled, -1, 110);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, FALSE);

    gtk_container_add (GTK_CONTAINER (scrolled), view);

    bind = g_new0 (PnTextBinding, 1);
    bind->target   = target;
    bind->property = name;
    bind->buffer   = buffer;

    text_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_text_target_notify), bind);
    g_free (signal_name);

    if (writable)
        g_signal_connect (buffer, "changed",
                          G_CALLBACK (on_text_buffer_changed), bind);

    gtk_text_view_set_editable (GTK_TEXT_VIEW (view), writable);
    gtk_widget_set_sensitive (scrolled, writable);

    g_object_set_data_full (G_OBJECT (scrolled),
                            "pn-text-binding", bind, text_binding_free);

    return scrolled;
}

/* ------------------------------------------------------------------ */
/*  Code editor (GtkSourceView)                                        */
/*                                                                     */
/*  %PN_EDITOR_CODE: a GtkSourceView with JSON syntax highlighting,    */
/*  line numbers and bracket matching, bound to a string property      */
/*  through the same re-entrancy-guarded #PnTextBinding the multiline   */
/*  editor uses (a #GtkSourceBuffer is a #GtkTextBuffer).  This is the  */
/*  declarative home of the editor that used to live in the deleted     */
/*  pn-rewrite-gui.c; it is GUI-tier only, so the headless core never   */
/*  pulls GtkSourceView.                                                */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_code_editor (GObject    *target,
                   GParamSpec *pspec)
{
    const gchar              *name     = pspec->name;
    gboolean                  writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GtkSourceLanguageManager *langs    =
            gtk_source_language_manager_get_default ();
    GtkSourceLanguage        *json     =
            gtk_source_language_manager_get_language (langs, "json");
    GtkSourceBuffer          *buffer   =
            gtk_source_buffer_new_with_language (json);
    GtkWidget                *view;
    GtkWidget                *scrolled = gtk_scrolled_window_new (NULL, NULL);
    PnTextBinding            *bind;
    gchar                    *signal_name;

    gtk_source_buffer_set_highlight_syntax            (buffer, TRUE);
    gtk_source_buffer_set_highlight_matching_brackets (buffer, TRUE);

    view = gtk_source_view_new_with_buffer (buffer);
    gtk_source_view_set_show_line_numbers      (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_auto_indent            (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_indent_on_tab          (GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_tab_width              (GTK_SOURCE_VIEW (view), 2);
    gtk_source_view_set_insert_spaces_instead_of_tabs (
            GTK_SOURCE_VIEW (view), TRUE);
    gtk_source_view_set_highlight_current_line (GTK_SOURCE_VIEW (view), TRUE);
    gtk_text_view_set_monospace (GTK_TEXT_VIEW (view), TRUE);
    g_object_unref (buffer);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    /* Twelve-ish lines: the default JSON template plus a few added
     * fields before the editor feels cramped (matching the old tab). */
    gtk_widget_set_size_request (scrolled, -1, 240);
    gtk_widget_set_hexpand (scrolled, TRUE);
    gtk_widget_set_vexpand (scrolled, TRUE);
    gtk_container_add (GTK_CONTAINER (scrolled), view);

    bind = g_new0 (PnTextBinding, 1);
    bind->target   = target;
    bind->property = name;
    bind->buffer   = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));

    text_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_text_target_notify), bind);
    g_free (signal_name);

    if (writable)
        g_signal_connect (bind->buffer, "changed",
                          G_CALLBACK (on_text_buffer_changed), bind);

    gtk_text_view_set_editable (GTK_TEXT_VIEW (view), writable);
    gtk_widget_set_sensitive (scrolled, writable);

    g_object_set_data_full (G_OBJECT (scrolled),
                            "pn-text-binding", bind, text_binding_free);

    return scrolled;
}

/* ------------------------------------------------------------------ */
/*  File-chooser editor (%PN_EDITOR_FILE)                              */
/*                                                                     */
/*  GtkFileChooserButton bound to a path-string property.  The button  */
/*  exposes no bindable "filename" GObject property, so the two ends    */
/*  are synced by hand with the same re-entrancy guard as the text      */
/*  editor above.                                                       */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject        *target;    /* borrowed                              */
    const gchar    *property;  /* static, from pspec->name              */
    GtkFileChooser *chooser;
    gulong          notify_handler;
    gboolean        updating;
} PnFileBinding;

static void
file_binding_free (gpointer data)
{
    PnFileBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
file_binding_pull (PnFileBinding *bind)
{
    gchar *value = NULL;

    if (bind->updating)
        return;

    g_object_get (bind->target, bind->property, &value, NULL);

    bind->updating = TRUE;
    if (value != NULL && *value != '\0')
        gtk_file_chooser_set_filename (bind->chooser, value);
    else
        gtk_file_chooser_unselect_all (bind->chooser);
    bind->updating = FALSE;

    g_free (value);
}

static void
on_file_target_notify (GObject    *object G_GNUC_UNUSED,
                       GParamSpec *pspec  G_GNUC_UNUSED,
                       gpointer    user_data)
{
    file_binding_pull (user_data);
}

static void
on_file_set (GtkFileChooserButton *button G_GNUC_UNUSED,
             gpointer              user_data)
{
    PnFileBinding *bind = user_data;
    gchar         *filename;

    if (bind->updating)
        return;

    filename = gtk_file_chooser_get_filename (bind->chooser);

    bind->updating = TRUE;
    g_object_set (bind->target, bind->property,
                  filename != NULL ? filename : "", NULL);
    bind->updating = FALSE;

    g_free (filename);
}

static GtkWidget *
build_file_editor (GObject    *target,
                   GParamSpec *pspec)
{
    const gchar   *name     = pspec->name;
    gboolean       writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GtkWidget     *button   = gtk_file_chooser_button_new (
            g_param_spec_get_nick (pspec) != NULL
                ? g_param_spec_get_nick (pspec) : "Select File",
            GTK_FILE_CHOOSER_ACTION_OPEN);
    PnFileBinding *bind;
    gchar         *signal_name;

    gtk_widget_set_hexpand (button, TRUE);

    bind = g_new0 (PnFileBinding, 1);
    bind->target   = target;
    bind->property = name;
    bind->chooser  = GTK_FILE_CHOOSER (button);

    file_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_file_target_notify), bind);
    g_free (signal_name);

    if (writable)
        g_signal_connect (button, "file-set",
                          G_CALLBACK (on_file_set), bind);

    gtk_widget_set_sensitive (button, writable);

    g_object_set_data_full (G_OBJECT (button),
                            "pn-file-binding", bind, file_binding_free);

    return button;
}

/* ------------------------------------------------------------------ */
/*  Simple editors (entry / check / combo-from-choices / label)        */
/* ------------------------------------------------------------------ */

/* Single-line entry, forced even for a string carrying the multiline
 * hint (the point of %PN_EDITOR_ENTRY).  GLib's value transforms cover a
 * non-string property bound to the entry's "text". */
static GtkWidget *
build_entry_editor (GObject    *target,
                    GParamSpec *pspec)
{
    gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);
    GtkWidget    *entry    = gtk_entry_new ();

    gtk_widget_set_hexpand   (entry, TRUE);
    gtk_widget_set_sensitive (entry, writable);
    g_object_bind_property (target, pspec->name, entry, "text", flags);

    return entry;
}

static GtkWidget *
build_check_editor (GObject    *target,
                    GParamSpec *pspec)
{
    gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);
    GtkWidget    *check    = gtk_check_button_new ();

    gtk_widget_set_sensitive (check, writable);
    g_object_bind_property (target, pspec->name, check, "active", flags);

    return check;
}

/* Combo over an explicit, non-enum choice list.  Each choice is both
 * the visible label and the stored value, so the combo's "active-id"
 * (a string) binds straight to a string property — the non-enum
 * counterpart of the auto dialog's enum combo. */
static GtkWidget *
build_choices_editor (GObject            *target,
                      GParamSpec         *pspec,
                      const gchar *const *choices)
{
    gboolean      writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);
    GtkWidget    *combo    = gtk_combo_box_text_new ();
    guint         i;

    for (i = 0; choices != NULL && choices[i] != NULL; i++)
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo),
                                   choices[i], choices[i]);

    gtk_widget_set_hexpand   (combo, TRUE);
    gtk_widget_set_sensitive (combo, writable);
    g_object_bind_property (target, pspec->name, combo, "active-id", flags);

    return combo;
}

/* Read-only, selectable label rendering the property's current value —
 * the explicit %PN_EDITOR_LABEL counterpart of the auto dialog's
 * final-fallback label. */
static GtkWidget *
build_label_editor (GObject    *target,
                    GParamSpec *pspec)
{
    GValue     value = G_VALUE_INIT;
    gchar     *text;
    GtkWidget *label;

    g_value_init (&value, G_PARAM_SPEC_VALUE_TYPE (pspec));
    g_object_get_property (target, pspec->name, &value);
    text  = g_strdup_value_contents (&value);
    label = gtk_label_new (text != NULL ? text : "");

    gtk_widget_set_halign (label, GTK_ALIGN_START);
    gtk_label_set_selectable (GTK_LABEL (label), TRUE);

    g_free (text);
    g_value_unset (&value);

    return label;
}

/* ------------------------------------------------------------------ */
/*  Conditional sensitivity (enable-when)                              */
/* ------------------------------------------------------------------ */

static gboolean
parse_bool (const gchar *s)
{
    return g_ascii_strcasecmp (s, "true") == 0 ||
           g_ascii_strcasecmp (s, "yes")  == 0 ||
           g_ascii_strcasecmp (s, "on")   == 0 ||
           g_strcmp0 (s, "1") == 0;
}

static gboolean
gvalue_as_double (const GValue *v, gdouble *out)
{
    GType t = G_VALUE_TYPE (v);

    if (t == G_TYPE_INT)         { *out = g_value_get_int    (v); return TRUE; }
    if (t == G_TYPE_UINT)        { *out = g_value_get_uint   (v); return TRUE; }
    if (t == G_TYPE_LONG)        { *out = g_value_get_long   (v); return TRUE; }
    if (t == G_TYPE_ULONG)       { *out = g_value_get_ulong  (v); return TRUE; }
    if (t == G_TYPE_INT64)       { *out = g_value_get_int64  (v); return TRUE; }
    if (t == G_TYPE_UINT64)      { *out = g_value_get_uint64 (v); return TRUE; }
    if (t == G_TYPE_FLOAT)       { *out = g_value_get_float  (v); return TRUE; }
    if (t == G_TYPE_DOUBLE)      { *out = g_value_get_double (v); return TRUE; }

    return FALSE;
}

/* Evaluate the row's enable-when test against @target's current state.
 * @when_value == NULL is the truthy test; otherwise it is parsed against
 * @when_prop's type (enum by nick/name, boolean by true/1/yes/on, number
 * by value, string by equality).  An unresolved controller leaves the
 * row enabled (nothing to gate on). */
static gboolean
condition_satisfied (GObject     *target,
                     const gchar *when_prop,
                     const gchar *when_value)
{
    GParamSpec *pspec = g_object_class_find_property (
            G_OBJECT_GET_CLASS (target), when_prop);
    GType       ptype;
    GValue      val = G_VALUE_INIT;
    gboolean    result = TRUE;

    if (pspec == NULL)
        return TRUE;

    ptype = G_PARAM_SPEC_VALUE_TYPE (pspec);
    g_value_init (&val, ptype);
    g_object_get_property (target, when_prop, &val);

    if (when_value == NULL)
    {
        /* Truthy test. */
        if (G_TYPE_IS_ENUM (ptype))
            result = g_value_get_enum (&val) != 0;
        else if (ptype == G_TYPE_BOOLEAN)
            result = g_value_get_boolean (&val);
        else if (ptype == G_TYPE_STRING)
        {
            const gchar *s = g_value_get_string (&val);
            result = (s != NULL && *s != '\0');
        }
        else
        {
            gdouble d = 0.0;
            if (gvalue_as_double (&val, &d))
                result = (d != 0.0);
        }
    }
    else if (G_TYPE_IS_ENUM (ptype))
    {
        GEnumClass *ec = g_type_class_ref (ptype);
        GEnumValue *ev = g_enum_get_value_by_nick (ec, when_value);

        if (ev == NULL)
            ev = g_enum_get_value_by_name (ec, when_value);

        result = (ev != NULL)
                 ? (g_value_get_enum (&val) == ev->value)
                 : (g_value_get_enum (&val) == atoi (when_value));
        g_type_class_unref (ec);
    }
    else if (ptype == G_TYPE_BOOLEAN)
    {
        result = (g_value_get_boolean (&val) == parse_bool (when_value));
    }
    else if (ptype == G_TYPE_STRING)
    {
        result = (g_strcmp0 (g_value_get_string (&val), when_value) == 0);
    }
    else
    {
        gdouble cur = 0.0;
        if (gvalue_as_double (&val, &cur))
            result = (cur == g_ascii_strtod (when_value, NULL));
    }

    g_value_unset (&val);
    return result;
}

typedef struct
{
    GObject     *target;        /* borrowed; outlives the editor          */
    const gchar *when_prop;     /* borrowed from the schema (process life) */
    const gchar *when_value;    /* borrowed; NULL == truthy test           */
    GtkWidget   *editor;        /* borrowed                                */
    gboolean     base_sensitive;/* sensitivity the builder left, ANDed in  */
} PnSenseBinding;

static void
sense_apply (PnSenseBinding *bind)
{
    gboolean ok = condition_satisfied (bind->target,
                                       bind->when_prop, bind->when_value);
    gtk_widget_set_sensitive (bind->editor, bind->base_sensitive && ok);
}

static void
on_controller_notify (GObject    *controller G_GNUC_UNUSED,
                      GParamSpec *pspec      G_GNUC_UNUSED,
                      gpointer    editor)
{
    PnSenseBinding *bind = g_object_get_data (G_OBJECT (editor),
                                              "pn-sense-binding");
    if (bind != NULL)
        sense_apply (bind);
}

/* Wire @editor's sensitivity to follow @when_prop on @target.  The
 * handler is scoped to @editor with g_signal_connect_object(), so it
 * auto-disconnects when the editor is destroyed (the node outlives the
 * dialog).  Nothing happens if the controller property does not exist. */
static void
install_sensitivity (GObject     *target,
                     GtkWidget   *editor,
                     const gchar *when_prop,
                     const gchar *when_value)
{
    PnSenseBinding *bind;
    gchar          *signal_name;

    if (when_prop == NULL)
        return;
    if (g_object_class_find_property (G_OBJECT_GET_CLASS (target),
                                      when_prop) == NULL)
        return;

    bind = g_new0 (PnSenseBinding, 1);
    bind->target         = target;
    bind->when_prop      = when_prop;
    bind->when_value     = when_value;
    bind->editor         = editor;
    bind->base_sensitive = gtk_widget_get_sensitive (editor);

    g_object_set_data_full (G_OBJECT (editor),
                            "pn-sense-binding", bind, g_free);

    signal_name = g_strdup_printf ("notify::%s", when_prop);
    g_signal_connect_object (target, signal_name,
                             G_CALLBACK (on_controller_notify),
                             editor, 0);
    g_free (signal_name);

    sense_apply (bind);
}

/* ------------------------------------------------------------------ */
/*  Row → editor                                                       */
/* ------------------------------------------------------------------ */

/* Build the editor for @pspec honouring the schema row attributes,
 * binding to @target.  @kind == %PN_EDITOR_AUTO (and any kind whose
 * widget the type-driven default already produces) delegates to
 * pn_node_dialog_default_editor(), so the schema only ever overrides the
 * default, never regresses it. */
static GtkWidget *
build_row_editor (GObject            *target,
                  GParamSpec         *pspec,
                  PnEditorKind        kind,
                  const gchar *const *choices,
                  PnRowFlags          flags,
                  const gchar        *when_prop,
                  const gchar        *when_value)
{
    GtkWidget *editor;

    switch (kind)
    {
    case PN_EDITOR_ENTRY:
        editor = build_entry_editor (target, pspec);
        break;
    case PN_EDITOR_CHECK:
        editor = build_check_editor (target, pspec);
        break;
    case PN_EDITOR_MULTILINE:
        editor = build_multiline_editor (target, pspec);
        break;
    case PN_EDITOR_CODE:
        editor = build_code_editor (target, pspec);
        break;
    case PN_EDITOR_COMBO:
        editor = (choices != NULL)
                 ? build_choices_editor (target, pspec, choices)
                 /* No explicit list: an enum gets the default enum combo. */
                 : pn_node_dialog_default_editor (target, pspec);
        break;
    case PN_EDITOR_FILE:
        editor = build_file_editor (target, pspec);
        break;
    case PN_EDITOR_LABEL:
        editor = build_label_editor (target, pspec);
        break;
    case PN_EDITOR_SPIN:
    case PN_EDITOR_COLOR:
    case PN_EDITOR_AUTO:
    default:
        /* The type-driven default already produces the right widget for
         * these (spin for numerics, colour button for #PnColor); going
         * through the host keeps range extraction and the #PnColor↔GdkRGBA
         * transforms in one place. */
        editor = pn_node_dialog_default_editor (target, pspec);
        break;
    }

    /* Stamp the same property-derived widget name the default editor
     * uses, so a functional test finds the editor regardless of the path
     * that built it (pn_node_dialog_default_editor() already named the
     * delegated ones; re-stamping with the identical name is harmless). */
    if (editor != NULL && pspec->name != NULL)
    {
        gchar *wname = g_strconcat ("pn-prop-", pspec->name, NULL);
        gtk_widget_set_name (editor, wname);
        g_free (wname);
    }

    if ((flags & PN_ROW_FLAG_READONLY) != 0)
        gtk_widget_set_sensitive (editor, FALSE);

    /* Wire conditional sensitivity AFTER the readonly flag so the base
     * sensitivity it captures already reflects every static decision. */
    install_sensitivity (target, editor, when_prop, when_value);

    return editor;
}

/* ------------------------------------------------------------------ */
/*  Public: single-property editor                                     */
/* ------------------------------------------------------------------ */

/* Locate the row describing @prop anywhere in @schema. */
static gboolean
find_row_for_prop (PnSettingsSchema *schema,
                   const gchar      *prop,
                   guint            *tab_out,
                   guint            *row_out)
{
    guint n_tabs = pn_settings_schema_get_n_tabs (schema);
    guint t;

    for (t = 0; t < n_tabs; t++)
    {
        guint n_rows = pn_settings_schema_get_n_rows (schema, t);
        guint r;

        for (r = 0; r < n_rows; r++)
        {
            if (g_strcmp0 (pn_settings_schema_row_prop (schema, t, r),
                           prop) == 0)
            {
                *tab_out = t;
                *row_out = r;
                return TRUE;
            }
        }
    }

    return FALSE;
}

GtkWidget *
pn_settings_render_editor (GObject          *target,
                           GParamSpec       *pspec,
                           PnSettingsSchema *schema)
{
    guint tab;
    guint row;

    g_return_val_if_fail (G_IS_OBJECT (target), NULL);
    g_return_val_if_fail (pspec != NULL, NULL);

    if (schema != NULL &&
        find_row_for_prop (schema, pspec->name, &tab, &row))
    {
        return build_row_editor (
                target, pspec,
                pn_settings_schema_row_kind       (schema, tab, row),
                pn_settings_schema_row_choices    (schema, tab, row),
                pn_settings_schema_row_get_flags  (schema, tab, row),
                pn_settings_schema_row_when_prop  (schema, tab, row),
                pn_settings_schema_row_when_value (schema, tab, row));
    }

    /* No schema, or no row for this property: exactly the default. */
    return pn_node_dialog_default_editor (target, pspec);
}

/* ------------------------------------------------------------------ */
/*  Public: render the schema's tabs                                   */
/* ------------------------------------------------------------------ */

/* Append one tab's rows to a fresh property grid.  Returns the grid, or
 * %NULL if no row yielded a widget (every property unresolved). */
static GtkWidget *
build_tab_grid (PnSettingsSchema *schema,
                guint             tab,
                PnNode           *node)
{
    GObjectClass *klass   = G_OBJECT_GET_CLASS (node);
    GtkWidget    *grid    = pn_node_dialog_new_property_grid ();
    guint         n_rows  = pn_settings_schema_get_n_rows (schema, tab);
    guint         grid_row = 0;
    guint         r;

    for (r = 0; r < n_rows; r++)
    {
        const gchar *prop  = pn_settings_schema_row_prop (schema, tab, r);
        GParamSpec  *pspec = g_object_class_find_property (klass, prop);
        PnRowFlags   flags;
        GtkWidget   *editor;

        /* Silently skip a row whose property does not exist or cannot be
         * edited (read-only via LABEL is still fine; construct-only would
         * raise a CRITICAL on g_object_set, so it is dropped — matching
         * the auto tab in pn-node-dialog.c). */
        if (pspec == NULL)
            continue;
        if ((pspec->flags & G_PARAM_CONSTRUCT_ONLY) != 0)
            continue;

        flags = pn_settings_schema_row_get_flags (schema, tab, r);

        /* A hidden row is omitted entirely — never built, no grid cell. */
        if ((flags & PN_ROW_FLAG_HIDDEN) != 0)
            continue;

        editor = build_row_editor (
                G_OBJECT (node), pspec,
                pn_settings_schema_row_kind       (schema, tab, r),
                pn_settings_schema_row_choices    (schema, tab, r),
                flags,
                pn_settings_schema_row_when_prop  (schema, tab, r),
                pn_settings_schema_row_when_value (schema, tab, r));

        if ((flags & PN_ROW_FLAG_FULL_WIDTH) != 0)
        {
            /* No label cell: span the editor across both columns and let
             * it grow to fill the page, the way a big expression / code
             * body reads best (the multiline editor caps its own minimum
             * height, so vexpand only adds the slack above that). */
            gtk_widget_set_hexpand (editor, TRUE);
            gtk_widget_set_vexpand (editor, TRUE);
            gtk_grid_attach (GTK_GRID (grid), editor, 0, grid_row, 2, 1);
        }
        else
        {
            const gchar *nick = g_param_spec_get_nick (pspec);
            if (nick == NULL || *nick == '\0')
                nick = pspec->name;
            pn_node_dialog_attach_row (GTK_GRID (grid), grid_row, nick, editor);
        }

        grid_row++;
    }

    if (grid_row == 0)
    {
        g_object_ref_sink (grid);
        g_object_unref (grid);
        return NULL;
    }

    return grid;
}

guint
pn_settings_schema_render_class (PnSettingsSchema *schema,
                                 PnNode           *node,
                                 GtkNotebook      *notebook,
                                 GtkWindow        *parent G_GNUC_UNUSED)
{
    guint n_tabs;
    guint appended = 0;
    guint t;

    g_return_val_if_fail (schema != NULL, 0);
    g_return_val_if_fail (PN_IS_NODE (node), 0);
    g_return_val_if_fail (GTK_IS_NOTEBOOK (notebook), 0);

    n_tabs = pn_settings_schema_get_n_tabs (schema);

    for (t = 0; t < n_tabs; t++)
    {
        const gchar *title = pn_settings_schema_get_tab_title (schema, t);
        GtkWidget   *page  = build_tab_grid (schema, t, node);

        if (page == NULL)
            continue;

        pn_node_dialog_append_page (notebook, page,
                                    title != NULL ? title : "");
        appended++;
    }

    return appended;
}
