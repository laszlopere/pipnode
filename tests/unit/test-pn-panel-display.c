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

/* Unit tests for PnPanelDisplay: a sink that derives a short display
 * string from each message (text member, else formatted value, else
 * topic), stores it for pn_panel_display_dup_text(), and emits
 * "value-changed" when the string actually changes. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "pntest.h"
#include "pn-panel-display.h"

typedef struct
{
    guint  count;
    gchar *last;
} Cap;

static void
on_value_changed (PnPanelDisplay *display, const gchar *text, gpointer user_data)
{
    Cap *cap = user_data;
    (void) display;
    cap->count++;
    g_free (cap->last);
    cap->last = g_strdup (text);
}

static void
recv_double (PnNode *node, const gchar *key, gdouble v)
{
    PnMessage *m = pn_message_new (NULL, NULL);
    pn_message_set_double (m, key, v);
    pn_node_receive_message (node, m);
    g_object_unref (m);
}

/* dup_text helper that asserts and frees in one place. */
static void
check_text (PnNode *node, const gchar *expect)
{
    gchar *t = pn_panel_display_dup_text (PN_PANEL_DISPLAY (node));
    PN_CHECK_CMPSTR (t, ==, expect);
    g_free (t);
}

static void
test_formats_value (void)
{
    Cap     cap  = { 0, NULL };
    PnNode *node = g_object_new (PN_TYPE_PANEL_DISPLAY, NULL);

    g_signal_connect (node, "value-changed",
                      G_CALLBACK (on_value_changed), &cap);

    recv_double (node, "value", 42.0);
    PN_CHECK_CMPINT (cap.count, ==, 1);
    PN_CHECK_CMPSTR (cap.last, ==, "42");
    check_text (node, "42");

    g_free (cap.last);
    g_object_unref (node);
}

static void
test_text_member_wins (void)
{
    PnNode    *node = g_object_new (PN_TYPE_PANEL_DISPLAY, NULL);
    PnMessage *m    = pn_message_new (NULL, NULL);

    /* An explicit text member takes precedence over a numeric value. */
    pn_message_set_string (m, "text", "hello");
    pn_message_set_double (m, "value", 1.0);
    pn_node_receive_message (node, m);
    g_object_unref (m);

    check_text (node, "hello");
    g_object_unref (node);
}

static void
test_boolean_on_off (void)
{
    PnNode    *node = g_object_new (PN_TYPE_PANEL_DISPLAY, NULL);
    PnMessage *m;

    m = pn_message_new (NULL, NULL);
    pn_message_set_boolean (m, "value", TRUE);
    pn_node_receive_message (node, m);
    g_object_unref (m);
    check_text (node, "on");

    m = pn_message_new (NULL, NULL);
    pn_message_set_boolean (m, "value", FALSE);
    pn_node_receive_message (node, m);
    g_object_unref (m);
    check_text (node, "off");

    g_object_unref (node);
}

static void
test_topic_fallback (void)
{
    PnNode    *node = g_object_new (PN_TYPE_PANEL_DISPLAY, NULL);
    PnMessage *m    = pn_message_new (NULL, "weather/now");

    /* No text and no value: fall back to the topic. */
    pn_node_receive_message (node, m);
    g_object_unref (m);

    check_text (node, "weather/now");
    g_object_unref (node);
}

static void
test_dedup_same_value (void)
{
    Cap     cap  = { 0, NULL };
    PnNode *node = g_object_new (PN_TYPE_PANEL_DISPLAY, NULL);

    g_signal_connect (node, "value-changed",
                      G_CALLBACK (on_value_changed), &cap);

    /* Repeated identical values keep the panel quiet: only the first
     * fires value-changed. */
    recv_double (node, "value", 5.0);
    recv_double (node, "value", 5.0);
    PN_CHECK_CMPINT (cap.count, ==, 1);

    recv_double (node, "value", 6.0);   /* a real change fires again */
    PN_CHECK_CMPINT (cap.count, ==, 2);

    g_free (cap.last);
    g_object_unref (node);
}

static void
test_ports (void)
{
    PnNode *node = g_object_new (PN_TYPE_PANEL_DISPLAY, NULL);

    /* A terminal sink: input only. */
    PN_CHECK       (pn_node_get_has_input (node));
    PN_CHECK_FALSE (pn_node_get_has_output (node));

    /* Empty before any message. */
    check_text (node, "");

    g_object_unref (node);
}

int
main (int argc, char **argv)
{
    pn_test_init (&argc, &argv, "pn-panel-display");
    pn_test_add ("formats_value",  test_formats_value);
    pn_test_add ("text_wins",      test_text_member_wins);
    pn_test_add ("boolean_on_off", test_boolean_on_off);
    pn_test_add ("topic_fallback", test_topic_fallback);
    pn_test_add ("dedup_same",     test_dedup_same_value);
    pn_test_add ("ports",          test_ports);
    return pn_test_run ();
}
