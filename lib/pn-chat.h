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

#ifndef PN_CHAT_H
#define PN_CHAT_H

#include "pn-node.h"

G_BEGIN_DECLS

/* Headless-core invariant (TODO #23): this header must compile against
 * gobject alone, so the GUI-only key handler below takes #GdkEventKey by
 * pointer through an opaque forward declaration rather than pulling GDK.
 * The struct tag matches GDK's own, so the real <gdk/gdk.h> definition
 * coexists with this in the GUI-tier translation units that include both. */
typedef struct _GdkEventKey GdkEventKey;

/* ------------------------------------------------------------------ */
/*  PnChat                                                             */
/*                                                                     */
/*  Sink+source node that displays a scrolling list of chat bubbles    */
/*  in its plot extension and a single-line entry strip with an inline */
/*  send button along the bottom edge.  Each incoming message becomes  */
/*  one bubble: the bubble's text is read from the message's `data`    */
/*  bag at the configurable #text-path (default "data/output"), and    */
/*  the sender label / bubble fill colour is keyed off                 */
/*  #sender-path (default "data/from_long_name").  Messages whose      */
/*  source is the chat node itself are treated as "mine" and rendered  */
/*  right-aligned in the configurable #me-color.                       */
/*                                                                     */
/*  The entry strip is a fully canvas-resident, cairo-painted control: */
/*  there is no GtkEntry / GtkPopover floating above the worksheet.    */
/*  Clicking inside the entry rectangle focuses the chat for keyboard  */
/*  input — the worksheet routes its key events into                  */
/*  pn_chat_handle_key_press(); Enter (or clicking Send) commits the   */
/*  draft via pn_chat_submit(); Escape clears focus without sending.   */
/*  The chat paints a blinking caret while focused so the user can     */
/*  see where the next character will land.                            */
/*                                                                     */
/*  Geometry mirrors PnTable / PnGraph: a 280 px standard header sits  */
/*  on top, and a separately-painted body of the same width hangs     */
/*  below it.  Clicking the bubbles area lifts the chat into the      */
/*  standard centred zoom overlay; while zoomed, the mouse wheel      */
/*  scrolls back through the history.                                  */
/* ------------------------------------------------------------------ */

#define PN_TYPE_CHAT (pn_chat_get_type ())

G_DECLARE_FINAL_TYPE (PnChat, pn_chat, PN, CHAT, PnNode)

/**
 * PnChatHit:
 * @PN_CHAT_HIT_NONE:   click is outside the chat's body
 * @PN_CHAT_HIT_ENTRY:  click landed inside the entry rectangle —
 *                      focus the chat for keyboard input
 * @PN_CHAT_HIT_SEND:   click landed inside the inline Send button —
 *                      submit the draft immediately
 *
 * Three-way hit-test result for the chat's bottom-row controls.
 * Returned by pn_chat_hit_input() so the worksheet can route a
 * primary press to the right action without re-doing the rectangle
 * arithmetic itself.
 */
typedef enum
{
    PN_CHAT_HIT_NONE  = 0,
    PN_CHAT_HIT_ENTRY = 1,
    PN_CHAT_HIT_SEND  = 2,
} PnChatHit;

PnChat *pn_chat_new (void);

/**
 * pn_chat_hit_input:
 * @self: the chat node
 * @px:   x in worksheet coordinates
 * @py:   y in worksheet coordinates
 *
 * Classifies (@px, @py) against the chat's bottom-row controls.
 * #PN_CHAT_HIT_ENTRY means a click on the typing area — the
 * worksheet should call pn_chat_set_focused() to start text input.
 * #PN_CHAT_HIT_SEND means a click on the inline Send button — the
 * worksheet should call pn_chat_submit().  #PN_CHAT_HIT_NONE means
 * the click is outside the entry strip and should fall through to
 * the standard plot-zoom-lift / drag handling.
 */
PnChatHit pn_chat_hit_input (PnChat *self,
                             double  px,
                             double  py);

/**
 * pn_chat_hit_input_in_rect:
 * @self:   the chat node
 * @rect_x: top-left x of the rectangle the chat body is currently
 *          painted into, in the same coordinate system as (@px, @py)
 * @rect_y: top-left y of that rectangle
 * @rect_w: width of that rectangle
 * @rect_h: height of that rectangle
 * @px:     x of the press to classify
 * @py:     y of the press to classify
 *
 * Same three-way hit test as pn_chat_hit_input(), but driven by an
 * arbitrary plot-rect rather than by the chat's stored worksheet
 * position.  Used by the worksheet's zoom-overlay press handler so a
 * maximised chat's entry / Send button can still receive clicks even
 * though it is no longer painted at the body's natural location.
 * The entry-strip placement inside (@rect_x, @rect_y, @rect_w,
 * @rect_h) is the same as pn_chat_paint_plot()'s, so a click on the
 * pixel showing the strip is classified consistently with what the
 * user sees.
 */
PnChatHit pn_chat_hit_input_in_rect (PnChat *self,
                                     double  rect_x,
                                     double  rect_y,
                                     double  rect_w,
                                     double  rect_h,
                                     double  px,
                                     double  py);

/**
 * pn_chat_set_focused:
 * @self:    the chat node
 * @focused: %TRUE to start a typing session, %FALSE to end one
 *
 * Toggle the chat's keyboard-input focus state.  While focused the
 * entry strip paints the draft text and a blinking caret instead of
 * the placeholder hint, and the worksheet forwards key events to
 * pn_chat_handle_key_press().  Defocusing keeps the draft buffer so
 * a subsequent click on the entry resumes where the user left off;
 * pn_chat_submit() clears it after sending.
 */
void      pn_chat_set_focused (PnChat   *self,
                               gboolean  focused);

/**
 * pn_chat_get_focused:
 *
 * Returns whether the chat is currently the keyboard-input target.
 * The worksheet checks this before dispatching key events into the
 * chat so a non-focused chat does not eat keystrokes that should
 * have reached its Delete-selected-nodes handler.
 */
gboolean  pn_chat_get_focused (PnChat *self);

/**
 * pn_chat_handle_key_press:
 * @self:  the chat node, must be focused
 * @event: borrowed pointer to the worksheet's #GdkEventKey
 *
 * Mutates the draft buffer in response to a keystroke.  Printable
 * characters insert at the caret; Backspace / Delete remove the
 * neighbouring character; Left / Right / Home / End move the caret;
 * Enter submits via pn_chat_submit(); Escape clears the focus
 * without submitting.  Returns %TRUE iff the keystroke was
 * consumed — the worksheet's caller propagates the event when this
 * returns %FALSE (e.g. unrecognised modifier combos that should
 * still reach a global accelerator).
 */
gboolean  pn_chat_handle_key_press (PnChat      *self,
                                    GdkEventKey *event);

/**
 * pn_chat_submit:
 * @self: the chat node
 *
 * Commits the current draft: emits a fresh #PnMessage on the chat's
 * output port carrying the typed text under `data/output`, the
 * configured #me-name under `data/from_long_name`, and the
 * configured #topic; pushes a "mine" bubble onto the local history
 * so the user sees what they sent; clears the draft buffer and
 * caret.  No-op when the draft is empty / whitespace-only.
 */
void      pn_chat_submit (PnChat *self);

/* Number of bubbles currently buffered.  Read-only inspection seam:
 * receive() and pn_chat_submit() append bubbles and trim to #limit,
 * but the bubble store is private, so headless tests observe the
 * result through this accessor. */
guint     pn_chat_get_bubble_count (PnChat *self);

G_END_DECLS

#endif /* PN_CHAT_H */
