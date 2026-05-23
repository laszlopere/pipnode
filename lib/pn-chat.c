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

#include "pn-chat.h"
#include "pn-json-path.h"
#include "pn-message.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Geometry                                                           */
/*                                                                     */
/*  Match PnGraph / PnTable so a row of mixed sinks aligns visually:   */
/*  280 px wide, 40 px standard header, 4 px gap.  The body is taller  */
/*  than the table's so a few wrapped bubbles plus the entry strip     */
/*  fit at the canonical size; the entry strip is a fixed 28 px so a  */
/*  tall body still leaves the maximum room for bubbles.               */
/* ------------------------------------------------------------------ */

#define PN_CHAT_WIDTH         280.0
#define PN_CHAT_HEADER_HEIGHT  40.0
#define PN_CHAT_GAP             4.0
#define PN_CHAT_BODY_HEIGHT   220.0
#define PN_CHAT_TOTAL_HEIGHT  (PN_CHAT_HEADER_HEIGHT + \
                               PN_CHAT_GAP +           \
                               PN_CHAT_BODY_HEIGHT)
#define PN_CHAT_INPUT_HEIGHT   28.0

/* Hard ceiling on the configurable history limit.  A chat is a UI
 * surface, not a database; past a few hundred bubbles the per-bubble
 * Pango layout footprint and re-paint cost start to matter on a
 * high-arrival-rate feed. */
#define PN_CHAT_LIMIT_MAX 1000

/* Match PnTable / PnGraph's 10 Hz repaint cap so a high-frequency
 * meshtastic feed cannot push the worksheet to redraw at the display
 * refresh rate. */
#define PN_CHAT_MIN_REPAINT_INTERVAL_US (G_TIME_SPAN_MILLISECOND * 100)

/* Inset and bubble layout. */
#define PN_CHAT_INSET           8.0
#define PN_CHAT_BUBBLE_GAP      6.0
#define PN_CHAT_BUBBLE_PAD_X    8.0
#define PN_CHAT_BUBBLE_PAD_Y    4.0
#define PN_CHAT_BUBBLE_RADIUS   8.0
#define PN_CHAT_BUBBLE_MAX_FRAC 0.78    /* of inner_w */
#define PN_CHAT_FONT_PX         11.0
#define PN_CHAT_SENDER_FONT_PX   9.0

/* Send-button width inside the entry strip (right edge). */
#define PN_CHAT_SEND_BUTTON_W   42.0

/* Caret blink period (milliseconds for one full on→off→on cycle).
 * Matches typical GtkEntry feel without bringing in
 * gtk_settings_get_default()'s cursor-blink-time read overhead. */
#define PN_CHAT_CARET_BLINK_MS  1000

/* ------------------------------------------------------------------ */
/*  Per-bubble state                                                   */
/*                                                                     */
/*  PnChatBubble is published in pn-chat.h: it is plain data (no GTK)   */
/*  and the gui-tier painter has to read the same bubbles receive() and */
/*  pn_chat_submit() push, so the struct crosses the tier boundary      */
/*  through the public header.                                          */
/* ------------------------------------------------------------------ */

static void
pn_chat_bubble_free (gpointer ptr)
{
    PnChatBubble *b = ptr;
    if (b == NULL)
        return;
    g_free (b->sender);
    g_free (b->text);
    g_free (b);
}

struct _PnChat
{
    PnNode parent_instance;

    /* User-facing properties. */
    gchar   *text_path;
    gchar   *sender_path;
    gchar   *me_name;
    guint    limit;
    PnColor  background_color;
    PnColor  border_color;
    PnColor  text_color;
    PnColor  me_color;
    PnColor  input_background_color;
    PnColor  send_button_color;

    /* Bubble buffer.  Newest at tail (chat-style: scroll bottom). */
    GQueue *bubbles;

    /* Scroll state — number of bubbles to skip from the *tail* (newest)
     * end when painting.  Honoured both on the canvas body and in the
     * zoom overlay; the wheel adjusts it via the scroll vfunc. */
    int scroll_offset;

    /* Repaint throttle, identical shape to PnTable's. */
    gint64 last_repaint_us;
    guint  pending_repaint_id;

    /* Canvas-resident text-input state.  draft is owned by this
     * struct and never NULL once allocated; caret_byte is a byte
     * offset into draft (always at a UTF-8 character boundary).
     * focused drives the caret-blink timer the worksheet uses to
     * decide whether to dispatch keystrokes here. */
    gboolean focused;
    GString *draft;
    gsize    caret_byte;

    /* Caret blink timer.  Runs only while #focused is TRUE; toggles
     * #caret_visible on each tick and queues a repaint. */
    guint    caret_blink_id;
    gboolean caret_visible;

    /* Horizontal scroll within the entry rectangle, in pixels.  The
     * paint pass updates this so the caret is always inside the
     * visible portion of the rectangle even when the draft is wider
     * than the entry. */
    double draft_scroll_px;
};

/* ------------------------------------------------------------------ */
/*  Properties                                                         */
/* ------------------------------------------------------------------ */

enum
{
    PROP_0,
    PROP_TEXT_PATH,
    PROP_SENDER_PATH,
    PROP_ME_NAME,
    PROP_LIMIT,
    PROP_BACKGROUND_COLOR,
    PROP_BORDER_COLOR,
    PROP_TEXT_COLOR,
    PROP_ME_COLOR,
    PROP_INPUT_BACKGROUND_COLOR,
    PROP_SEND_BUTTON_COLOR,
    N_PROPS
};

static GParamSpec *props[N_PROPS];

G_DEFINE_TYPE (PnChat, pn_chat, PN_TYPE_NODE)

/* Forward declarations. */
static void schedule_repaint (PnChat *self);
static void pn_chat_trim_to_limit (PnChat *self);
static void caret_timer_stop (PnChat *self);
static void caret_timer_start (PnChat *self);

/* ------------------------------------------------------------------ */
/*  Repaint throttle                                                   */
/* ------------------------------------------------------------------ */

static gboolean
on_pending_repaint (gpointer user_data)
{
    PnChat *self = PN_CHAT (user_data);

    self->pending_repaint_id = 0;
    self->last_repaint_us    = g_get_monotonic_time ();
    pn_node_request_repaint (PN_NODE (self));

    return G_SOURCE_REMOVE;
}

static void
schedule_repaint (PnChat *self)
{
    gint64 now_us  = g_get_monotonic_time ();
    gint64 elapsed = now_us - self->last_repaint_us;

    if (self->pending_repaint_id != 0)
        return;

    if (elapsed >= PN_CHAT_MIN_REPAINT_INTERVAL_US)
    {
        self->last_repaint_us = now_us;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    {
        gint64 remaining_us = PN_CHAT_MIN_REPAINT_INTERVAL_US - elapsed;
        guint  delay_ms     = (guint) ((remaining_us + 999) / 1000);

        if (delay_ms == 0)
            delay_ms = 1;
        self->pending_repaint_id =
                g_timeout_add (delay_ms, on_pending_repaint, self);
    }
}

/* ------------------------------------------------------------------ */
/*  Bubble buffer management                                           */
/* ------------------------------------------------------------------ */

static void
pn_chat_clear_bubbles (PnChat *self)
{
    g_queue_clear_full (self->bubbles, pn_chat_bubble_free);
}

static void
pn_chat_trim_to_limit (PnChat *self)
{
    while (g_queue_get_length (self->bubbles) > self->limit)
    {
        PnChatBubble *b = g_queue_pop_head (self->bubbles);
        pn_chat_bubble_free (b);
    }
}

static void
pn_chat_push_bubble (
        PnChat      *self,
        const gchar *sender,
        const gchar *text,
        gboolean     mine)
{
    PnChatBubble *b;

    if (text == NULL || *text == '\0')
        return;

    b = g_new0 (PnChatBubble, 1);
    b->sender      = (sender != NULL && *sender != '\0')
                         ? g_strdup (sender) : NULL;
    b->text        = g_strdup (text);
    b->mine        = mine;
    b->received_us = g_get_monotonic_time ();

    g_queue_push_tail (self->bubbles, b);
    pn_chat_trim_to_limit (self);

    /* New activity always pins the view to the newest message — same
     * behaviour as a real chat client (scroll-to-bottom on send /
     * receive) so a stale scroll offset doesn't hide what just
     * arrived. */
    self->scroll_offset = 0;

    schedule_repaint (self);
}

/* ------------------------------------------------------------------ */
/*  JSON helpers                                                       */
/* ------------------------------------------------------------------ */

/** Resolve @path against the message's lookup root and stringify the
 *  result.  Returns %NULL when the path is missing or resolves to a
 *  non-scalar (object / array) value — chat text and sender labels
 *  are expected to be scalars, and a complex value would render as a
 *  meaningless placeholder.  Caller owns the returned string. */
static gchar *
resolve_string (
        JsonObject  *root,
        const gchar *path)
{
    JsonNode *node;

    if (path == NULL || *path == '\0' || root == NULL)
        return NULL;

    node = pn_json_resolve_path (root, path);
    if (node == NULL)
        return NULL;

    switch (json_node_get_node_type (node))
    {
    case JSON_NODE_VALUE:
    {
        GType vt = json_node_get_value_type (node);
        if (vt == G_TYPE_STRING)
            return g_strdup (json_node_get_string (node));
        if (vt == G_TYPE_INT64)
            return g_strdup_printf ("%" G_GINT64_FORMAT,
                                    json_node_get_int (node));
        if (vt == G_TYPE_DOUBLE)
        {
            gchar buf[G_ASCII_DTOSTR_BUF_SIZE];
            g_ascii_formatd (buf, sizeof buf, "%g",
                             json_node_get_double (node));
            return g_strdup (buf);
        }
        if (vt == G_TYPE_BOOLEAN)
            return g_strdup (json_node_get_boolean (node)
                                 ? "true" : "false");
        return NULL;
    }
    default:
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Caret blink timer                                                  */
/* ------------------------------------------------------------------ */

static gboolean
on_caret_blink_tick (gpointer user_data)
{
    PnChat *self = PN_CHAT (user_data);

    self->caret_visible = !self->caret_visible;
    pn_node_request_repaint (PN_NODE (self));
    return G_SOURCE_CONTINUE;
}

static void
caret_timer_start (PnChat *self)
{
    if (self->caret_blink_id != 0)
        return;
    self->caret_visible = TRUE;
    /* Half the full cycle per toggle. */
    self->caret_blink_id = g_timeout_add (PN_CHAT_CARET_BLINK_MS / 2,
                                          on_caret_blink_tick, self);
}

static void
caret_timer_stop (PnChat *self)
{
    if (self->caret_blink_id != 0)
    {
        g_source_remove (self->caret_blink_id);
        self->caret_blink_id = 0;
    }
    self->caret_visible = FALSE;
}

/* ------------------------------------------------------------------ */
/*  Receive                                                            */
/* ------------------------------------------------------------------ */

static void
pn_chat_receive (
        PnNode    *node,
        PnMessage *message)
{
    PnChat     *self = PN_CHAT (node);
    JsonObject *root;
    gchar      *text;
    gchar      *sender;
    PnNode     *source;

    /* Self-loop suppression: a chat that emits on its output and is
     * wired back into its own input would otherwise duplicate every
     * sent bubble (once at send, once on the loop-back receive).  The
     * send path already pushes the "mine" bubble locally, so drop the
     * loop-back. */
    source = pn_message_get_source (message);
    if (source == node)
        return;

    root   = pn_json_lookup_root_for_message (message);
    text   = resolve_string (root, self->text_path);
    sender = resolve_string (root, self->sender_path);
    json_object_unref (root);

    if (text != NULL && *text != '\0')
        pn_chat_push_bubble (self, sender, text, FALSE);

    g_free (text);
    g_free (sender);
}

/* ------------------------------------------------------------------ */
/*  Scroll                                                             */
/* ------------------------------------------------------------------ */

static void
pn_chat_scroll (PnNode *node, double dy)
{
    PnChat *self = PN_CHAT (node);
    int     step = (int) lround (dy * 2.0);

    if (step == 0)
        step = (dy > 0) ? 1 : (dy < 0 ? -1 : 0);
    if (step == 0)
        return;

    /* Wheel-up (negative dy in GTK) scrolls back through history,
     * showing older bubbles — same direction convention as the table
     * sink. */
    self->scroll_offset += step;
    if (self->scroll_offset < 0)
        self->scroll_offset = 0;

    pn_node_request_repaint (PN_NODE (self));
}

/* ------------------------------------------------------------------ */
/*  Geometry vfuncs                                                    */
/* ------------------------------------------------------------------ */

static void
pn_chat_get_size (
        PnNode *self,
        double *out_width,
        double *out_height)
{
    (void) self;
    if (out_width  != NULL) *out_width  = PN_CHAT_WIDTH;
    if (out_height != NULL) *out_height = PN_CHAT_TOTAL_HEIGHT;
}

static double
pn_chat_get_header_height (PnNode *self)
{
    (void) self;
    return PN_CHAT_HEADER_HEIGHT;
}

/* ------------------------------------------------------------------ */
/*  Hit-testing                                                        */
/* ------------------------------------------------------------------ */

PnChatHit
pn_chat_hit_input_in_rect (
        PnChat *self,
        double  rect_x,
        double  rect_y,
        double  rect_w,
        double  rect_h,
        double  px,
        double  py)
{
    /* Mirror the entry-strip placement in pn_chat_paint_plot so a
     * pixel showing the strip always classifies as a strip hit, no
     * matter where the body has been painted (natural worksheet
     * position, lifted zoom overlay, …). */
    const double inset   = PN_CHAT_INSET;
    const double input_h = PN_CHAT_INPUT_HEIGHT;
    const double ix      = rect_x + inset;
    const double iy      = rect_y + rect_h - input_h - inset / 2.0;
    const double iw      = rect_w - 2 * inset;
    const double ih      = input_h;
    const double sb_w    = PN_CHAT_SEND_BUTTON_W;
    const double sb_x    = ix + iw - sb_w;

    g_return_val_if_fail (PN_IS_CHAT (self), PN_CHAT_HIT_NONE);

    if (!(px >= ix && px < ix + iw &&
          py >= iy && py < iy + ih))
        return PN_CHAT_HIT_NONE;
    if (px >= sb_x)
        return PN_CHAT_HIT_SEND;
    return PN_CHAT_HIT_ENTRY;
}

PnChatHit
pn_chat_hit_input (
        PnChat *self,
        double  px,
        double  py)
{
    const PnPoint *p;
    double         body_x, body_y;

    g_return_val_if_fail (PN_IS_CHAT (self), PN_CHAT_HIT_NONE);

    p      = pn_node_get_position (PN_NODE (self));
    body_x = p->x;
    body_y = p->y + PN_CHAT_HEADER_HEIGHT + PN_CHAT_GAP;

    return pn_chat_hit_input_in_rect (self,
                                      body_x, body_y,
                                      PN_CHAT_WIDTH,
                                      PN_CHAT_BODY_HEIGHT,
                                      px, py);
}

/* ------------------------------------------------------------------ */
/*  Focus / submit / key handling                                      */
/* ------------------------------------------------------------------ */

void
pn_chat_set_focused (PnChat *self, gboolean focused)
{
    g_return_if_fail (PN_IS_CHAT (self));

    focused = !!focused;
    if (self->focused == focused)
        return;

    self->focused = focused;
    if (focused)
        caret_timer_start (self);
    else
        caret_timer_stop (self);

    pn_node_request_repaint (PN_NODE (self));
}

gboolean
pn_chat_get_focused (PnChat *self)
{
    g_return_val_if_fail (PN_IS_CHAT (self), FALSE);
    return self->focused;
}

void
pn_chat_submit (PnChat *self)
{
    PnMessage *msg;
    gchar     *trimmed;

    g_return_if_fail (PN_IS_CHAT (self));

    if (self->draft == NULL || self->draft->len == 0)
        return;

    /* Strip leading/trailing whitespace so an accidental bare-Enter
     * or trailing space doesn't fire a near-empty message. */
    trimmed = g_strstrip (g_strdup (self->draft->str));
    if (*trimmed == '\0')
    {
        g_free (trimmed);
        g_string_truncate (self->draft, 0);
        self->caret_byte      = 0;
        self->draft_scroll_px = 0.0;
        pn_node_request_repaint (PN_NODE (self));
        return;
    }

    /* Pass %NULL so the envelope picks up the node's resolved topic
     * (the base-class template "/pnode/${nodeclass}/${nodename}", or
     * whatever override the user typed in the dialog).  The old chat-
     * local "topic" property has been retired now that PnNode itself
     * carries one. */
    msg = pn_message_new (PN_NODE (self), NULL);
    pn_message_set_string  (msg, "output", trimmed);
    pn_message_set_boolean (msg, "success", TRUE);
    if (self->me_name != NULL && *self->me_name != '\0')
        pn_message_set_string (msg, "from_long_name", self->me_name);

    pn_node_emit_message (PN_NODE (self), msg);
    g_object_unref (msg);

    pn_chat_push_bubble (self,
                         (self->me_name != NULL && *self->me_name != '\0')
                             ? self->me_name : "Me",
                         trimmed, TRUE);

    g_free (trimmed);

    g_string_truncate (self->draft, 0);
    self->caret_byte      = 0;
    self->draft_scroll_px = 0.0;
    pn_node_request_repaint (PN_NODE (self));
}

/** Step @byte_off backward by one UTF-8 character within @str.
 *  Returns the new offset; clamps at 0 when already at the start. */
static gsize
utf8_prev (const gchar *str, gsize byte_off)
{
    if (byte_off == 0)
        return 0;
    do {
        byte_off--;
    } while (byte_off > 0 &&
             (((unsigned char) str[byte_off]) & 0xC0) == 0x80);
    return byte_off;
}

/** Step @byte_off forward by one UTF-8 character within @str.
 *  Returns the new offset; clamps at strlen(@str) when already at the
 *  end. */
static gsize
utf8_next (const gchar *str, gsize byte_off, gsize len)
{
    if (byte_off >= len)
        return len;
    /* Advance past the lead byte. */
    byte_off++;
    while (byte_off < len &&
           (((unsigned char) str[byte_off]) & 0xC0) == 0x80)
        byte_off++;
    return byte_off;
}

/* The GdkEventKey translator (pn_chat_handle_key_press) lives in the gui
 * tier — it reads GDK keyvals — and dispatches into the GTK-free
 * draft-editing primitives below.  Each primitive mutates the draft /
 * caret, shows the caret and queues a repaint exactly as the old inline
 * handler did. */

void
pn_chat_draft_insert (PnChat *self, const gchar *text, gssize len)
{
    g_return_if_fail (PN_IS_CHAT (self));
    if (text == NULL || len == 0)
        return;

    if (self->draft == NULL)
        self->draft = g_string_new (NULL);

    g_string_insert_len (self->draft, (gssize) self->caret_byte, text, len);
    self->caret_byte   += (gsize) (len < 0 ? (gssize) strlen (text) : len);
    self->caret_visible = TRUE;
    pn_node_request_repaint (PN_NODE (self));
}

void
pn_chat_draft_backspace (PnChat *self)
{
    g_return_if_fail (PN_IS_CHAT (self));
    if (self->draft != NULL && self->caret_byte > 0)
    {
        gsize prev = utf8_prev (self->draft->str, self->caret_byte);
        g_string_erase (self->draft,
                        (gssize) prev,
                        (gssize) (self->caret_byte - prev));
        self->caret_byte    = prev;
        self->caret_visible = TRUE;
        pn_node_request_repaint (PN_NODE (self));
    }
}

void
pn_chat_draft_delete (PnChat *self)
{
    g_return_if_fail (PN_IS_CHAT (self));
    if (self->draft != NULL && self->caret_byte < self->draft->len)
    {
        gsize next = utf8_next (self->draft->str,
                                self->caret_byte,
                                self->draft->len);
        g_string_erase (self->draft,
                        (gssize) self->caret_byte,
                        (gssize) (next - self->caret_byte));
        self->caret_visible = TRUE;
        pn_node_request_repaint (PN_NODE (self));
    }
}

void
pn_chat_caret_left (PnChat *self)
{
    g_return_if_fail (PN_IS_CHAT (self));
    if (self->draft != NULL && self->caret_byte > 0)
    {
        self->caret_byte    = utf8_prev (self->draft->str, self->caret_byte);
        self->caret_visible = TRUE;
        pn_node_request_repaint (PN_NODE (self));
    }
}

void
pn_chat_caret_right (PnChat *self)
{
    g_return_if_fail (PN_IS_CHAT (self));
    if (self->draft != NULL && self->caret_byte < self->draft->len)
    {
        self->caret_byte    = utf8_next (self->draft->str,
                                         self->caret_byte,
                                         self->draft->len);
        self->caret_visible = TRUE;
        pn_node_request_repaint (PN_NODE (self));
    }
}

void
pn_chat_caret_home (PnChat *self)
{
    g_return_if_fail (PN_IS_CHAT (self));
    self->caret_byte    = 0;
    self->caret_visible = TRUE;
    pn_node_request_repaint (PN_NODE (self));
}

void
pn_chat_caret_end (PnChat *self)
{
    g_return_if_fail (PN_IS_CHAT (self));
    if (self->draft != NULL)
        self->caret_byte = self->draft->len;
    self->caret_visible = TRUE;
    pn_node_request_repaint (PN_NODE (self));
}

/* ------------------------------------------------------------------ */
/*  GUI read seam (GTK-free)                                           */
/*                                                                     */
/*  The gui-tier cairo/Pango painter (pn-chat-gui.c) reads the scalar   */
/*  drawing config through a snapshot and the bubble buffer + draft      */
/*  through borrowed-pointer accessors.  The painter's only write-back   */
/*  is the horizontal draft scroll, since only it knows the live entry   */
/*  width + caret pixel position.                                        */
/* ------------------------------------------------------------------ */

void
pn_chat_get_paint_state (PnChat *self, PnChatPaintState *out)
{
    g_return_if_fail (PN_IS_CHAT (self));
    g_return_if_fail (out != NULL);

    out->background_color       = self->background_color;
    out->border_color           = self->border_color;
    out->text_color             = self->text_color;
    out->me_color               = self->me_color;
    out->input_background_color  = self->input_background_color;
    out->send_button_color      = self->send_button_color;

    out->focused          = self->focused;
    out->caret_visible    = self->caret_visible;
    out->scroll_offset    = self->scroll_offset;
    out->caret_byte       = self->caret_byte;
    out->draft_scroll_px  = self->draft_scroll_px;
}

GQueue *
pn_chat_peek_bubbles (PnChat *self)
{
    g_return_val_if_fail (PN_IS_CHAT (self), NULL);
    return self->bubbles;
}

const gchar *
pn_chat_peek_draft (PnChat *self)
{
    g_return_val_if_fail (PN_IS_CHAT (self), "");
    return (self->draft != NULL) ? self->draft->str : "";
}

void
pn_chat_set_draft_scroll_px (PnChat *self, gdouble px)
{
    g_return_if_fail (PN_IS_CHAT (self));
    self->draft_scroll_px = px;
}

/* ------------------------------------------------------------------ */
/*  GObject plumbing                                                   */
/* ------------------------------------------------------------------ */

static void
pn_chat_get_property (
        GObject    *object,
        guint       prop_id,
        GValue     *value,
        GParamSpec *pspec)
{
    PnChat *self = PN_CHAT (object);

    switch (prop_id)
    {
    case PROP_TEXT_PATH:
        g_value_set_string (value, self->text_path);
        break;
    case PROP_SENDER_PATH:
        g_value_set_string (value, self->sender_path);
        break;
    case PROP_ME_NAME:
        g_value_set_string (value, self->me_name);
        break;
    case PROP_LIMIT:
        g_value_set_uint (value, self->limit);
        break;
    case PROP_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->background_color);
        break;
    case PROP_BORDER_COLOR:
        g_value_set_boxed (value, &self->border_color);
        break;
    case PROP_TEXT_COLOR:
        g_value_set_boxed (value, &self->text_color);
        break;
    case PROP_ME_COLOR:
        g_value_set_boxed (value, &self->me_color);
        break;
    case PROP_INPUT_BACKGROUND_COLOR:
        g_value_set_boxed (value, &self->input_background_color);
        break;
    case PROP_SEND_BUTTON_COLOR:
        g_value_set_boxed (value, &self->send_button_color);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_chat_set_property (
        GObject      *object,
        guint         prop_id,
        const GValue *value,
        GParamSpec   *pspec)
{
    PnChat *self = PN_CHAT (object);

    switch (prop_id)
    {
    case PROP_TEXT_PATH:
        g_free (self->text_path);
        self->text_path = g_strdup (g_value_get_string (value));
        break;
    case PROP_SENDER_PATH:
        g_free (self->sender_path);
        self->sender_path = g_strdup (g_value_get_string (value));
        break;
    case PROP_ME_NAME:
        g_free (self->me_name);
        self->me_name = g_strdup (g_value_get_string (value));
        break;
    case PROP_LIMIT:
    {
        guint v = g_value_get_uint (value);
        if (v < 1) v = 1;
        if (v > PN_CHAT_LIMIT_MAX) v = PN_CHAT_LIMIT_MAX;
        self->limit = v;
        pn_chat_trim_to_limit (self);
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_BACKGROUND_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_BORDER_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->border_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_TEXT_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->text_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_ME_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->me_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_INPUT_BACKGROUND_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->input_background_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    case PROP_SEND_BUTTON_COLOR:
    {
        const PnColor *c = g_value_get_boxed (value);
        if (c != NULL) self->send_button_color = *c;
        pn_node_request_repaint (PN_NODE (self));
        break;
    }
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pn_chat_finalize (GObject *object)
{
    PnChat *self = PN_CHAT (object);

    if (self->pending_repaint_id != 0)
    {
        g_source_remove (self->pending_repaint_id);
        self->pending_repaint_id = 0;
    }
    caret_timer_stop (self);

    pn_chat_clear_bubbles (self);
    g_queue_free (self->bubbles);
    self->bubbles = NULL;

    if (self->draft != NULL)
    {
        g_string_free (self->draft, TRUE);
        self->draft = NULL;
    }

    g_clear_pointer (&self->text_path,   g_free);
    g_clear_pointer (&self->sender_path, g_free);
    g_clear_pointer (&self->me_name,     g_free);

    G_OBJECT_CLASS (pn_chat_parent_class)->finalize (object);
}

static void
pn_chat_class_init (PnChatClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    PnNodeClass  *node_class   = PN_NODE_CLASS (klass);

    object_class->get_property = pn_chat_get_property;
    object_class->set_property = pn_chat_set_property;
    object_class->finalize     = pn_chat_finalize;

    node_class->receive           = pn_chat_receive;
    node_class->get_size          = pn_chat_get_size;
    node_class->get_header_height = pn_chat_get_header_height;
    node_class->scroll            = pn_chat_scroll;
    /* The cairo/Pango chat painter (paint_plot) is installed onto this
     * class by the gui tier — pn_chat_gui_install() in pn-chat-gui.c —
     * so the headless core carries no GTK/cairo/pango.  The scroll vfunc
     * stays here: it only nudges an int. */

    node_class->class_name        = "Chat";
    node_class->icon              = "\xef\x82\x86";  /* fa-comments U+F086 */
    node_class->color             = (PnColor){ 0.40, 0.55, 0.80, 1.0 };
    node_class->category          = "Sinks";
    node_class->has_input         = TRUE;
    node_class->has_output        = TRUE;

    props[PROP_TEXT_PATH] = g_param_spec_string (
            "text-path", "Text path",
            "JSON pointer (\"/\"-separated) resolved against the "
            "incoming message to extract the bubble's text payload. "
            "Defaults to \"data/output\" so a #PnMeshtastic feed "
            "(which writes its received text to data/output) plugs "
            "in unchanged.",
            "data/output",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SENDER_PATH] = g_param_spec_string (
            "sender-path", "Sender path",
            "JSON pointer resolved against the incoming message to "
            "extract the sender label.  The label is shown above the "
            "bubble and hashed into the bubble's pastel fill colour "
            "so two messages from the same sender always paint the "
            "same colour.  Defaults to \"data/from_long_name\" to "
            "match the #PnMeshtastic envelope.",
            "data/from_long_name",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ME_NAME] = g_param_spec_string (
            "me-name", "My name",
            "Display name attached to messages the user sends from "
            "this node.  Stamped under data/from_long_name on every "
            "outgoing message and shown above the right-aligned "
            "\"mine\" bubbles in the local history.",
            "Me",
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_LIMIT] = g_param_spec_uint (
            "limit", "Limit",
            "Maximum number of bubbles kept in the history buffer.  "
            "Older bubbles are dropped from the top as new ones "
            "arrive.",
            1, PN_CHAT_LIMIT_MAX, 200,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BACKGROUND_COLOR] = g_param_spec_boxed (
            "background-color", "Background colour",
            "Fill colour of the chat body rectangle behind the "
            "bubbles and entry strip",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_BORDER_COLOR] = g_param_spec_boxed (
            "border-color", "Border colour",
            "Colour of the body frame, the bubble outlines, and the "
            "separator between the entry and the Send button",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_TEXT_COLOR] = g_param_spec_boxed (
            "text-color", "Text colour",
            "Colour of the bubble text and (at lower alpha) the "
            "sender label",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_ME_COLOR] = g_param_spec_boxed (
            "me-color", "My-bubble colour",
            "Fill colour of the right-aligned bubbles representing "
            "messages the user sent from this node",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_INPUT_BACKGROUND_COLOR] = g_param_spec_boxed (
            "input-background-color", "Input background",
            "Fill colour of the entry strip rectangle along the "
            "bottom edge of the chat body",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    props[PROP_SEND_BUTTON_COLOR] = g_param_spec_boxed (
            "send-button-color", "Send button colour",
            "Fill colour of the inline Send button at the right end "
            "of the entry strip",
            PN_TYPE_COLOR,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, props);
}

static void
pn_chat_init (PnChat *self)
{
    PnNode  *node = PN_NODE (self);
    PnColor  body_color = { 0.40, 0.55, 0.80, 1.0 };

    self->text_path   = g_strdup ("data/output");
    self->sender_path = g_strdup ("data/from_long_name");
    self->me_name     = g_strdup ("Me");
    self->limit       = 200;

    self->background_color        = (PnColor){ 0.97, 0.97, 0.99, 1.0 };
    self->border_color            = (PnColor){ 0.55, 0.55, 0.60, 1.0 };
    self->text_color              = (PnColor){ 0.10, 0.10, 0.10, 1.0 };
    self->me_color                = (PnColor){ 0.74, 0.86, 0.99, 1.0 };
    self->input_background_color  = (PnColor){ 1.00, 1.00, 1.00, 1.0 };
    self->send_button_color       = (PnColor){ 0.27, 0.55, 0.85, 1.0 };

    self->bubbles            = g_queue_new ();
    self->scroll_offset      = 0;
    self->last_repaint_us    = 0;
    self->pending_repaint_id = 0;

    self->focused         = FALSE;
    self->draft           = g_string_new (NULL);
    self->caret_byte      = 0;
    self->caret_blink_id  = 0;
    self->caret_visible   = FALSE;
    self->draft_scroll_px = 0.0;

    pn_node_set_class_name (node, "Chat");
    pn_node_set_icon       (node, "\xef\x82\x86");  /* fa-comments U+F086 */
    pn_node_set_color (node, &body_color);
    pn_node_set_has_input  (node, TRUE);
    pn_node_set_has_output (node, TRUE);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

PnChat *
pn_chat_new (void)
{
    return g_object_new (PN_TYPE_CHAT, NULL);
}

guint
pn_chat_get_bubble_count (PnChat *self)
{
    g_return_val_if_fail (PN_IS_CHAT (self), 0);
    return g_queue_get_length (self->bubbles);
}
