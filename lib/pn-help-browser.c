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

#include "pn-help-browser.h"

#ifdef HAVE_WEBKIT

/* ================================================================== */
/*  WebKit2GTK implementation                                          */
/*                                                                     */
/*  Selected at configure time when webkit2gtk-4.1 is available and    */
/*  --with-webview is not set to "no".                                 */
/* ================================================================== */

#include <webkit2/webkit2.h>

struct _PnHelpBrowser
{
    GtkWindow parent_instance;

    WebKitWebView *web_view;

    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *home_button;
    GtkWidget *title_label;

    gchar *home_uri;
};

G_DEFINE_TYPE (PnHelpBrowser, pn_help_browser, GTK_TYPE_WINDOW)

static void
on_back_clicked (
        GtkButton     *button,
        PnHelpBrowser *self)
{
    (void) button;

    if (webkit_web_view_can_go_back (self->web_view))
        webkit_web_view_go_back (self->web_view);
}

static void
on_forward_clicked (
        GtkButton     *button,
        PnHelpBrowser *self)
{
    (void) button;

    if (webkit_web_view_can_go_forward (self->web_view))
        webkit_web_view_go_forward (self->web_view);
}

static void
on_home_clicked (
        GtkButton     *button,
        PnHelpBrowser *self)
{
    (void) button;

    if (self->home_uri)
        webkit_web_view_load_uri (self->web_view, self->home_uri);
}

static void
on_can_go_back_changed (
        WebKitWebView *web_view,
        GParamSpec    *pspec,
        PnHelpBrowser *self)
{
    (void) pspec;

    gtk_widget_set_sensitive (
            self->back_button,
            webkit_web_view_can_go_back (web_view));
}

static void
on_can_go_forward_changed (
        WebKitWebView *web_view,
        GParamSpec    *pspec,
        PnHelpBrowser *self)
{
    (void) pspec;

    gtk_widget_set_sensitive (
            self->forward_button,
            webkit_web_view_can_go_forward (web_view));
}

static void
on_title_changed (
        WebKitWebView *web_view,
        GParamSpec    *pspec,
        PnHelpBrowser *self)
{
    const gchar *title;

    (void) pspec;

    title = webkit_web_view_get_title (web_view);

    if (title && *title)
    {
        gtk_label_set_text (GTK_LABEL (self->title_label), title);
        gtk_window_set_title (GTK_WINDOW (self), title);
    }
}

static void
pn_help_browser_finalize (GObject *object)
{
    PnHelpBrowser *self = PN_HELP_BROWSER (object);

    g_free (self->home_uri);

    G_OBJECT_CLASS (pn_help_browser_parent_class)->finalize (object);
}

static void
pn_help_browser_class_init (PnHelpBrowserClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = pn_help_browser_finalize;
}

static void
pn_help_browser_init (PnHelpBrowser *self)
{
    GtkWidget *vbox;
    GtkWidget *navbar;
    GtkWidget *separator;
    GtkWidget *scrolled;

    gtk_window_set_default_size (GTK_WINDOW (self), 640, 720);
    gtk_window_set_title (GTK_WINDOW (self), "Help");

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (self), vbox);

    /* Navigation bar. */
    navbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class (
            gtk_widget_get_style_context (navbar), "toolbar");
    g_object_set (navbar, "margin", 2, NULL);
    gtk_box_pack_start (GTK_BOX (vbox), navbar, FALSE, FALSE, 0);

    self->back_button = gtk_button_new_from_icon_name (
            "go-previous", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_tooltip_text (self->back_button, "Back");
    gtk_widget_set_sensitive (self->back_button, FALSE);
    g_signal_connect (self->back_button, "clicked",
                      G_CALLBACK (on_back_clicked), self);
    gtk_box_pack_start (GTK_BOX (navbar), self->back_button,
                        FALSE, FALSE, 0);

    self->forward_button = gtk_button_new_from_icon_name (
            "go-next", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_tooltip_text (self->forward_button, "Forward");
    gtk_widget_set_sensitive (self->forward_button, FALSE);
    g_signal_connect (self->forward_button, "clicked",
                      G_CALLBACK (on_forward_clicked), self);
    gtk_box_pack_start (GTK_BOX (navbar), self->forward_button,
                        FALSE, FALSE, 0);

    self->home_button = gtk_button_new_from_icon_name (
            "go-home", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_tooltip_text (self->home_button, "Home");
    g_signal_connect (self->home_button, "clicked",
                      G_CALLBACK (on_home_clicked), self);
    gtk_box_pack_start (GTK_BOX (navbar), self->home_button,
                        FALSE, FALSE, 0);

    separator = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start (GTK_BOX (navbar), separator, FALSE, FALSE, 4);

    self->title_label = gtk_label_new ("Help");
    gtk_label_set_ellipsize (
            GTK_LABEL (self->title_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start (GTK_BOX (navbar), self->title_label,
                        TRUE, TRUE, 0);

    /* Web view. */
    self->web_view = WEBKIT_WEB_VIEW (webkit_web_view_new ());
    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_container_add (GTK_CONTAINER (scrolled),
                       GTK_WIDGET (self->web_view));
    gtk_box_pack_start (GTK_BOX (vbox), scrolled, TRUE, TRUE, 0);

    g_signal_connect (self->web_view, "notify::can-go-back",
                      G_CALLBACK (on_can_go_back_changed), self);
    g_signal_connect (self->web_view, "notify::can-go-forward",
                      G_CALLBACK (on_can_go_forward_changed), self);
    g_signal_connect (self->web_view, "notify::title",
                      G_CALLBACK (on_title_changed), self);
}

PnHelpBrowser *
pn_help_browser_new (
        GtkWindow   *parent,
        const gchar *page_path,
        const gchar *anchor)
{
    PnHelpBrowser *self = g_object_new (
            PN_TYPE_HELP_BROWSER,
            "transient-for", parent,
            "destroy-with-parent", TRUE,
            NULL);

    pn_help_browser_load_page (self, page_path, anchor);

    return self;
}

void
pn_help_browser_load_page (
        PnHelpBrowser *self,
        const gchar   *page_path,
        const gchar   *anchor)
{
    gchar *uri;
    gchar *full_uri;
    GError *error = NULL;

    g_return_if_fail (PN_IS_HELP_BROWSER (self));
    g_return_if_fail (page_path != NULL);

    uri = g_filename_to_uri (page_path, NULL, &error);

    if (!uri)
    {
        g_warning ("Failed to convert path to URI: %s",
                   error->message);
        g_error_free (error);
        return;
    }

    /* Home is the anchor-less form so the Home button always lands
     * at the top of the document. */
    if (!self->home_uri)
        self->home_uri = g_strdup (uri);

    /* Anchor IDs we emit are simple ASCII identifiers (GType names),
     * so plain concatenation is safe.  Any future caller passing a
     * fragment with reserved characters should percent-encode it
     * before calling us. */
    if (anchor != NULL && *anchor != '\0')
        full_uri = g_strconcat (uri, "#", anchor, NULL);
    else
        full_uri = g_strdup (uri);

    webkit_web_view_load_uri (self->web_view, full_uri);

    g_free (uri);
    g_free (full_uri);
}

#else  /* !HAVE_WEBKIT */

/* ================================================================== */
/*  GtkTextView fallback implementation                                */
/*                                                                     */
/*  Used when WebKit2GTK is not available (or --with-webview=no).      */
/*  Strips HTML tags and renders the file as plain text so the help    */
/*  browser still functions on hosts without WebKit's GL stack.        */
/* ================================================================== */

struct _PnHelpBrowser
{
    GtkWindow parent_instance;

    GtkTextView *text_view;
    GtkWidget   *title_label;

    gchar *home_path;
};

G_DEFINE_TYPE (PnHelpBrowser, pn_help_browser, GTK_TYPE_WINDOW)

/** Strip a very small subset of HTML to produce readable text.
 *  Good enough for the bundled help pages; not a real renderer. */
static gchar *
strip_html (const gchar *html)
{
    GString *out = g_string_new (NULL);
    const char *p = html;
    gboolean in_tag = FALSE;

    while (*p)
    {
        if (in_tag)
        {
            if (*p == '>')
                in_tag = FALSE;
        }
        else
        {
            if (*p == '<')
                in_tag = TRUE;
            else
                g_string_append_c (out, *p);
        }
        p++;
    }

    return g_string_free (out, FALSE);
}

static void
on_home_clicked (
        GtkButton     *button,
        PnHelpBrowser *self)
{
    (void) button;

    if (self->home_path)
        pn_help_browser_load_page (self, self->home_path, NULL);
}

static void
pn_help_browser_finalize (GObject *object)
{
    PnHelpBrowser *self = PN_HELP_BROWSER (object);

    g_free (self->home_path);

    G_OBJECT_CLASS (pn_help_browser_parent_class)->finalize (object);
}

static void
pn_help_browser_class_init (PnHelpBrowserClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = pn_help_browser_finalize;
}

static void
pn_help_browser_init (PnHelpBrowser *self)
{
    GtkWidget *vbox;
    GtkWidget *navbar;
    GtkWidget *home_button;
    GtkWidget *separator;
    GtkWidget *scrolled;
    GtkWidget *text_view;

    gtk_window_set_default_size (GTK_WINDOW (self), 640, 720);
    gtk_window_set_title (GTK_WINDOW (self), "Help");

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (self), vbox);

    navbar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class (
            gtk_widget_get_style_context (navbar), "toolbar");
    g_object_set (navbar, "margin", 2, NULL);
    gtk_box_pack_start (GTK_BOX (vbox), navbar, FALSE, FALSE, 0);

    home_button = gtk_button_new_from_icon_name (
            "go-home", GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_tooltip_text (home_button, "Home");
    g_signal_connect (home_button, "clicked",
                      G_CALLBACK (on_home_clicked), self);
    gtk_box_pack_start (GTK_BOX (navbar), home_button,
                        FALSE, FALSE, 0);

    separator = gtk_separator_new (GTK_ORIENTATION_VERTICAL);
    gtk_box_pack_start (GTK_BOX (navbar), separator, FALSE, FALSE, 4);

    self->title_label = gtk_label_new ("Help");
    gtk_label_set_ellipsize (
            GTK_LABEL (self->title_label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start (GTK_BOX (navbar), self->title_label,
                        TRUE, TRUE, 0);

    text_view = gtk_text_view_new ();
    self->text_view = GTK_TEXT_VIEW (text_view);
    gtk_text_view_set_editable      (self->text_view, FALSE);
    gtk_text_view_set_cursor_visible (self->text_view, FALSE);
    gtk_text_view_set_wrap_mode     (self->text_view, GTK_WRAP_WORD);
    g_object_set (text_view,
                  "left-margin",   8,
                  "right-margin",  8,
                  "top-margin",    8,
                  "bottom-margin", 8,
                  NULL);

    scrolled = gtk_scrolled_window_new (NULL, NULL);
    gtk_container_add (GTK_CONTAINER (scrolled), text_view);
    gtk_box_pack_start (GTK_BOX (vbox), scrolled, TRUE, TRUE, 0);
}

PnHelpBrowser *
pn_help_browser_new (
        GtkWindow   *parent,
        const gchar *page_path,
        const gchar *anchor)
{
    PnHelpBrowser *self = g_object_new (
            PN_TYPE_HELP_BROWSER,
            "transient-for", parent,
            "destroy-with-parent", TRUE,
            NULL);

    pn_help_browser_load_page (self, page_path, anchor);

    return self;
}

void
pn_help_browser_load_page (
        PnHelpBrowser *self,
        const gchar   *page_path,
        const gchar   *anchor)
{
    gchar *contents = NULL;
    gchar *plain;
    GError *error = NULL;
    GtkTextBuffer *buffer;

    /* The strip-html fallback collapses the document into untagged
     * plain text and so cannot honour fragment anchors.  Accept the
     * argument for API symmetry with the WebKit branch but ignore
     * it; users without WebKit see the page from the top. */
    (void) anchor;

    g_return_if_fail (PN_IS_HELP_BROWSER (self));
    g_return_if_fail (page_path != NULL);

    if (!g_file_get_contents (page_path, &contents, NULL, &error))
    {
        g_warning ("Failed to read help page %s: %s",
                   page_path, error->message);
        g_error_free (error);
        return;
    }

    if (!self->home_path)
        self->home_path = g_strdup (page_path);

    plain = strip_html (contents);
    g_free (contents);

    buffer = gtk_text_view_get_buffer (self->text_view);
    gtk_text_buffer_set_text (buffer, plain, -1);
    g_free (plain);

    {
        gchar *base = g_path_get_basename (page_path);
        gtk_label_set_text (GTK_LABEL (self->title_label), base);
        g_free (base);
    }
}

#endif  /* HAVE_WEBKIT */
