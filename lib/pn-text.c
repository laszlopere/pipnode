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

#include "pn-text.h"
#include "pn-message.h"

#include <string.h>

struct _PnText
{
    PnNode parent_instance;

    /* The fixed string written to data.output on every message.  Held
     * verbatim as the user typed it in the multi-line editor; the
     * leading/trailing newline trim happens at emit time so the editor
     * never fights the user mid-keystroke. */
    gchar *text;
};

G_DEFINE_TYPE (PnText, pn_text, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_TEXT,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/** Return a newly-allocated copy of @s with every leading and trailing
 *  newline character (\n or \r) removed; interior newlines and all
 *  other whitespace are preserved.  @s may be %NULL, which yields "". */
static gchar *
strip_edge_newlines (const gchar *s)
{
    const gchar *start, *end;

    if (s == NULL)
        return g_strdup ("");

    start = s;
    while (*start == '\n' || *start == '\r')
        start++;

    end = start + strlen (start);
    while (end > start && (end[-1] == '\n' || end[-1] == '\r'))
        end--;

    return g_strndup (start, (gsize) (end - start));
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_text_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnText *self = PN_TEXT (node);
    gchar  *out  = strip_edge_newlines (self->text);

    pn_message_set_string (message, "output", out);
    g_free (out);

    pn_node_emit_message (node, message);
}

/* ------------------------------------------------------------------ */
/*  Property plumbing                                                  */
/* ------------------------------------------------------------------ */

static void
pn_text_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnText *self = PN_TEXT (object);

    switch (prop_id)
    {
    case PROP_TEXT:
        g_value_set_string (value, self->text);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_text_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnText *self = PN_TEXT (object);

    switch (prop_id)
    {
    case PROP_TEXT:
        {
            const gchar *s = g_value_get_string (value);
            if (g_strcmp0 (self->text, s) != 0)
            {
                g_free (self->text);
                self->text = g_strdup (s ? s : "");
                g_object_notify_by_pspec (object, props[PROP_TEXT]);
            }
        }
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* ------------------------------------------------------------------ */
/*  GObject lifecycle                                                  */
/* ------------------------------------------------------------------ */

static void
pn_text_finalize (GObject *object)
{
    PnText *self = PN_TEXT (object);

    g_clear_pointer (&self->text, g_free);

    G_OBJECT_CLASS (pn_text_parent_class)->finalize (object);
}

static void
pn_text_class_init (PnTextClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_text_get_property;
    object_class->set_property = pn_text_set_property;
    object_class->finalize     = pn_text_finalize;
    node_class->receive        = pn_text_receive;

    node_class->class_name     = "Text";
    node_class->icon           = "\xef\x80\xb1";  /* fa-font U+F031 */
    node_class->color          = (PnColor){ 0.36, 0.60, 0.66, 1.0 };
    node_class->category       = "Filters/Reshape";
    node_class->has_input      = TRUE;
    node_class->has_output     = TRUE;

    props[PROP_TEXT] = g_param_spec_string (
            "text", "Text",
            "Fixed string written to data.output on every message; "
            "leading and trailing newlines are stripped",
            "",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    /* Render the property as a multi-line editor in the dialog, spanning
     * the full width with no caption label — the node is itself named
     * "Text", so a "Text:" label beside the editor would just echo it. */
    pn_param_spec_set_multiline  (props[PROP_TEXT]);
    pn_param_spec_set_full_width (props[PROP_TEXT]);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_text_init (PnText *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  teal = { 0.36, 0.60, 0.66, 1.0 };

    self->text = g_strdup ("");

    pn_node_set_class_name (node, "Text");
    pn_node_set_icon       (node, "\xef\x80\xb1");  /* fa-font U+F031 */
    pn_node_set_color      (node, &teal);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnText *
pn_text_new (void)
{
    return g_object_new (PN_TYPE_TEXT, NULL);
}
