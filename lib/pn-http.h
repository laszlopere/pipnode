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

#ifndef PN_HTTP_H
#define PN_HTTP_H

#include "pn-auto-trigger.h"

#include <libsoup/soup.h>

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnHttp                                                             */
/*                                                                     */
/*  Derivable auto-trigger node that performs an HTTP GET against the  */
/*  configured URL once per #PnAutoTrigger:period seconds and emits   */
/*  the response on its output port.  The fetch runs in-process via a */
/*  blocking libsoup request on the inherited worker thread, so the   */
/*  GUI never blocks on slow or unreachable servers.                  */
/*                                                                     */
/*  Subclasses customise the request, response handling, and visual  */
/*  identity by overriding the #PnHttpClass virtuals and class fields*/
/*  in their `class_init`.  They typically do *not* override         */
/*  #PnAutoTriggerClass.trigger directly; the base class drives the  */
/*  request loop and calls the virtuals at the right points.         */
/*                                                                     */
/*  The node has a "configuration required" state: while             */
/*  #PnHttpClass.is_configured returns %FALSE the node switches to a */
/*  red body with a warning glyph and skips its triggers entirely.   */
/* ------------------------------------------------------------------ */

#define PN_TYPE_HTTP (pn_http_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnHttp, pn_http, PN, HTTP, PnAutoTrigger)

struct _PnHttpClass
{
    PnAutoTriggerClass parent_class;

    /**
     * PnHttpClass.normal_icon:
     *
     * Glyph shown on the node body while it is configured (the
     * warning icon used when not configured is fixed by the base
     * class).  %NULL falls back to the base class's globe glyph.
     * The string is owned by the class and must outlive every
     * instance — typically a static literal.
     */
    const gchar *normal_icon;

    /**
     * PnHttpClass.normal_color:
     *
     * Body colour used while the node is configured.  Subclasses
     * pick a colour distinctive from the plain HTTP green.  The base
     * class falls back to its own default if this is left at the
     * zero-initialised value (alpha 0).
     */
    PnColor normal_color;

    /**
     * PnHttpClass::is_configured:
     * @self: the node instance
     *
     * Return %TRUE when @self has enough configuration to perform a
     * meaningful request.  Called on the worker thread before each
     * tick; while it returns %FALSE the node is rendered in its
     * warning state and no request is made.
     *
     * The default implementation reports configured iff the inherited
     * #PnHttp:url is non-empty.
     */
    gboolean (*is_configured) (PnHttp *self);

    /**
     * PnHttpClass::build_request:
     * @self: the node instance
     *
     * Build the #SoupMessage to send for one tick.  Returns a new
     * GET message (transfer full) the base class sends and then
     * unrefs, or %NULL when the URL is missing/malformed.  Called on
     * the worker thread.
     *
     * The default implementation issues an HTTP GET to #PnHttp:url
     * (after ${...} variable expansion).  Subclasses build a
     * provider-specific URL and set any request headers they need
     * (Accept, User-Agent, ...) via soup_message_get_request_headers().
     * The per-tick timeout and redirect-following are applied by the
     * base class, so overrides need not touch them.
     */
    SoupMessage *(*build_request) (PnHttp *self);

    /**
     * PnHttpClass::emit_message:
     * @self:        the node instance
     * @ok:          %TRUE when an HTTP response was received (a non-2xx
     *               status still counts as @ok; @http_status carries it)
     * @http_status: the HTTP status code, or 0 when no response arrived
     * @body:        (nullable): the response body (never %NULL when @ok,
     *               but may be the empty string)
     * @error:       (nullable): a one-line transport-error description
     *               when !@ok (DNS / connect / timeout), else %NULL
     *
     * Build the outgoing #PnMessage from the response and pass it to
     * pn_auto_trigger_emit_on_main().  Called on the worker thread.
     *
     * The default implementation emits the standard HTTP fields:
     * "url", "success", "status" (when known), and "output" (the
     * response body).
     */
    void (*emit_message) (PnHttp      *self,
                          gboolean     ok,
                          gint         http_status,
                          const gchar *body,
                          const gchar *error);
};

PnHttp *pn_http_new (void);

/* ------------------------------------------------------------------ */
/*  Subclass helpers                                                   */
/* ------------------------------------------------------------------ */

/**
 * pn_http_dup_url:
 * @self: an HTTP node
 *
 * Returns the current value of #PnHttp:url as a freshly-allocated
 * copy; safe to call from the worker thread.  The caller frees the
 * result with g_free().  May return %NULL when no URL has been set.
 */
gchar *pn_http_dup_url (PnHttp *self);

/**
 * pn_http_apply_visual_state:
 * @self:       an HTTP node
 * @configured: whether the node currently has all required settings
 *
 * Re-paint the node body for the given configuration state, using
 * the subclass's class-level normal_icon/normal_color when
 * @configured is %TRUE and the shared warning identity otherwise.
 * Subclasses with multi-field configuration call this whenever any
 * of their inputs change.  Safe to call from the main thread only.
 */
void pn_http_apply_visual_state (PnHttp *self, gboolean configured);

G_END_DECLS

#endif /* PN_HTTP_H */
