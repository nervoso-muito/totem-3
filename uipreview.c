/*
 * uipreview.c -- Display all windows from a GTK3 .ui file
 *
 * Usage: uipreview <file.ui>
 *
 * Loads the GtkBuilder XML, finds every GtkWindow/GtkDialog, shows
 * them all.  Exits when the last window is closed.
 *
 * Copyright (c) 2026 nervoso@k1.com.br with Claude Code (Anthropic)
 */

#include <gtk/gtk.h>
#include <stdlib.h>

static gint window_count = 0;

static void
on_destroy(GtkWidget *w, gpointer data)
{
	(void)w;
	(void)data;
	if (--window_count <= 0)
		gtk_main_quit();
}

int
main(int argc, char *argv[])
{
	if (argc < 2) {
		g_printerr("Usage: %s <file.ui>\n", argv[0]);
		return (1);
	}

	gtk_init(&argc, &argv);

	GtkBuilder *builder = gtk_builder_new();
	GError *err = NULL;

	if (!gtk_builder_add_from_file(builder, argv[1], &err)) {
		g_printerr("Error loading %s: %s\n", argv[1],
		    err->message);
		g_error_free(err);
		return (1);
	}

	/* Apply CSS that totem.c sets programmatically */
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css,
	    "#video_background { background-color: black; }\n"
	    "#video_frame { background-color: #333333; }\n",
	    -1, NULL);
	gtk_style_context_add_provider_for_screen(
	    gdk_screen_get_default(),
	    GTK_STYLE_PROVIDER(css),
	    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);

	/* Show every GtkWindow/GtkDialog found in the .ui file */
	GSList *objects = gtk_builder_get_objects(builder);
	for (GSList *l = objects; l; l = l->next) {
		if (!GTK_IS_WINDOW(l->data))
			continue;
		GtkWidget *win = GTK_WIDGET(l->data);
		g_signal_connect(win, "destroy",
		    G_CALLBACK(on_destroy), NULL);
		gtk_widget_show_all(win);
		window_count++;
	}
	g_slist_free(objects);

	if (window_count == 0) {
		g_printerr("No windows found in %s\n", argv[1]);
		g_object_unref(builder);
		return (1);
	}

	g_print("Showing %d window(s) from %s\n", window_count, argv[1]);

	gtk_main();
	g_object_unref(builder);

	return (0);
}
