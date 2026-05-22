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

#ifndef PN_SWITCH_H
#define PN_SWITCH_H

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnSwitch                                                           */
/*                                                                     */
/*  Manual source / passthrough node carrying a two-position slide     */
/*  switch on its header.  Clicking the switch toggles its state and  */
/*  emits a single message whose "value" data member is 1.0 (on) or   */
/*  0.0 (off).  Incoming messages are forwarded unchanged; if their   */
/*  "value" member crosses zero the switch flips to match (so an      */
/*  upstream gauge or another switch can drive this one), but the    */
/*  passthrough is the canonical reaction -- the visual state simply  */
/*  shadows whatever the latest "value" said.                          */
/* ------------------------------------------------------------------ */

#define PN_TYPE_SWITCH (pn_switch_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnSwitch, pn_switch, PN, SWITCH, PnNode)

/* ------------------------------------------------------------------ */
/*  PnSwitchClass                                                      */
/*                                                                     */
/*  Two vfunc hooks let device-flavoured subclasses (e.g.              */
/*  #PnTasmotaSwitch) reuse the slider widget + click-routing the     */
/*  worksheet already special-cases on `PN_IS_SWITCH`, while           */
/*  specialising the visual scheme and the shape of the outbound       */
/*  message a user-click emits.  Subclasses chain to the defaults via  */
/*  `pn_switch_real_apply_visual_state` / `..._build_outbound_message` */
/*  if they want to extend the base behaviour instead of replacing it. */
/* ------------------------------------------------------------------ */

struct _PnSwitchClass
{
    PnNodeClass parent_class;

    /* Refresh whatever visual cue tracks the latch position (icon,
     * body colour).  Called on every state change -- initial config,
     * inbound-driven flip, user click.  Default implementation flips
     * the header glyph between fa-toggle-on / fa-toggle-off. */
    void       (*apply_visual_state)     (PnSwitch *self);

    /* Construct (do not emit) the message a user-click emits to
     * downstream nodes.  The dispatcher takes ownership of the
     * returned reference and emits it once.  Default implementation
     * returns a fresh message carrying `data.value` (1.0/0.0) and
     * `data.success` (true/false); device-flavoured subclasses can
     * stamp envelope topics and payload bodies. */
    PnMessage *(*build_outbound_message) (PnSwitch *self);

    /* Reserved slots so adding more vfuncs later does not break the
     * ABI of out-of-tree plugins built against this header. */
    gpointer _reserved[6];
};

/* Default vfunc implementations exposed so a subclass override that
 * wants to extend (rather than replace) the base behaviour can call
 * up to them.  Not normally used directly. */
void       pn_switch_real_apply_visual_state     (PnSwitch *self);
PnMessage *pn_switch_real_build_outbound_message (PnSwitch *self);

PnSwitch *pn_switch_new       (void);

/**
 * pn_switch_get_on:
 * @self: the switch node
 *
 * Returns the current latched state (TRUE while the switch is in the
 * "on" position, FALSE while "off").
 */
gboolean  pn_switch_get_on    (PnSwitch *self);

/**
 * pn_switch_toggle:
 * @self: the switch node
 *
 * Flips the switch to the opposite position and emits a fresh
 * message carrying the new value (1.0 or 0.0) on its "value" data
 * member.  Must be called from the main thread.  This is the entry
 * point the worksheet's click handler uses; programmatic callers may
 * use it the same way.
 */
void      pn_switch_toggle    (PnSwitch *self);

/**
 * pn_switch_set_announce_on_startup:
 * @self:     the switch node
 * @announce: whether to emit the latch state once shortly after construction
 *
 * By default a switch announces its current position once, shortly
 * after it is constructed, so a freshly-loaded worksheet lets
 * downstream nodes learn the latch state without a user click -- the
 * same "report on startup" behaviour #PnKnob and the periodic data
 * sources have.
 *
 * Subclasses whose outbound message has an outward side effect -- e.g.
 * #PnTasmotaSwitch, whose build_outbound_message() produces an MQTT
 * *command* that would flip a physical relay -- call this with @announce
 * = %FALSE from their instance init (which runs before the base
 * constructed schedules the shot) so opening a worksheet never silently
 * actuates hardware.  Passing %FALSE after construction also cancels an
 * already-scheduled announce.
 */
void      pn_switch_set_announce_on_startup (PnSwitch *self,
                                             gboolean  announce);

/**
 * pn_switch_hit_slider:
 * @self: the switch node
 * @px:   x in worksheet coordinates
 * @py:   y in worksheet coordinates
 *
 * Reports whether (@px, @py) lands on the slider's rectangular hit
 * area.  Exposed so the worksheet can route a primary press straight
 * into pn_switch_toggle() without duplicating the decoration's
 * geometry.
 */
gboolean  pn_switch_hit_slider (PnSwitch *self,
                                double    px,
                                double    py);

G_END_DECLS

#endif /* PN_SWITCH_H */
