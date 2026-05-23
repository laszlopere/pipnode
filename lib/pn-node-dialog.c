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

#include "pn-node-dialog.h"
#include "pn-node-dialog-helpers.h"
#include "pn-node.h"

#include <glib/gi18n.h>

/* ------------------------------------------------------------------ */
/*  PnNodeDialog                                                       */
/*                                                                     */
/*  See pn-node-dialog.h for the high-level description.  This file   */
/*  walks the node's GObject type chain from #PnNode down to the      */
/*  leaf type, partitions the properties by their declaring class    */
/*  (pspec->owner_type), and builds one notebook tab per class that  */
/*  contributes any property.                                          */
/* ------------------------------------------------------------------ */

struct _PnNodeDialog
{
    GtkDialog parent_instance;

    PnNode    *node;
    GtkWidget *notebook;
    GtkWidget *busy_overlay;   /* centered spinner + label, hidden when idle */
};

G_DEFINE_TYPE (PnNodeDialog, pn_node_dialog, GTK_TYPE_DIALOG)

enum {
    PROP_0,
    PROP_NODE,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  PnPoint editor                                                     */
/*                                                                     */
/*  GtkSpinButton pair backing a single PN_TYPE_POINT property.  The  */
/*  helper struct lives until the dialog is finalised; we attach it   */
/*  to one of the spin buttons so it tracks its lifetime.             */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject       *target;          /* borrowed */
    const gchar   *property;        /* static, from pspec->name */
    GtkSpinButton *spin_x;
    GtkSpinButton *spin_y;
    gulong         notify_handler;
    gboolean       updating;        /* re-entrancy guard */
} PnPointBinding;

static void
point_binding_free (gpointer data)
{
    PnPointBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
point_binding_pull (PnPointBinding *bind)
{
    PnPoint *value = NULL;

    if (bind->updating)
        return;

    g_object_get (bind->target, bind->property, &value, NULL);
    if (value == NULL)
        return;

    bind->updating = TRUE;
    gtk_spin_button_set_value (bind->spin_x, value->x);
    gtk_spin_button_set_value (bind->spin_y, value->y);
    bind->updating = FALSE;

    pn_point_free (value);
}

static void
point_binding_push (PnPointBinding *bind)
{
    PnPoint p;

    if (bind->updating)
        return;

    p.x = gtk_spin_button_get_value (bind->spin_x);
    p.y = gtk_spin_button_get_value (bind->spin_y);

    bind->updating = TRUE;
    g_object_set (bind->target, bind->property, &p, NULL);
    bind->updating = FALSE;
}

static void
on_point_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    point_binding_pull (user_data);
}

static void
on_point_spin_changed (
        GtkSpinButton *spin,
        gpointer       user_data)
{
    (void) spin;

    point_binding_push (user_data);
}

/* ------------------------------------------------------------------ */
/*  Enum editor                                                        */
/*                                                                     */
/*  GtkComboBoxText backed by an enum property.  We marshal between   */
/*  the integer enum value and the combo's "active-id" string in      */
/*  both directions and gate each side with a re-entrancy flag so a   */
/*  push does not loop back through the matching pull.                */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject     *target;        /* borrowed */
    const gchar *property;      /* static: pspec->name */
    GtkComboBox *combo;
    gulong       notify_handler;
    gboolean     updating;
} PnEnumBinding;

static void
enum_binding_free (gpointer data)
{
    PnEnumBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
enum_binding_pull (PnEnumBinding *bind)
{
    gint   current = 0;
    gchar *id;

    if (bind->updating)
        return;

    g_object_get (bind->target, bind->property, &current, NULL);
    id = g_strdup_printf ("%d", current);

    bind->updating = TRUE;
    gtk_combo_box_set_active_id (bind->combo, id);
    bind->updating = FALSE;

    g_free (id);
}

static void
on_enum_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    enum_binding_pull (user_data);
}

static void
on_enum_combo_changed (
        GtkComboBox *combo,
        gpointer     user_data)
{
    PnEnumBinding *bind = user_data;
    const gchar   *id;

    if (bind->updating)
        return;

    id = gtk_combo_box_get_active_id (combo);
    if (id == NULL)
        return;

    bind->updating = TRUE;
    g_object_set (bind->target, bind->property, atoi (id), NULL);
    bind->updating = FALSE;
}

static GtkWidget *
build_enum_editor (
        GObject    *target,
        GParamSpec *pspec)
{
    GType          ptype     = G_PARAM_SPEC_VALUE_TYPE (pspec);
    const gchar   *name      = pspec->name;
    gboolean       writable  = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GtkWidget     *combo     = gtk_combo_box_text_new ();
    GEnumClass    *eclass    = g_type_class_ref (ptype);
    PnEnumBinding *bind;
    gchar         *signal_name;
    guint          i;

    for (i = 0; i < eclass->n_values; i++)
    {
        const GEnumValue *v   = &eclass->values[i];
        gchar            *id  = g_strdup_printf ("%d", v->value);
        const gchar      *lbl = (v->value_nick != NULL && *v->value_nick != '\0')
                                    ? v->value_nick : v->value_name;

        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (combo), id, lbl);
        g_free (id);
    }

    g_type_class_unref (eclass);

    bind = g_new0 (PnEnumBinding, 1);
    bind->target   = target;
    bind->property = name;
    bind->combo    = GTK_COMBO_BOX (combo);

    enum_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_enum_target_notify), bind);
    g_free (signal_name);

    if (writable)
        g_signal_connect (combo, "changed",
                          G_CALLBACK (on_enum_combo_changed), bind);

    gtk_widget_set_sensitive (combo, writable);

    g_object_set_data_full (G_OBJECT (combo),
                            "pn-enum-binding",
                            bind, enum_binding_free);

    return combo;
}

/* ------------------------------------------------------------------ */
/*  Multi-line string editor                                           */
/*                                                                     */
/*  GtkTextView in a scrolled window backing a string property whose   */
/*  value spans multiple lines.  The naïve approach — bind the         */
/*  property to GtkTextBuffer:text — works but every keystroke moves   */
/*  the buffer's cursor to the end as the property re-emits notify;    */
/*  the manual binding here gates each direction with a re-entrancy    */
/*  flag so user typing is left alone while external property changes  */
/*  still propagate into the buffer.                                    */
/* ------------------------------------------------------------------ */

typedef struct
{
    GObject       *target;          /* borrowed */
    const gchar   *property;        /* static, from pspec->name */
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

    /* Avoid clobbering the buffer (and the user's cursor / selection)
     * when the property already matches what the buffer holds. */
    gtk_text_buffer_get_bounds (bind->buffer, &start, &end);
    current = gtk_text_buffer_get_text (bind->buffer, &start, &end, FALSE);

    if (g_strcmp0 (current, value ? value : "") != 0)
    {
        bind->updating = TRUE;
        gtk_text_buffer_set_text (bind->buffer,
                                  value ? value : "",
                                  -1);
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
on_text_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    text_binding_pull (user_data);
}

static void
on_text_buffer_changed (
        GtkTextBuffer *buffer,
        gpointer       user_data)
{
    (void) buffer;

    text_binding_push (user_data);
}

static GtkWidget *
build_multiline_editor (
        GObject    *target,
        GParamSpec *pspec)
{
    const gchar   *name     = pspec->name;
    gboolean       writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GtkWidget     *scrolled = gtk_scrolled_window_new (NULL, NULL);
    GtkWidget     *view     = gtk_text_view_new ();
    GtkTextBuffer *buffer   = gtk_text_view_get_buffer (GTK_TEXT_VIEW (view));
    PnTextBinding *bind;
    gchar         *signal_name;

    gtk_text_view_set_wrap_mode    (GTK_TEXT_VIEW (view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_monospace    (GTK_TEXT_VIEW (view), TRUE);
    gtk_text_view_set_accepts_tab  (GTK_TEXT_VIEW (view), FALSE);

    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_AUTOMATIC,
                                    GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (scrolled),
                                         GTK_SHADOW_IN);
    /* Six lines of text is enough to comfortably hold a small list of
     * addresses without dwarfing the rest of the dialog; the scrolled
     * window absorbs anything longer. */
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
                            "pn-text-binding",
                            bind, text_binding_free);

    return scrolled;
}

/* ------------------------------------------------------------------ */
/*  Property → editor                                                  */
/* ------------------------------------------------------------------ */

/** Build the host's default editor widget for @pspec, bound
 *  bidirectionally to @target.  Returns the editor as a #GtkWidget.
 *  Read-only properties get a disabled editor.  Property types we
 *  do not understand fall through to a label rendering of the
 *  current value.
 *
 *  This function carries no knowledge of any concrete #PnNode
 *  subclass — every customisation a specific node type used to
 *  contribute via a `PN_IS_FOO` branch here now lives in the type's
 *  own translation unit and reaches the dialog through the
 *  #PnNodeClass.build_property_editor vfunc the dialog tries first
 *  in attach_property_row(), so this function only needs to handle
 *  the type-system-level dispatch (string, bool, color, enum, …)
 *  that every property has.  The pn_param_spec_get_multiline()
 *  hint is the one exception: it lives on the param spec itself so
 *  any node type — host or plugin — can opt into the wider editor
 *  without the host having to know about the type.  Exposed
 *  publicly via pn_node_dialog_default_editor() so a subclass
 *  override can delegate the rows it does not want to customise
 *  back to the host default. */

/* ------------------------------------------------------------------ */
/*  Hostname-hint overlay                                              */
/*                                                                     */
/*  GTK3's GtkEntry:placeholder-text only renders when the entry has   */
/*  no focus -- the moment the user clicks in, the hint disappears.    */
/*  For host-monitoring entries we want the local machine name to      */
/*  stay visible until the user actually types something (matches the  */
/*  GTK4 GtkText behaviour and what most modern toolkits do).  Since   */
/*  GTK3 exposes no property for this, we draw the hint ourselves in   */
/*  a `draw`-after handler whenever the entry's buffer is empty.       */
/*                                                                     */
/*  Cursor positioning is unchanged because GtkEntry's own draw has    */
/*  already run -- our hint just paints on top of an otherwise blank   */
/*  text area, so the blinking caret appears to sit at the start of    */
/*  the hint text rather than over a void.                             */
/* ------------------------------------------------------------------ */

static gboolean
on_hostname_hint_draw (
        GtkWidget *widget,
        cairo_t   *cr,
        gpointer   user_data)
{
    GtkEntry        *entry = GTK_ENTRY (widget);
    const gchar     *hint  = (const gchar *) user_data;
    const gchar     *text;
    GtkStyleContext *ctx;
    PangoLayout     *layout;
    GdkRGBA          color;
    gint             layout_x = 0;
    gint             layout_y = 0;

    text = gtk_entry_get_text (entry);
    if (text != NULL && *text != '\0')
        return FALSE;     /* User typed something -- hide the hint. */
    if (hint == NULL || *hint == '\0')
        return FALSE;     /* Defensive: nothing to draw. */

    /* gtk_entry_get_layout_offsets() returns the widget-coordinate
     * origin GtkEntry uses for its own PangoLayout, accounting for
     * border / padding / icon area.  Drawing our hint at the same
     * spot lines it up character-for-character with whatever the
     * user is about to type. */
    gtk_entry_get_layout_offsets (entry, &layout_x, &layout_y);

    /* Pull the theme's placeholder color via the `.placeholder` CSS
     * class so the hint blends with the active GTK theme (Adwaita
     * dark, Yaru, Breeze, ...) rather than being a hard-coded gray
     * that would look wrong in half of them. */
    ctx = gtk_widget_get_style_context (widget);
    gtk_style_context_save (ctx);
    gtk_style_context_add_class (ctx, "placeholder");
    gtk_style_context_get_color (ctx,
                                 gtk_style_context_get_state (ctx),
                                 &color);
    gtk_style_context_restore (ctx);

    layout = gtk_widget_create_pango_layout (widget, hint);

    cairo_save (cr);
    gdk_cairo_set_source_rgba (cr, &color);
    cairo_move_to (cr, layout_x, layout_y);
    pango_cairo_show_layout (cr, layout);
    cairo_restore (cr);

    g_object_unref (layout);
    return FALSE;
}

void
pn_node_dialog_attach_hostname_hint (GtkEntry *entry)
{
    /* g_get_host_name() returns a process-lifetime cached string
     * (GLib owns the storage and never frees it), so it is safe to
     * pass through as `user_data` without a destroy notifier and
     * without copying.  The cast away from `const` is required by
     * GClosure's signature but the callback only reads the string. */
    const gchar *hint = g_get_host_name ();

    g_signal_connect_after (entry, "draw",
                            G_CALLBACK (on_hostname_hint_draw),
                            (gpointer) hint);
}

static GtkWidget *
default_editor_impl (
        GObject    *target,
        GParamSpec *pspec)
{
    GType        ptype     = G_PARAM_SPEC_VALUE_TYPE (pspec);
    const gchar *name      = pspec->name;
    gboolean     writable  = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GBindingFlags flags    = G_BINDING_SYNC_CREATE
                             | (writable ? G_BINDING_BIDIRECTIONAL : 0);

    if (ptype == G_TYPE_STRING)
    {
        /* Tall #GtkTextView editor for any string property the type
         * asked for via pn_param_spec_set_multiline().  The hint
         * lives on the param spec rather than on a hard PN_IS_FOO
         * check here so plugin-shipped node types — whose concrete C
         * type is unknown to the host — can opt into the wider
         * editor the same way the in-tree types do. */
        if (pn_param_spec_get_multiline (pspec))
            return build_multiline_editor (target, pspec);

        GtkWidget *entry = gtk_entry_new ();
        gtk_widget_set_hexpand (entry, TRUE);
        gtk_widget_set_sensitive (entry, writable);
        /* Gray hint showing the local computer's name -- opted into by
         * the property via pn_param_spec_set_hostname_hint() so the
         * user can see at a glance which host the node will actually
         * probe when they leave the field blank.  Hint is purely
         * visual: the bound value stays the empty string until the
         * user types something.
         *
         * We deliberately do NOT use gtk_entry_set_placeholder_text()
         * here.  GTK3's native placeholder is gated on
         * `!gtk_widget_has_focus (entry)` (see should_show_placeholder_text
         * in gtkentry.c -- it intentionally hides the hint the moment
         * focus arrives), and GTK3 exposes no property to keep it
         * visible across the focus transition.  GTK4 fixed that by
         * moving placeholder rendering into GtkText, but we are on
         * gtk+-3.0, so the only way to keep the hint visible while
         * the user is parked on an empty entry is to render it
         * ourselves -- which is exactly what attach_hostname_hint()
         * sets up via a draw-after handler.  Cursor is still drawn
         * (entry's own draw ran first), it just blinks "inside" the
         * first character of the hint, matching GTK4's behaviour. */
        if (pn_param_spec_get_hostname_hint (pspec))
            pn_node_dialog_attach_hostname_hint (GTK_ENTRY (entry));
        g_object_bind_property (target, name, entry, "text", flags);
        return entry;
    }

    if (ptype == G_TYPE_BOOLEAN)
    {
        GtkWidget *check = gtk_check_button_new ();
        gtk_widget_set_sensitive (check, writable);
        g_object_bind_property (target, name, check, "active", flags);
        return check;
    }

    if (ptype == GDK_TYPE_RGBA)
    {
        GtkWidget *btn = gtk_color_button_new ();
        gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (btn), TRUE);
        gtk_widget_set_sensitive (btn, writable);
        g_object_bind_property (target, name, btn, "rgba", flags);
        return btn;
    }

    if (G_TYPE_IS_ENUM (ptype))
    {
        return build_enum_editor (target, pspec);
    }

    if (ptype == PN_TYPE_POINT)
    {
        GtkWidget      *box     = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget      *spin_x  = gtk_spin_button_new_with_range (-1e6, 1e6, 1.0);
        GtkWidget      *spin_y  = gtk_spin_button_new_with_range (-1e6, 1e6, 1.0);
        GtkWidget      *label_x = gtk_label_new ("x");
        GtkWidget      *label_y = gtk_label_new ("y");
        PnPointBinding *bind    = g_new0 (PnPointBinding, 1);
        gchar          *signal_name;

        gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin_x), 0);
        gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin_y), 0);

        gtk_box_pack_start (GTK_BOX (box), label_x, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), spin_x,  TRUE,  TRUE,  0);
        gtk_box_pack_start (GTK_BOX (box), label_y, FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), spin_y,  TRUE,  TRUE,  0);

        gtk_widget_set_sensitive (box, writable);

        bind->target   = target;
        bind->property = name;
        bind->spin_x   = GTK_SPIN_BUTTON (spin_x);
        bind->spin_y   = GTK_SPIN_BUTTON (spin_y);

        /* Initial pull, then keep both ends in sync. */
        point_binding_pull (bind);

        signal_name = g_strdup_printf ("notify::%s", name);
        bind->notify_handler = g_signal_connect (
                target, signal_name,
                G_CALLBACK (on_point_target_notify), bind);
        g_free (signal_name);

        if (writable)
        {
            g_signal_connect (spin_x, "value-changed",
                              G_CALLBACK (on_point_spin_changed), bind);
            g_signal_connect (spin_y, "value-changed",
                              G_CALLBACK (on_point_spin_changed), bind);
        }

        /* Tie binding lifetime to the editor widget. */
        g_object_set_data_full (G_OBJECT (box),
                                "pn-point-binding",
                                bind, point_binding_free);

        return box;
    }

    if (ptype == G_TYPE_INT  || ptype == G_TYPE_UINT  ||
        ptype == G_TYPE_LONG || ptype == G_TYPE_ULONG ||
        ptype == G_TYPE_INT64 || ptype == G_TYPE_DOUBLE ||
        ptype == G_TYPE_FLOAT)
    {
        gdouble lo = -1e9;
        gdouble hi =  1e9;
        gdouble step = 1.0;
        GtkWidget *spin;

        if (G_IS_PARAM_SPEC_INT (pspec))
        {
            GParamSpecInt *p = G_PARAM_SPEC_INT (pspec);
            lo = p->minimum; hi = p->maximum;
        }
        else if (G_IS_PARAM_SPEC_UINT (pspec))
        {
            GParamSpecUInt *p = G_PARAM_SPEC_UINT (pspec);
            lo = p->minimum; hi = p->maximum;
        }
        else if (G_IS_PARAM_SPEC_DOUBLE (pspec))
        {
            GParamSpecDouble *p = G_PARAM_SPEC_DOUBLE (pspec);
            lo = p->minimum; hi = p->maximum; step = 0.1;
        }
        else if (G_IS_PARAM_SPEC_FLOAT (pspec))
        {
            GParamSpecFloat *p = G_PARAM_SPEC_FLOAT (pspec);
            lo = p->minimum; hi = p->maximum; step = 0.1;
        }

        spin = gtk_spin_button_new_with_range (lo, hi, step);
        if (ptype == G_TYPE_DOUBLE || ptype == G_TYPE_FLOAT)
            gtk_spin_button_set_digits (GTK_SPIN_BUTTON (spin), 2);
        gtk_widget_set_sensitive (spin, writable);
        g_object_bind_property (target, name, spin, "value", flags);
        return spin;
    }

    /* Fallback: read-only label printing the current value. */
    {
        GValue       value = G_VALUE_INIT;
        gchar       *text;
        GtkWidget   *label;

        g_value_init (&value, ptype);
        g_object_get_property (target, name, &value);
        text  = g_strdup_value_contents (&value);
        label = gtk_label_new (text ? text : "");
        gtk_widget_set_halign (label, GTK_ALIGN_START);
        gtk_label_set_selectable (GTK_LABEL (label), TRUE);
        g_free (text);
        g_value_unset (&value);
        return label;
    }
}

GtkWidget *
pn_node_dialog_default_editor (
        GObject    *target,
        GParamSpec *pspec)
{
    GtkWidget *editor = default_editor_impl (target, pspec);

    /* Stamp the editor widget with a stable, property-derived
     * #GtkWidget name so a functional test can locate the editor for
     * a given property by name and drive it the way a user would.
     * Purely additive — no CSS in the tree targets these names, so it
     * changes nothing the user sees.  Set here, in the single builder
     * every default-editor caller funnels through (the auto-generated
     * tabs, #PnDial's build_class_tabs, and any plugin override that
     * delegates back to us), so naming coverage matches editor
     * coverage without touching each call site. */
    if (editor != NULL && pspec != NULL && pspec->name != NULL)
    {
        gchar *wname = g_strconcat ("pn-prop-", pspec->name, NULL);
        gtk_widget_set_name (editor, wname);
        g_free (wname);
    }

    return editor;
}

/* ------------------------------------------------------------------ */
/*  Tab construction                                                   */
/* ------------------------------------------------------------------ */

/** Strip a "Pn" prefix from @type_name to produce a friendlier tab
 *  label, e.g. "PnRtc" → "Rtc".  Returns a newly-allocated string. */
static gchar *
prettify_type_name (const gchar *type_name)
{
    if (type_name == NULL)
        return g_strdup ("");

    if (g_str_has_prefix (type_name, "Pn"))
        return g_strdup (type_name + 2);

    return g_strdup (type_name);
}

/** Build a fresh, empty grid ready to host one property-row per line.
 *  Exposed publicly via the helper header so a subclass building a
 *  custom class tab can match the layout of the auto-generated ones. */
GtkWidget *
pn_node_dialog_new_property_grid (void)
{
    GtkWidget *grid = gtk_grid_new ();

    gtk_grid_set_row_spacing    (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    g_object_set (grid,
                  "margin-start",  12,
                  "margin-end",    12,
                  "margin-top",    12,
                  "margin-bottom", 12,
                  NULL);

    return grid;
}

/** Attach a labelled editor row at @row in @grid using the host's
 *  standard layout.  Public helper. */
void
pn_node_dialog_attach_row (
        GtkGrid     *grid,
        guint        row,
        const gchar *label,
        GtkWidget   *editor)
{
    GtkWidget *label_widget = gtk_label_new (label != NULL ? label : "");

    gtk_widget_set_halign  (label_widget, GTK_ALIGN_START);
    gtk_widget_set_hexpand (editor,       TRUE);

    gtk_grid_attach (grid, label_widget, 0, row, 1, 1);
    gtk_grid_attach (grid, editor,       1, row, 1, 1);
}

/** Append @page to @notebook with a fresh #GtkLabel as the tab label.
 *  Public helper. */
void
pn_node_dialog_append_page (
        GtkNotebook *notebook,
        GtkWidget   *page,
        const gchar *title)
{
    gtk_notebook_append_page (notebook, page,
                              gtk_label_new (title != NULL ? title : ""));
}

/** Build the editor widget for @pspec on @target, preferring the
 *  per-class #PnNodeClass.build_property_editor override when the
 *  leaf class provides one and falling back to the host's default
 *  editor when it does not (or when the override returns %NULL to
 *  delegate this specific property).  @parent is the dialog window,
 *  threaded through so a subclass override can use it as the
 *  transient-for of any child dialog (file chooser, confirmation
 *  prompt) it spawns. */
static GtkWidget *
dispatch_property_editor (
        GObject     *target,
        GParamSpec  *pspec,
        GtkWindow   *parent)
{
    PnNodeClass *klass;
    GtkWidget   *editor = NULL;

    if (PN_IS_NODE (target))
    {
        klass = PN_NODE_GET_CLASS (target);
        if (klass != NULL && klass->build_property_editor != NULL)
            editor = klass->build_property_editor (PN_NODE (target),
                                                   pspec, target, parent);
    }

    if (editor == NULL)
        editor = pn_node_dialog_default_editor (target, pspec);

    return editor;
}

/** Append the standard "label : editor" row for @pspec to @grid at
 *  @row.  Honours the per-class build_property_editor override when
 *  one is installed. */
static void
attach_property_row (
        GtkGrid    *grid,
        GObject    *target,
        GParamSpec *pspec,
        guint       row,
        GtkWindow  *parent)
{
    const gchar *nick = g_param_spec_get_nick (pspec);
    GtkWidget   *editor;

    if (nick == NULL || *nick == '\0')
        nick = pspec->name;

    editor = dispatch_property_editor (target, pspec, parent);
    pn_node_dialog_attach_row (grid, row, nick, editor);
}

/** Build a notebook page listing all properties whose owner_type
 *  matches @type.  Returns %NULL if there are no such properties. */
static GtkWidget *
build_tab_for_type (
        GObject   *target,
        GType      type,
        GtkWindow *parent)
{
    GObjectClass *klass = g_type_class_ref (type);
    guint         n_props;
    GParamSpec  **pspecs = g_object_class_list_properties (klass, &n_props);
    GtkWidget    *grid   = pn_node_dialog_new_property_grid ();
    guint         row    = 0;
    guint         i;

    for (i = 0; i < n_props; i++)
    {
        if (pspecs[i]->owner_type != type)
            continue;

        /* Skip read-only properties (e.g. PnMeshtastic's `busy`
         * runtime flag): they are transient state surfaced for other
         * subsystems — the spinner overlay reads `busy`, the JSON
         * serialiser deliberately skips it — and have nothing to
         * offer in a user-facing settings form. */
        if ((pspecs[i]->flags & G_PARAM_WRITABLE) == 0)
            continue;

        /* Skip construct-only properties (e.g. PnAutoTrigger:autostart,
         * the Clock node's test seam): they can only be set when the
         * object is created, so editing one here would do nothing —
         * g_object_set_property() raises a GObject-CRITICAL on a live
         * instance.  The JSON serialiser excludes them for the same
         * reason (see should_serialize_property() in pn-flow.c). */
        if ((pspecs[i]->flags & G_PARAM_CONSTRUCT_ONLY) != 0)
            continue;

        attach_property_row (GTK_GRID (grid), target, pspecs[i], row, parent);
        row++;
    }

    g_free (pspecs);
    g_type_class_unref (klass);

    if (row == 0)
    {
        g_object_ref_sink (grid);
        g_object_unref (grid);
        return NULL;
    }

    return grid;
}

/** Build the read-only "Class" tab.  Surfaces the five class-level
 *  fields pinned by each subclass on its #PnNodeClass struct
 *  (plugin_name, category, class_name, icon, color) — the canonical
 *  "what is this type?" answer the inspector and any future plugin /
 *  discovery UI wants to read straight from the class without
 *  consulting the per-instance properties (which the runtime
 *  unconfigured-state machinery on #PnHttp / #PnPing / … flips away
 *  from the type's "rest" identity).  Every widget is rendered
 *  disabled because none of these values are user-editable: they are
 *  attributes of the type, not of the instance.
 *
 *  Distinct from the #PnNodeClass.build_class_tab vfunc — that one
 *  is a per-leaf-class hook that overrides the auto-generated
 *  property tab; this builder is the dialog's own header tab and
 *  has nothing to do with property editing. */
static GtkWidget *
build_class_metadata_tab (PnNode *node)
{
    PnNodeClass *klass = PN_NODE_GET_CLASS (node);
    GtkWidget   *grid  = pn_node_dialog_new_property_grid ();
    guint        row   = 0;
    guint        i;

    static const struct {
        const gchar  *label;
        const gchar *(*getter) (PnNodeClass *);
    } string_rows[] = {
        { "Plugin",     pn_node_class_get_plugin_name },
        { "Category",   pn_node_class_get_category    },
        { "Class name", pn_node_class_get_class_name  },
        { "Icon",       pn_node_class_get_icon        },
    };

    for (i = 0; i < G_N_ELEMENTS (string_rows); i++)
    {
        const gchar *value = string_rows[i].getter (klass);
        GtkWidget   *label = gtk_label_new (string_rows[i].label);
        GtkWidget   *entry = gtk_entry_new ();

        gtk_widget_set_halign  (label, GTK_ALIGN_START);
        gtk_entry_set_text     (GTK_ENTRY (entry), value ? value : "");
        gtk_widget_set_hexpand (entry, TRUE);
        gtk_widget_set_sensitive (label, FALSE);
        gtk_widget_set_sensitive (entry, FALSE);

        gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), entry, 1, row, 1, 1);
        row++;
    }

    {
        /* PnColor is layout-identical to GdkRGBA (see pn-color.h); the
         * GUI tier casts at the gdk boundary. */
        const GdkRGBA *color = (const GdkRGBA *) pn_node_class_get_color (klass);
        GtkWidget     *label = gtk_label_new ("Color");
        GtkWidget     *btn   = gtk_color_button_new_with_rgba (
                color != NULL ? color : &(GdkRGBA){ 0.0, 0.0, 0.0, 0.0 });

        gtk_color_chooser_set_use_alpha (GTK_COLOR_CHOOSER (btn), TRUE);
        gtk_widget_set_halign    (label, GTK_ALIGN_START);
        gtk_widget_set_halign    (btn,   GTK_ALIGN_START);
        gtk_widget_set_sensitive (label, FALSE);
        gtk_widget_set_sensitive (btn,   FALSE);

        gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), btn,   1, row, 1, 1);
        row++;
    }

    /* Ports row: two labelled checkboxes on a single line, mirroring
     * the layout the PnNode tab used to host. */
    {
        GtkWidget *label = gtk_label_new ("Ports");
        GtkWidget *box   = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *check_in  = gtk_check_button_new ();
        GtkWidget *check_out = gtk_check_button_new ();

        gtk_toggle_button_set_active (
                GTK_TOGGLE_BUTTON (check_in),
                pn_node_class_get_has_input (klass));
        gtk_toggle_button_set_active (
                GTK_TOGGLE_BUTTON (check_out),
                pn_node_class_get_has_output (klass));

        gtk_box_pack_start (GTK_BOX (box), check_in,                FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), gtk_label_new ("Has input"),
                            FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box),
                            gtk_separator_new (GTK_ORIENTATION_VERTICAL),
                            FALSE, FALSE, 6);
        gtk_box_pack_start (GTK_BOX (box), check_out,               FALSE, FALSE, 0);
        gtk_box_pack_start (GTK_BOX (box), gtk_label_new ("Has output"),
                            FALSE, FALSE, 0);

        gtk_widget_set_halign    (label, GTK_ALIGN_START);
        gtk_widget_set_sensitive (label, FALSE);
        gtk_widget_set_sensitive (box,   FALSE);

        gtk_grid_attach (GTK_GRID (grid), label, 0, row, 1, 1);
        gtk_grid_attach (GTK_GRID (grid), box,   1, row, 1, 1);
        row++;
    }

    return grid;
}

/** Build the #PnNode tab.  Limited now to the per-instance settings
 *  the user actually edits (display name, worksheet position, the
 *  enabled / disabled flag); the type-level identity (class name,
 *  icon, colour, ports) lives in the dedicated Class tab built by
 *  build_class_metadata_tab(). */
static GtkWidget *
build_pn_node_tab (GObject   *target,
                   GtkWindow *parent)
{
    GObjectClass *klass = g_type_class_ref (PN_TYPE_NODE);
    GtkWidget    *grid  = pn_node_dialog_new_property_grid ();
    guint         row   = 0;
    guint         i;

    static const gchar * const order[] = {
            "topic", "name", "position", "disabled" };

    for (i = 0; i < G_N_ELEMENTS (order); i++)
    {
        GParamSpec *pspec = g_object_class_find_property (klass, order[i]);
        if (pspec != NULL)
            attach_property_row (GTK_GRID (grid), target, pspec, row++,
                                 parent);
    }

    g_type_class_unref (klass);
    return grid;
}


/** Sync the spinner overlay and the notebook's sensitivity to the
 *  current value of the node's "busy" boolean property. */
static void
sync_busy_state (PnNodeDialog *self)
{
    gboolean busy = FALSE;
    g_object_get (self->node, "busy", &busy, NULL);

    gtk_widget_set_visible  (self->busy_overlay, busy);
    gtk_widget_set_sensitive (self->notebook,    !busy);
}

static void
on_node_busy_notify (GObject    *obj   G_GNUC_UNUSED,
                     GParamSpec *pspec G_GNUC_UNUSED,
                     gpointer    user_data)
{
    sync_busy_state (PN_NODE_DIALOG (user_data));
}

/** If the node exposes a boolean `busy` property, wire the overlay
 *  spinner to it so configuration controls lock during the node's
 *  initialisation window.  Nodes without the property leave the
 *  overlay hidden and the form fully editable. */
static void
maybe_install_busy_watcher (PnNodeDialog *self)
{
    GParamSpec *pspec = g_object_class_find_property (
            G_OBJECT_GET_CLASS (self->node), "busy");
    if (pspec == NULL || G_PARAM_SPEC_VALUE_TYPE (pspec) != G_TYPE_BOOLEAN)
        return;

    /* g_signal_connect_object scopes the handler to the dialog's
     * lifetime — closing the dialog auto-disconnects it from the
     * node, which outlives us. */
    g_signal_connect_object (self->node, "notify::busy",
                             G_CALLBACK (on_node_busy_notify),
                             self, 0);
    sync_busy_state (self);
}

/** Append one tab per class in the chain from #PnNode down to the
 *  leaf type, skipping classes that introduce no properties.  Each
 *  class along the chain may install:
 *
 *    - #PnNodeClass.build_class_tab to replace the auto-generated
 *      property tab with a fully custom widget (PnFilter, PnSet,
 *      out-of-tree plugins with structured / list-shaped configuration).
 *
 *    - #PnNodeClass.build_property_editor to take over a single row
 *      in the auto-generated tab (PnMeshtastic device / channel
 *      combos, PnSound / PnTts file pickers, PnRate read-only
 *      labels) — handled inside attach_property_row().
 *
 *    - #PnNodeClass.build_extra_pages to append additional notebook
 *      pages beyond the per-class property tab (Diagnostics tabs,
 *      Logs, scan buttons) — invoked in the second-pass walk below
 *      after the per-class tabs are in place. */
static void
populate_notebook (PnNodeDialog *self)
{
    GType      leaf   = G_OBJECT_TYPE (self->node);
    GtkWindow *parent = GTK_WINDOW (self);
    GType     *chain;
    guint      depth = 0;
    GType      t;
    guint      i;

    /* Class tab first: the read-only summary of the type's identity
     * (plugin / category / class name / icon / colour).  Inserted
     * before the per-class property tabs so the inspector reads
     * top-down as "what is this type" → "what can I configure". */
    gtk_notebook_append_page (GTK_NOTEBOOK (self->notebook),
                              build_class_metadata_tab (self->node),
                              gtk_label_new ("Class"));

    /* Walk up to count the chain length, then fill it in order from
     * #PnNode at index 0 down to the leaf type. */
    for (t = leaf; t != G_TYPE_INVALID; t = g_type_parent (t))
    {
        depth++;
        if (t == PN_TYPE_NODE)
            break;
    }

    chain = g_new0 (GType, depth);
    t = leaf;
    for (i = depth; i > 0; i--)
    {
        chain[i - 1] = t;
        t = g_type_parent (t);
    }

    for (i = 0; i < depth; i++)
    {
        GtkWidget *page  = NULL;
        gchar     *title;

        /* PnNode itself stays special-cased: the auto-generated tab
         * for the base class would surface every property the base
         * declares (color, has-input, has-output, …), but the
         * dialog deliberately exposes only name / position /
         * disabled here and surfaces the rest via the read-only
         * Class tab.  Every other class along the chain — including
         * plugin-provided leaf types — goes through the standard
         * vfunc-or-default path. */
        if (chain[i] == PN_TYPE_NODE)
        {
            page = build_pn_node_tab (G_OBJECT (self->node), parent);
        }
        else
        {
            PnNodeClass *klass = g_type_class_ref (chain[i]);

            /* build_class_tabs (plural) wins over build_class_tab:
             * a class that opts into the multi-page form contributes
             * its pages straight onto the dialog's top-level notebook
             * and we skip the single-page paths entirely. */
            if (klass != NULL && klass->build_class_tabs != NULL)
            {
                klass->build_class_tabs (self->node,
                                         GTK_NOTEBOOK (self->notebook),
                                         parent);
                g_type_class_unref (klass);
                continue;
            }

            if (klass != NULL && klass->build_class_tab != NULL)
                page = klass->build_class_tab (self->node, parent);

            g_type_class_unref (klass);

            if (page == NULL)
                page = build_tab_for_type (G_OBJECT (self->node),
                                           chain[i], parent);
        }

        if (page == NULL)
            continue;

        title = prettify_type_name (g_type_name (chain[i]));
        gtk_notebook_append_page (GTK_NOTEBOOK (self->notebook),
                                  page,
                                  gtk_label_new (title));
        g_free (title);
    }

    /* Second pass: give each class along the chain a chance to
     * append additional notebook pages beyond its property tab.
     * Walked in the same order as the first pass so a leaf class's
     * extras land at the end of the notebook, after every parent
     * class has had its turn. */
    for (i = 0; i < depth; i++)
    {
        PnNodeClass *klass = g_type_class_ref (chain[i]);

        if (klass != NULL && klass->build_extra_pages != NULL)
            klass->build_extra_pages (self->node,
                                      GTK_NOTEBOOK (self->notebook),
                                      parent);

        g_type_class_unref (klass);
    }

    g_free (chain);
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_node_dialog_constructed (GObject *object)
{
    PnNodeDialog *self = PN_NODE_DIALOG (object);
    GtkWidget    *content;
    const gchar  *class_name;
    gchar        *title;

    G_OBJECT_CLASS (pn_node_dialog_parent_class)->constructed (object);

    g_return_if_fail (self->node != NULL);

    class_name = pn_node_get_class_name (self->node);
    title = g_strdup_printf ("Node Properties — %s",
                             class_name ? class_name : "node");
    gtk_window_set_title (GTK_WINDOW (self), title);
    g_free (title);

    gtk_window_set_default_size (GTK_WINDOW (self), 420, 320);

    gtk_dialog_add_button (GTK_DIALOG (self), "_Close", GTK_RESPONSE_CLOSE);

    self->notebook = gtk_notebook_new ();
    gtk_widget_set_hexpand (self->notebook, TRUE);
    gtk_widget_set_vexpand (self->notebook, TRUE);

    /* Overlay so a centered spinner can sit on top of the notebook
     * while the node is initialising (e.g. PnMeshtastic is opening
     * the serial port and running the multi-second config
     * handshake).  Activated by maybe_install_busy_watcher() below
     * for any node that exposes a boolean "busy" property; for
     * everything else the overlay child stays hidden and the dialog
     * looks identical to before. */
    GtkWidget *overlay = gtk_overlay_new ();
    gtk_container_add (GTK_CONTAINER (overlay), self->notebook);

    self->busy_overlay = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_halign (self->busy_overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (self->busy_overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all (self->busy_overlay, TRUE);

    GtkWidget *spinner = gtk_spinner_new ();
    gtk_widget_set_size_request (spinner, 48, 48);
    gtk_widget_show (spinner);
    gtk_spinner_start (GTK_SPINNER (spinner));
    gtk_box_pack_start (GTK_BOX (self->busy_overlay), spinner, FALSE, FALSE, 0);

    GtkWidget *label = gtk_label_new ("Initializing device…");
    gtk_widget_show (label);
    gtk_box_pack_start (GTK_BOX (self->busy_overlay), label, FALSE, FALSE, 0);

    gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->busy_overlay);

    content = gtk_dialog_get_content_area (GTK_DIALOG (self));
    gtk_box_pack_start (GTK_BOX (content), overlay, TRUE, TRUE, 0);

    populate_notebook (self);
    maybe_install_busy_watcher (self);

    gtk_widget_show_all (content);

    /* Land on the leaf-type tab — the most-specific page, where the
     * per-instance settings the user is likely to want to edit live —
     * rather than the read-only Class tab the notebook would default
     * to as page 0.  Done *after* show_all because GtkNotebook
     * silently snaps current-page back to 0 when its first page is
     * realised, so the call only sticks once every page exists in
     * the realised tree. */
    {
        gint last = gtk_notebook_get_n_pages (
                GTK_NOTEBOOK (self->notebook)) - 1;
        if (last >= 0)
            gtk_notebook_set_current_page (
                    GTK_NOTEBOOK (self->notebook), last);
    }
}

static void
pn_node_dialog_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnNodeDialog *self = PN_NODE_DIALOG (object);

    switch (prop_id)
    {
    case PROP_NODE:
        g_value_set_object (value, self->node);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_node_dialog_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnNodeDialog *self = PN_NODE_DIALOG (object);

    switch (prop_id)
    {
    case PROP_NODE:
        /* Construct-only. */
        g_assert (self->node == NULL);
        self->node = g_value_dup_object (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_node_dialog_dispose (GObject *object)
{
    PnNodeDialog *self = PN_NODE_DIALOG (object);

    g_clear_object (&self->node);

    G_OBJECT_CLASS (pn_node_dialog_parent_class)->dispose (object);
}

static void
pn_node_dialog_class_init (PnNodeDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->constructed  = pn_node_dialog_constructed;
    object_class->get_property = pn_node_dialog_get_property;
    object_class->set_property = pn_node_dialog_set_property;
    object_class->dispose      = pn_node_dialog_dispose;

    props[PROP_NODE] = g_param_spec_object (
            "node", "Node",
            "Node whose properties are being edited",
            PN_TYPE_NODE,
            G_PARAM_READWRITE
            | G_PARAM_CONSTRUCT_ONLY
            | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_node_dialog_init (PnNodeDialog *self)
{
    (void) self;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_node_dialog_new (
        GtkWindow *parent,
        PnNode    *node)
{
    g_return_val_if_fail (PN_IS_NODE (node), NULL);

    return g_object_new (PN_TYPE_NODE_DIALOG,
                         "transient-for", parent,
                         "modal",         TRUE,
                         "node",          node,
                         "use-header-bar", FALSE,
                         NULL);
}
