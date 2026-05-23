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
/*  PnChat — gui tier.                                                  */
/*                                                                     */
/*  The cairo/Pango chat painter (the scrolling bubble list with        */
/*  per-sender pastel hues and the inline entry strip + Send button      */
/*  with its blinking caret) and the GdkEventKey key-press translator.   */
/*  The node's GType, properties, receive() with self-loop suppression,  */
/*  bubble accumulation + #limit trimming, the focus flag, the           */
/*  caret-blink timer, submit and the GTK-free draft-editing primitives  */
/*  live in the core file pn-chat.c; this file installs the paint_plot    */
/*  vfunc onto that class at editor startup (pn_chat_gui_install),        */
/*  reading the chat's state through the core's GTK-free accessors.  The  */
/*  key handler translates a GdkEventKey into calls to the core's        */
/*  draft-editing primitives, so the keyval lookup is the only GDK touch  */
/*  in the editing path.  The headless runtime never loads this file's    */
/*  half, so the Chat logic runs without GTK.                            */
/* ------------------------------------------------------------------ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pn-chat-gui.h"
#include "pn-chat.h"

#include <gtk/gtk.h>
#include <math.h>
#include <pango/pangocairo.h>

/* ------------------------------------------------------------------ */
/*  Geometry (painter-side copy)                                       */
/* ------------------------------------------------------------------ */

#define PN_CHAT_INPUT_HEIGHT   28.0
#define PN_CHAT_INSET           8.0
#define PN_CHAT_BUBBLE_GAP      6.0
#define PN_CHAT_BUBBLE_PAD_X    8.0
#define PN_CHAT_BUBBLE_PAD_Y    4.0
#define PN_CHAT_BUBBLE_RADIUS   8.0
#define PN_CHAT_BUBBLE_MAX_FRAC 0.78    /* of inner_w */
#define PN_CHAT_FONT_PX         11.0
#define PN_CHAT_SENDER_FONT_PX   9.0
#define PN_CHAT_SEND_BUTTON_W   42.0

/* ------------------------------------------------------------------ */
/*  Sender → colour                                                    */
/* ------------------------------------------------------------------ */

/** Hash @s into a stable HSV hue (0..1) so two messages from the same
 *  sender always paint with the same bubble colour. */
static double
sender_hue (const gchar *s)
{
    guint32 h = 5381u;
    if (s == NULL)
        return 0.0;
    while (*s != '\0')
    {
        h = ((h << 5) + h) + (guint8) *s;
        s++;
    }
    return (double) (h % 360u) / 360.0;
}

/** Convert HSV (h, s, v all in [0, 1]) to RGB.  Output written into
 *  @out's r/g/b; alpha left untouched. */
static void
hsv_to_rgb (
        double   h,
        double   s,
        double   v,
        PnColor *out)
{
    double r, g, b;
    double i = floor (h * 6.0);
    double f = h * 6.0 - i;
    double p = v * (1.0 - s);
    double q = v * (1.0 - f * s);
    double t = v * (1.0 - (1.0 - f) * s);
    int    ii = ((int) i) % 6;

    switch (ii)
    {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }

    out->red   = r;
    out->green = g;
    out->blue  = b;
}

/* ------------------------------------------------------------------ */
/*  Painting                                                           */
/* ------------------------------------------------------------------ */

static void
rounded_rect (
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h,
        double   r)
{
    if (r > w * 0.5) r = w * 0.5;
    if (r > h * 0.5) r = h * 0.5;

    cairo_new_sub_path (cr);
    cairo_arc (cr, x + w - r, y + r,     r, -G_PI_2, 0);
    cairo_arc (cr, x + w - r, y + h - r, r, 0,        G_PI_2);
    cairo_arc (cr, x + r,     y + h - r, r,  G_PI_2,  G_PI);
    cairo_arc (cr, x + r,     y + r,     r,  G_PI,    1.5 * G_PI);
    cairo_close_path (cr);
}

/** Build a Pango layout for @text constrained to @max_w pixels with
 *  word wrap.  Returns the layout (caller unrefs). */
static PangoLayout *
make_text_layout (
        cairo_t     *cr,
        const gchar *text,
        double       max_w,
        double       font_px,
        gboolean     bold,
        int         *out_w,
        int         *out_h)
{
    PangoLayout          *layout = pango_cairo_create_layout (cr);
    PangoFontDescription *desc;
    gchar                *desc_str;

    desc_str = g_strdup_printf ("Sans%s %.0f",
                                bold ? " Bold" : "",
                                font_px);
    desc = pango_font_description_from_string (desc_str);
    g_free (desc_str);

    pango_layout_set_font_description (layout, desc);
    pango_font_description_free (desc);

    pango_layout_set_text  (layout, text, -1);
    pango_layout_set_width (layout, (int) (max_w * PANGO_SCALE));
    pango_layout_set_wrap  (layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_get_pixel_size (layout, out_w, out_h);

    return layout;
}

/** Single-line variant of make_text_layout — no wrap, no width
 *  constraint. */
static PangoLayout *
make_single_line_layout (
        cairo_t     *cr,
        const gchar *text,
        double       font_px,
        gboolean     bold,
        int         *out_w,
        int         *out_h)
{
    PangoLayout          *layout = pango_cairo_create_layout (cr);
    PangoFontDescription *desc;
    gchar                *desc_str;

    desc_str = g_strdup_printf ("Sans%s %.0f",
                                bold ? " Bold" : "",
                                font_px);
    desc = pango_font_description_from_string (desc_str);
    g_free (desc_str);

    pango_layout_set_font_description (layout, desc);
    pango_font_description_free (desc);

    pango_layout_set_text (layout, text, -1);
    pango_layout_get_pixel_size (layout, out_w, out_h);

    return layout;
}

static void
pn_chat_paint_plot (
        PnNode  *node,
        cairo_t *cr,
        double   x,
        double   y,
        double   w,
        double   h)
{
    PnChat      *self        = PN_CHAT (node);
    const double inset       = PN_CHAT_INSET;
    const double inner_x     = x + inset;
    const double inner_w     = w - 2 * inset;
    const double input_h     = PN_CHAT_INPUT_HEIGHT;
    const double bubbles_y0  = y + inset;
    const double bubbles_y1  = y + h - input_h - inset;
    const double bubbles_h   = bubbles_y1 - bubbles_y0;
    const double bubble_max  = inner_w * PN_CHAT_BUBBLE_MAX_FRAC;
    PnChatPaintState  ps;
    GQueue           *bubbles;
    const gchar      *draft;
    guint             count;
    GList            *iter;
    int               skip;
    double            cursor_y;

    pn_chat_get_paint_state (self, &ps);
    bubbles = pn_chat_peek_bubbles (self);
    draft   = pn_chat_peek_draft (self);
    count   = g_queue_get_length (bubbles);

    /* Background + frame. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.background_color);
    cairo_fill_preserve (cr);
    gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.border_color);
    cairo_set_line_width (cr, 1.0);
    cairo_stroke (cr);
    cairo_restore (cr);

    /* Clip to the body so a stray-long bubble can't bleed onto the
     * neighbouring node. */
    cairo_save (cr);
    cairo_rectangle (cr, x, y, w, h);
    cairo_clip (cr);

    /* Bubbles: walk newest-first (tail) up to the top of the bubbles
     * area, painting each bubble with its newest at the bottom.  The
     * scroll offset skips that many bubbles from the tail. */
    cursor_y = bubbles_y1;
    skip     = ps.scroll_offset;
    if (skip < 0) skip = 0;
    if (skip >= (int) count) skip = (int) count - 1;
    if (skip < 0) skip = 0;

    iter = g_queue_peek_tail_link (bubbles);
    while (iter != NULL && skip > 0)
    {
        iter = iter->prev;
        skip--;
    }

    while (iter != NULL && cursor_y > bubbles_y0)
    {
        PnChatBubble *b = iter->data;
        PangoLayout  *text_layout;
        PangoLayout  *sender_layout = NULL;
        int           tw, th;
        int           sw = 0, sh = 0;
        double        bw, bh;
        double        bx, by;
        PnColor       fill;

        text_layout = make_text_layout (cr, b->text,
                                        bubble_max - 2 * PN_CHAT_BUBBLE_PAD_X,
                                        PN_CHAT_FONT_PX,
                                        FALSE,
                                        &tw, &th);

        if (b->sender != NULL && !b->mine)
            sender_layout = make_text_layout (
                    cr, b->sender,
                    bubble_max,
                    PN_CHAT_SENDER_FONT_PX,
                    TRUE,
                    &sw, &sh);

        bw = (double) tw + 2 * PN_CHAT_BUBBLE_PAD_X;
        bh = (double) th + 2 * PN_CHAT_BUBBLE_PAD_Y;
        if (bw > bubble_max) bw = bubble_max;

        /* Bubble sits with its bottom at cursor_y; sender label
         * (if any) sits 1 px above the bubble. */
        by = cursor_y - bh;
        bx = b->mine
                 ? (inner_x + inner_w - bw)
                 : (inner_x);

        /* Sender label, only for non-"mine" bubbles. */
        if (sender_layout != NULL)
        {
            cairo_save (cr);
            cairo_set_source_rgba (cr,
                                   ps.text_color.red,
                                   ps.text_color.green,
                                   ps.text_color.blue,
                                   ps.text_color.alpha * 0.55);
            cairo_move_to (cr, bx + 2.0, by - sh - 1.0);
            pango_cairo_show_layout (cr, sender_layout);
            cairo_restore (cr);
            g_object_unref (sender_layout);
        }

        /* Bubble fill colour: hash sender → pastel hue for received
         * bubbles, the configured #me-color for sent bubbles. */
        if (b->mine)
        {
            fill = ps.me_color;
        }
        else
        {
            fill.alpha = 1.0;
            hsv_to_rgb (sender_hue (b->sender), 0.35, 0.96, &fill);
        }

        rounded_rect (cr, bx, by, bw, bh, PN_CHAT_BUBBLE_RADIUS);
        gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &fill);
        cairo_fill_preserve (cr);
        cairo_set_line_width (cr, 0.75);
        cairo_set_source_rgba (cr,
                               ps.border_color.red,
                               ps.border_color.green,
                               ps.border_color.blue,
                               ps.border_color.alpha * 0.6);
        cairo_stroke (cr);

        /* Bubble text. */
        cairo_save (cr);
        gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.text_color);
        cairo_move_to (cr,
                       bx + PN_CHAT_BUBBLE_PAD_X,
                       by + PN_CHAT_BUBBLE_PAD_Y);
        pango_cairo_show_layout (cr, text_layout);
        cairo_restore (cr);

        g_object_unref (text_layout);

        /* Move the cursor up past this bubble (and its sender label
         * if present) for the next iteration. */
        cursor_y = by - PN_CHAT_BUBBLE_GAP;
        if (sender_layout != NULL)
            cursor_y -= sh + 1.0;

        iter = iter->prev;
    }

    /* Hint when there are no bubbles yet. */
    if (count == 0)
    {
        PangoLayout *l;
        int          lw, lh;

        l = make_text_layout (cr, "No messages yet.",
                              inner_w, PN_CHAT_FONT_PX, FALSE, &lw, &lh);
        cairo_save (cr);
        cairo_set_source_rgba (cr,
                               ps.text_color.red,
                               ps.text_color.green,
                               ps.text_color.blue,
                               ps.text_color.alpha * 0.45);
        cairo_move_to (cr,
                       inner_x + (inner_w - lw) / 2.0,
                       bubbles_y0 + (bubbles_h - lh) / 2.0);
        pango_cairo_show_layout (cr, l);
        cairo_restore (cr);
        g_object_unref (l);
    }

    /* Entry strip across the bottom edge of the body. */
    {
        const double ix    = x + inset;
        const double iy    = y + h - input_h - inset / 2.0;
        const double iw    = w - 2 * inset;
        const double ih    = input_h;
        const double sb_w  = PN_CHAT_SEND_BUTTON_W;
        const double sb_x  = ix + iw - sb_w;
        const double txt_pad = 6.0;
        const double txt_x   = ix + txt_pad;
        const double txt_w   = sb_x - ix - 2 * txt_pad;
        PangoLayout *send_label;
        int          ssw, ssh;

        /* Entry rectangle.  When the chat is focused the rectangle
         * gets a subtle accent border so the user can see at a
         * glance which chat is taking keystrokes. */
        rounded_rect (cr, ix, iy, iw, ih, 4.0);
        gdk_cairo_set_source_rgba (cr,
                                   (const GdkRGBA *) &ps.input_background_color);
        cairo_fill_preserve (cr);
        if (ps.focused)
        {
            cairo_set_line_width (cr, 1.5);
            gdk_cairo_set_source_rgba (cr,
                                       (const GdkRGBA *) &ps.send_button_color);
        }
        else
        {
            cairo_set_line_width (cr, 1.0);
            gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.border_color);
        }
        cairo_stroke (cr);

        /* Send button rectangle, painted as the right end of the
         * entry strip so the whole row reads as a single chat input
         * with an inline button. */
        rounded_rect (cr, sb_x, iy, sb_w, ih, 4.0);
        gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.send_button_color);
        cairo_fill (cr);

        /* Vertical separator between entry and send button. */
        gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.border_color);
        cairo_set_line_width (cr, 1.0);
        cairo_move_to (cr, sb_x + 0.5, iy + 3.0);
        cairo_line_to (cr, sb_x + 0.5, iy + ih - 3.0);
        cairo_stroke (cr);

        /* Clip text rendering to the entry rectangle so a long
         * draft cannot spill onto the Send button. */
        cairo_save (cr);
        cairo_rectangle (cr, ix, iy, sb_x - ix, ih);
        cairo_clip (cr);

        if (draft != NULL && *draft != '\0')
        {
            PangoLayout *layout;
            int          tw, th;
            int          caret_x_pango;
            PangoRectangle strong;
            double       caret_x;
            double       baseline_y;
            double       draft_scroll_px = ps.draft_scroll_px;

            layout = make_single_line_layout (cr, draft,
                                              PN_CHAT_FONT_PX, FALSE,
                                              &tw, &th);

            /* Caret pixel position from the byte offset.  Pango's
             * cursor_pos returns 1024ths of a pixel (PANGO_SCALE).
             * Use the strong cursor position so RTL doesn't surprise
             * the caret. */
            pango_layout_get_cursor_pos (layout,
                                         (int) ps.caret_byte,
                                         &strong, NULL);
            caret_x_pango = strong.x;

            /* Pin the caret inside the visible rectangle: nudge
             * draft_scroll_px until the caret is in [4, txt_w-4]. */
            caret_x = (double) caret_x_pango / PANGO_SCALE
                    - draft_scroll_px;
            if (caret_x < 4.0)
                draft_scroll_px += caret_x - 4.0;
            else if (caret_x > txt_w - 4.0)
                draft_scroll_px += caret_x - (txt_w - 4.0);
            if (draft_scroll_px < 0.0)
                draft_scroll_px = 0.0;
            pn_chat_set_draft_scroll_px (self, draft_scroll_px);
            caret_x = (double) caret_x_pango / PANGO_SCALE
                    - draft_scroll_px;

            baseline_y = iy + (ih - th) / 2.0;

            cairo_save (cr);
            gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.text_color);
            cairo_move_to (cr,
                           txt_x - draft_scroll_px,
                           baseline_y);
            pango_cairo_show_layout (cr, layout);
            cairo_restore (cr);

            if (ps.focused && ps.caret_visible)
            {
                cairo_save (cr);
                gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.text_color);
                cairo_set_line_width (cr, 1.0);
                cairo_move_to (cr, txt_x + caret_x + 0.5, iy + 4.0);
                cairo_line_to (cr, txt_x + caret_x + 0.5, iy + ih - 4.0);
                cairo_stroke (cr);
                cairo_restore (cr);
            }

            g_object_unref (layout);
        }
        else
        {
            /* No draft.  Paint either the placeholder hint or — when
             * focused with an empty draft — just the caret at the
             * left edge so the user sees the input is live. */
            if (!ps.focused)
            {
                PangoLayout *placeholder;
                int          pw, ph;
                placeholder = make_single_line_layout (
                        cr, "Type a message…",
                        PN_CHAT_FONT_PX, FALSE, &pw, &ph);
                cairo_save (cr);
                cairo_set_source_rgba (cr,
                                       ps.text_color.red,
                                       ps.text_color.green,
                                       ps.text_color.blue,
                                       ps.text_color.alpha * 0.45);
                cairo_move_to (cr, txt_x, iy + (ih - ph) / 2.0);
                pango_cairo_show_layout (cr, placeholder);
                cairo_restore (cr);
                g_object_unref (placeholder);
            }
            else if (ps.caret_visible)
            {
                cairo_save (cr);
                gdk_cairo_set_source_rgba (cr, (const GdkRGBA *) &ps.text_color);
                cairo_set_line_width (cr, 1.0);
                cairo_move_to (cr, txt_x + 0.5, iy + 4.0);
                cairo_line_to (cr, txt_x + 0.5, iy + ih - 4.0);
                cairo_stroke (cr);
                cairo_restore (cr);
            }
        }

        cairo_restore (cr);

        /* "Send" label centred in the button. */
        send_label = make_single_line_layout (
                cr, "Send", PN_CHAT_FONT_PX, TRUE, &ssw, &ssh);
        cairo_save (cr);
        cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.95);
        cairo_move_to (cr,
                       sb_x + (sb_w - ssw) / 2.0,
                       iy + (ih - ssh) / 2.0);
        pango_cairo_show_layout (cr, send_label);
        cairo_restore (cr);
        g_object_unref (send_label);
    }

    cairo_restore (cr);
}

/* ------------------------------------------------------------------ */
/*  Key handling                                                       */
/*                                                                     */
/*  The only GDK-aware step in the editing path: classify the keyval    */
/*  and dispatch to the core's GTK-free draft-editing primitives.       */
/* ------------------------------------------------------------------ */

gboolean
pn_chat_handle_key_press (
        PnChat      *self,
        GdkEventKey *event)
{
    g_return_val_if_fail (PN_IS_CHAT (self), FALSE);
    g_return_val_if_fail (event != NULL, FALSE);

    if (!pn_chat_get_focused (self))
        return FALSE;

    /* Ignore Ctrl-/ Alt-modified keystrokes so global accelerators
     * (Ctrl+C copy etc.) still reach the worksheet.  Shift is fine
     * — it is part of normal text input. */
    if ((event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) != 0)
        return FALSE;

    switch (event->keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        pn_chat_submit (self);
        return TRUE;

    case GDK_KEY_Escape:
        pn_chat_set_focused (self, FALSE);
        return TRUE;

    case GDK_KEY_BackSpace:
        pn_chat_draft_backspace (self);
        return TRUE;

    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        pn_chat_draft_delete (self);
        return TRUE;

    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
        pn_chat_caret_left (self);
        return TRUE;

    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
        pn_chat_caret_right (self);
        return TRUE;

    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
        pn_chat_caret_home (self);
        return TRUE;

    case GDK_KEY_End:
    case GDK_KEY_KP_End:
        pn_chat_caret_end (self);
        return TRUE;

    default:
        break;
    }

    /* Printable keystroke: convert keyval to its UTF-8 form and
     * insert at the caret.  gdk_keyval_to_unicode returns 0 for
     * non-printable keys we should not consume. */
    {
        guint32 uc = gdk_keyval_to_unicode (event->keyval);
        if (uc == 0 || g_unichar_iscntrl (uc))
            return FALSE;

        {
            gchar  buf[8];
            gint   n = g_unichar_to_utf8 (uc, buf);
            if (n <= 0)
                return FALSE;

            pn_chat_draft_insert (self, buf, n);
        }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/*  vfunc installation                                                 */
/* ------------------------------------------------------------------ */

void
pn_chat_gui_install (void)
{
    PnNodeClass *node_class = PN_NODE_CLASS (g_type_class_ref (PN_TYPE_CHAT));

    node_class->paint_plot = pn_chat_paint_plot;

    /* The class ref is intentionally held for the process lifetime —
     * the same lifetime the factory keeps it alive for — so the slot we
     * just wrote stays valid.  (One leaked ref on a singleton class,
     * mirroring pn_node_factory_register.) */
}
