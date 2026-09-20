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

#include "pn-calc-engine.h"
#include "pn-message.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <string.h>

/* fa-cogs U+F085 — the works behind the keys. */
#define PN_CALC_ENGINE_ICON "\xef\x82\x85"

/* What an errored display reads.  Not localised: it travels in
 * data.output as a machine-visible token too, and a downstream Filter
 * matching on it should not depend on the user's locale. */
#define PN_CALC_ENGINE_ERROR_TEXT "Error"

/* Typed-entry buffer.  Comfortably larger than max-digits allows, so a
 * decimal point, a leading "0." and a sign never overrun it. */
#define PN_CALC_ENGINE_ENTRY_MAX 64

#define PN_CALC_ENGINE_DIGITS_MIN 1
#define PN_CALC_ENGINE_DIGITS_MAX 15
#define PN_CALC_ENGINE_DIGITS_DEF 12

struct _PnCalcEngine
{
    PnNode parent_instance;

    /* How many digits may be typed into one entry. */
    guint    max_digits;

    /* The running total, and the operator waiting for its right-hand
     * operand ('+', '-', '*', '/', or 0 for none). */
    gdouble  accumulator;
    gchar    pending;

    /* The number as typed.  Always the thing on the display: an
     * operator key overwrites it with the folded accumulator, so the
     * readout and this buffer never disagree. */
    gchar    entry[PN_CALC_ENGINE_ENTRY_MAX];

    /* %TRUE while the user is typing into @entry.  %FALSE means the
     * next digit starts a fresh number rather than appending to a
     * result. */
    gboolean entering;

    /* Latched on a division by zero; only "C" clears it. */
    gboolean error;
};

G_DEFINE_TYPE (PnCalcEngine, pn_calc_engine, PN_TYPE_NODE)

enum {
    PROP_0,
    PROP_MAX_DIGITS,
    N_PROPS,
};

static GParamSpec *props[N_PROPS];

/* ------------------------------------------------------------------ */
/*  Display formatting                                                 */
/* ------------------------------------------------------------------ */

/** Render @v into the entry buffer.  "%.10g" keeps a result readable
 *  (1/3 reads 0.3333333333, not 0.33333333333333331) while still
 *  round-tripping through the strtod() on the way back in, so folding
 *  a result into the next operation loses nothing the display showed.
 *  Locale-independent: the entry is parsed with g_ascii_strtod, so it
 *  must be written with the ASCII decimal point too. */
static void
set_entry_from_double (PnCalcEngine *self, gdouble v)
{
    /* Collapse the negative zero a subtraction can leave behind, so the
     * display reads "0" rather than "-0". */
    if (v == 0.0)
        v = 0.0;

    g_ascii_formatd (self->entry, sizeof self->entry, "%.10g", v);
}

/** Digits typed so far, ignoring the decimal point and any sign — the
 *  count max-digits caps. */
static guint
entry_digit_count (PnCalcEngine *self)
{
    const gchar *p;
    guint        n = 0;

    for (p = self->entry; *p != '\0'; p++)
        if (g_ascii_isdigit (*p))
            n++;

    return n;
}

/* ------------------------------------------------------------------ */
/*  The state machine                                                  */
/* ------------------------------------------------------------------ */

void
pn_calc_engine_reset (PnCalcEngine *self)
{
    g_return_if_fail (PN_IS_CALC_ENGINE (self));

    self->accumulator = 0.0;
    self->pending     = 0;
    self->entering    = FALSE;
    self->error       = FALSE;
    g_strlcpy (self->entry, "0", sizeof self->entry);

    pn_node_set_has_error (PN_NODE (self), FALSE);
}

/** Fold the typed entry into the accumulator under the pending
 *  operator, then show the result.  With no pending operator the entry
 *  simply becomes the accumulator, which is what makes the first
 *  operand of a sum work without a special case. */
static void
apply_pending (PnCalcEngine *self)
{
    gdouble cur = g_ascii_strtod (self->entry, NULL);

    switch (self->pending)
    {
    case '+': self->accumulator += cur; break;
    case '-': self->accumulator -= cur; break;
    case '*': self->accumulator *= cur; break;
    case '/':
        if (cur == 0.0)
        {
            /* The one dead end a four-function calculator has.  Latch
             * it rather than letting an infinity propagate downstream:
             * the node paints its error state and the next message a
             * Filter sees says success = FALSE. */
            self->error       = TRUE;
            self->accumulator = 0.0;
            self->entering    = FALSE;
            g_strlcpy (self->entry, PN_CALC_ENGINE_ERROR_TEXT,
                       sizeof self->entry);
            pn_node_set_has_error (PN_NODE (self), TRUE);
            return;
        }
        self->accumulator /= cur;
        break;
    default:
        /* No pending operation: the entry *is* the running total. */
        self->accumulator = cur;
        break;
    }

    /* A calculation that runs off to infinity or NaN (a long chain of
     * multiplications, say) is an error for the same reason a division
     * by zero is — there is no number to show. */
    if (!isfinite (self->accumulator))
    {
        self->error       = TRUE;
        self->accumulator = 0.0;
        self->entering    = FALSE;
        g_strlcpy (self->entry, PN_CALC_ENGINE_ERROR_TEXT,
                   sizeof self->entry);
        pn_node_set_has_error (PN_NODE (self), TRUE);
        return;
    }

    set_entry_from_double (self, self->accumulator);
    self->entering = FALSE;
}

/** Append one typed digit. */
static void
press_digit (PnCalcEngine *self, gchar d)
{
    gsize len;

    if (!self->entering)
    {
        /* A digit after a result starts a new number instead of
         * extending the result. */
        self->entry[0]  = '\0';
        self->entering  = TRUE;
    }

    /* A leading zero is a placeholder, not a digit: "0" then "5" is 5,
     * not 05.  "0." is left alone — there the zero is real. */
    if (g_strcmp0 (self->entry, "0") == 0)
        self->entry[0] = '\0';

    if (entry_digit_count (self) >= self->max_digits)
        return;                      /* the entry is full */

    len = strlen (self->entry);
    if (len + 2 > sizeof self->entry)
        return;

    self->entry[len]     = d;
    self->entry[len + 1] = '\0';
}

/** Append the decimal point, at most one per number. */
static void
press_point (PnCalcEngine *self)
{
    gsize len;

    if (!self->entering)
    {
        /* A point after a result starts "0." rather than tacking a
         * fractional part onto the result. */
        g_strlcpy (self->entry, "0", sizeof self->entry);
        self->entering = TRUE;
    }

    if (strchr (self->entry, '.') != NULL)
        return;                      /* already has one */

    len = strlen (self->entry);
    if (len + 2 > sizeof self->entry)
        return;

    self->entry[len]     = '.';
    self->entry[len + 1] = '\0';
}

gboolean
pn_calc_engine_press (PnCalcEngine *self, const gchar *key)
{
    g_return_val_if_fail (PN_IS_CALC_ENGINE (self), FALSE);
    g_return_val_if_fail (key != NULL, FALSE);

    /* An errored engine is a dead end until it is cleared — exactly
     * what a pocket calculator showing "E" does. */
    if (self->error && g_strcmp0 (key, "C") != 0)
        return FALSE;

    if (key[0] != '\0' && key[1] == '\0' && g_ascii_isdigit (key[0]))
    {
        press_digit (self, key[0]);
        return TRUE;
    }

    if (g_strcmp0 (key, ".") == 0)
    {
        press_point (self);
        return TRUE;
    }

    if (g_strcmp0 (key, "+") == 0 || g_strcmp0 (key, "-") == 0 ||
        g_strcmp0 (key, "*") == 0 || g_strcmp0 (key, "/") == 0)
    {
        /* Fold what is typed into the running total, show it, and hold
         * the new operator for the operand still to come. */
        apply_pending (self);
        if (!self->error)
            self->pending = key[0];
        return TRUE;
    }

    if (g_strcmp0 (key, "=") == 0)
    {
        apply_pending (self);
        self->pending = 0;
        return TRUE;
    }

    if (g_strcmp0 (key, "C") == 0)
    {
        pn_calc_engine_reset (self);
        return TRUE;
    }

    if (g_strcmp0 (key, "CE") == 0)
    {
        /* Only the number being typed goes; the accumulator and the
         * pending operator survive, so a mistyped operand can be
         * retyped without restarting the sum. */
        g_strlcpy (self->entry, "0", sizeof self->entry);
        self->entering = FALSE;
        return TRUE;
    }

    /* Not a key this calculator has. */
    return FALSE;
}

/* ------------------------------------------------------------------ */
/*  Read seam                                                          */
/* ------------------------------------------------------------------ */

gdouble
pn_calc_engine_get_display (PnCalcEngine *self)
{
    g_return_val_if_fail (PN_IS_CALC_ENGINE (self), 0.0);

    if (self->error)
        return 0.0;

    return g_ascii_strtod (self->entry, NULL);
}

const gchar *
pn_calc_engine_get_display_text (PnCalcEngine *self)
{
    g_return_val_if_fail (PN_IS_CALC_ENGINE (self), "");
    return self->entry;
}

gboolean
pn_calc_engine_get_error (PnCalcEngine *self)
{
    g_return_val_if_fail (PN_IS_CALC_ENGINE (self), FALSE);
    return self->error;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

/** Pull the key code out of @message under "key".  Only a JSON string
 *  counts: a keypad emits one, and anything else is a message that
 *  happens to have flowed in here rather than a keystroke. */
static const gchar *
read_key (PnMessage *message)
{
    JsonNode *node = pn_message_get_member (message, "key");

    if (node == NULL || !JSON_NODE_HOLDS_VALUE (node))
        return NULL;
    if (json_node_get_value_type (node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string (node);
}

static void
pn_calc_engine_receive (PnNode *node, PnMessage *message)
{
    PnCalcEngine *self = PN_CALC_ENGINE (node);
    const gchar  *key  = read_key (message);
    PnMessage    *out;

    /* Nothing that looks like a keystroke — stay silent and keep the
     * half-typed sum intact. */
    if (key == NULL)
        return;

    if (!pn_calc_engine_press (self, key))
        return;

    /* Emit the whole display on every accepted key, not just on "=",
     * so a seven-segment readout downstream tracks the typing digit by
     * digit the way a calculator's own display does. */
    out = pn_message_new (node, NULL);
    pn_message_set_double  (out, "value",   pn_calc_engine_get_display (self));
    pn_message_set_string  (out, "output",  self->entry);
    pn_message_set_boolean (out, "success", !self->error);

    pn_node_emit_message (node, out);
    g_object_unref (out);
}

/* ------------------------------------------------------------------ */
/*  GObject boilerplate                                                */
/* ------------------------------------------------------------------ */

static void
pn_calc_engine_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnCalcEngine *self = PN_CALC_ENGINE (object);

    switch (prop_id)
    {
    case PROP_MAX_DIGITS:
        g_value_set_uint (value, self->max_digits);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_calc_engine_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnCalcEngine *self = PN_CALC_ENGINE (object);

    switch (prop_id)
    {
    case PROP_MAX_DIGITS:
        self->max_digits = g_value_get_uint (value);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_calc_engine_class_init (PnCalcEngineClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_calc_engine_get_property;
    object_class->set_property = pn_calc_engine_set_property;

    node_class->receive = pn_calc_engine_receive;

    node_class->palette_icon = PN_CALC_ENGINE_ICON;
    node_class->class_name   = "Calculator Engine";
    node_class->icon         = PN_CALC_ENGINE_ICON;
    /* The Calculator / Keypad violet: the three nodes are one family
     * and a worksheet should show that. */
    node_class->color        = (PnColor){ 0.55, 0.45, 0.80, 1.0 };
    node_class->category     = "Filters/Expressions";
    node_class->has_input    = TRUE;
    node_class->has_output   = TRUE;

    props[PROP_MAX_DIGITS] = g_param_spec_uint (
            "max-digits", "Maximum digits",
            "How many digits may be typed into one number before the "
            "entry stops accepting them — the digit count of the "
            "calculator being imitated.  Caps typing only: a computed "
            "result is shown in full.",
            PN_CALC_ENGINE_DIGITS_MIN,
            PN_CALC_ENGINE_DIGITS_MAX,
            PN_CALC_ENGINE_DIGITS_DEF,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_calc_engine_init (PnCalcEngine *self)
{
    PnNode *node = PN_NODE (self);

    self->max_digits = PN_CALC_ENGINE_DIGITS_DEF;
    pn_calc_engine_reset (self);

    pn_node_set_class_name (node, "Calculator Engine");
    pn_node_set_icon       (node, PN_CALC_ENGINE_ICON);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);

    {
        PnColor violet = { 0.55, 0.45, 0.80, 1.0 };
        pn_node_set_color (node, &violet);
    }
}

PnCalcEngine *
pn_calc_engine_new (void)
{
    return g_object_new (PN_TYPE_CALC_ENGINE, NULL);
}
