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

#ifndef PN_MESSAGE_H
#define PN_MESSAGE_H

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "pn-node.h"

G_BEGIN_DECLS

/* ------------------------------------------------------------------ */
/*  PnMessage                                                          */
/*                                                                     */
/*  Derivable base class for the values that travel from a node's      */
/*  output port to every node connected on the other end.  Modelled    */
/*  after Node-RED's `msg` object: a topic, a unique id, a back-       */
/*  pointer to the source node, and an arbitrarily nested bag of      */
/*  named values (the "data" object) that nodes use to communicate.   */
/*                                                                     */
/*  The data bag is backed by a #JsonObject so values can be nested   */
/*  to any depth and serialise to JSON without a second pass.          */
/* ------------------------------------------------------------------ */

#define PN_TYPE_MESSAGE (pn_message_get_type ())

G_DECLARE_DERIVABLE_TYPE (PnMessage, pn_message, PN, MESSAGE, GObject)

struct _PnMessageClass
{
    GObjectClass parent_class;

    /**
     * PnMessageClass::copy_private:
     * @dst: freshly cloned message — same #GType as @src, with its
     *       envelope (id, topic, created, source) and JSON data bag
     *       already populated by pn_message_clone()
     * @src: the message being cloned
     *
     * Hook letting a subclass carry its out-of-band, non-JSON state
     * (image pixbufs, file descriptors, decoded buffers, …) across
     * pn_message_clone() — the path the wire layer takes to give every
     * fan-out branch its own copy.  The base class has no such state
     * and leaves this %NULL.  Implementations typically ref-share the
     * pointer rather than deep-copying the payload, which is the whole
     * point of passing heavy values by pointer in a subclass.
     */
    void (*copy_private) (PnMessage *dst, PnMessage *src);
};

/**
 * pn_message_new:
 * @source: (nullable) (transfer none): node that emitted the message
 * @topic:  (nullable): short string tag
 *
 * Convenience constructor.  The id is generated automatically and
 * the data bag starts empty; use the typed setters below to fill it.
 */
PnMessage    *pn_message_new            (PnNode      *source,
                                         const gchar *topic);

const gchar  *pn_message_get_id         (PnMessage *self);
void          pn_message_set_id         (PnMessage *self, const gchar *id);

const gchar  *pn_message_get_topic      (PnMessage *self);
void          pn_message_set_topic      (PnMessage *self, const gchar *topic);

/* ISO-8601 / RFC-3339 timestamp captured when the message was
 * constructed (e.g. "2026-05-07T14:32:11+02:00").  The setter is
 * provided so a deserializer can restore the original creation time;
 * normal senders should leave it alone. */
const gchar  *pn_message_get_created    (PnMessage *self);
void          pn_message_set_created    (PnMessage *self, const gchar *created);

/* The source getter returns a borrowed reference; ref it if you need
 * to keep the node alive past the message. */
PnNode       *pn_message_get_source     (PnMessage *self);
void          pn_message_set_source     (PnMessage *self, PnNode *source);

/* ------------------------------------------------------------------ */
/*  Data accessors                                                     */
/*                                                                     */
/*  The data object is a JsonObject the message owns.  Typed setters  */
/*  cover the common scalar cases; pn_message_set_member() takes any  */
/*  #JsonNode (including nested objects and arrays) so a sender can   */
/*  build arbitrarily deep structures.                                 */
/* ------------------------------------------------------------------ */

/* Returns the borrowed data bag.  Mutate it in place if you need a
 * surface area not covered by the typed setters. */
JsonObject   *pn_message_get_data       (PnMessage *self);

void          pn_message_set_string     (PnMessage   *self,
                                         const gchar *name,
                                         const gchar *value);
void          pn_message_set_int        (PnMessage   *self,
                                         const gchar *name,
                                         gint         value);
void          pn_message_set_int64      (PnMessage   *self,
                                         const gchar *name,
                                         gint64       value);
void          pn_message_set_double     (PnMessage   *self,
                                         const gchar *name,
                                         gdouble      value);
void          pn_message_set_boolean    (PnMessage   *self,
                                         const gchar *name,
                                         gboolean     value);

/**
 * pn_message_set_member:
 * @self: the message
 * @name: member name
 * @node: (transfer full): JSON value to insert; may be a nested
 *        object or array
 *
 * Generic setter for non-scalar values.  Takes ownership of @node.
 */
void          pn_message_set_member     (PnMessage   *self,
                                         const gchar *name,
                                         JsonNode    *node);

/* Borrowed pointer; %NULL when @name is not present. */
JsonNode     *pn_message_get_member     (PnMessage   *self,
                                         const gchar *name);

/**
 * pn_message_clone:
 * @self: the message to clone
 *
 * Returns a new #PnMessage that is a deep copy of @self: same id,
 * topic, created timestamp, and source back-reference; the data bag
 * is deep-copied so subsequent mutations of either copy's data leave
 * the other untouched.  Used by the wire layer to give every
 * fan-out branch its own independent message.
 *
 * Returns: (transfer full): a new #PnMessage with its own ref count.
 */
PnMessage    *pn_message_clone          (PnMessage *self);

G_END_DECLS

#endif /* PN_MESSAGE_H */
