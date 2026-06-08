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

#include "pn-credentials-dialog.h"

#include "pn-file-chooser-entry.h"
#include "pn-help-browser.h"
#include "pn-node-factory.h"
#include "pn-profile-schema.h"
#include "pn-vault.h"

/* The manager edits the process-wide vault, so unlike the per-document
 * settings dialog there is exactly one instance for the whole app. */
static PnCredentialsDialog *singleton_dialog = NULL;

#define FIELD_CTX_KEY   "pn-cred-field-ctx"
#define PROFILE_CTX_KEY "pn-cred-profile-ctx"

struct _PnCredentialsDialog
{
    GtkWindow parent_instance;

    GtkTreeStore *category_store;
    GtkWidget    *category_view;
    GtkStack     *stack;

    /* type_id (owned) -> the GtkBox holding that type's profile rows
     * (borrowed; lives in the stack).  Rebuilt wholesale on structural
     * edits, the same way the document-settings dialog rebuilds its rows. */
    GHashTable   *lists;

    /* TRUE while a page is being (re)filled so the editors' own change
     * signals do not write straight back. */
    gboolean      rebuilding;
};

G_DEFINE_TYPE (PnCredentialsDialog, pn_credentials_dialog, GTK_TYPE_WINDOW)

enum {
    COL_LABEL,
    COL_PAGE_ID,
    N_COLS,
};

enum {
    SIG_HELP_REQUESTED,
    N_SIGNALS,
};

static guint signals[N_SIGNALS];

static void rebuild_type_page (PnCredentialsDialog *self, const gchar *type_id);

/* ------------------------------------------------------------------ */
/*  Conditional field visibility (TODO #47.8)                          */
/*                                                                     */
/*  One FrameCtx per profile card tracks every field row so a change   */
/*  to a controller field (an auth-schema combobox, say) can re-run    */
/*  the schema's visible-when rules and hide/show the dependent rows.  */
/* ------------------------------------------------------------------ */

typedef struct {
    GtkWidget *label;       /* row label   (borrowed; lives in the grid) */
    GtkWidget *editor;      /* row editor  (borrowed; lives in the grid) */
    guint      index;       /* schema field index */
} RowVis;

typedef struct _FrameCtx {
    PnCredentialsDialog *self;
    PnProfileSchema     *schema;     /* borrowed; factory-owned, stable */
    gchar               *profile_id; /* owned */
    GPtrArray           *rows;       /* of RowVis* (owned) */
} FrameCtx;

static void frame_apply_visibility (FrameCtx *fc);

/* ------------------------------------------------------------------ */
/*  Per-field editor state                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    PnCredentialsDialog *self;
    gchar               *profile_id;
    gchar               *field;
    FrameCtx            *frame;      /* borrowed; for visible-when re-eval */
} FieldCtx;

static void
field_ctx_free (gpointer data)
{
    FieldCtx *ctx = data;
    g_free (ctx->profile_id);
    g_free (ctx->field);
    g_free (ctx);
}

static FieldCtx *
field_ctx_new (PnCredentialsDialog *self,
               const gchar         *profile_id,
               const gchar         *field,
               FrameCtx            *frame)
{
    FieldCtx *ctx = g_new0 (FieldCtx, 1);
    ctx->self       = self;
    ctx->profile_id = g_strdup (profile_id);
    ctx->field      = g_strdup (field);
    ctx->frame      = frame;
    return ctx;
}

/* Resolve the live profile a FieldCtx refers to, or %NULL if it is gone. */
static PnProfile *
field_ctx_profile (FieldCtx *ctx)
{
    return pn_vault_get_profile (pn_vault_get_default (), ctx->profile_id);
}

/* ------------------------------------------------------------------ */
/*  Field editor callbacks                                            */
/* ------------------------------------------------------------------ */

static void
on_field_entry_commit (FieldCtx *ctx, GtkEntry *entry)
{
    PnProfile *p;

    if (ctx->self->rebuilding)
        return;
    p = field_ctx_profile (ctx);
    if (p != NULL)
        pn_profile_set_field (p, ctx->field, gtk_entry_get_text (entry));
}

static void
on_field_entry_activate (GtkEntry *entry, gpointer user_data)
{
    on_field_entry_commit (user_data, entry);
}

static gboolean
on_field_entry_focus_out (GtkWidget *widget, GdkEvent *event,
                          gpointer user_data)
{
    (void) event;
    on_field_entry_commit (user_data, GTK_ENTRY (widget));
    return FALSE;
}

static void
on_field_toggled (GtkToggleButton *button, gpointer user_data)
{
    FieldCtx  *ctx = user_data;
    PnProfile *p;

    if (ctx->self->rebuilding)
        return;
    p = field_ctx_profile (ctx);
    if (p != NULL)
        pn_profile_set_field (p, ctx->field,
                              gtk_toggle_button_get_active (button)
                                  ? "true" : "false");
}

static void
on_field_combo_changed (GtkComboBox *combo, gpointer user_data)
{
    FieldCtx    *ctx = user_data;
    PnProfile   *p;
    const gchar *id;

    if (ctx->self->rebuilding)
        return;
    p = field_ctx_profile (ctx);
    if (p == NULL)
        return;
    id = gtk_combo_box_get_active_id (combo);
    pn_profile_set_field (p, ctx->field, id != NULL ? id : "");

    /* A combobox may be a controller for other fields' visibility. */
    if (ctx->frame != NULL)
        frame_apply_visibility (ctx->frame);
}

static void
on_field_file_changed (PnFileChooserEntry *entry, gpointer user_data)
{
    FieldCtx  *ctx = user_data;
    PnProfile *p;

    if (ctx->self->rebuilding)
        return;
    p = field_ctx_profile (ctx);
    if (p != NULL)
        pn_profile_set_field (p, ctx->field,
                              pn_file_chooser_entry_get_text (entry));
}

static void
on_secret_icon_press (GtkEntry             *entry,
                      GtkEntryIconPosition  pos,
                      GdkEvent             *event,
                      gpointer              user_data)
{
    (void) event; (void) user_data;
    if (pos == GTK_ENTRY_ICON_SECONDARY)
        gtk_entry_set_visibility (entry, !gtk_entry_get_visibility (entry));
}

/** Build the editor widget for one schema field, primed from @p and wired to
 *  write straight back to the vault. */
static GtkWidget *
build_field_editor (PnCredentialsDialog *self,
                    PnProfileSchema     *schema,
                    PnProfile           *p,
                    guint                fi,
                    FrameCtx            *frame)
{
    const gchar        *field = pn_profile_schema_field_name (schema, fi);
    PnProfileFieldKind  kind  = pn_profile_schema_field_get_kind (schema, fi);
    FieldCtx           *ctx   = field_ctx_new (self, pn_profile_get_id (p),
                                               field, frame);
    GtkWidget          *w;

    if (kind == PN_FIELD_BOOL || kind == PN_FIELD_PERMISSION)
    {
        w = gtk_check_button_new ();
        gtk_toggle_button_set_active (
                GTK_TOGGLE_BUTTON (w),
                kind == PN_FIELD_PERMISSION
                    ? pn_profile_get_permission (p, field)
                    : pn_profile_get_bool (p, field));
        gtk_widget_set_halign (w, GTK_ALIGN_START);
        g_signal_connect (w, "toggled", G_CALLBACK (on_field_toggled), ctx);
    }
    else if (kind == PN_FIELD_FILE)
    {
        gchar *cur = pn_profile_get_string (p, field);

        w = pn_file_chooser_entry_new ();
        pn_file_chooser_entry_set_text (PN_FILE_CHOOSER_ENTRY (w), cur);
        pn_file_chooser_entry_set_title (
                PN_FILE_CHOOSER_ENTRY (w),
                pn_profile_schema_field_get_label (schema, fi));
        gtk_widget_set_hexpand (w, TRUE);
        g_free (cur);
        g_signal_connect (w, "changed",
                          G_CALLBACK (on_field_file_changed), ctx);
    }
    else if (kind == PN_FIELD_ENUM)
    {
        const gchar *const *choices =
                pn_profile_schema_field_get_choices (schema, fi);
        gchar *cur = pn_profile_get_string (p, field);
        guint  i;

        w = gtk_combo_box_text_new ();
        for (i = 0; choices != NULL && choices[i] != NULL; i++)
            gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (w),
                                       choices[i], choices[i]);
        gtk_combo_box_set_active_id (GTK_COMBO_BOX (w), cur);
        gtk_widget_set_hexpand (w, TRUE);
        g_free (cur);
        g_signal_connect (w, "changed",
                          G_CALLBACK (on_field_combo_changed), ctx);
    }
    else /* STRING / INT / SECRET — all entry-backed */
    {
        gchar *cur;

        w = gtk_entry_new ();
        if (kind == PN_FIELD_INT)
            cur = g_strdup_printf ("%" G_GINT64_FORMAT,
                                   pn_profile_get_int (p, field));
        else
            cur = pn_profile_get_string (p, field);
        gtk_entry_set_text (GTK_ENTRY (w), cur);
        g_free (cur);
        gtk_widget_set_hexpand (w, TRUE);

        if (kind == PN_FIELD_SECRET)
        {
            gtk_entry_set_visibility (GTK_ENTRY (w), FALSE);
            gtk_entry_set_icon_from_icon_name (GTK_ENTRY (w),
                    GTK_ENTRY_ICON_SECONDARY, "view-reveal-symbolic");
            gtk_entry_set_icon_activatable (GTK_ENTRY (w),
                    GTK_ENTRY_ICON_SECONDARY, TRUE);
            gtk_entry_set_icon_tooltip_text (GTK_ENTRY (w),
                    GTK_ENTRY_ICON_SECONDARY, "Show or hide the secret");
            g_signal_connect (w, "icon-press",
                              G_CALLBACK (on_secret_icon_press), NULL);
        }

        g_signal_connect (w, "activate",
                          G_CALLBACK (on_field_entry_activate), ctx);
        g_signal_connect (w, "focus-out-event",
                          G_CALLBACK (on_field_entry_focus_out), ctx);
    }

    g_object_set_data_full (G_OBJECT (w), FIELD_CTX_KEY, ctx, field_ctx_free);
    return w;
}

/* ------------------------------------------------------------------ */
/*  Per-profile state (name / primary / delete)                       */
/* ------------------------------------------------------------------ */

typedef struct {
    PnCredentialsDialog *self;
    gchar               *profile_id;
    gchar               *type_id;
} ProfileCtx;

static void
profile_ctx_free (gpointer data)
{
    ProfileCtx *ctx = data;
    g_free (ctx->profile_id);
    g_free (ctx->type_id);
    g_free (ctx);
}

static void
on_name_commit (ProfileCtx *ctx, GtkEntry *entry)
{
    PnProfile *p;

    if (ctx->self->rebuilding)
        return;
    p = pn_vault_get_profile (pn_vault_get_default (), ctx->profile_id);
    if (p != NULL)
        pn_profile_set_name (p, gtk_entry_get_text (entry));
}

static void
on_name_activate (GtkEntry *entry, gpointer user_data)
{
    on_name_commit (user_data, entry);
}

static gboolean
on_name_focus_out (GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
    (void) event;
    on_name_commit (user_data, GTK_ENTRY (widget));
    return FALSE;
}

static void
on_primary_toggled (GtkToggleButton *button, gpointer user_data)
{
    ProfileCtx *ctx = user_data;

    if (ctx->self->rebuilding)
        return;
    if (!gtk_toggle_button_get_active (button))
        return;
    pn_vault_set_default_profile (pn_vault_get_default (), ctx->profile_id);
    rebuild_type_page (ctx->self, ctx->type_id);
}

static void
on_profile_delete (GtkButton *button, gpointer user_data)
{
    ProfileCtx *ctx = user_data;

    (void) button;
    if (ctx->self->rebuilding)
        return;
    pn_vault_delete_profile (pn_vault_get_default (), ctx->profile_id);
    rebuild_type_page (ctx->self, ctx->type_id);
}

/* ------------------------------------------------------------------ */
/*  Profile frame + type page rebuild                                 */
/* ------------------------------------------------------------------ */

#define FRAME_CTX_KEY "pn-cred-frame-ctx"

static void
frame_ctx_free (gpointer data)
{
    FrameCtx *fc = data;
    g_ptr_array_free (fc->rows, TRUE);  /* RowVis are plain, g_free frees them */
    g_free (fc->profile_id);
    g_free (fc);
}

/* Re-evaluate every row's visible-when rule against the live profile and
 * show/hide both the label and the editor accordingly. */
static void
frame_apply_visibility (FrameCtx *fc)
{
    PnProfile *p;
    guint      i;

    p = pn_vault_get_profile (pn_vault_get_default (), fc->profile_id);
    if (p == NULL)
        return;

    for (i = 0; i < fc->rows->len; i++)
    {
        RowVis      *r    = g_ptr_array_index (fc->rows, i);
        const gchar *ctrl =
                pn_profile_schema_field_get_visible_when (fc->schema, r->index);
        gboolean     vis;

        if (ctrl == NULL)
            vis = TRUE;                 /* no rule: always visible */
        else
        {
            gchar *cv = pn_profile_get_string (p, ctrl);
            vis = pn_profile_schema_field_visible_for (fc->schema, r->index, cv);
            g_free (cv);
        }

        gtk_widget_set_visible (r->label,  vis);
        gtk_widget_set_visible (r->editor, vis);
    }
}

static GtkWidget *
build_profile_frame (PnCredentialsDialog *self,
                     PnProfileSchema     *schema,
                     PnProfile           *p,
                     gboolean             is_primary)
{
    GtkWidget  *frame = gtk_frame_new (NULL);
    GtkWidget  *box   = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget  *head  = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget  *name  = gtk_entry_new ();
    GtkWidget  *primary = gtk_check_button_new_with_label ("Primary");
    GtkWidget  *del   = gtk_button_new_from_icon_name ("edit-delete-symbolic",
                                                       GTK_ICON_SIZE_BUTTON);
    GtkWidget  *grid  = gtk_grid_new ();
    ProfileCtx *pctx  = g_new0 (ProfileCtx, 1);
    FrameCtx   *fctx  = g_new0 (FrameCtx, 1);
    guint       n     = pn_profile_schema_get_n_fields (schema);
    guint       i;

    fctx->self       = self;
    fctx->schema     = schema;
    fctx->profile_id = g_strdup (pn_profile_get_id (p));
    fctx->rows       = g_ptr_array_new_with_free_func (g_free);

    pctx->self       = self;
    pctx->profile_id = g_strdup (pn_profile_get_id (p));
    pctx->type_id    = g_strdup (pn_profile_schema_get_type_id (schema));

    g_object_set (box, "margin-start", 10, "margin-end", 10,
                  "margin-top", 8, "margin-bottom", 8, NULL);

    /* Header: name entry + primary toggle + delete. */
    gtk_entry_set_text (GTK_ENTRY (name), pn_profile_get_name (p));
    gtk_widget_set_hexpand (name, TRUE);
    gtk_entry_set_placeholder_text (GTK_ENTRY (name), "Profile name");
    g_signal_connect (name, "activate", G_CALLBACK (on_name_activate), pctx);
    g_signal_connect (name, "focus-out-event",
                      G_CALLBACK (on_name_focus_out), pctx);
    gtk_box_pack_start (GTK_BOX (head), name, TRUE, TRUE, 0);

    gtk_widget_set_tooltip_text (primary,
            "Nodes that do not pick a specific profile use the primary one");
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (primary), is_primary);
    /* The current primary cannot be un-set by clicking it (pick another to
     * change which is primary); the others toggle it over. */
    gtk_widget_set_sensitive (primary, !is_primary);
    g_signal_connect (primary, "toggled",
                      G_CALLBACK (on_primary_toggled), pctx);
    gtk_box_pack_start (GTK_BOX (head), primary, FALSE, FALSE, 0);

    gtk_widget_set_tooltip_text (del, "Delete this profile");
    g_signal_connect (del, "clicked", G_CALLBACK (on_profile_delete), pctx);
    gtk_box_pack_start (GTK_BOX (head), del, FALSE, FALSE, 0);

    gtk_box_pack_start (GTK_BOX (box), head, FALSE, FALSE, 0);

    /* Field grid. */
    gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 10);
    for (i = 0; i < n; i++)
    {
        const gchar *label = pn_profile_schema_field_get_label (schema, i);
        const gchar *tip   = pn_profile_schema_field_get_tooltip (schema, i);
        GtkWidget   *lab   = gtk_label_new (label);
        GtkWidget   *editor;
        RowVis      *row;

        gtk_label_set_xalign (GTK_LABEL (lab), 0.0);
        gtk_widget_set_halign (lab, GTK_ALIGN_START);
        if (tip != NULL)
            gtk_widget_set_tooltip_text (lab, tip);
        gtk_grid_attach (GTK_GRID (grid), lab, 0, (gint) i, 1, 1);

        editor = build_field_editor (self, schema, p, i, fctx);
        gtk_widget_set_hexpand (editor, TRUE);
        if (tip != NULL)
            gtk_widget_set_tooltip_text (editor, tip);
        gtk_grid_attach (GTK_GRID (grid), editor, 1, (gint) i, 1, 1);

        row         = g_new0 (RowVis, 1);
        row->label  = lab;
        row->editor = editor;
        row->index  = i;
        g_ptr_array_add (fctx->rows, row);
    }
    gtk_box_pack_start (GTK_BOX (box), grid, FALSE, FALSE, 0);

    g_object_set_data_full (G_OBJECT (frame), PROFILE_CTX_KEY, pctx,
                            profile_ctx_free);
    g_object_set_data_full (G_OBJECT (frame), FRAME_CTX_KEY, fctx,
                            frame_ctx_free);
    gtk_container_add (GTK_CONTAINER (frame), box);
    return frame;
}

static void
rebuild_type_page (PnCredentialsDialog *self, const gchar *type_id)
{
    GtkWidget       *list = g_hash_table_lookup (self->lists, type_id);
    PnNodeFactory   *factory = pn_node_factory_get_default ();
    PnProfileSchema *schema;
    PnVault         *vault = pn_vault_get_default ();
    PnProfile       *primary;
    GList           *profiles, *l, *children;

    if (list == NULL)
        return;
    schema = pn_node_factory_lookup_profile_type (factory, type_id);
    if (schema == NULL)
        return;

    self->rebuilding = TRUE;

    children = gtk_container_get_children (GTK_CONTAINER (list));
    for (l = children; l != NULL; l = l->next)
        gtk_widget_destroy (GTK_WIDGET (l->data));
    g_list_free (children);

    primary  = pn_vault_get_default_profile (vault, type_id);
    profiles = pn_vault_list_profiles (vault, type_id);

    if (profiles == NULL)
    {
        GtkWidget *empty = gtk_label_new ("No profiles yet.");
        gtk_label_set_xalign (GTK_LABEL (empty), 0.0);
        gtk_widget_set_sensitive (empty, FALSE);
        gtk_box_pack_start (GTK_BOX (list), empty, FALSE, FALSE, 0);
    }
    else
    {
        for (l = profiles; l != NULL; l = l->next)
        {
            PnProfile *p     = l->data;
            GtkWidget *frame =
                    build_profile_frame (self, schema, p, p == primary);
            gtk_box_pack_start (GTK_BOX (list), frame, FALSE, FALSE, 0);
        }
        g_list_free (profiles);
    }

    gtk_widget_show_all (list);

    /* show_all just re-showed every row; apply each card's visible-when rules
     * on top so conditionally-hidden fields start hidden. */
    children = gtk_container_get_children (GTK_CONTAINER (list));
    for (l = children; l != NULL; l = l->next)
    {
        FrameCtx *fc = g_object_get_data (G_OBJECT (l->data), FRAME_CTX_KEY);
        if (fc != NULL)
            frame_apply_visibility (fc);
    }
    g_list_free (children);

    self->rebuilding = FALSE;
}

/* ------------------------------------------------------------------ */
/*  Add button + type page construction                               */
/* ------------------------------------------------------------------ */

static void
on_add_clicked (GtkButton *button, gpointer user_data)
{
    ProfileCtx      *actx = user_data;  /* only ->self and ->type_id used */
    PnNodeFactory   *factory = pn_node_factory_get_default ();
    PnProfileSchema *schema;

    (void) button;
    if (actx->self->rebuilding)
        return;
    schema = pn_node_factory_lookup_profile_type (factory, actx->type_id);
    if (schema == NULL)
        return;

    pn_vault_create_profile (pn_vault_get_default (), actx->type_id,
                             pn_profile_schema_get_display_name (schema));
    rebuild_type_page (actx->self, actx->type_id);
}

typedef struct {
    PnCredentialsDialog *self;
    gchar               *help_page;
} HelpBtnCtx;

static void
help_btn_ctx_free (gpointer data, GClosure *closure)
{
    HelpBtnCtx *ctx = data;
    (void) closure;
    g_free (ctx->help_page);
    g_free (ctx);
}

static void
on_help_clicked (GtkButton *button, gpointer user_data)
{
    HelpBtnCtx *ctx = user_data;
    (void) button;
    g_signal_emit (ctx->self, signals[SIG_HELP_REQUESTED], 0, ctx->help_page);
}

static GtkWidget *
build_type_page (PnCredentialsDialog *self, PnProfileSchema *schema)
{
    const gchar   *type_id = pn_profile_schema_get_type_id (schema);
    const gchar   *help_page = pn_profile_schema_get_help_page (schema);
    GtkWidget     *box     = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget     *head_row = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget     *heading = gtk_label_new (
            pn_profile_schema_get_display_name (schema));
    GtkWidget     *blurb;
    GtkWidget     *scroll;
    GtkWidget     *list;
    GtkWidget     *add_btn;
    GtkWidget     *add_box;
    ProfileCtx    *actx;
    PangoAttrList *attrs;
    gchar         *blurb_text;
    gchar         *add_label;

    g_object_set (box, "margin-start", 16, "margin-end", 16,
                  "margin-top", 16, "margin-bottom", 16, NULL);

    attrs = pango_attr_list_new ();
    pango_attr_list_insert (attrs, pango_attr_weight_new (PANGO_WEIGHT_BOLD));
    pango_attr_list_insert (attrs, pango_attr_scale_new (PANGO_SCALE_LARGE));
    gtk_label_set_attributes (GTK_LABEL (heading), attrs);
    pango_attr_list_unref (attrs);
    gtk_widget_set_halign (heading, GTK_ALIGN_START);
    gtk_widget_set_hexpand (heading, TRUE);
    gtk_box_pack_start (GTK_BOX (head_row), heading, TRUE, TRUE, 0);

    /* Per-type Help button: visible only when the plugin registered a help
     * page for this profile type.  Routes through the ::help-requested
     * signal so the dialog stays ignorant of help-page paths. */
    if (help_page != NULL)
    {
        GtkWidget  *help_btn = gtk_button_new_from_icon_name (
                "help-browser-symbolic", GTK_ICON_SIZE_BUTTON);
        HelpBtnCtx *hctx     = g_new0 (HelpBtnCtx, 1);

        hctx->self      = self;
        hctx->help_page = g_strdup (help_page);

        gtk_widget_set_tooltip_text (help_btn,
                "Show setup help for this credential type");
        gtk_widget_set_valign (help_btn, GTK_ALIGN_CENTER);
        g_signal_connect_data (help_btn, "clicked",
                               G_CALLBACK (on_help_clicked), hctx,
                               help_btn_ctx_free, 0);
        gtk_box_pack_start (GTK_BOX (head_row), help_btn, FALSE, FALSE, 0);
    }

    gtk_box_pack_start (GTK_BOX (box), head_row, FALSE, FALSE, 0);

    blurb_text = g_strdup_printf (
            "Profiles of type \"%s\".  Secrets are stored in a private "
            "file in your config directory (mode 0600), never in your "
            "workflow files.  A node that does not pick a profile uses the "
            "one marked Primary.", type_id);
    blurb = gtk_label_new (blurb_text);
    g_free (blurb_text);
    gtk_label_set_line_wrap (GTK_LABEL (blurb), TRUE);
    gtk_label_set_xalign (GTK_LABEL (blurb), 0.0);
    gtk_label_set_max_width_chars (GTK_LABEL (blurb), 54);
    gtk_box_pack_start (GTK_BOX (box), blurb, FALSE, FALSE, 0);

    list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (scroll), list);
    gtk_box_pack_start (GTK_BOX (box), scroll, TRUE, TRUE, 0);
    g_hash_table_insert (self->lists, g_strdup (type_id), list);

    /* The add button carries a ProfileCtx with only self + type_id set; it
     * is freed with the button. */
    actx = g_new0 (ProfileCtx, 1);
    actx->self    = self;
    actx->type_id = g_strdup (type_id);

    add_label = g_strdup_printf ("_Add %s",
                                 pn_profile_schema_get_display_name (schema));
    add_btn = gtk_button_new_with_mnemonic (add_label);
    g_free (add_label);
    gtk_button_set_image (GTK_BUTTON (add_btn),
                          gtk_image_new_from_icon_name ("list-add-symbolic",
                                                        GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image (GTK_BUTTON (add_btn), TRUE);
    gtk_widget_set_halign (add_btn, GTK_ALIGN_START);
    g_signal_connect_data (add_btn, "clicked", G_CALLBACK (on_add_clicked),
                           actx, (GClosureNotify) profile_ctx_free, 0);
    add_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start (GTK_BOX (add_box), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (box), add_box, FALSE, FALSE, 0);

    return box;
}

/* ------------------------------------------------------------------ */
/*  Category tree + pages                                             */
/* ------------------------------------------------------------------ */

static void
on_category_selection_changed (GtkTreeSelection *selection,
                               gpointer          user_data)
{
    PnCredentialsDialog *self = user_data;
    GtkTreeIter          iter;
    GtkTreeModel        *model;
    gchar               *page_id = NULL;

    if (!gtk_tree_selection_get_selected (selection, &model, &iter))
        return;
    gtk_tree_model_get (model, &iter, COL_PAGE_ID, &page_id, -1);
    if (page_id != NULL)
        gtk_stack_set_visible_child_name (self->stack, page_id);
    g_free (page_id);
}

static void
build_pages (PnCredentialsDialog *self)
{
    PnNodeFactory     *factory = pn_node_factory_get_default ();
    GList             *types   = pn_node_factory_list_profile_types (factory);
    GList             *l;
    GtkCellRenderer   *renderer;
    GtkTreeViewColumn *column;
    GtkTreeSelection  *selection;

    self->category_store = gtk_tree_store_new (N_COLS,
                                               G_TYPE_STRING, G_TYPE_STRING);

    if (types == NULL)
    {
        /* No plugin has declared a profile type — show a single explainer. */
        GtkWidget   *page = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
        GtkWidget   *lab  = gtk_label_new (
                "No credential profile types are available.\n\n"
                "Profile types are declared by plugins (for example the "
                "bundled network plugin's MQTT broker and HTTP basic auth).  "
                "Load such a plugin to manage its credentials here.");
        GtkTreeIter  iter;

        gtk_label_set_line_wrap (GTK_LABEL (lab), TRUE);
        gtk_label_set_xalign (GTK_LABEL (lab), 0.0);
        g_object_set (page, "margin-start", 16, "margin-end", 16,
                      "margin-top", 16, "margin-bottom", 16, NULL);
        gtk_box_pack_start (GTK_BOX (page), lab, FALSE, FALSE, 0);
        gtk_stack_add_named (self->stack, page, "none");

        gtk_tree_store_append (self->category_store, &iter, NULL);
        gtk_tree_store_set (self->category_store, &iter,
                            COL_LABEL, "Credentials", COL_PAGE_ID, "none", -1);
    }
    else
    {
        for (l = types; l != NULL; l = l->next)
        {
            PnProfileSchema *schema  = l->data;
            const gchar     *type_id = pn_profile_schema_get_type_id (schema);
            GtkWidget       *page    = build_type_page (self, schema);
            GtkTreeIter      iter;

            gtk_stack_add_named (self->stack, page, type_id);
            gtk_tree_store_append (self->category_store, &iter, NULL);
            gtk_tree_store_set (self->category_store, &iter,
                                COL_LABEL,
                                pn_profile_schema_get_display_name (schema),
                                COL_PAGE_ID, type_id, -1);
        }
        g_list_free (types);
    }

    self->category_view = gtk_tree_view_new_with_model (
            GTK_TREE_MODEL (self->category_store));
    g_object_unref (self->category_store);
    gtk_tree_view_set_headers_visible (GTK_TREE_VIEW (self->category_view),
                                       FALSE);
    renderer = gtk_cell_renderer_text_new ();
    column   = gtk_tree_view_column_new_with_attributes (NULL, renderer,
                                                         "text", COL_LABEL,
                                                         NULL);
    gtk_tree_view_append_column (GTK_TREE_VIEW (self->category_view), column);

    selection = gtk_tree_view_get_selection (
            GTK_TREE_VIEW (self->category_view));
    gtk_tree_selection_set_mode (selection, GTK_SELECTION_BROWSE);
    g_signal_connect (selection, "changed",
                      G_CALLBACK (on_category_selection_changed), self);
}

/* Select the category row whose page id is @type_id (no-op if absent). */
static void
select_category (PnCredentialsDialog *self, const gchar *type_id)
{
    GtkTreeModel     *model = GTK_TREE_MODEL (self->category_store);
    GtkTreeSelection *sel   = gtk_tree_view_get_selection (
            GTK_TREE_VIEW (self->category_view));
    GtkTreeIter       iter;
    gboolean          ok;

    if (type_id == NULL)
        return;
    for (ok = gtk_tree_model_get_iter_first (model, &iter); ok;
         ok = gtk_tree_model_iter_next (model, &iter))
    {
        gchar *pid = NULL;
        gtk_tree_model_get (model, &iter, COL_PAGE_ID, &pid, -1);
        if (g_strcmp0 (pid, type_id) == 0)
        {
            gtk_tree_selection_select_iter (sel, &iter);
            g_free (pid);
            return;
        }
        g_free (pid);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_credentials_dialog_dispose (GObject *object)
{
    PnCredentialsDialog *self = PN_CREDENTIALS_DIALOG (object);

    if (singleton_dialog == self)
        singleton_dialog = NULL;
    g_clear_pointer (&self->lists, g_hash_table_unref);

    G_OBJECT_CLASS (pn_credentials_dialog_parent_class)->dispose (object);
}

/* Default handler: open the help page via pn_help_browser_open_page().
 * Connected with G_SIGNAL_RUN_LAST so a host that wants its own help
 * routing (e.g. an embedded browser pane) can stop_emission_by_name()
 * inside its own connector. */
static void
pn_credentials_dialog_real_help_requested (PnCredentialsDialog *self,
                                           const gchar         *help_page)
{
    if (help_page == NULL || *help_page == '\0')
        return;
    pn_help_browser_open_page (GTK_WINDOW (self), help_page, NULL);
}

static void
pn_credentials_dialog_class_init (PnCredentialsDialogClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = pn_credentials_dialog_dispose;

    /**
     * PnCredentialsDialog::help-requested:
     * @self: the dialog
     * @help_page: filename of the HTML help page registered by the
     *      plugin owning the currently visible profile type (the value
     *      passed to pn_profile_schema_set_help_page()).
     *
     * Emitted when the user clicks the per-type Help button.  The
     * default class handler resolves @help_page through the help-page
     * search path and opens it in #PnHelpBrowser; a host that wants its
     * own routing can connect with G_CONNECT_AFTER ahead of the default
     * and call g_signal_stop_emission_by_name() to override.
     */
    signals[SIG_HELP_REQUESTED] = g_signal_new_class_handler (
            "help-requested",
            PN_TYPE_CREDENTIALS_DIALOG,
            G_SIGNAL_RUN_LAST,
            G_CALLBACK (pn_credentials_dialog_real_help_requested),
            NULL, NULL,
            NULL,
            G_TYPE_NONE,
            1,
            G_TYPE_STRING);
}

static void
pn_credentials_dialog_init (PnCredentialsDialog *self)
{
    GtkWidget *outer;
    GtkWidget *paned;
    GtkWidget *left_scroll;
    GtkWidget *button_box;
    GtkWidget *close_button;

    self->lists = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, NULL);

    gtk_window_set_title (GTK_WINDOW (self), "Credentials");
    gtk_window_set_default_size (GTK_WINDOW (self), 640, 460);
    gtk_window_set_modal (GTK_WINDOW (self), FALSE);
    gtk_window_set_position (GTK_WINDOW (self), GTK_WIN_POS_CENTER_ON_PARENT);

    outer = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (self), outer);

    paned = gtk_paned_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position (GTK_PANED (paned), 170);
    gtk_paned_set_wide_handle (GTK_PANED (paned), TRUE);
    gtk_box_pack_start (GTK_BOX (outer), paned, TRUE, TRUE, 0);

    self->stack = GTK_STACK (gtk_stack_new ());
    gtk_stack_set_transition_type (self->stack, GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_hhomogeneous (self->stack, FALSE);
    gtk_stack_set_vhomogeneous (self->stack, FALSE);

    build_pages (self);

    left_scroll = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (left_scroll),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add (GTK_CONTAINER (left_scroll), self->category_view);
    gtk_paned_pack1 (GTK_PANED (paned), left_scroll, FALSE, FALSE);
    gtk_paned_pack2 (GTK_PANED (paned), GTK_WIDGET (self->stack), TRUE, FALSE);

    button_box = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout (GTK_BUTTON_BOX (button_box), GTK_BUTTONBOX_END);
    g_object_set (button_box, "margin-start", 8, "margin-end", 8,
                  "margin-top", 6, "margin-bottom", 6, NULL);
    gtk_box_pack_start (GTK_BOX (outer), button_box, FALSE, FALSE, 0);

    close_button = gtk_button_new_with_mnemonic ("_Close");
    g_signal_connect_swapped (close_button, "clicked",
                              G_CALLBACK (gtk_widget_destroy), self);
    gtk_container_add (GTK_CONTAINER (button_box), close_button);
}

/* ------------------------------------------------------------------ */
/*  Public surface                                                    */
/* ------------------------------------------------------------------ */

GtkWidget *
pn_credentials_dialog_present (GtkWindow *parent, const gchar *select_type_id)
{
    PnCredentialsDialog *dialog = singleton_dialog;
    GHashTableIter       it;
    gpointer             key;

    if (dialog == NULL)
    {
        dialog = g_object_new (PN_TYPE_CREDENTIALS_DIALOG, NULL);
        singleton_dialog = dialog;
    }

    if (parent != NULL)
    {
        gtk_window_set_transient_for (GTK_WINDOW (dialog), parent);
        gtk_window_set_destroy_with_parent (GTK_WINDOW (dialog), TRUE);
        /* Inherit the opener's modality.  Opened from a node's profile
         * picker the parent is the *modal* node settings dialog, whose modal
         * grab would make us unresponsive unless we are modal-and-transient
         * over it; opened from the main window's Edit menu the parent is the
         * non-modal window, so we stay non-modal too. */
        gtk_window_set_modal (GTK_WINDOW (dialog),
                              gtk_window_get_modal (parent));
    }
    else
    {
        gtk_window_set_modal (GTK_WINDOW (dialog), FALSE);
    }

    /* Refill every type page from the current vault state (profiles may have
     * been added by a legacy-credentials import since this was last shown). */
    g_hash_table_iter_init (&it, dialog->lists);
    while (g_hash_table_iter_next (&it, &key, NULL))
        rebuild_type_page (dialog, key);

    select_category (dialog, select_type_id);

    gtk_widget_show_all (GTK_WIDGET (dialog));
    gtk_window_present (GTK_WINDOW (dialog));
    return GTK_WIDGET (dialog);
}
