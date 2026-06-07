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

/* ------------------------------------------------------------------ */
/*  PnRate — gui tier.                                                  */
/*                                                                     */
/*  The settings-dialog customisation for the Rate node.  The node's    */
/*  GType, properties, the periodic fetch/cache logic and the           */
/*  receive()-time conversion live in the GTK-free core file pn-rate.c; */
/*  this file installs the build_property_editor vfunc slot onto that    */
/*  class at editor startup (pn_rate_gui_install).  The dialog reads     */
/*  and writes the node's state purely through its GObject properties    */
/*  and the public pn_currency_get_icon_name() / PN_TYPE_CURRENCY, so    */
/*  the headless runtime never loads this half and the Rate logic runs   */
/*  without GTK.                                                        */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-rate-gui.h"
#include "pn-rate.h"

#include <gtk/gtk.h>

/* ------------------------------------------------------------------ */
/*  Settings dialog: PnNodeClass.build_property_editor override        */
/*                                                                     */
/*  Three of #PnRate's four properties want a non-default editor:      */
/*                                                                     */
/*    * `rate` and `last-update` are written by the node itself        */
/*      (cached fetch state) and the user is meant to read, not edit   */
/*      them.  A spinbutton with the default 2 decimal digits renders  */
/*      the PLS→USD rate of ~7.5e-06 as a misleading "0.00" and a      */
/*      string entry is no protection against accidentally clobbering  */
/*      the timestamp; route both through a read-only label renderer   */
/*      (%g for the double, verbatim for the string).                  */
/*                                                                     */
/*    * `from` and `to` are #PnCurrency enums; the default enum combo  */
/*      shows just the ticker, but a richer combo with the currency's  */
/*      icon next to the ticker is faster for the eye to scan when     */
/*      picking from 23 entries.  Icons live as PNGs under             */
/*      $datadir/pipnode/icons/<nick>.png (or in the in-tree           */
/*      data/icons/ when running uninstalled); a missing PNG is shown  */
/*      as a blank gap so the rest of the rows still line up.          */
/*                                                                     */
/*  Pre-2.0 all three lived as PN_IS_RATE branches in the host's       */
/*  pn-node-dialog.c.                                                  */
/* ------------------------------------------------------------------ */

/* --- Read-only label editor for `rate` / `last-update` ----------- */

typedef struct
{
    GObject     *target;        /* borrowed */
    const gchar *property;      /* static, from pspec->name */
    GType        ptype;         /* property's value type */
    GtkLabel    *label;
    gulong       notify_handler;
} PnReadOnlyBinding;

static void
readonly_binding_free (gpointer data)
{
    PnReadOnlyBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
readonly_binding_pull (PnReadOnlyBinding *bind)
{
    if (bind->ptype == G_TYPE_DOUBLE || bind->ptype == G_TYPE_FLOAT)
    {
        gdouble v = 0.0;
        gchar   buf[G_ASCII_DTOSTR_BUF_SIZE];

        g_object_get (bind->target, bind->property, &v, NULL);
        g_ascii_formatd (buf, sizeof buf, "%g", v);
        gtk_label_set_text (bind->label, buf);
    }
    else if (bind->ptype == G_TYPE_STRING)
    {
        gchar *v = NULL;

        g_object_get (bind->target, bind->property, &v, NULL);

        /* An empty `last-update` means the rate has never been fetched
         * since the worksheet loaded; spell that out rather than showing
         * a blank cell. */
        if ((v == NULL || *v == '\0') &&
            g_strcmp0 (bind->property, "last-update") == 0)
            gtk_label_set_text (bind->label, "Never");
        else
            gtk_label_set_text (bind->label, v != NULL ? v : "");

        g_free (v);
    }
    else
    {
        GValue value = G_VALUE_INIT;
        gchar *text;

        g_value_init (&value, bind->ptype);
        g_object_get_property (bind->target, bind->property, &value);
        text = g_strdup_value_contents (&value);
        gtk_label_set_text (bind->label, text != NULL ? text : "");
        g_free (text);
        g_value_unset (&value);
    }
}

static void
on_readonly_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    readonly_binding_pull (user_data);
}

static GtkWidget *
build_readonly_label_editor (
        GObject    *target,
        GParamSpec *pspec)
{
    GtkWidget         *label = gtk_label_new ("");
    PnReadOnlyBinding *bind;
    gchar             *signal_name;

    gtk_label_set_xalign     (GTK_LABEL (label), 0.0);
    gtk_label_set_selectable (GTK_LABEL (label), TRUE);
    /* The mono font matches the look of an editor, makes wide
     * scientific-notation values line up across rows, and visually
     * distinguishes read-only displays from editable entries. */
    {
        PangoAttrList *attrs = pango_attr_list_new ();
        pango_attr_list_insert (attrs, pango_attr_family_new ("Monospace"));
        gtk_label_set_attributes (GTK_LABEL (label), attrs);
        pango_attr_list_unref (attrs);
    }

    bind = g_new0 (PnReadOnlyBinding, 1);
    bind->target   = target;
    bind->property = pspec->name;
    bind->ptype    = G_PARAM_SPEC_VALUE_TYPE (pspec);
    bind->label    = GTK_LABEL (label);

    readonly_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", pspec->name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_readonly_target_notify), bind);
    g_free (signal_name);

    g_object_set_data_full (G_OBJECT (label),
                            "pn-readonly-binding",
                            bind, readonly_binding_free);

    return label;
}

/* --- Currency editor for `from` / `to` --------------------------- */

typedef struct
{
    GObject     *target;        /* borrowed */
    const gchar *property;      /* static: pspec->name */
    GtkComboBox *combo;
    gulong       notify_handler;
    gboolean     updating;
} PnCurrencyBinding;

static void
currency_binding_free (gpointer data)
{
    PnCurrencyBinding *bind = data;

    if (bind->target != NULL && bind->notify_handler != 0)
        g_signal_handler_disconnect (bind->target, bind->notify_handler);

    g_free (bind);
}

static void
currency_binding_pull (PnCurrencyBinding *bind)
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
on_currency_target_notify (
        GObject    *object,
        GParamSpec *pspec,
        gpointer    user_data)
{
    (void) object;
    (void) pspec;

    currency_binding_pull (user_data);
}

static void
on_currency_combo_changed (
        GtkComboBox *combo,
        gpointer     user_data)
{
    PnCurrencyBinding *bind = user_data;
    const gchar       *id;

    if (bind->updating)
        return;

    id = gtk_combo_box_get_active_id (combo);
    if (id == NULL)
        return;

    bind->updating = TRUE;
    g_object_set (bind->target, bind->property, atoi (id), NULL);
    bind->updating = FALSE;
}

static gchar *
resolve_currency_icon_path (const gchar *basename)
{
    gchar *path;

#ifdef SRCDIR
    path = g_build_filename (SRCDIR, "data", "icons", basename, NULL);
    if (g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
    g_free (path);
#endif

#ifdef PKGDATADIR
    path = g_build_filename (PKGDATADIR, "icons", basename, NULL);
    if (g_file_test (path, G_FILE_TEST_EXISTS))
        return path;
    g_free (path);
#endif

    return NULL;
}

static GdkPixbuf *
load_currency_icon (const gchar *nick)
{
    gchar     *filename;
    gchar     *path;
    GdkPixbuf *pix = NULL;

    if (nick == NULL || *nick == '\0')
        return NULL;

    filename = g_strdup_printf ("%s.png", nick);
    path = resolve_currency_icon_path (filename);
    g_free (filename);

    if (path != NULL)
    {
        pix = gdk_pixbuf_new_from_file_at_size (path, 20, 20, NULL);
        g_free (path);
    }

    return pix;
}

static GtkWidget *
build_currency_editor (
        GObject    *target,
        GParamSpec *pspec)
{
    enum { COL_ID, COL_LABEL, COL_PIX, N_COLS };
    GType              ptype    = G_PARAM_SPEC_VALUE_TYPE (pspec);
    const gchar       *name     = pspec->name;
    gboolean           writable = (pspec->flags & G_PARAM_WRITABLE) != 0;
    GtkListStore      *store    = gtk_list_store_new (
            N_COLS, G_TYPE_STRING, G_TYPE_STRING, GDK_TYPE_PIXBUF);
    GtkWidget         *combo    = gtk_combo_box_new_with_model (
            GTK_TREE_MODEL (store));
    GtkCellRenderer   *pix_r    = gtk_cell_renderer_pixbuf_new ();
    GtkCellRenderer   *txt_r    = gtk_cell_renderer_text_new ();
    GEnumClass        *eclass   = g_type_class_ref (ptype);
    PnCurrencyBinding *bind;
    gchar             *signal_name;
    guint              i;

    gtk_combo_box_set_id_column (GTK_COMBO_BOX (combo), COL_ID);

    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), pix_r, FALSE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (combo),
                                   pix_r, "pixbuf", COL_PIX);

    gtk_cell_layout_pack_start (GTK_CELL_LAYOUT (combo), txt_r, TRUE);
    gtk_cell_layout_add_attribute (GTK_CELL_LAYOUT (combo),
                                   txt_r, "text", COL_LABEL);

    for (i = 0; i < eclass->n_values; i++)
    {
        const GEnumValue *v   = &eclass->values[i];
        const gchar      *lbl = (v->value_nick != NULL && *v->value_nick != '\0')
                                    ? v->value_nick : v->value_name;
        gchar            *id  = g_strdup_printf ("%d", v->value);
        GdkPixbuf        *pix = load_currency_icon (
                pn_currency_get_icon_name ((PnCurrency) v->value));
        GtkTreeIter       iter;

        gtk_list_store_append (store, &iter);
        gtk_list_store_set (store, &iter,
                            COL_ID,    id,
                            COL_LABEL, lbl,
                            COL_PIX,   pix,
                            -1);
        if (pix != NULL)
            g_object_unref (pix);
        g_free (id);
    }

    g_type_class_unref (eclass);
    g_object_unref (store);

    bind = g_new0 (PnCurrencyBinding, 1);
    bind->target   = target;
    bind->property = name;
    bind->combo    = GTK_COMBO_BOX (combo);

    currency_binding_pull (bind);

    signal_name = g_strdup_printf ("notify::%s", name);
    bind->notify_handler = g_signal_connect (
            target, signal_name,
            G_CALLBACK (on_currency_target_notify), bind);
    g_free (signal_name);

    if (writable)
        g_signal_connect (combo, "changed",
                          G_CALLBACK (on_currency_combo_changed), bind);

    gtk_widget_set_sensitive (combo, writable);

    g_object_set_data_full (G_OBJECT (combo),
                            "pn-currency-binding",
                            bind, currency_binding_free);

    return combo;
}

static GtkWidget *
pn_rate_build_property_editor (PnNode      *self      G_GNUC_UNUSED,
                               GParamSpec  *pspec,
                               GObject     *target,
                               GtkWindow   *parent    G_GNUC_UNUSED)
{
    const gchar *name  = pspec->name;
    GType        ptype = G_PARAM_SPEC_VALUE_TYPE (pspec);

    if (g_strcmp0 (name, "rate")        == 0 ||
        g_strcmp0 (name, "last-update") == 0 ||
        g_strcmp0 (name, "status")      == 0)
        return build_readonly_label_editor (target, pspec);

    if ((g_strcmp0 (name, "from") == 0 || g_strcmp0 (name, "to") == 0) &&
        ptype == PN_TYPE_CURRENCY)
        return build_currency_editor (target, pspec);

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_rate_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_RATE));

    node_class->build_property_editor = pn_rate_build_property_editor;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot
     * we just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
