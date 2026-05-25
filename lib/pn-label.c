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
/*  PnLabel — logic tier (headless core).                              */
/*                                                                     */
/*  The GTK-free half of the Label node: the GType, all properties     */
/*  (colours, font, lines, alignment, fixed/flexible size), the        */
/*  receive() that reads data.output as a string, the tail computation */
/*  that keeps only the last one or two lines, the geometry fallback   */
/*  and the read seam the gui tier paints from.  The cairo/Pango       */
/*  drawing and the flexible measuring live in pn-label-gui.c, which    */
/*  installs its paint_plot and get_size slots onto this class at       */
/*  editor startup (pn_label_gui_install).  The headless runtime        */
/*  registers and runs this node without pulling GTK.                   */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <json-glib/json-glib.h>

#include "pn-label.h"
#include "pn-message.h"
#include "pn-settings-schema.h"

/* ------------------------------------------------------------------ */
/*  Instance                                                           */
/* ------------------------------------------------------------------ */

struct _PnLabel
{
    PnNode    parent_instance;

    /* Configuration. */
    gint      lines;             /* 1 or 2 trailing lines of output       */
    gchar    *font_family;       /* "Sans", "Monospace", …                */
    gint      font_scale;        /* text height, % of its line's share    */
    gchar    *weight;            /* "Normal"/"Medium"/"Semi-Bold"/"Bold"  */
    gboolean  italic;
    gchar    *alignment;         /* "Left" / "Center" / "Right"           */
    PnColor   text_color;
    PnColor   background_color;

    /* Live state.  @output is the most-recent data.output seen with its
     * carriage returns stripped (or %NULL once nothing is showing);
     * @display_text is that output tailed to the last @lines lines, kept
     * pre-computed so the gui read seam is a plain field copy. */
    gchar    *output;
    gchar    *display_text;      /* never %NULL                           */
};

G_DEFINE_FINAL_TYPE (PnLabel, pn_label, PN_TYPE_NODE)

enum
{
    PROP_0,
    PROP_LINES,
    PROP_FONT_FAMILY,
    PROP_FONT_SCALE,
    PROP_WEIGHT,
    PROP_ITALIC,
    PROP_ALIGNMENT,
    PROP_TEXT_COLOR,
    PROP_BACKGROUND_COLOR,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static PnLabelAlign
align_from_string (const gchar *s)
{
    if (g_strcmp0 (s, "Left") == 0)
        return PN_LABEL_ALIGN_LEFT;
    if (g_strcmp0 (s, "Right") == 0)
        return PN_LABEL_ALIGN_RIGHT;
    return PN_LABEL_ALIGN_CENTER;
}

/* The Pango weight number for a weight name.  These are the values of the
 * PangoWeight enum, used as plain ints so the core stays GTK-free; the gui
 * tier passes them straight to pango_font_description_set_weight(). */
static gint
weight_to_pango (const gchar *w)
{
    if (g_strcmp0 (w, "Bold") == 0)      return 700;
    if (g_strcmp0 (w, "Semi-Bold") == 0) return 600;
    if (g_strcmp0 (w, "Medium") == 0)    return 500;
    return 400;  /* Normal */
}

/* Normalise @s for display: interpret the common backslash escapes so a
 * source that sends an escaped newline ("line1\nline2") breaks into two
 * lines instead of printing a literal "\n", and drop carriage returns so
 * CRLF output renders as clean lines.  Real newline characters are kept
 * as-is.  Returns %NULL for a %NULL, empty, or all-blank result so an
 * output-less message blanks the label. */
static gchar *
label_normalize (const gchar *s)
{
    GString *g;

    if (s == NULL || *s == '\0')
        return NULL;

    g = g_string_new (NULL);
    while (*s != '\0')
    {
        if (*s == '\\' && s[1] != '\0')
        {
            switch (s[1])
            {
            case 'n':  g_string_append_c (g, '\n'); s += 2; continue;
            case 't':  g_string_append_c (g, '\t'); s += 2; continue;
            case 'r':                               s += 2; continue;
            case '\\': g_string_append_c (g, '\\'); s += 2; continue;
            default:   break;
            }
        }
        if (*s != '\r')
            g_string_append_c (g, *s);
        s++;
    }

    if (g->len == 0)
    {
        g_string_free (g, TRUE);
        return NULL;
    }
    return g_string_free (g, FALSE);
}

/* The last @n lines of @text, joined by '\n'.  Trailing blank lines are
 * dropped first so a command's terminating newline does not show as an
 * empty last line.  Returns a fresh "" when there is nothing to show. */
static gchar *
label_tail (const gchar *text, gint n)
{
    gsize  len;
    gssize i;
    gint   found;
    gsize  start;

    if (text == NULL)
        return g_strdup ("");

    len = strlen (text);
    while (len > 0 && text[len - 1] == '\n')
        len--;
    if (len == 0)
        return g_strdup ("");

    if (n < 1)
        n = 1;

    start = 0;
    found = 0;
    for (i = (gssize) len - 1; i >= 0; i--)
    {
        if (text[i] == '\n')
        {
            found++;
            if (found == n)
            {
                start = (gsize) i + 1;
                break;
            }
        }
    }

    return g_strndup (text + start, len - start);
}

/* Recompute @display_text from the current @output and @lines. */
static void
label_recompute_display (PnLabel *self)
{
    g_free (self->display_text);
    self->display_text = label_tail (self->output, self->lines);
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_label_receive (PnNode *node, PnMessage *message)
{
    PnLabel     *self = PN_LABEL (node);
    JsonNode    *member;
    const gchar *text = NULL;

    /* The headline string is data.output.  Anything else (a number-only
     * message, an output member that is not a string) blanks the label —
     * the label only ever shows an output string. */
    member = pn_message_get_member (message, "output");
    if (member != NULL && JSON_NODE_HOLDS_VALUE (member)
        && json_node_get_value_type (member) == G_TYPE_STRING)
        text = json_node_get_string (member);

    pn_label_set_text (self, text);
    /* Pure sink: never forwards. */
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/* ------------------------------------------------------------------ */

void
pn_label_get_paint_state (PnLabel *self, PnLabelPaintState *out)
{
    g_return_if_fail (PN_IS_LABEL (self));
    g_return_if_fail (out != NULL);

    out->text        = self->display_text != NULL ? self->display_text : "";
    out->font_family = self->font_family != NULL ? self->font_family : "";
    out->lines            = self->lines;
    out->font_scale       = self->font_scale;
    out->weight           = weight_to_pango (self->weight);
    out->italic           = self->italic;
    out->align            = align_from_string (self->alignment);
    out->text_color       = self->text_color;
    out->background_color = self->background_color;
}

/* ------------------------------------------------------------------ */
/*  Size vfuncs                                                        */
/*                                                                     */
/*  The footprint is fixed at the Digital Clock's — the node never      */
/*  resizes with its data.  GTK-free, so it is the same in the headless */
/*  runtime and the editor (the gui tier only adds the painter).        */
/* ------------------------------------------------------------------ */

static void
pn_label_get_size (PnNode *node, double *out_w, double *out_h)
{
    (void) node;
    if (out_w != NULL) *out_w = PN_LABEL_WIDTH;
    if (out_h != NULL)
        *out_h = PN_LABEL_HEADER_HEIGHT + PN_LABEL_GAP + PN_LABEL_BODY_HEIGHT;
}

static double
pn_label_get_header_height (PnNode *node)
{
    (void) node;
    return PN_LABEL_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_label_get_property (GObject    *object,
                       guint       prop_id,
                       GValue     *value,
                       GParamSpec *pspec)
{
    PnLabel *self = PN_LABEL (object);

    switch (prop_id)
    {
    case PROP_LINES:
        g_value_set_int (value, self->lines);
        break;
    case PROP_FONT_FAMILY:
        g_value_set_string (value, self->font_family);
        break;
    case PROP_FONT_SCALE:
        g_value_set_int (value, self->font_scale);
        break;
    case PROP_WEIGHT:
        g_value_set_string (value, self->weight);
        break;
    case PROP_ITALIC:
        g_value_set_boolean (value, self->italic);
        break;
    case PROP_ALIGNMENT:
        g_value_set_string (value, self->alignment);
        break;
    case PROP_TEXT_COLOR:
        g_value_set_boxed (value, &self->text_color);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
set_color_prop (PnLabel *self, PnColor *slot,
                const GValue *value, guint prop_id)
{
    const PnColor *new_value = g_value_get_boxed (value);

    if (new_value == NULL || pn_color_equal (slot, new_value))
        return;
    *slot = *new_value;
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
set_string_prop (PnLabel *self, gchar **slot,
                 const GValue *value, guint prop_id)
{
    const gchar *new_value = g_value_get_string (value);

    if (g_strcmp0 (*slot, new_value) == 0)
        return;
    g_free (*slot);
    *slot = g_strdup (new_value);
    g_object_notify_by_pspec (G_OBJECT (self), props[prop_id]);
    pn_node_request_repaint (PN_NODE (self));
}

static void
pn_label_set_property (GObject      *object,
                       guint         prop_id,
                       const GValue *value,
                       GParamSpec   *pspec)
{
    PnLabel *self = PN_LABEL (object);

    switch (prop_id)
    {
    case PROP_LINES:
    {
        gint v = g_value_get_int (value);
        if (v != self->lines)
        {
            self->lines = v;
            /* The visible tail depends on the line count — recompute it
             * before asking for a repaint so the new box re-measures. */
            label_recompute_display (self);
            g_object_notify_by_pspec (object, props[PROP_LINES]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_FONT_FAMILY:
        set_string_prop (self, &self->font_family, value, PROP_FONT_FAMILY);
        break;
    case PROP_FONT_SCALE:
    {
        gint v = g_value_get_int (value);
        if (v != self->font_scale)
        {
            self->font_scale = v;
            g_object_notify_by_pspec (object, props[PROP_FONT_SCALE]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_WEIGHT:
        set_string_prop (self, &self->weight, value, PROP_WEIGHT);
        break;
    case PROP_ITALIC:
    {
        gboolean v = g_value_get_boolean (value);
        if (v != self->italic)
        {
            self->italic = v;
            g_object_notify_by_pspec (object, props[PROP_ITALIC]);
            pn_node_request_repaint (PN_NODE (self));
        }
        break;
    }
    case PROP_ALIGNMENT:
        set_string_prop (self, &self->alignment, value, PROP_ALIGNMENT);
        break;
    case PROP_TEXT_COLOR:
        set_color_prop (self, &self->text_color, value, PROP_TEXT_COLOR);
        break;
    case PROP_BACKGROUND_COLOR:
        set_color_prop (self, &self->background_color, value,
                        PROP_BACKGROUND_COLOR);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_label_finalize (GObject *object)
{
    PnLabel *self = PN_LABEL (object);

    g_free (self->font_family);
    g_free (self->weight);
    g_free (self->alignment);
    g_free (self->output);
    g_free (self->display_text);

    G_OBJECT_CLASS (pn_label_parent_class)->finalize (object);
}

static void
pn_label_class_init (PnLabelClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_label_get_property;
    object_class->set_property = pn_label_set_property;
    object_class->finalize     = pn_label_finalize;

    /* Logic + intrinsic geometry stay in the core class; the cairo/Pango
     * drawing (paint_plot) and the flexible Pango-measuring get_size are
     * installed by the gui tier — see pn_label_gui_install() in
     * pn-label-gui.c. */
    node_class->receive           = pn_label_receive;
    node_class->get_size          = pn_label_get_size;
    node_class->get_header_height = pn_label_get_header_height;

    node_class->class_name = "Label";
    node_class->icon       = "\xef\x80\xb1";  /* fa-font U+F031 */
    /* A teal sink, grouped by colour with the other panel-mirrored
     * readouts (Panel Display, Switch, LED) so a worksheet's panel row
     * reads as one family. */
    node_class->color      = (PnColor){ 0.30, 0.62, 0.62, 1.0 };
    node_class->category   = "Sinks";
    node_class->has_input  = TRUE;
    node_class->has_output = FALSE;

    props[PROP_LINES] = g_param_spec_int (
            "lines", "Lines",
            "How many trailing lines of the output to show — the last "
            "line, or the last two.",
            1, 2, 1,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FONT_FAMILY] = g_param_spec_string (
            "font-family", "Font family",
            "The font family the text is drawn in (e.g. Sans, Serif, "
            "Monospace).  Leave empty to use the desktop's default font, "
            "matching the panel clock.",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_FONT_SCALE] = g_param_spec_int (
            "font-scale", "Font scale",
            "Text height as a percentage of the space one line gets in the "
            "box.  Being relative to the client area, the worksheet node, "
            "the panel-editor preview and the live panel widget all show "
            "the same readout at their own scale.",
            20, 100, 100,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_WEIGHT] = g_param_spec_string (
            "weight", "Weight",
            "Font weight: Normal, Medium, Semi-Bold or Bold.",
            "Normal",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ITALIC] = g_param_spec_boolean (
            "italic", "Italic",
            "Draw the text in an italic style.",
            FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ALIGNMENT] = g_param_spec_string (
            "alignment", "Alignment",
            "Horizontal alignment of the text within the box "
            "(Left, Center or Right).",
            "Center",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the text — a soft off-white by default.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the box behind the text — a dark slate by "
            "default; set its alpha to zero for a transparent label.",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);

    /* Declarative settings dialog: a Text page for the layout / font
     * rows and a Colours page for the two PnColor rows.  No -gui.so
     * companion is needed for the dialog; only the painter lives in the
     * gui tier. */
    {
        PnSettingsSchema *schema = pn_settings_schema_new ();
        static const gchar *const aligns[] = {
            "Left", "Center", "Right", NULL
        };
        static const gchar *const weights[] = {
            "Normal", "Medium", "Semi-Bold", "Bold", NULL
        };

        pn_settings_schema_tab (schema, "Text");
        pn_settings_schema_row (schema, "lines",       PN_EDITOR_SPIN);
        pn_settings_schema_row (schema, "alignment",   PN_EDITOR_COMBO);
        pn_settings_schema_choices (schema, "alignment", aligns);
        pn_settings_schema_row (schema, "font-family", PN_EDITOR_ENTRY);
        pn_settings_schema_row (schema, "font-scale",  PN_EDITOR_SPIN);
        pn_settings_schema_row (schema, "weight",      PN_EDITOR_COMBO);
        pn_settings_schema_choices (schema, "weight", weights);
        pn_settings_schema_row (schema, "italic",      PN_EDITOR_AUTO);

        pn_settings_schema_tab (schema, "Colours");
        pn_settings_schema_row (schema, "text-color",       PN_EDITOR_AUTO);
        pn_settings_schema_row (schema, "background-color", PN_EDITOR_AUTO);

        pn_node_class_set_settings_schema (node_class, schema);
    }
}

static void
pn_label_init (PnLabel *self)
{
    PnNode *node = PN_NODE (self);

    self->lines            = 1;
    self->font_family      = g_strdup ("");
    self->font_scale       = 100;
    self->weight           = g_strdup ("Normal");
    self->italic           = FALSE;
    self->alignment        = g_strdup ("Center");
    self->text_color       = (PnColor){ 0.90, 0.92, 0.94, 1.0 };
    self->background_color = (PnColor){ 0.10, 0.11, 0.13, 1.0 };

    self->output       = NULL;
    self->display_text = g_strdup ("");

    {
        PnColor teal = { 0.30, 0.62, 0.62, 1.0 };
        pn_node_set_color (node, &teal);
    }
    pn_node_set_class_name (node, "Label");
    pn_node_set_icon       (node, "\xef\x80\xb1");  /* fa-font */
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, FALSE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnLabel *
pn_label_new (void)
{
    return g_object_new (PN_TYPE_LABEL, NULL);
}

void
pn_label_set_text (PnLabel *self, const gchar *output)
{
    gchar *norm;

    g_return_if_fail (PN_IS_LABEL (self));

    norm = label_normalize (output);

    /* No change to the raw output — nothing to repaint. */
    if (g_strcmp0 (norm, self->output) == 0)
    {
        g_free (norm);
        return;
    }

    g_free (self->output);
    self->output = norm;
    label_recompute_display (self);

    pn_node_request_repaint (PN_NODE (self));
}
