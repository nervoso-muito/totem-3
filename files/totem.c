/*
 * Totem 3.0 -- Movie Player for GNOME/MATE Desktop
 *
 * Copyright (c) 2026 nervoso@k1.com.br with Claude Code (Anthropic)
 * Based on Totem 2.32 by Bastien Nocera
 *
 * GTK3 + GStreamer 1.0 rewrite preserving the Totem 2.32 look and feel:
 *   - Menu bar (Movie/Edit/View/Go/Sound/Help)
 *   - HPaned: video area + controls on left, playlist sidebar on right
 *   - Seek slider with time label, volume button, sidebar toggle
 *   - Statusbar, fullscreen mode, keyboard shortcuts, drag & drop
 *   - Preferences: remember position, subtitle encoding
 *   - State persistence via GKeyFile (~/.config/totem.ini)
 */

#include <gtk/gtk.h>
#include <gst/gst.h>
#include <gst/video/videooverlay.h>
#include <gdk/gdk.h>
#include <totem-pl-parser.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <glib/gstdio.h>
#include <libintl.h>
#include <locale.h>

#define _(s)	gettext(s)
#define N_(s)	(s)

#define TOTEM_DOMAIN	"totem"
#define TOTEM_LOCALEDIR	"/usr/pkg/share/locale"

/* ------------------------------------------------------------------ */
/* Data types                                                          */
/* ------------------------------------------------------------------ */

enum {
	COL_PLAYING,	/* gint: 0=not playing, 1=playing */
	COL_NAME,	/* gchararray: display name */
	COL_URI,	/* gchararray: file URI */
	N_COLUMNS
};

typedef struct {
	/* Main window */
	GtkWidget	*window;
	GtkWidget	*main_vbox;
	GtkWidget	*menubar;
	GtkWidget	*paned;
	GtkWidget	*statusbar;
	guint		 statusbar_ctx;

	/* Video area */
	GtkWidget	*video_overlay;	 /* GtkOverlay */
	GtkWidget	*video_area;	 /* from gtksink */
	GtkWidget	*background;	 /* GtkImage placeholder */

	/* Controls */
	GtkWidget	*controls_vbox;
	GtkWidget	*seek_scale;
	GtkWidget	*time_label;
	GtkWidget	*volume_button;
	GtkWidget	*sidebar_button;

	/* Playlist sidebar */
	GtkWidget	*sidebar_box;
	GtkWidget	*playlist_view;
	GtkListStore	*playlist_store;
	GtkWidget	*pl_add_btn;
	GtkWidget	*pl_remove_btn;
	GtkWidget	*pl_save_btn;
	GtkWidget	*pl_up_btn;
	GtkWidget	*pl_down_btn;

	/* GStreamer */
	GstElement	*player;
	GstElement	*video_sink;
	GstElement	*equalizer;
	GstElement	*preamp;

	/* State */
	gboolean	 sidebar_visible;
	gboolean	 fullscreen;
	gboolean	 seeking;	 /* user dragging seek bar */
	gboolean	 repeat_mode;
	gboolean	 shuffle_mode;
	gboolean	 remember_position;
	gboolean	 eq_disabled;
	gboolean	 normalize_720p;

	/* Fullscreen controls */
	GtkWidget	*fs_window;	 /* fullscreen control overlay */
	guint		 fs_timeout_id;

	/* Position update timer */
	guint		 position_timer;
} TotemApp;

static TotemApp *app = NULL;

/* Forward declarations */
static void	totem_play_uri(TotemApp *app, const gchar *uri);
static void	totem_play_pause(TotemApp *app);
static void	totem_stop(TotemApp *app);
static void	totem_next(TotemApp *app);
static void	totem_prev(TotemApp *app);
static void	totem_seek_relative(TotemApp *app, gint64 offset_sec);
static void	totem_set_fullscreen(TotemApp *app, gboolean fs);
static void	totem_toggle_sidebar(TotemApp *app);
static void	totem_save_state(TotemApp *app);
static void	totem_load_state(TotemApp *app);
static void	totem_update_sensitivity(TotemApp *app);
static gchar   *totem_config_path(void);
static void	totem_playlist_add_uri(TotemApp *app, const gchar *uri, const gchar *name);
static gboolean	totem_playlist_get_current_uri(TotemApp *app, gchar **uri);
static void	totem_playlist_set_playing(TotemApp *app, GtkTreeIter *iter);
static void	totem_statusbar_push(TotemApp *app, const gchar *msg);
static gboolean	totem_open_uri_or_playlist(TotemApp *app, const gchar *uri);

/* ------------------------------------------------------------------ */
/* Utility                                                             */
/* ------------------------------------------------------------------ */

/*
 * format_time -- Format a GStreamer time value (nanoseconds) as HH:MM:SS
 * or MM:SS into the given buffer.
 */
static void
format_time(gint64 ns, char *buf, size_t len)
{
	gint64 total_sec = ns / GST_SECOND;
	gint hours = total_sec / 3600;
	gint minutes = (total_sec % 3600) / 60;
	gint seconds = total_sec % 60;

	if (hours > 0)
		snprintf(buf, len, "%d:%02d:%02d", hours, minutes, seconds);
	else
		snprintf(buf, len, "%02d:%02d", minutes, seconds);
}

/*
 * totem_config_path -- Return the path to ~/.config/totem.ini.
 * Caller must g_free() the result.
 */
static gchar *
totem_config_path(void)
{
	return g_build_filename(g_get_user_config_dir(), "totem.ini", NULL);
}

/* ------------------------------------------------------------------ */
/* State persistence                                                   */
/* ------------------------------------------------------------------ */

/*
 * totem_save_state -- Persist application state to ~/.config/totem.ini.
 * Saves window geometry, sidebar state, preferences, volume, playlist
 * URIs, and current playback position (if remember_position is enabled).
 */
static void
totem_save_state(TotemApp *app)
{
	GKeyFile *kf = g_key_file_new();
	gchar *path = totem_config_path();

	/* Load existing to preserve other keys */
	g_key_file_load_from_file(kf, path, G_KEY_FILE_KEEP_COMMENTS, NULL);

	/* Window geometry */
	gint w, h;
	gtk_window_get_size(GTK_WINDOW(app->window), &w, &h);
	g_key_file_set_integer(kf, "Window", "width", w);
	g_key_file_set_integer(kf, "Window", "height", h);

	/* Sidebar state */
	g_key_file_set_boolean(kf, "Window", "sidebar_visible", app->sidebar_visible);
	if (app->sidebar_visible) {
		gint pos = gtk_paned_get_position(GTK_PANED(app->paned));
		g_key_file_set_integer(kf, "Window", "paned_position", pos);
	}

	/* Preferences */
	g_key_file_set_boolean(kf, "Prefs", "remember_position", app->remember_position);
	g_key_file_set_boolean(kf, "Prefs", "repeat_mode", app->repeat_mode);
	g_key_file_set_boolean(kf, "Prefs", "shuffle_mode", app->shuffle_mode);
	g_key_file_set_boolean(kf, "Prefs", "normalize_720p", app->normalize_720p);

	/* Volume */
	gdouble vol = gtk_scale_button_get_value(GTK_SCALE_BUTTON(app->volume_button));
	g_key_file_set_double(kf, "Playback", "volume", vol);

	/* Current file position (if remember_position) */
	if (app->remember_position && app->player) {
		gchar *uri = NULL;
		if (totem_playlist_get_current_uri(app, &uri) && uri) {
			gint64 pos;
			if (gst_element_query_position(app->player, GST_FORMAT_TIME, &pos)) {
				g_key_file_set_double(kf, uri, "position",
				    (double)pos / GST_SECOND);
			}
			g_free(uri);
		}
	}

	/* Save playlist */
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(
	    GTK_TREE_MODEL(app->playlist_store), &iter);
	GPtrArray *uris = g_ptr_array_new_with_free_func(g_free);
	while (valid) {
		gchar *uri;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_URI, &uri, -1);
		g_ptr_array_add(uris, uri);
		valid = gtk_tree_model_iter_next(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
	}
	if (uris->len > 0) {
		g_key_file_set_string_list(kf, "Playlist", "uris",
		    (const gchar *const *)uris->pdata, uris->len);
	} else {
		g_key_file_remove_key(kf, "Playlist", "uris", NULL);
	}
	g_ptr_array_free(uris, TRUE);

	/* Write */
	gsize data_len;
	gchar *data = g_key_file_to_data(kf, &data_len, NULL);
	g_file_set_contents(path, data, data_len, NULL);
	g_free(data);
	g_key_file_free(kf);
	g_free(path);
}

/*
 * totem_load_state -- Restore application state from ~/.config/totem.ini.
 * Restores window size, sidebar visibility, preferences, volume level,
 * saved playlist, and paned position.
 */
static void
totem_load_state(TotemApp *app)
{
	GKeyFile *kf = g_key_file_new();
	gchar *path = totem_config_path();

	if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
		g_key_file_free(kf);
		g_free(path);
		return;
	}

	/* Window geometry */
	gint w = g_key_file_get_integer(kf, "Window", "width", NULL);
	gint h = g_key_file_get_integer(kf, "Window", "height", NULL);
	if (w > 0 && h > 0)
		gtk_window_set_default_size(GTK_WINDOW(app->window), w, h);

	/* Sidebar */
	app->sidebar_visible = g_key_file_get_boolean(kf, "Window",
	    "sidebar_visible", NULL);

	/* Preferences */
	app->remember_position = g_key_file_get_boolean(kf, "Prefs",
	    "remember_position", NULL);
	app->repeat_mode = g_key_file_get_boolean(kf, "Prefs",
	    "repeat_mode", NULL);
	app->shuffle_mode = g_key_file_get_boolean(kf, "Prefs",
	    "shuffle_mode", NULL);
	app->normalize_720p = g_key_file_get_boolean(kf, "Prefs",
	    "normalize_720p", NULL);

	/* Volume */
	if (g_key_file_has_key(kf, "Playback", "volume", NULL)) {
		gdouble vol = g_key_file_get_double(kf, "Playback",
		    "volume", NULL);
		gtk_scale_button_set_value(
		    GTK_SCALE_BUTTON(app->volume_button), vol);
	}



	/* Playlist */
	gsize n;
	gchar **uris = g_key_file_get_string_list(kf, "Playlist", "uris",
	    &n, NULL);
	if (uris) {
		for (gsize i = 0; i < n; i++) {
			gchar *basename = g_path_get_basename(uris[i]);
			/* Strip file:// for display */
			gchar *name = g_uri_unescape_string(basename, NULL);
			totem_playlist_add_uri(app, uris[i], name ? name : basename);
			g_free(name);
			g_free(basename);
		}
		g_strfreev(uris);
	}

	/* Paned position */
	if (app->sidebar_visible) {
		gint pos = g_key_file_get_integer(kf, "Window",
		    "paned_position", NULL);
		if (pos > 0)
			gtk_paned_set_position(GTK_PANED(app->paned), pos);
	}

	g_key_file_free(kf);
	g_free(path);
}

/* ------------------------------------------------------------------ */
/* Playlist management                                                 */
/* ------------------------------------------------------------------ */

/*
 * totem_playlist_add_uri -- Add a URI to the playlist sidebar.
 * Skips the entry if the same URI already exists (duplicate prevention).
 * Updates button sensitivity after adding.
 */
static void
totem_playlist_add_uri(TotemApp *app, const gchar *uri, const gchar *name)
{
	/* Skip if URI already in playlist */
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(
	    GTK_TREE_MODEL(app->playlist_store), &iter);
	while (valid) {
		gchar *existing;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_URI, &existing, -1);
		gboolean dup = (g_strcmp0(existing, uri) == 0);
		g_free(existing);
		if (dup)
			return;
		valid = gtk_tree_model_iter_next(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
	}

	gtk_list_store_append(app->playlist_store, &iter);
	gtk_list_store_set(app->playlist_store, &iter,
	    COL_PLAYING, 0,
	    COL_NAME, name,
	    COL_URI, uri,
	    -1);
	totem_update_sensitivity(app);
}

/* ------------------------------------------------------------------ */
/* Playlist file parsing via totem-pl-parser                           */
/* ------------------------------------------------------------------ */

/*
 * on_pl_parser_entry_parsed -- Callback from totem-pl-parser when a
 * playlist entry is parsed.  Extracts title metadata (if available)
 * and adds the entry to the playlist sidebar.
 */
static void
on_pl_parser_entry_parsed(TotemPlParser *parser, const gchar *uri,
    GHashTable *metadata, TotemApp *app)
{
	(void)parser;
	const gchar *title = NULL;
	if (metadata)
		title = g_hash_table_lookup(metadata,
		    TOTEM_PL_PARSER_FIELD_TITLE);

	gchar *basename = g_path_get_basename(uri);
	gchar *name_unesc = g_uri_unescape_string(
	    title ? title : basename, NULL);
	totem_playlist_add_uri(app, uri,
	    name_unesc ? name_unesc : basename);
	g_free(name_unesc);
	g_free(basename);
}

/*
 * Try to parse uri as a playlist (m3u, pls, xspf, etc.) via
 * totem-pl-parser.  If it's a playlist, entries are added to
 * the playlist sidebar.  If it's a plain media file, add it
 * directly.  Returns TRUE if anything was added.
 */
static gboolean
totem_open_uri_or_playlist(TotemApp *app, const gchar *uri)
{
	TotemPlParser *parser = totem_pl_parser_new();

	g_object_set(parser, "recurse", TRUE, "disable-unsafe", TRUE, NULL);
	g_signal_connect(parser, "entry-parsed",
	    G_CALLBACK(on_pl_parser_entry_parsed), app);

	TotemPlParserResult res = totem_pl_parser_parse(parser, uri, FALSE);

	if (res == TOTEM_PL_PARSER_RESULT_SUCCESS) {
		/* Playlist was parsed -- entries already added by signal */
		g_object_unref(parser);
		return TRUE;
	}

	g_object_unref(parser);

	/* Not a playlist -- add as a plain media file */
	gchar *basename = g_path_get_basename(uri);
	gchar *name = g_uri_unescape_string(basename, NULL);
	totem_playlist_add_uri(app, uri, name ? name : basename);
	g_free(name);
	g_free(basename);
	return TRUE;
}

/*
 * totem_playlist_get_current_uri -- Find the currently playing entry
 * in the playlist.  If uri is non-NULL, stores the URI (caller must
 * g_free).  Returns TRUE if a playing entry was found.
 */
static gboolean
totem_playlist_get_current_uri(TotemApp *app, gchar **uri)
{
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(
	    GTK_TREE_MODEL(app->playlist_store), &iter);

	while (valid) {
		gint playing;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_PLAYING, &playing, -1);
		if (playing) {
			if (uri)
				gtk_tree_model_get(
				    GTK_TREE_MODEL(app->playlist_store), &iter,
				    COL_URI, uri, -1);
			return TRUE;
		}
		valid = gtk_tree_model_iter_next(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
	}
	return FALSE;
}

/*
 * totem_playlist_set_playing -- Mark a playlist entry as the currently
 * playing track.  Clears COL_PLAYING on all rows, sets it on the given
 * iter, moves the tree view cursor to highlight it, and scrolls it
 * into view.
 */
static void
totem_playlist_set_playing(TotemApp *app, GtkTreeIter *iter)
{
	/* Clear all playing flags */
	GtkTreeIter it;
	gboolean valid = gtk_tree_model_get_iter_first(
	    GTK_TREE_MODEL(app->playlist_store), &it);
	while (valid) {
		gtk_list_store_set(app->playlist_store, &it, COL_PLAYING, 0, -1);
		valid = gtk_tree_model_iter_next(
		    GTK_TREE_MODEL(app->playlist_store), &it);
	}
	if (iter) {
		gtk_list_store_set(app->playlist_store, iter, COL_PLAYING, 1, -1);

		/* Highlight the playing row in the playlist view */
		GtkTreePath *path = gtk_tree_model_get_path(
		    GTK_TREE_MODEL(app->playlist_store), iter);
		if (path) {
			gtk_tree_view_set_cursor(
			    GTK_TREE_VIEW(app->playlist_view), path, NULL, FALSE);
			gtk_tree_view_scroll_to_cell(
			    GTK_TREE_VIEW(app->playlist_view), path,
			    NULL, FALSE, 0, 0);
			gtk_tree_path_free(path);
		}
	}
}

/*
 * totem_playlist_next -- Find the next playlist entry after the currently
 * playing one.  If repeat mode is on, wraps around to the first entry.
 * Returns TRUE and sets out to the next iter, FALSE if at the end.
 */
static gboolean
totem_playlist_next(TotemApp *app, GtkTreeIter *out)
{
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first(
	    GTK_TREE_MODEL(app->playlist_store), &iter);
	gboolean found_current = FALSE;

	while (valid) {
		gint playing;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_PLAYING, &playing, -1);
		if (playing) {
			found_current = TRUE;
			valid = gtk_tree_model_iter_next(
			    GTK_TREE_MODEL(app->playlist_store), &iter);
			if (valid) {
				*out = iter;
				return TRUE;
			}
			/* Wrap around if repeat */
			if (app->repeat_mode) {
				if (gtk_tree_model_get_iter_first(
				    GTK_TREE_MODEL(app->playlist_store), out))
					return TRUE;
			}
			return FALSE;
		}
		valid = gtk_tree_model_iter_next(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
	}

	/* Nothing playing, start from beginning */
	if (!found_current) {
		if (gtk_tree_model_get_iter_first(
		    GTK_TREE_MODEL(app->playlist_store), out))
			return TRUE;
	}
	return FALSE;
}

/*
 * totem_playlist_prev -- Find the previous playlist entry before the
 * currently playing one.  Returns TRUE and sets out if found, FALSE
 * if the current entry is the first.
 */
static gboolean
totem_playlist_prev(TotemApp *app, GtkTreeIter *out)
{
	GtkTreeIter iter, prev_iter;
	gboolean valid = gtk_tree_model_get_iter_first(
	    GTK_TREE_MODEL(app->playlist_store), &iter);
	gboolean have_prev = FALSE;

	while (valid) {
		gint playing;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_PLAYING, &playing, -1);
		if (playing) {
			if (have_prev) {
				*out = prev_iter;
				return TRUE;
			}
			return FALSE;
		}
		prev_iter = iter;
		have_prev = TRUE;
		valid = gtk_tree_model_iter_next(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
	}
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Statusbar                                                           */
/* ------------------------------------------------------------------ */

/*
 * totem_statusbar_push -- Replace the statusbar text with the given message.
 */
static void
totem_statusbar_push(TotemApp *app, const gchar *msg)
{
	gtk_statusbar_pop(GTK_STATUSBAR(app->statusbar), app->statusbar_ctx);
	gtk_statusbar_push(GTK_STATUSBAR(app->statusbar), app->statusbar_ctx, msg);
}

/* ------------------------------------------------------------------ */
/* GStreamer callbacks                                                  */
/* ------------------------------------------------------------------ */

/*
 * on_eos -- GStreamer end-of-stream callback.  Saves state and advances
 * to the next playlist entry.
 */
static void
on_eos(GstBus *bus, GstMessage *msg, TotemApp *app)
{
	(void)bus; (void)msg;
	totem_save_state(app);
	totem_next(app);
}

/*
 * on_error -- GStreamer error callback.  Shows the error in the statusbar
 * and in a modal dialog, then stops playback.
 */
static void
on_error(GstBus *bus, GstMessage *msg, TotemApp *app)
{
	(void)bus;
	GError *err = NULL;
	gchar *debug = NULL;
	gst_message_parse_error(msg, &err, &debug);

	gchar *status = g_strdup_printf(_("Error: %s"), err->message);
	totem_statusbar_push(app, status);
	g_free(status);

	GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(app->window),
	    GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
	    _("Error: %s"), err->message);
	gtk_dialog_run(GTK_DIALOG(dlg));
	gtk_widget_destroy(dlg);

	g_error_free(err);
	g_free(debug);
	gst_element_set_state(app->player, GST_STATE_NULL);
}

/*
 * on_state_changed -- GStreamer state-changed callback.  When playbin
 * reaches PLAYING, queries the video sink for the stream's dimensions
 * and pixel aspect ratio, then resizes the window to match (capped at
 * 75% of screen size).
 */
static void
on_state_changed(GstBus *bus, GstMessage *msg, TotemApp *app)
{
	(void)bus;

	/* Only handle messages from the playbin itself */
	if (GST_MESSAGE_SRC(msg) != GST_OBJECT(app->player))
		return;

	GstState old_state, new_state;
	gst_message_parse_state_changed(msg, &old_state, &new_state, NULL);

	if (new_state != GST_STATE_PLAYING)
		return;

	/* Query video dimensions from the video sink pad */
	GstPad *pad = gst_element_get_static_pad(app->video_sink, "sink");
	if (!pad)
		return;

	GstCaps *caps = gst_pad_get_current_caps(pad);
	gst_object_unref(pad);
	if (!caps)
		return;

	GstStructure *s = gst_caps_get_structure(caps, 0);
	gint video_w = 0, video_h = 0;
	if (!gst_structure_get_int(s, "width", &video_w) ||
	    !gst_structure_get_int(s, "height", &video_h) ||
	    video_w <= 0 || video_h <= 0) {
		gst_caps_unref(caps);
		return;
	}

	/* Account for pixel aspect ratio */
	gint par_n = 1, par_d = 1;
	gst_structure_get_fraction(s, "pixel-aspect-ratio", &par_n, &par_d);
	gst_caps_unref(caps);

	gint display_w, display_h;

	if (app->normalize_720p) {
		/* Force 720p, preserving aspect ratio */
		display_w = 1280;
		display_h = 720;
	} else {
		display_w = video_w * par_n / par_d;
		display_h = video_h;
	}

	/* Get screen size to cap the window */
	GdkDisplay *display = gtk_widget_get_display(app->window);
	GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
	if (!monitor)
		monitor = gdk_display_get_monitor(display, 0);
	GdkRectangle geom;
	gdk_monitor_get_geometry(monitor, &geom);
	gint screen_w = geom.width;
	gint screen_h = geom.height;
	gint max_w = screen_w * 3 / 4;
	gint max_h = screen_h * 3 / 4;

	/* Scale down if needed, preserving aspect ratio */
	if (display_w > max_w || display_h > max_h) {
		gdouble scale = fmin((gdouble)max_w / display_w,
		    (gdouble)max_h / display_h);
		display_w = (gint)(display_w * scale);
		display_h = (gint)(display_h * scale);
	}

	/* Get extra height from menu, controls, statusbar */
	GtkAllocation alloc;
	gtk_widget_get_allocation(app->window, &alloc);
	GtkAllocation video_alloc;
	gtk_widget_get_allocation(app->video_overlay, &video_alloc);
	gint extra_h = alloc.height - video_alloc.height;
	gint extra_w = alloc.width - video_alloc.width;

	gtk_window_resize(GTK_WINDOW(app->window),
	    display_w + extra_w, display_h + extra_h);
}

/*
 * update_position -- Timer callback (250ms interval) that updates the
 * seek slider position and time label while media is playing or paused.
 */
static gboolean
update_position(gpointer data)
{
	TotemApp *app = data;
	if (app->seeking)
		return G_SOURCE_CONTINUE;

	GstState state;
	gst_element_get_state(app->player, &state, NULL, 0);
	if (state != GST_STATE_PLAYING && state != GST_STATE_PAUSED)
		return G_SOURCE_CONTINUE;

	gint64 position = 0, duration = 0;
	if (!gst_element_query_position(app->player, GST_FORMAT_TIME, &position) ||
	    !gst_element_query_duration(app->player, GST_FORMAT_TIME, &duration) ||
	    duration <= 0)
		return G_SOURCE_CONTINUE;

	/* Update seek bar */
	g_signal_handlers_block_by_func(app->seek_scale,
	    G_CALLBACK(gtk_range_get_value), NULL);  /* placeholder */
	gtk_range_set_range(GTK_RANGE(app->seek_scale), 0,
	    (gdouble)duration / GST_SECOND);
	gtk_range_set_value(GTK_RANGE(app->seek_scale),
	    (gdouble)position / GST_SECOND);
	gtk_widget_set_sensitive(app->seek_scale, TRUE);

	/* Update time label */
	char pos_str[32], dur_str[32], label[80];
	format_time(position, pos_str, sizeof(pos_str));
	format_time(duration, dur_str, sizeof(dur_str));
	snprintf(label, sizeof(label), "%s / %s", pos_str, dur_str);
	gtk_label_set_text(GTK_LABEL(app->time_label), label);

	return G_SOURCE_CONTINUE;
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

/*
 * totem_play_uri -- Start playing the given URI.  Resets the pipeline,
 * sets the URI on playbin, enters PLAYING state, optionally restores
 * the last playback position, shows the video widget, and updates the
 * window title and statusbar.
 */
static void
totem_play_uri(TotemApp *app, const gchar *uri)
{
	gst_element_set_state(app->player, GST_STATE_NULL);

	g_object_set(app->player, "uri", uri, NULL);
	gst_element_set_state(app->player, GST_STATE_PLAYING);

	/* Restore position if enabled */
	if (app->remember_position) {
		GKeyFile *kf = g_key_file_new();
		gchar *path = totem_config_path();
		if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
			if (g_key_file_has_key(kf, uri, "position", NULL)) {
				gdouble pos = g_key_file_get_double(kf, uri,
				    "position", NULL);
				if (pos > 1.0) {
					/* Delay seek until playing */
					g_object_set_data(G_OBJECT(app->player),
					    "restore-pos",
					    GINT_TO_POINTER((gint)(pos)));
				}
			}
		}
		g_key_file_free(kf);
		g_free(path);
	}

	/* Show video widget */
	if (app->video_area) {
		gtk_widget_show(app->video_area);
		gtk_widget_hide(app->background);
	}

	/* Update title */
	gchar *basename = g_path_get_basename(uri);
	gchar *name = g_uri_unescape_string(basename, NULL);
	gchar *title = g_strdup_printf(_("%s - Movie Player"),
	    name ? name : basename);
	gtk_window_set_title(GTK_WINDOW(app->window), title);
	totem_statusbar_push(app, name ? name : basename);
	g_free(title);
	g_free(name);
	g_free(basename);
}

/*
 * totem_play_pause -- Toggle between play and pause.  If nothing is
 * playing, starts playback of the current or first playlist entry.
 */
static void
totem_play_pause(TotemApp *app)
{
	GstState state;
	gst_element_get_state(app->player, &state, NULL, 0);

	if (state == GST_STATE_PLAYING) {
		gst_element_set_state(app->player, GST_STATE_PAUSED);
		totem_statusbar_push(app, _("Paused"));
	} else if (state == GST_STATE_PAUSED) {
		gst_element_set_state(app->player, GST_STATE_PLAYING);
		totem_statusbar_push(app, _("Playing"));
	} else {
		/* Nothing playing -- play current playlist item */
		gchar *uri = NULL;
		if (totem_playlist_get_current_uri(app, &uri)) {
			totem_play_uri(app, uri);
			g_free(uri);
		} else {
			/* Try first item */
			GtkTreeIter iter;
			if (gtk_tree_model_get_iter_first(
			    GTK_TREE_MODEL(app->playlist_store), &iter)) {
				gchar *u;
				gtk_tree_model_get(
				    GTK_TREE_MODEL(app->playlist_store), &iter,
				    COL_URI, &u, -1);
				totem_playlist_set_playing(app, &iter);
				totem_play_uri(app, u);
				g_free(u);
			}
		}
	}
}

/*
 * totem_stop -- Stop playback.  Saves state, resets the pipeline to NULL,
 * clears the seek bar and time label, restores the black background,
 * and hides the video widget.
 */
static void
totem_stop(TotemApp *app)
{
	totem_save_state(app);
	gst_element_set_state(app->player, GST_STATE_NULL);
	gtk_range_set_value(GTK_RANGE(app->seek_scale), 0);
	gtk_widget_set_sensitive(app->seek_scale, FALSE);
	gtk_label_set_text(GTK_LABEL(app->time_label), "00:00 / 00:00");
	gtk_window_set_title(GTK_WINDOW(app->window), _("Movie Player"));
	totem_statusbar_push(app, _("Stopped"));

	if (app->background) {
		gtk_widget_show(app->background);
		if (app->video_area)
			gtk_widget_hide(app->video_area);
	}
}

/*
 * totem_next -- Advance to the next playlist entry and play it.
 * Stops playback if there is no next entry.
 */
static void
totem_next(TotemApp *app)
{
	GtkTreeIter iter;
	if (totem_playlist_next(app, &iter)) {
		gchar *uri;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_URI, &uri, -1);
		totem_playlist_set_playing(app, &iter);
		totem_play_uri(app, uri);
		g_free(uri);
	} else {
		totem_stop(app);
	}
}

/*
 * totem_prev -- Go to the previous playlist entry.  If more than 3
 * seconds into the current track, restarts it instead of going back.
 */
static void
totem_prev(TotemApp *app)
{
	/* If more than 3 seconds in, restart current track */
	gint64 position = 0;
	if (gst_element_query_position(app->player, GST_FORMAT_TIME, &position)) {
		if (position > 3 * GST_SECOND) {
			gst_element_seek_simple(app->player, GST_FORMAT_TIME,
			    GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, 0);
			return;
		}
	}

	GtkTreeIter iter;
	if (totem_playlist_prev(app, &iter)) {
		gchar *uri;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_URI, &uri, -1);
		totem_playlist_set_playing(app, &iter);
		totem_play_uri(app, uri);
		g_free(uri);
	}
}

/*
 * totem_seek_relative -- Seek forward or backward by offset_sec seconds
 * relative to the current position.  Clamps to [0, duration].
 */
static void
totem_seek_relative(TotemApp *app, gint64 offset_sec)
{
	gint64 position = 0, duration = 0;
	if (!gst_element_query_position(app->player, GST_FORMAT_TIME, &position))
		return;
	gst_element_query_duration(app->player, GST_FORMAT_TIME, &duration);

	gint64 new_pos = position + offset_sec * GST_SECOND;
	if (new_pos < 0) new_pos = 0;
	if (duration > 0 && new_pos > duration) new_pos = duration;

	gst_element_seek_simple(app->player, GST_FORMAT_TIME,
	    GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT, new_pos);
}

/*
 * totem_update_sensitivity -- Enable or disable playlist buttons
 * (remove, save, up, down) based on the number of playlist entries.
 */
static void
totem_update_sensitivity(TotemApp *app)
{
	gint n = gtk_tree_model_iter_n_children(
	    GTK_TREE_MODEL(app->playlist_store), NULL);
	gtk_widget_set_sensitive(app->pl_remove_btn, n > 0);
	gtk_widget_set_sensitive(app->pl_save_btn, n > 0);
	gtk_widget_set_sensitive(app->pl_up_btn, n > 1);
	gtk_widget_set_sensitive(app->pl_down_btn, n > 1);
}

/* ------------------------------------------------------------------ */
/* UI Callbacks                                                        */
/* ------------------------------------------------------------------ */

/*
 * on_seek_press -- Mark seeking flag and jump the slider to the clicked
 * position instead of stepping incrementally (default GtkScale behavior).
 */
static gboolean
on_seek_press(GtkWidget *w, GdkEventButton *ev, TotemApp *app)
{
	app->seeking = TRUE;

	/* Jump to click position */
	GtkAllocation alloc;
	gtk_widget_get_allocation(w, &alloc);
	gdouble lo = gtk_adjustment_get_lower(
	    gtk_range_get_adjustment(GTK_RANGE(w)));
	gdouble hi = gtk_adjustment_get_upper(
	    gtk_range_get_adjustment(GTK_RANGE(w)));
	gdouble frac = ev->x / (gdouble)alloc.width;
	if (frac < 0.0) frac = 0.0;
	if (frac > 1.0) frac = 1.0;
	gdouble val = lo + frac * (hi - lo);
	gtk_range_set_value(GTK_RANGE(w), val);

	return FALSE;
}

/* on_seek_release -- Perform the actual seek when user releases the slider. */
static gboolean
on_seek_release(GtkWidget *w, GdkEventButton *ev, TotemApp *app)
{
	(void)w; (void)ev;
	app->seeking = FALSE;
	gdouble val = gtk_range_get_value(GTK_RANGE(app->seek_scale));
	gst_element_seek_simple(app->player, GST_FORMAT_TIME,
	    GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
	    (gint64)(val * GST_SECOND));
	return FALSE;
}

/* on_volume_changed -- Sync the GStreamer volume with the GTK volume button. */
static void
on_volume_changed(GtkScaleButton *btn, gdouble val, TotemApp *app)
{
	(void)btn;
	if (app->player)
		g_object_set(app->player, "volume", val, NULL);
}

/* on_playlist_row_activated -- Play a track when double-clicked in the playlist. */
static void
on_playlist_row_activated(GtkTreeView *tv, GtkTreePath *path,
    GtkTreeViewColumn *col, TotemApp *app)
{
	(void)tv; (void)col;
	GtkTreeIter iter;
	if (gtk_tree_model_get_iter(GTK_TREE_MODEL(app->playlist_store),
	    &iter, path)) {
		gchar *uri;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_URI, &uri, -1);
		totem_save_state(app);
		totem_playlist_set_playing(app, &iter);
		totem_play_uri(app, uri);
		g_free(uri);
	}
}

/* on_pl_add_clicked -- Open a file chooser to add media files or playlists. */
static void
on_pl_add_clicked(GtkButton *btn, TotemApp *app)
{
	(void)btn;
	GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Add Files"),
	    GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
	    _("_Cancel"), GTK_RESPONSE_CANCEL,
	    _("_Open"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);

	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, _("Media Files"));
	gtk_file_filter_add_mime_type(filter, "video/*");
	gtk_file_filter_add_mime_type(filter, "audio/*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

	GtkFileFilter *all = gtk_file_filter_new();
	gtk_file_filter_set_name(all, _("All Files"));
	gtk_file_filter_add_pattern(all, "*");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), all);

	/* Add playlist filter */
	GtkFileFilter *pl_filter = gtk_file_filter_new();
	gtk_file_filter_set_name(pl_filter, _("Playlists"));
	gtk_file_filter_add_pattern(pl_filter, "*.m3u");
	gtk_file_filter_add_pattern(pl_filter, "*.m3u8");
	gtk_file_filter_add_pattern(pl_filter, "*.pls");
	gtk_file_filter_add_pattern(pl_filter, "*.xspf");
	gtk_file_filter_add_pattern(pl_filter, "*.asx");
	gtk_file_filter_add_pattern(pl_filter, "*.wpl");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), pl_filter);

	if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
		GSList *files = gtk_file_chooser_get_uris(GTK_FILE_CHOOSER(dlg));
		for (GSList *l = files; l; l = l->next) {
			gchar *uri = l->data;
			totem_open_uri_or_playlist(app, uri);
		}
		g_slist_free_full(files, g_free);
	}
	gtk_widget_destroy(dlg);
}

/* on_pl_remove_clicked -- Remove the selected entry from the playlist. */
static void
on_pl_remove_clicked(GtkButton *btn, TotemApp *app)
{
	(void)btn;
	GtkTreeSelection *sel = gtk_tree_view_get_selection(
	    GTK_TREE_VIEW(app->playlist_view));
	GtkTreeIter iter;
	if (gtk_tree_selection_get_selected(sel, NULL, &iter)) {
		gint playing;
		gtk_tree_model_get(GTK_TREE_MODEL(app->playlist_store), &iter,
		    COL_PLAYING, &playing, -1);
		gtk_list_store_remove(app->playlist_store, &iter);
		if (playing)
			totem_stop(app);
	}
	totem_update_sensitivity(app);
}

/*
 * on_pl_save_clicked -- Save the playlist to a file (PLS, M3U, or XSPF)
 * using totem-pl-parser.  Format is determined by the file extension.
 */
static void
on_pl_save_clicked(GtkButton *btn, TotemApp *app)
{
	(void)btn;
	GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Save Playlist"),
	    GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_SAVE,
	    _("_Cancel"), GTK_RESPONSE_CANCEL,
	    _("_Save"), GTK_RESPONSE_ACCEPT, NULL);
	gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
	gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), "playlist.pls");

	if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
		gchar *filename = gtk_file_chooser_get_filename(
		    GTK_FILE_CHOOSER(dlg));

		/* Build a TotemPlPlaylist from our GtkListStore */
		TotemPlPlaylist *playlist = totem_pl_playlist_new();
		GtkTreeIter iter;
		gboolean valid = gtk_tree_model_get_iter_first(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
		while (valid) {
			gchar *uri, *name;
			gtk_tree_model_get(
			    GTK_TREE_MODEL(app->playlist_store), &iter,
			    COL_URI, &uri, COL_NAME, &name, -1);
			TotemPlPlaylistIter pl_iter;
			totem_pl_playlist_append(playlist, &pl_iter);
			totem_pl_playlist_set(playlist, &pl_iter,
			    TOTEM_PL_PARSER_FIELD_URI, uri,
			    TOTEM_PL_PARSER_FIELD_TITLE, name,
			    NULL);
			g_free(uri);
			g_free(name);
			valid = gtk_tree_model_iter_next(
			    GTK_TREE_MODEL(app->playlist_store), &iter);
		}

		/* Determine format from extension */
		TotemPlParserType type = TOTEM_PL_PARSER_PLS;
		if (g_str_has_suffix(filename, ".m3u") ||
		    g_str_has_suffix(filename, ".m3u8"))
			type = TOTEM_PL_PARSER_M3U;
		else if (g_str_has_suffix(filename, ".xspf"))
			type = TOTEM_PL_PARSER_XSPF;

		GFile *dest = g_file_new_for_path(filename);
		TotemPlParser *parser = totem_pl_parser_new();
		GError *error = NULL;

		if (totem_pl_parser_save(parser, playlist, dest,
		    "Totem Playlist", type, &error)) {
			totem_statusbar_push(app, _("Playlist saved"));
		} else {
			gchar *msg = g_strdup_printf(_("Save failed: %s"),
			    error ? error->message : "unknown error");
			totem_statusbar_push(app, msg);
			g_free(msg);
			g_clear_error(&error);
		}

		g_object_unref(parser);
		g_object_unref(playlist);
		g_object_unref(dest);
		g_free(filename);
	}
	gtk_widget_destroy(dlg);
}

/* on_pl_up_clicked -- Move the selected playlist entry up by one position. */
static void
on_pl_up_clicked(GtkButton *btn, TotemApp *app)
{
	(void)btn;
	GtkTreeSelection *sel = gtk_tree_view_get_selection(
	    GTK_TREE_VIEW(app->playlist_view));
	GtkTreeIter iter;
	if (gtk_tree_selection_get_selected(sel, NULL, &iter)) {
		GtkTreeIter prev = iter;
		GtkTreePath *path = gtk_tree_model_get_path(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
		if (gtk_tree_path_prev(path)) {
			gtk_tree_model_get_iter(
			    GTK_TREE_MODEL(app->playlist_store), &prev, path);
			gtk_list_store_move_before(app->playlist_store,
			    &iter, &prev);
		}
		gtk_tree_path_free(path);
	}
}

/* on_pl_down_clicked -- Move the selected playlist entry down by one position. */
static void
on_pl_down_clicked(GtkButton *btn, TotemApp *app)
{
	(void)btn;
	GtkTreeSelection *sel = gtk_tree_view_get_selection(
	    GTK_TREE_VIEW(app->playlist_view));
	GtkTreeIter iter;
	if (gtk_tree_selection_get_selected(sel, NULL, &iter)) {
		GtkTreeIter next = iter;
		if (gtk_tree_model_iter_next(
		    GTK_TREE_MODEL(app->playlist_store), &next)) {
			gtk_list_store_move_after(app->playlist_store,
			    &iter, &next);
		}
	}
}

/*
 * on_drag_data_received -- Handle files dragged onto the window.  Parses
 * the URI list, adds each to the playlist, and starts playback if nothing
 * was already playing.
 */
static void
on_drag_data_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
    GtkSelectionData *data, guint info, guint time, TotemApp *app)
{
	(void)w; (void)x; (void)y; (void)info;

	if (gtk_selection_data_get_format(data) != 8 ||
	    gtk_selection_data_get_length(data) <= 0) {
		gtk_drag_finish(ctx, FALSE, FALSE, time);
		return;
	}

	const gchar *raw = (const gchar *)gtk_selection_data_get_data(data);
	gchar **uris = g_strsplit(raw, "\r\n", 0);
	gboolean added = FALSE;
	gchar *first_uri = NULL;

	for (int i = 0; uris[i] && uris[i][0]; i++) {
		gchar *uri = g_strstrip(uris[i]);
		if (uri[0] == '\0') continue;

		if (!first_uri)
			first_uri = g_strdup(uri);
		totem_open_uri_or_playlist(app, uri);
		added = TRUE;
	}
	g_strfreev(uris);

	if (added && first_uri) {
		/* Immediately play the first dropped file */
		GtkTreeIter iter;
		gboolean valid = gtk_tree_model_get_iter_first(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
		while (valid) {
			gchar *uri;
			gtk_tree_model_get(
			    GTK_TREE_MODEL(app->playlist_store), &iter,
			    COL_URI, &uri, -1);
			if (g_strcmp0(uri, first_uri) == 0) {
				totem_playlist_set_playing(app, &iter);
				totem_play_uri(app, uri);
				g_free(uri);
				break;
			}
			g_free(uri);
			valid = gtk_tree_model_iter_next(
			    GTK_TREE_MODEL(app->playlist_store), &iter);
		}
	}
	g_free(first_uri);

	gtk_drag_finish(ctx, added, FALSE, time);
}

/*
 * on_key_press -- Global keyboard shortcut handler.
 * Space=play/pause, Left/Right=seek, Alt+Left/Right=prev/next,
 * Up/Down=volume, F11=fullscreen, F9=sidebar, Escape=exit fullscreen,
 * Ctrl+Q=quit.
 */
static gboolean
on_key_press(GtkWidget *w, GdkEventKey *ev, TotemApp *app)
{
	(void)w;

	switch (ev->keyval) {
	case GDK_KEY_space:
		totem_play_pause(app);
		return TRUE;
	case GDK_KEY_Left:
		if (ev->state & GDK_MOD1_MASK)
			totem_prev(app);
		else
			totem_seek_relative(app, -15);
		return TRUE;
	case GDK_KEY_Right:
		if (ev->state & GDK_MOD1_MASK)
			totem_next(app);
		else
			totem_seek_relative(app, 15);
		return TRUE;
	case GDK_KEY_Up: {
		gdouble vol = gtk_scale_button_get_value(
		    GTK_SCALE_BUTTON(app->volume_button));
		gtk_scale_button_set_value(
		    GTK_SCALE_BUTTON(app->volume_button),
		    MIN(1.0, vol + 0.05));
		return TRUE;
	}
	case GDK_KEY_Down: {
		gdouble vol = gtk_scale_button_get_value(
		    GTK_SCALE_BUTTON(app->volume_button));
		gtk_scale_button_set_value(
		    GTK_SCALE_BUTTON(app->volume_button),
		    MAX(0.0, vol - 0.05));
		return TRUE;
	}
	case GDK_KEY_F11:
		totem_set_fullscreen(app, !app->fullscreen);
		return TRUE;
	case GDK_KEY_F9:
		totem_toggle_sidebar(app);
		return TRUE;
	case GDK_KEY_Escape:
		if (app->fullscreen) {
			totem_set_fullscreen(app, FALSE);
			return TRUE;
		}
		break;
	case GDK_KEY_q:
		if (ev->state & GDK_CONTROL_MASK) {
			totem_save_state(app);
			gtk_main_quit();
			return TRUE;
		}
		break;
	}
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Fullscreen                                                          */
/* ------------------------------------------------------------------ */

/*
 * totem_set_fullscreen -- Enter or leave fullscreen mode.  Hides or
 * shows the menu bar, controls, statusbar, and sidebar accordingly.
 */
static void
totem_set_fullscreen(TotemApp *app, gboolean fs)
{
	app->fullscreen = fs;
	if (fs) {
		gtk_widget_hide(app->menubar);
		gtk_widget_hide(app->controls_vbox);
		gtk_widget_hide(app->statusbar);
		if (app->sidebar_visible)
			gtk_widget_hide(gtk_widget_get_parent(app->sidebar_box));
		gtk_window_fullscreen(GTK_WINDOW(app->window));
	} else {
		gtk_window_unfullscreen(GTK_WINDOW(app->window));
		gtk_widget_show(app->menubar);
		gtk_widget_show(app->controls_vbox);
		gtk_widget_show(app->statusbar);
		if (app->sidebar_visible)
			gtk_widget_show(gtk_widget_get_parent(app->sidebar_box));
	}
}

/* totem_toggle_sidebar -- Show or hide the playlist sidebar. */
static void
totem_toggle_sidebar(TotemApp *app)
{
	app->sidebar_visible = !app->sidebar_visible;
	if (app->sidebar_visible)
		gtk_widget_show(gtk_widget_get_parent(app->sidebar_box));
	else
		gtk_widget_hide(gtk_widget_get_parent(app->sidebar_box));
}

/* ------------------------------------------------------------------ */
/* Menu callbacks                                                      */
/* ------------------------------------------------------------------ */

/* on_menu_open -- Movie > Open: delegates to the playlist Add button. */
static void
on_menu_open(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	on_pl_add_clicked(NULL, app);
}

/* on_menu_open_location -- Movie > Open Location: enter a URL to stream. */
static void
on_menu_open_location(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Open Location"),
	    GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
	    _("_Cancel"), GTK_RESPONSE_CANCEL,
	    _("_Open"), GTK_RESPONSE_ACCEPT, NULL);

	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(hbox), 12);

	GtkWidget *label = gtk_label_new(_("URL:"));
	GtkWidget *entry = gtk_entry_new();
	gtk_entry_set_width_chars(GTK_ENTRY(entry), 50);
	gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
	gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);

	gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(hbox), entry, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(content), hbox, TRUE, TRUE, 0);
	gtk_widget_show_all(hbox);

	if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
		const gchar *uri = gtk_entry_get_text(GTK_ENTRY(entry));
		if (uri && uri[0]) {
			gchar *basename = g_path_get_basename(uri);
			totem_playlist_add_uri(app, uri, basename);
			g_free(basename);

			/* Play it */
			GtkTreeIter iter;
			gint n = gtk_tree_model_iter_n_children(
			    GTK_TREE_MODEL(app->playlist_store), NULL);
			GtkTreePath *path = gtk_tree_path_new_from_indices(n - 1, -1);
			gtk_tree_model_get_iter(
			    GTK_TREE_MODEL(app->playlist_store), &iter, path);
			gtk_tree_path_free(path);
			totem_playlist_set_playing(app, &iter);
			totem_play_uri(app, uri);
		}
	}
	gtk_widget_destroy(dlg);
}

/* on_menu_play -- Movie > Play/Pause menu item. */
static void
on_menu_play(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	totem_play_pause(app);
}

/* on_menu_quit -- Movie > Quit: save state and exit. */
static void
on_menu_quit(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	totem_save_state(app);
	gtk_main_quit();
}

/* on_menu_clear_playlist -- Edit > Clear Playlist: stop and remove all entries. */
static void
on_menu_clear_playlist(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	totem_stop(app);
	gtk_list_store_clear(app->playlist_store);
	totem_update_sensitivity(app);
}

/* on_menu_repeat -- Edit > Repeat: toggle playlist repeat mode. */
static void
on_menu_repeat(GtkCheckMenuItem *item, TotemApp *app)
{
	app->repeat_mode = gtk_check_menu_item_get_active(item);
}

/* on_menu_shuffle -- Edit > Shuffle: toggle playlist shuffle mode. */
static void
on_menu_shuffle(GtkCheckMenuItem *item, TotemApp *app)
{
	app->shuffle_mode = gtk_check_menu_item_get_active(item);
}

/* on_menu_preferences -- Edit > Preferences: dialog with general settings. */
static void
on_menu_preferences(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Totem Preferences"),
	    GTK_WINDOW(app->window), GTK_DIALOG_MODAL,
	    _("_Close"), GTK_RESPONSE_CLOSE, NULL);
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	GtkWidget *notebook = gtk_notebook_new();
	gtk_container_set_border_width(GTK_CONTAINER(notebook), 6);

	/* General tab */
	GtkWidget *gen_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(gen_vbox), 12);

	GtkWidget *remember_cb = gtk_check_button_new_with_label(
	    _("Start playing files from last position"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(remember_cb),
	    app->remember_position);
	gtk_box_pack_start(GTK_BOX(gen_vbox), remember_cb, FALSE, FALSE, 0);

	gtk_notebook_append_page(GTK_NOTEBOOK(notebook), gen_vbox,
	    gtk_label_new(_("General")));

	gtk_box_pack_start(GTK_BOX(content), notebook, TRUE, TRUE, 0);
	gtk_widget_show_all(content);

	gtk_dialog_run(GTK_DIALOG(dlg));

	/* Read back values */
	app->remember_position = gtk_toggle_button_get_active(
	    GTK_TOGGLE_BUTTON(remember_cb));

	gtk_widget_destroy(dlg);
}

/* on_menu_fullscreen -- View > Fullscreen: toggle fullscreen mode. */
static void
on_menu_fullscreen(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	totem_set_fullscreen(app, !app->fullscreen);
}

/* on_menu_sidebar -- View > Sidebar: toggle sidebar visibility. */
static void
on_menu_sidebar(GtkCheckMenuItem *item, TotemApp *app)
{
	app->sidebar_visible = gtk_check_menu_item_get_active(item);
	if (app->sidebar_visible)
		gtk_widget_show(gtk_widget_get_parent(app->sidebar_box));
	else
		gtk_widget_hide(gtk_widget_get_parent(app->sidebar_box));
}

/*
 * on_menu_normalize_720p -- View > Normalize 720p: when enabled, resize
 * the video area to 1280x720 regardless of the source resolution.
 * Applies immediately if a video is currently playing.
 */
static void
on_menu_normalize_720p(GtkCheckMenuItem *item, TotemApp *app)
{
	app->normalize_720p = gtk_check_menu_item_get_active(item);

	/* Apply immediately if playing */
	GstState state;
	gst_element_get_state(app->player, &state, NULL, 0);
	if (state == GST_STATE_PLAYING || state == GST_STATE_PAUSED) {
		if (app->normalize_720p) {
			GtkAllocation alloc;
			gtk_widget_get_allocation(app->window, &alloc);
			GtkAllocation video_alloc;
			gtk_widget_get_allocation(app->video_overlay,
			    &video_alloc);
			gint extra_h = alloc.height - video_alloc.height;
			gint extra_w = alloc.width - video_alloc.width;
			gtk_window_resize(GTK_WINDOW(app->window),
			    1280 + extra_w, 720 + extra_h);
		} else {
			/* Re-trigger native resolution resize */
			GstMessage *msg = gst_message_new_state_changed(
			    GST_OBJECT(app->player),
			    GST_STATE_PAUSED, GST_STATE_PLAYING,
			    GST_STATE_VOID_PENDING);
			on_state_changed(NULL, msg, app);
			gst_message_unref(msg);
		}
	}
}

/*
 * on_menu_visualization -- View > Visualization submenu: switch the
 * playbin vis-plugin to the selected GStreamer visualization element.
 */
static void
on_menu_visualization(GtkRadioMenuItem *item, TotemApp *app)
{
	if (!gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(item)))
		return;
	const gchar *element_name = g_object_get_data(
	    G_OBJECT(item), "vis-element");
	if (!element_name)
		return;
	GstElement *vis = gst_element_factory_make(element_name, "vis");
	if (vis)
		g_object_set(app->player, "vis-plugin", vis, NULL);
}

/* on_menu_next -- Go > Next: advance to the next playlist entry. */
static void
on_menu_next(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	totem_next(app);
}

/* on_menu_prev -- Go > Previous: go to the previous playlist entry. */
static void
on_menu_prev(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	totem_prev(app);
}

/* on_menu_skip_forward -- Go > Skip Forward: seek +15 seconds. */
static void
on_menu_skip_forward(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	totem_seek_relative(app, 15);
}

/* on_menu_skip_backward -- Go > Skip Backward: seek -15 seconds. */
static void
on_menu_skip_backward(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	totem_seek_relative(app, -15);
}

/* on_menu_volume_up -- Sound > Volume Up: increase volume by 5%. */
static void
on_menu_volume_up(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	gdouble vol = gtk_scale_button_get_value(
	    GTK_SCALE_BUTTON(app->volume_button));
	gtk_scale_button_set_value(
	    GTK_SCALE_BUTTON(app->volume_button), MIN(1.0, vol + 0.05));
}

/* on_menu_volume_down -- Sound > Volume Down: decrease volume by 5%. */
static void
on_menu_volume_down(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	gdouble vol = gtk_scale_button_get_value(
	    GTK_SCALE_BUTTON(app->volume_button));
	gtk_scale_button_set_value(
	    GTK_SCALE_BUTTON(app->volume_button), MAX(0.0, vol - 0.05));
}

/* on_eq_band_changed -- Tone control slider moved: update equalizer band gain. */
static void
on_eq_band_changed(GtkRange *range, gpointer data)
{
	if (app->eq_disabled)
		return;
	GstElement *eq = GST_ELEMENT(data);
	gdouble val = gtk_range_get_value(range);
	const gchar *band = g_object_get_data(G_OBJECT(range), "band");
	g_object_set(eq, band, val, NULL);
}

/* on_preamp_changed -- Preamp slider moved: convert dB to linear and set volume. */
static void
on_preamp_changed(GtkRange *range, gpointer data)
{
	if (app->eq_disabled)
		return;
	GstElement *preamp = GST_ELEMENT(data);
	gdouble db = gtk_range_get_value(range);
	/* Convert dB to linear: volume = 10^(dB/20) */
	gdouble linear = pow(10.0, db / 20.0);
	g_object_set(preamp, "volume", linear, NULL);
}

/* on_eq_reset -- Reset all tone control sliders (preamp + EQ bands) to 0 dB. */
static void
on_eq_reset(GtkButton *btn, gpointer data)
{
	(void)btn;
	GtkWidget **sliders = data;
	/* sliders[0]=preamp, sliders[1..3]=bass/mid/treble */
	for (int i = 0; i < 4; i++)
		gtk_range_set_value(GTK_RANGE(sliders[i]), 0.0);
}

/*
 * on_eq_disable_toggled -- "Disable" checkbox: when active, bypass the
 * equalizer by setting preamp to unity and all bands to 0 dB, and grey
 * out the sliders.  When unchecked, restore slider values to the elements.
 */
static void
on_eq_disable_toggled(GtkToggleButton *toggle, gpointer data)
{
	GtkWidget **sliders = data;
	TotemApp *a = app;
	gboolean disabled = gtk_toggle_button_get_active(toggle);
	a->eq_disabled = disabled;

	const gchar *bands[] = { "band0", "band1", "band2" };

	if (disabled) {
		/* Bypass: set flat response */
		if (a->preamp)
			g_object_set(a->preamp, "volume", 1.0, NULL);
		for (int i = 0; i < 3; i++)
			g_object_set(a->equalizer, bands[i], 0.0, NULL);
	} else {
		/* Restore: push slider values back to elements */
		if (a->preamp) {
			gdouble db = gtk_range_get_value(
			    GTK_RANGE(sliders[0]));
			gdouble lin = pow(10.0, db / 20.0);
			g_object_set(a->preamp, "volume", lin, NULL);
		}
		for (int i = 0; i < 3; i++) {
			gdouble val = gtk_range_get_value(
			    GTK_RANGE(sliders[i + 1]));
			g_object_set(a->equalizer, bands[i], val, NULL);
		}
	}

	/* Grey out sliders when disabled */
	for (int i = 0; i < 4; i++)
		gtk_widget_set_sensitive(sliders[i], !disabled);
}

/*
 * on_menu_tone_control -- Sound > Tone Control: open a dialog with preamp
 * (-12 to +24 dB) and 3-band equalizer (bass/mid/treble, -24 to +12 dB).
 * Changes take effect in real time.  Settings are saved to totem.ini
 * when the dialog is closed.
 */
static void
on_menu_tone_control(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	if (!app->equalizer)
		return;

	GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Tone Control"),
	    GTK_WINDOW(app->window), GTK_DIALOG_DESTROY_WITH_PARENT,
	    _("_Close"), GTK_RESPONSE_CLOSE, NULL);
	gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);

	GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
	gtk_container_set_border_width(GTK_CONTAINER(content), 12);

	GtkWidget *grid = gtk_grid_new();
	gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

	/* sliders[0]=preamp, sliders[1..3]=bass/mid/treble */
	static GtkWidget *sliders[4];
	const gchar *bands[] = { "band0", "band1", "band2" };

	/* Preamp slider (row 0) */
	GtkWidget *pre_lbl = gtk_label_new(_("Preamp"));
	gtk_widget_set_halign(pre_lbl, GTK_ALIGN_END);
	gtk_grid_attach(GTK_GRID(grid), pre_lbl, 0, 0, 1, 1);

	sliders[0] = gtk_scale_new_with_range(
	    GTK_ORIENTATION_HORIZONTAL, -12.0, 24.0, 1.0);
	gtk_widget_set_size_request(sliders[0], 250, -1);
	gtk_scale_set_draw_value(GTK_SCALE(sliders[0]), TRUE);
	gtk_scale_set_value_pos(GTK_SCALE(sliders[0]), GTK_POS_RIGHT);
	gtk_scale_add_mark(GTK_SCALE(sliders[0]), 0.0,
	    GTK_POS_BOTTOM, "0");

	/* Get current preamp value: convert linear to dB */
	if (app->preamp) {
		gdouble lin = 1.0;
		g_object_get(app->preamp, "volume", &lin, NULL);
		gdouble db = 20.0 * log10(lin);
		gtk_range_set_value(GTK_RANGE(sliders[0]), db);
		g_signal_connect(sliders[0], "value-changed",
		    G_CALLBACK(on_preamp_changed), app->preamp);
	}
	gtk_grid_attach(GTK_GRID(grid), sliders[0], 1, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), gtk_label_new("dB"), 2, 0, 1, 1);

	/* Separator */
	gtk_grid_attach(GTK_GRID(grid),
	    gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), 0, 1, 3, 1);

	/* EQ band sliders (rows 2-4) */
	const gchar *labels[] = { _("Bass"), _("Mid"), _("Treble") };
	for (int i = 0; i < 3; i++) {
		gint row = i + 2;
		GtkWidget *lbl = gtk_label_new(labels[i]);
		gtk_widget_set_halign(lbl, GTK_ALIGN_END);
		gtk_grid_attach(GTK_GRID(grid), lbl, 0, row, 1, 1);

		sliders[i + 1] = gtk_scale_new_with_range(
		    GTK_ORIENTATION_HORIZONTAL, -24.0, 12.0, 1.0);
		gtk_widget_set_size_request(sliders[i + 1], 250, -1);
		gtk_scale_set_draw_value(GTK_SCALE(sliders[i + 1]), TRUE);
		gtk_scale_set_value_pos(GTK_SCALE(sliders[i + 1]),
		    GTK_POS_RIGHT);

		gdouble cur = 0.0;
		g_object_get(app->equalizer, bands[i], &cur, NULL);
		gtk_range_set_value(GTK_RANGE(sliders[i + 1]), cur);

		gtk_scale_add_mark(GTK_SCALE(sliders[i + 1]), 0.0,
		    GTK_POS_BOTTOM, "0");

		g_object_set_data(G_OBJECT(sliders[i + 1]), "band",
		    (gpointer)bands[i]);
		g_signal_connect(sliders[i + 1], "value-changed",
		    G_CALLBACK(on_eq_band_changed), app->equalizer);

		gtk_grid_attach(GTK_GRID(grid), sliders[i + 1],
		    1, row, 1, 1);
		gtk_grid_attach(GTK_GRID(grid), gtk_label_new("dB"),
		    2, row, 1, 1);
	}

	/* Button row: Reset + Disable checkbox */
	GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

	GtkWidget *reset_btn = gtk_button_new_with_label(_("Reset"));
	g_signal_connect(reset_btn, "clicked",
	    G_CALLBACK(on_eq_reset), sliders);
	gtk_box_pack_start(GTK_BOX(btn_box), reset_btn, FALSE, FALSE, 0);

	GtkWidget *disable_chk = gtk_check_button_new_with_label(
	    _("Disable"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(disable_chk),
	    app->eq_disabled);
	g_signal_connect(disable_chk, "toggled",
	    G_CALLBACK(on_eq_disable_toggled), sliders);
	gtk_box_pack_start(GTK_BOX(btn_box), disable_chk, FALSE, FALSE, 0);

	gtk_grid_attach(GTK_GRID(grid), btn_box, 1, 5, 1, 1);

	/* Grey out sliders if EQ is currently disabled */
	if (app->eq_disabled) {
		for (int i = 0; i < 4; i++)
			gtk_widget_set_sensitive(sliders[i], FALSE);
	}

	gtk_box_pack_start(GTK_BOX(content), grid, TRUE, TRUE, 0);
	gtk_widget_show_all(content);

	gtk_dialog_run(GTK_DIALOG(dlg));

	/* Save EQ + preamp state to config (save slider values, not
	 * bypassed element values, so settings survive disable/enable) */
	GKeyFile *kf = g_key_file_new();
	gchar *path = totem_config_path();
	g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
	for (int i = 0; i < 3; i++) {
		gdouble val = gtk_range_get_value(GTK_RANGE(sliders[i + 1]));
		g_key_file_set_double(kf, "Equalizer", bands[i], val);
	}
	if (app->preamp) {
		gdouble db = gtk_range_get_value(GTK_RANGE(sliders[0]));
		g_key_file_set_double(kf, "Equalizer", "preamp", db);
	}
	g_key_file_set_boolean(kf, "Equalizer", "disabled",
	    app->eq_disabled);
	gchar *data = g_key_file_to_data(kf, NULL, NULL);
	g_file_set_contents(path, data, -1, NULL);
	g_free(data);
	g_free(path);
	g_key_file_free(kf);

	gtk_widget_destroy(dlg);
}

/* on_menu_select_subtitle -- View > Subtitles: choose an external subtitle file. */
static void
on_menu_select_subtitle(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Select Subtitles"),
	    GTK_WINDOW(app->window), GTK_FILE_CHOOSER_ACTION_OPEN,
	    _("_Cancel"), GTK_RESPONSE_CANCEL,
	    _("_Open"), GTK_RESPONSE_ACCEPT, NULL);

	GtkFileFilter *filter = gtk_file_filter_new();
	gtk_file_filter_set_name(filter, _("Subtitle Files"));
	gtk_file_filter_add_pattern(filter, "*.srt");
	gtk_file_filter_add_pattern(filter, "*.sub");
	gtk_file_filter_add_pattern(filter, "*.ass");
	gtk_file_filter_add_pattern(filter, "*.ssa");
	gtk_file_filter_add_pattern(filter, "*.vtt");
	gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

	if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
		gchar *sub_uri = gtk_file_chooser_get_uri(
		    GTK_FILE_CHOOSER(dlg));
		if (sub_uri) {
			/*
			 * Setting suburi requires restarting playback.
			 * Save position, stop, set suburi, then resume.
			 */
			gint64 pos = 0;
			gst_element_query_position(app->player,
			    GST_FORMAT_TIME, &pos);
			gchar *media_uri = NULL;
			g_object_get(app->player, "uri", &media_uri, NULL);

			gst_element_set_state(app->player, GST_STATE_NULL);
			g_object_set(app->player,
			    "suburi", sub_uri, NULL);
			if (media_uri) {
				g_object_set(app->player,
				    "uri", media_uri, NULL);
				g_free(media_uri);
			}
			gst_element_set_state(app->player,
			    GST_STATE_PLAYING);

			/* Seek back to position */
			if (pos > 0) {
				gst_element_get_state(app->player,
				    NULL, NULL, 2 * GST_SECOND);
				gst_element_seek_simple(app->player,
				    GST_FORMAT_TIME,
				    GST_SEEK_FLAG_FLUSH |
				    GST_SEEK_FLAG_KEY_UNIT, pos);
			}

			/* Show video widget for subtitle overlay */
			if (app->video_area) {
				gtk_widget_show(app->video_area);
				gtk_widget_hide(app->background);
			}

			totem_statusbar_push(app, _("Subtitles loaded"));
			g_free(sub_uri);
		}
	}
	gtk_widget_destroy(dlg);
}

/* on_menu_about -- Help > About: show the about dialog. */
static void
on_menu_about(GtkMenuItem *item, TotemApp *app)
{
	(void)item;
	const gchar *authors[] = {
		"nervoso@k1.com.br",
		"Claude Code (Anthropic)",
		"",
		"Based on Totem 2.32 by Bastien Nocera",
		NULL
	};
	gtk_show_about_dialog(GTK_WINDOW(app->window),
	    "program-name", _("Movie Player"),
	    "version", "3.0",
	    "copyright", "\302\251 2026 nervoso@k1.com.br",
	    "comments", _("A movie player for the GNOME/MATE desktop\n"
	                "based on GStreamer 1.0"),
	    "authors", authors,
	    "license-type", GTK_LICENSE_GPL_2_0,
	    NULL);
}

/* ------------------------------------------------------------------ */
/* Menu bar construction                                               */
/* ------------------------------------------------------------------ */

/* make_menu_item -- Helper to create a menu item with a mnemonic label and callback. */
static GtkWidget *
make_menu_item(const gchar *label, GCallback cb, TotemApp *app)
{
	GtkWidget *item = gtk_menu_item_new_with_mnemonic(label);
	g_signal_connect(item, "activate", cb, app);
	return item;
}

static GtkWidget *
make_check_menu_item(const gchar *label, gboolean active,
    GCallback cb, TotemApp *app)
{
	GtkWidget *item = gtk_check_menu_item_new_with_mnemonic(label);
	gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), active);
	g_signal_connect(item, "toggled", cb, app);
	return item;
}

static GtkWidget *
build_menubar(TotemApp *app)
{
	GtkWidget *menubar = gtk_menu_bar_new();

	/* --- Movie menu --- */
	GtkWidget *movie_menu = gtk_menu_new();
	GtkWidget *movie_item = gtk_menu_item_new_with_mnemonic(_("_Movie"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(movie_item), movie_menu);

	gtk_menu_shell_append(GTK_MENU_SHELL(movie_menu),
	    make_menu_item(_("_Open..."), G_CALLBACK(on_menu_open), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(movie_menu),
	    make_menu_item(_("Open _Location..."),
	    G_CALLBACK(on_menu_open_location), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(movie_menu),
	    gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(movie_menu),
	    make_menu_item(_("Play / P_ause"), G_CALLBACK(on_menu_play), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(movie_menu),
	    gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(movie_menu),
	    make_menu_item(_("_Quit"), G_CALLBACK(on_menu_quit), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), movie_item);

	/* --- Edit menu --- */
	GtkWidget *edit_menu = gtk_menu_new();
	GtkWidget *edit_item = gtk_menu_item_new_with_mnemonic(_("_Edit"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(edit_item), edit_menu);

	gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu),
	    make_check_menu_item(_("_Repeat Mode"), app->repeat_mode,
	    G_CALLBACK(on_menu_repeat), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu),
	    make_check_menu_item(_("Shuff_le Mode"), app->shuffle_mode,
	    G_CALLBACK(on_menu_shuffle), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu),
	    gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu),
	    make_menu_item(_("_Clear Playlist"),
	    G_CALLBACK(on_menu_clear_playlist), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu),
	    gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(edit_menu),
	    make_menu_item(_("Prefere_nces"),
	    G_CALLBACK(on_menu_preferences), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), edit_item);

	/* --- View menu --- */
	GtkWidget *view_menu = gtk_menu_new();
	GtkWidget *view_item = gtk_menu_item_new_with_mnemonic(_("_View"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);

	gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
	    make_menu_item(_("_Fullscreen"), G_CALLBACK(on_menu_fullscreen), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
	    make_check_menu_item(_("_Normalize 720p"), app->normalize_720p,
	    G_CALLBACK(on_menu_normalize_720p), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
	    gtk_separator_menu_item_new());

	/* Subtitles submenu */
	GtkWidget *sub_menu = gtk_menu_new();
	GtkWidget *sub_item = gtk_menu_item_new_with_mnemonic(_("S_ubtitles"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(sub_item), sub_menu);
	gtk_menu_shell_append(GTK_MENU_SHELL(sub_menu),
	    make_menu_item(_("_Select Text Subtitles..."),
	    G_CALLBACK(on_menu_select_subtitle), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), sub_item);

	/* Visualization submenu */
	gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
	    gtk_separator_menu_item_new());

	GtkWidget *vis_menu = gtk_menu_new();
	GtkWidget *vis_item = gtk_menu_item_new_with_mnemonic(
	    _("_Visualization"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(vis_item), vis_menu);

	struct { const gchar *name; const gchar *element; } vis_list[] = {
		{ "GOOM",		"goom" },
		{ "GOOM 2K1",		"goom2k1" },
		{ N_("Monoscope"),	"monoscope" },
		{ N_("Spectrum"),	"spectrascope" },
		{ N_("Waveform"),	"wavescope" },
		{ N_("Stereo"),		"spacescope" },
		{ N_("Synaesthesia"),	"synaescope" },
	};
	GSList *vis_group = NULL;
	for (gsize i = 0; i < G_N_ELEMENTS(vis_list); i++) {
		/* Only show if element is available */
		GstElementFactory *f = gst_element_factory_find(
		    vis_list[i].element);
		if (!f)
			continue;
		gst_object_unref(f);

		GtkWidget *mi = gtk_radio_menu_item_new_with_label(
		    vis_group, _(vis_list[i].name));
		vis_group = gtk_radio_menu_item_get_group(
		    GTK_RADIO_MENU_ITEM(mi));
		g_object_set_data(G_OBJECT(mi), "vis-element",
		    (gpointer)vis_list[i].element);
		/* Default: GOOM selected */
		if (i == 0)
			gtk_check_menu_item_set_active(
			    GTK_CHECK_MENU_ITEM(mi), TRUE);
		g_signal_connect(mi, "toggled",
		    G_CALLBACK(on_menu_visualization), app);
		gtk_menu_shell_append(GTK_MENU_SHELL(vis_menu), mi);
	}
	gtk_menu_shell_append(GTK_MENU_SHELL(view_menu), vis_item);

	gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
	    gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
	    make_check_menu_item(_("S_idebar"), app->sidebar_visible,
	    G_CALLBACK(on_menu_sidebar), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_item);

	/* --- Go menu --- */
	GtkWidget *go_menu = gtk_menu_new();
	GtkWidget *go_item = gtk_menu_item_new_with_mnemonic(_("_Go"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(go_item), go_menu);

	gtk_menu_shell_append(GTK_MENU_SHELL(go_menu),
	    make_menu_item(_("_Next Chapter/Movie"),
	    G_CALLBACK(on_menu_next), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(go_menu),
	    make_menu_item(_("_Previous Chapter/Movie"),
	    G_CALLBACK(on_menu_prev), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(go_menu),
	    gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(go_menu),
	    make_menu_item(_("Skip _Forward"),
	    G_CALLBACK(on_menu_skip_forward), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(go_menu),
	    make_menu_item(_("Skip _Backwards"),
	    G_CALLBACK(on_menu_skip_backward), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), go_item);

	/* --- Sound menu --- */
	GtkWidget *sound_menu = gtk_menu_new();
	GtkWidget *sound_item = gtk_menu_item_new_with_mnemonic(_("_Sound"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(sound_item), sound_menu);

	gtk_menu_shell_append(GTK_MENU_SHELL(sound_menu),
	    make_menu_item(_("Volume _Up"),
	    G_CALLBACK(on_menu_volume_up), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(sound_menu),
	    make_menu_item(_("Volume _Down"),
	    G_CALLBACK(on_menu_volume_down), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(sound_menu),
	    gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(sound_menu),
	    make_menu_item(_("_Tone Control..."),
	    G_CALLBACK(on_menu_tone_control), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), sound_item);

	/* --- Help menu --- */
	GtkWidget *help_menu = gtk_menu_new();
	GtkWidget *help_item = gtk_menu_item_new_with_mnemonic(_("_Help"));
	gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);

	gtk_menu_shell_append(GTK_MENU_SHELL(help_menu),
	    make_menu_item(_("_About"), G_CALLBACK(on_menu_about), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_item);

	return menubar;
}

/* ------------------------------------------------------------------ */
/* Right-click context menu                                            */
/* ------------------------------------------------------------------ */

/*
 * on_video_button_press -- Handle right-click on the video area to show
 * a context menu with Play/Pause, Fullscreen, and Open options.
 */
static void
on_video_button_press(GtkWidget *w, GdkEventButton *ev, TotemApp *app)
{
	(void)w;
	if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
		return;

	GtkWidget *menu = gtk_menu_new();
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
	    make_menu_item(_("Play / P_ause"), G_CALLBACK(on_menu_play), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
	    make_menu_item(_("_Next"), G_CALLBACK(on_menu_next), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
	    make_menu_item(_("_Previous"), G_CALLBACK(on_menu_prev), app));
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
	    gtk_separator_menu_item_new());
	gtk_menu_shell_append(GTK_MENU_SHELL(menu),
	    make_menu_item(_("_Fullscreen"),
	    G_CALLBACK(on_menu_fullscreen), app));

	gtk_widget_show_all(menu);
	gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
}

/* ------------------------------------------------------------------ */
/* Playlist sidebar construction                                       */
/* ------------------------------------------------------------------ */

static GtkWidget *
build_playlist_sidebar(TotemApp *app)
{
	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 0);

	/* Header */
	GtkWidget *header = gtk_label_new(NULL);
	{
		gchar *pl_markup = g_markup_printf_escaped("<b>%s</b>",
		    _("Playlist"));
		gtk_label_set_markup(GTK_LABEL(header), pl_markup);
		g_free(pl_markup);
	}
	gtk_label_set_xalign(GTK_LABEL(header), 0.0);
	gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 4);

	/* Scrolled tree view */
	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
	    GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
	    GTK_SHADOW_IN);

	app->playlist_store = gtk_list_store_new(N_COLUMNS,
	    G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);

	app->playlist_view = gtk_tree_view_new_with_model(
	    GTK_TREE_MODEL(app->playlist_store));
	gtk_tree_view_set_reorderable(
	    GTK_TREE_VIEW(app->playlist_view), TRUE);

	/* Column: filename */
	GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
	g_object_set(renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
	GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(
	    _("Name"), renderer, "text", COL_NAME, NULL);
	gtk_tree_view_column_set_expand(col, TRUE);
	gtk_tree_view_column_set_clickable(col, TRUE);
	gtk_tree_view_column_set_sort_column_id(col, COL_NAME);
	gtk_tree_view_set_headers_visible(
	    GTK_TREE_VIEW(app->playlist_view), TRUE);
	gtk_tree_view_append_column(
	    GTK_TREE_VIEW(app->playlist_view), col);

	g_signal_connect(app->playlist_view, "row-activated",
	    G_CALLBACK(on_playlist_row_activated), app);

	gtk_container_add(GTK_CONTAINER(scroll), app->playlist_view);
	gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

	/* Buttons row */
	GtkWidget *btnbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_box_set_homogeneous(GTK_BOX(btnbox), TRUE);

	app->pl_add_btn = gtk_button_new_from_icon_name("list-add",
	    GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_button_set_relief(GTK_BUTTON(app->pl_add_btn), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(app->pl_add_btn, _("Add..."));
	g_signal_connect(app->pl_add_btn, "clicked",
	    G_CALLBACK(on_pl_add_clicked), app);

	app->pl_remove_btn = gtk_button_new_from_icon_name("list-remove",
	    GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_button_set_relief(GTK_BUTTON(app->pl_remove_btn), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(app->pl_remove_btn, _("Remove"));
	gtk_widget_set_sensitive(app->pl_remove_btn, FALSE);
	g_signal_connect(app->pl_remove_btn, "clicked",
	    G_CALLBACK(on_pl_remove_clicked), app);

	app->pl_save_btn = gtk_button_new_from_icon_name("document-save",
	    GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_button_set_relief(GTK_BUTTON(app->pl_save_btn), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(app->pl_save_btn, _("Save Playlist..."));
	gtk_widget_set_sensitive(app->pl_save_btn, FALSE);
	g_signal_connect(app->pl_save_btn, "clicked",
	    G_CALLBACK(on_pl_save_clicked), app);

	app->pl_up_btn = gtk_button_new_from_icon_name("go-up",
	    GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_button_set_relief(GTK_BUTTON(app->pl_up_btn), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(app->pl_up_btn, _("Move Up"));
	gtk_widget_set_sensitive(app->pl_up_btn, FALSE);
	g_signal_connect(app->pl_up_btn, "clicked",
	    G_CALLBACK(on_pl_up_clicked), app);

	app->pl_down_btn = gtk_button_new_from_icon_name("go-down",
	    GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_button_set_relief(GTK_BUTTON(app->pl_down_btn), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(app->pl_down_btn, _("Move Down"));
	gtk_widget_set_sensitive(app->pl_down_btn, FALSE);
	g_signal_connect(app->pl_down_btn, "clicked",
	    G_CALLBACK(on_pl_down_clicked), app);

	gtk_box_pack_start(GTK_BOX(btnbox), app->pl_add_btn, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(btnbox), app->pl_remove_btn, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(btnbox), app->pl_save_btn, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(btnbox), app->pl_up_btn, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(btnbox), app->pl_down_btn, TRUE, TRUE, 0);

	gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 0);

	return vbox;
}

/* ------------------------------------------------------------------ */
/* Main window construction                                            */
/* ------------------------------------------------------------------ */

static void
on_window_destroy(GtkWidget *w, TotemApp *app)
{
	(void)w;
	totem_save_state(app);
	if (app->player) {
		gst_element_set_state(app->player, GST_STATE_NULL);
	}
	gtk_main_quit();
}

/*
 * build_main_window -- Construct the entire GTK UI: menu bar, HPaned with
 * video overlay + controls on the left and playlist sidebar on the right,
 * statusbar at the bottom.  Sets up drag-and-drop and keyboard handlers.
 */
static void
build_main_window(TotemApp *app)
{
	/* Window */
	app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(app->window), _("Movie Player"));
	gtk_window_set_default_size(GTK_WINDOW(app->window), 650, 480);
	g_signal_connect(app->window, "destroy",
	    G_CALLBACK(on_window_destroy), app);
	g_signal_connect(app->window, "key-press-event",
	    G_CALLBACK(on_key_press), app);

	/* Drag & drop */
	static GtkTargetEntry targets[] = {
		{ "text/uri-list", 0, 0 }
	};
	gtk_drag_dest_set(app->window, GTK_DEST_DEFAULT_ALL,
	    targets, G_N_ELEMENTS(targets), GDK_ACTION_COPY);
	g_signal_connect(app->window, "drag-data-received",
	    G_CALLBACK(on_drag_data_received), app);

	/* Main vertical box */
	app->main_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_container_add(GTK_CONTAINER(app->window), app->main_vbox);

	/* Menu bar */
	app->menubar = build_menubar(app);
	gtk_box_pack_start(GTK_BOX(app->main_vbox), app->menubar,
	    FALSE, FALSE, 0);

	/* HPaned: video on left, playlist on right */
	app->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_box_pack_start(GTK_BOX(app->main_vbox), app->paned,
	    TRUE, TRUE, 0);

	/* Left pane: video + controls */
	GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

	/* Video area (overlay with background) */
	app->video_overlay = gtk_overlay_new();
	gtk_widget_set_hexpand(app->video_overlay, TRUE);
	gtk_widget_set_vexpand(app->video_overlay, TRUE);

	app->background = gtk_event_box_new();
	gtk_widget_set_hexpand(app->background, TRUE);
	gtk_widget_set_vexpand(app->background, TRUE);
	gtk_widget_set_name(app->background, "video_background");
	GtkCssProvider *css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(css,
	    "#video_background { background-color: black; }", -1, NULL);
	gtk_style_context_add_provider(
	    gtk_widget_get_style_context(app->background),
	    GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
	gtk_container_add(GTK_CONTAINER(app->video_overlay), app->background);

	/* Right-click on video area */
	gtk_widget_add_events(app->video_overlay, GDK_BUTTON_PRESS_MASK);
	g_signal_connect(app->video_overlay, "button-press-event",
	    G_CALLBACK(on_video_button_press), app);

	/* Etched frame around video area */
	GtkWidget *video_frame = gtk_frame_new(NULL);
	gtk_frame_set_shadow_type(GTK_FRAME(video_frame),
	    GTK_SHADOW_ETCHED_IN);
	gtk_widget_set_name(video_frame, "video_frame");
	GtkCssProvider *frame_css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(frame_css,
	    "#video_frame { background-color: #333333; }", -1, NULL);
	gtk_style_context_add_provider(
	    gtk_widget_get_style_context(video_frame),
	    GTK_STYLE_PROVIDER(frame_css),
	    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(frame_css);
	gtk_container_add(GTK_CONTAINER(video_frame), app->video_overlay);
	gtk_box_pack_start(GTK_BOX(left_vbox), video_frame, TRUE, TRUE, 0);

	/* Controls area */
	app->controls_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(app->controls_vbox), 6);

	/* Seek bar row */
	GtkWidget *seek_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	app->time_label = gtk_label_new("00:00 / 00:00");
	gtk_box_pack_start(GTK_BOX(seek_hbox), app->time_label,
	    FALSE, FALSE, 0);

	app->seek_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL,
	    0, 100, 1);
	gtk_scale_set_draw_value(GTK_SCALE(app->seek_scale), FALSE);
	gtk_widget_set_sensitive(app->seek_scale, FALSE);
	g_signal_connect(app->seek_scale, "button-press-event",
	    G_CALLBACK(on_seek_press), app);
	g_signal_connect(app->seek_scale, "button-release-event",
	    G_CALLBACK(on_seek_release), app);
	gtk_box_pack_start(GTK_BOX(seek_hbox), app->seek_scale,
	    TRUE, TRUE, 0);

	gtk_box_pack_start(GTK_BOX(app->controls_vbox), seek_hbox,
	    FALSE, FALSE, 0);

	/* Buttons row */
	GtkWidget *btn_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

	/* Sidebar toggle button (left side) */
	app->sidebar_button = gtk_toggle_button_new_with_label(_("Sidebar"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->sidebar_button),
	    app->sidebar_visible);
	gtk_widget_set_tooltip_text(app->sidebar_button, _("Show/Hide Sidebar (F9)"));
	g_signal_connect_swapped(app->sidebar_button, "toggled",
	    G_CALLBACK(totem_toggle_sidebar), app);
	gtk_box_pack_start(GTK_BOX(btn_hbox), app->sidebar_button,
	    FALSE, FALSE, 0);

	/* Spacer (left) */
	gtk_box_pack_start(GTK_BOX(btn_hbox),
	    gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), TRUE, TRUE, 0);

	/* Playback buttons (centered) */
	GtkWidget *prev_btn = gtk_button_new_from_icon_name(
	    "media-skip-backward", GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_widget_set_tooltip_text(prev_btn, _("Previous (Alt+Left)"));
	g_signal_connect_swapped(prev_btn, "clicked",
	    G_CALLBACK(totem_prev), app);
	gtk_box_pack_start(GTK_BOX(btn_hbox), prev_btn, FALSE, FALSE, 0);

	GtkWidget *stop_btn = gtk_button_new_from_icon_name(
	    "media-playback-stop", GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_widget_set_tooltip_text(stop_btn, _("Stop"));
	g_signal_connect_swapped(stop_btn, "clicked",
	    G_CALLBACK(totem_stop), app);
	gtk_box_pack_start(GTK_BOX(btn_hbox), stop_btn, FALSE, FALSE, 0);

	GtkWidget *play_btn = gtk_button_new_from_icon_name(
	    "media-playback-start", GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_widget_set_tooltip_text(play_btn, _("Play/Pause (Space)"));
	g_signal_connect_swapped(play_btn, "clicked",
	    G_CALLBACK(totem_play_pause), app);
	gtk_box_pack_start(GTK_BOX(btn_hbox), play_btn, FALSE, FALSE, 0);

	GtkWidget *next_btn = gtk_button_new_from_icon_name(
	    "media-skip-forward", GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_widget_set_tooltip_text(next_btn, _("Next (Alt+Right)"));
	g_signal_connect_swapped(next_btn, "clicked",
	    G_CALLBACK(totem_next), app);
	gtk_box_pack_start(GTK_BOX(btn_hbox), next_btn, FALSE, FALSE, 0);

	/* Spacer (right) */
	gtk_box_pack_start(GTK_BOX(btn_hbox),
	    gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0), TRUE, TRUE, 0);

	/* Volume button (right side) */
	app->volume_button = gtk_volume_button_new();
	gtk_scale_button_set_value(GTK_SCALE_BUTTON(app->volume_button), 0.5);
	g_signal_connect(app->volume_button, "value-changed",
	    G_CALLBACK(on_volume_changed), app);
	gtk_box_pack_end(GTK_BOX(btn_hbox), app->volume_button,
	    FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(app->controls_vbox), btn_hbox,
	    FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(left_vbox), app->controls_vbox,
	    FALSE, FALSE, 0);

	gtk_paned_pack1(GTK_PANED(app->paned), left_vbox, TRUE, FALSE);

	/* Right pane: playlist sidebar with etched frame */
	app->sidebar_box = build_playlist_sidebar(app);
	gtk_widget_set_size_request(app->sidebar_box, 200, -1);
	GtkWidget *pl_frame = gtk_frame_new(NULL);
	gtk_frame_set_shadow_type(GTK_FRAME(pl_frame), GTK_SHADOW_ETCHED_IN);
	gtk_container_add(GTK_CONTAINER(pl_frame), app->sidebar_box);
	gtk_paned_pack2(GTK_PANED(app->paned), pl_frame, FALSE, TRUE);

	/* Statusbar */
	app->statusbar = gtk_statusbar_new();
	app->statusbar_ctx = gtk_statusbar_get_context_id(
	    GTK_STATUSBAR(app->statusbar), "main");
	gtk_box_pack_start(GTK_BOX(app->main_vbox), app->statusbar,
	    FALSE, FALSE, 0);
}

/* ------------------------------------------------------------------ */
/* GStreamer setup                                                     */
/* ------------------------------------------------------------------ */

/*
 * setup_gstreamer -- Initialize the GStreamer pipeline: playbin for
 * decoding, gtksink for video output, preamp (volume element) and
 * equalizer-3bands for tone control.  Connects bus signals for EOS,
 * error, and state-changed.  Starts the position update timer.
 */
static gboolean
setup_gstreamer(TotemApp *app)
{
	app->player = gst_element_factory_make("playbin", "player");
	if (!app->player) {
		g_printerr("Error: could not create playbin element\n");
		return FALSE;
	}

	app->video_sink = gst_element_factory_make("gtksink", "videosink");
	if (!app->video_sink) {
		g_printerr("Error: could not create gtksink element\n");
		gst_object_unref(app->player);
		app->player = NULL;
		return FALSE;
	}

	g_object_set(app->video_sink, "sync", TRUE, NULL);
	g_object_set(app->player, "video-sink", app->video_sink, NULL);

	/* Audio chain: preamp (volume) -> equalizer (3-band) */
	app->preamp = gst_element_factory_make("volume", "preamp");
	app->equalizer = gst_element_factory_make("equalizer-3bands", "eq");
	if (app->preamp && app->equalizer) {
		GstElement *audio_bin = gst_bin_new("audio_filter_bin");
		gst_bin_add_many(GST_BIN(audio_bin),
		    app->preamp, app->equalizer, NULL);
		gst_element_link(app->preamp, app->equalizer);

		/* Ghost pads so playbin can link to the bin */
		GstPad *sink_pad = gst_element_get_static_pad(
		    app->preamp, "sink");
		gst_element_add_pad(audio_bin,
		    gst_ghost_pad_new("sink", sink_pad));
		gst_object_unref(sink_pad);

		GstPad *src_pad = gst_element_get_static_pad(
		    app->equalizer, "src");
		gst_element_add_pad(audio_bin,
		    gst_ghost_pad_new("src", src_pad));
		gst_object_unref(src_pad);

		g_object_set(app->player, "audio-filter", audio_bin, NULL);
	} else if (app->equalizer) {
		g_object_set(app->player, "audio-filter",
		    app->equalizer, NULL);
	}

	/* GOOM visualization for audio-only files */
	GstElement *goom = gst_element_factory_make("goom", "vis");
	if (goom) {
		g_object_set(app->player, "vis-plugin", goom, NULL);
		/* Enable vis flag (0x08) in playbin flags */
		gint flags;
		g_object_get(app->player, "flags", &flags, NULL);
		flags |= 0x08;	/* GST_PLAY_FLAG_VIS */
		g_object_set(app->player, "flags", flags, NULL);
	}

	/* Get the video widget from gtksink */
	g_object_get(app->video_sink, "widget", &app->video_area, NULL);
	if (!app->video_area) {
		g_printerr("Error: could not get widget from gtksink\n");
		gst_object_unref(app->player);
		app->player = NULL;
		return FALSE;
	}

	gtk_widget_set_hexpand(app->video_area, TRUE);
	gtk_widget_set_vexpand(app->video_area, TRUE);
	gtk_overlay_add_overlay(GTK_OVERLAY(app->video_overlay),
	    app->video_area);
	gtk_widget_hide(app->video_area);

	/* Set initial volume */
	gdouble vol = gtk_scale_button_get_value(
	    GTK_SCALE_BUTTON(app->volume_button));
	g_object_set(app->player, "volume", vol, NULL);

	/* Bus signals */
	GstBus *bus = gst_element_get_bus(app->player);
	gst_bus_add_signal_watch(bus);
	g_signal_connect(bus, "message::eos", G_CALLBACK(on_eos), app);
	g_signal_connect(bus, "message::error", G_CALLBACK(on_error), app);
	g_signal_connect(bus, "message::state-changed",
	    G_CALLBACK(on_state_changed), app);
	gst_object_unref(bus);

	/* Position update timer */
	app->position_timer = g_timeout_add(250, update_position, app);

	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Signal handler                                                      */
/* ------------------------------------------------------------------ */

static void
/* signal_handler -- SIGINT/SIGTERM handler: save state and exit cleanly. */
signal_handler(int sig)
{
	(void)sig;
	if (app) {
		totem_save_state(app);
		gtk_main_quit();
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");
	bindtextdomain(TOTEM_DOMAIN, TOTEM_LOCALEDIR);
	textdomain(TOTEM_DOMAIN);

	gtk_init(&argc, &argv);
	gst_init(&argc, &argv);

	/* Splash screen */
	GtkWidget *splash = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_decorated(GTK_WINDOW(splash), FALSE);
	gtk_window_set_position(GTK_WINDOW(splash), GTK_WIN_POS_CENTER);
	gtk_window_set_resizable(GTK_WINDOW(splash), FALSE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(splash), TRUE);
	gtk_widget_set_name(splash, "splash");

	GtkCssProvider *splash_css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(splash_css,
	    "#splash { background-color: black; }", -1, NULL);
	gtk_style_context_add_provider(
	    gtk_widget_get_style_context(splash),
	    GTK_STYLE_PROVIDER(splash_css),
	    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(splash_css);

	GtkWidget *splash_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
	gtk_container_set_border_width(GTK_CONTAINER(splash_box), 40);

	GtkWidget *splash_icon = gtk_image_new_from_icon_name("totem",
	    GTK_ICON_SIZE_DIALOG);
	gtk_image_set_pixel_size(GTK_IMAGE(splash_icon), 64);
	gtk_box_pack_start(GTK_BOX(splash_box), splash_icon,
	    FALSE, FALSE, 0);

	GtkWidget *splash_label = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(splash_label),
	    "<span foreground='white' size='x-large' weight='bold'>"
	    "Totem</span>");
	gtk_box_pack_start(GTK_BOX(splash_box), splash_label,
	    FALSE, FALSE, 0);

	GtkWidget *splash_ver = gtk_label_new(NULL);
	gchar *splash_ver_markup = g_markup_printf_escaped(
	    "<span foreground='gray' size='small'>%s</span>",
	    _("Movie Player"));
	gtk_label_set_markup(GTK_LABEL(splash_ver), splash_ver_markup);
	g_free(splash_ver_markup);
	gtk_box_pack_start(GTK_BOX(splash_box), splash_ver,
	    FALSE, FALSE, 0);

	gtk_container_add(GTK_CONTAINER(splash), splash_box);
	gtk_widget_show_all(splash);

	/* Process events so splash is painted */
	while (gtk_events_pending())
		gtk_main_iteration();

	GTimer *splash_timer = g_timer_new();

	app = g_new0(TotemApp, 1);
	app->sidebar_visible = TRUE;
	app->remember_position = TRUE;

	/* Build UI */
	build_main_window(app);

	/* Load saved state (before GStreamer, so volume is set) */
	totem_load_state(app);

	/* Setup GStreamer */
	if (!setup_gstreamer(app)) {
		GtkWidget *dlg = gtk_message_dialog_new(NULL,
		    GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
		    _("Failed to initialize GStreamer.\n"
		    "Please install gst-plugins1-base and gst-plugins1-good."));
		gtk_dialog_run(GTK_DIALOG(dlg));
		gtk_widget_destroy(dlg);
		g_free(app);
		return 1;
	}

	/* Load saved equalizer and preamp settings */
	if (app->equalizer) {
		GKeyFile *kf = g_key_file_new();
		gchar *path = totem_config_path();
		if (g_key_file_load_from_file(kf, path,
		    G_KEY_FILE_NONE, NULL)) {
			const gchar *bands[] = { "band0", "band1", "band2" };
			for (int i = 0; i < 3; i++) {
				if (g_key_file_has_key(kf, "Equalizer",
				    bands[i], NULL)) {
					gdouble val = g_key_file_get_double(kf,
					    "Equalizer", bands[i], NULL);
					g_object_set(app->equalizer,
					    bands[i], val, NULL);
				}
			}
			if (app->preamp && g_key_file_has_key(kf,
			    "Equalizer", "preamp", NULL)) {
				gdouble db = g_key_file_get_double(kf,
				    "Equalizer", "preamp", NULL);
				gdouble lin = pow(10.0, db / 20.0);
				g_object_set(app->preamp,
				    "volume", lin, NULL);
			}
			/* Load EQ disabled state */
			if (g_key_file_has_key(kf, "Equalizer",
			    "disabled", NULL)) {
				app->eq_disabled = g_key_file_get_boolean(
				    kf, "Equalizer", "disabled", NULL);
				if (app->eq_disabled) {
					/* Bypass: flat response */
					if (app->preamp)
						g_object_set(app->preamp,
						    "volume", 1.0, NULL);
					for (int i = 0; i < 3; i++)
						g_object_set(app->equalizer,
						    bands[i], 0.0, NULL);
				}
			}
		}
		g_key_file_free(kf);
		g_free(path);
	}

	/* Apply sidebar visibility from loaded state */
	if (!app->sidebar_visible)
		gtk_widget_hide(gtk_widget_get_parent(app->sidebar_box));

	/* Handle command-line files -- add to playlist first */
	gchar *cmdline_uri = NULL;
	if (argc > 1) {
		for (int i = 1; i < argc; i++) {
			gchar *uri;
			if (g_str_has_prefix(argv[i], "file://") ||
			    g_str_has_prefix(argv[i], "http://") ||
			    g_str_has_prefix(argv[i], "https://") ||
			    g_str_has_prefix(argv[i], "rtsp://")) {
				uri = g_strdup(argv[i]);
			} else {
				gchar *abs = g_canonicalize_filename(argv[i],
				    g_get_current_dir());
				uri = g_strdup_printf("file://%s", abs);
				g_free(abs);
			}
			totem_open_uri_or_playlist(app, uri);
			/* Remember first command-line URI */
			if (i == 1)
				cmdline_uri = g_strdup(uri);
			g_free(uri);
		}
	}

	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* Wait for splash to complete 2 seconds */
	while (g_timer_elapsed(splash_timer, NULL) < 2.0) {
		while (gtk_events_pending())
			gtk_main_iteration();
		g_usleep(50000);
	}
	gtk_widget_destroy(splash);
	g_timer_destroy(splash_timer);

	gtk_widget_show_all(app->window);

	/* Restore sidebar visibility after show_all */
	if (!app->sidebar_visible)
		gtk_widget_hide(gtk_widget_get_parent(app->sidebar_box));

	/* Hide video widget until something is playing */
	if (app->video_area)
		gtk_widget_hide(app->video_area);

	/* Play first command-line file after window is fully shown */
	if (cmdline_uri) {
		GtkTreeIter iter;
		gboolean valid = gtk_tree_model_get_iter_first(
		    GTK_TREE_MODEL(app->playlist_store), &iter);
		while (valid) {
			gchar *uri;
			gtk_tree_model_get(
			    GTK_TREE_MODEL(app->playlist_store), &iter,
			    COL_URI, &uri, -1);
			if (g_strcmp0(uri, cmdline_uri) == 0) {
				totem_playlist_set_playing(app, &iter);
				totem_play_uri(app, uri);
				g_free(uri);
				break;
			}
			g_free(uri);
			valid = gtk_tree_model_iter_next(
			    GTK_TREE_MODEL(app->playlist_store), &iter);
		}
		g_free(cmdline_uri);
	}

	totem_statusbar_push(app, _("Ready"));

	gtk_main();

	/* Cleanup */
	if (app->player) {
		gst_element_set_state(app->player, GST_STATE_NULL);
		gst_object_unref(app->player);
	}
	g_free(app);
	app = NULL;

	return 0;
}
