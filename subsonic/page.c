#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "subsonic.h"

#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/streaming/radio.h"

#include "lvgl/lvgl.h"

// The Subsonic app: a browser for the user's own library on a Navidrome, Gonic,
// Airsonic or Ampache server, and a player for it.
//
// Three screens and no more:
//
//   home   the account, and the five ways in: artists, newest, random,
//          playlists, search;
//   list   one list at a time, with a stack of its own -- artists -> albums ->
//          tracks, playlists -> tracks -- so that going back walks the levels
//          the user walked instead of leaving the addon;
//   entry  one line of text and a keyboard, reused for the server, the user
//          name, the password and the search.
//
// Nothing that talks to the network runs on the UI thread: a request is a
// detached worker that fills the shared arrays and posts itself back, and a
// reply that arrives after the user has moved on is dropped by its generation.

#define LIST_MAX 100
#define LEVELS_MAX 6
#define TEXT_MAX 256

typedef enum {
	JOB_NONE = 0,
	JOB_ARTISTS,
	JOB_ALBUMS, // of an artist
	JOB_TRACKS, // of an album
	JOB_PLAYLISTS,
	JOB_PLAYLIST_TRACKS,
	JOB_NEWEST,
	JOB_RANDOM,
	JOB_SEARCH,
	JOB_LOGIN,
} job_kind_t;

// One level of the list screen: what was asked for and what to call it.
typedef struct {
	job_kind_t kind;
	char id[SUBSONIC_ID_MAX];
	char title[SUBSONIC_TITLE_MAX];
} level_t;

static gui_config_t *config;
static lv_obj_t *home_screen;
static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *entry_screen;
static lv_obj_t *entry_field;
static lv_obj_t *entry_title;
static keyboard_t *entry_keyboard;

static level_t levels[LEVELS_MAX];
static int level_count;

// The reply being read. `data_lock` covers the arrays and what describes them:
// one worker fills them at a time, and the UI thread reads them only after the
// post arrives. `gen_lock` is separate so that asking for something new does
// not wait for the request it replaces.
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t gen_lock = PTHREAD_MUTEX_INITIALIZER;
static int request_generation;
static job_kind_t result_kind;
static int result_count;
static char result_error[192];

static subsonic_artist_t artists[LIST_MAX];
static subsonic_album_t albums[LIST_MAX];
static subsonic_track_t tracks[LIST_MAX];
static subsonic_playlist_t playlists[LIST_MAX];

// What the sign-in flow has typed so far.
static char pending_server[SUBSONIC_SERVER_MAX];
static char pending_user[SUBSONIC_NAME_MAX];
static char pending_password[SUBSONIC_NAME_MAX];

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

// A page that scrolls, unlike settingsrow_page(): a hundred artists are taller
// than any panel.
static lv_obj_t *scrolling_page(lv_obj_t *screen, const char *title) {
	lv_obj_add_style(screen, &theme_style_screen, 0);
	settingsrow_title(screen, config, title);

	lv_obj_t *container = lv_obj_create(screen);
	lv_obj_set_size(container, lv_pct(100), config->screen_height - settingsrow_content_top(config));
	lv_obj_align(container, LV_ALIGN_TOP_LEFT, 0, settingsrow_content_top(config));
	lv_obj_set_style_bg_opa(container, 0, 0);
	lv_obj_set_style_border_width(container, 0, 0);
	lv_obj_set_style_radius(container, 0, 0);
	lv_obj_set_style_pad_hor(container, config->padding, 0);
	lv_obj_set_style_pad_ver(container, config->padding, 0);
	lv_obj_set_style_pad_gap(container, SC(8), 0);
	lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(container, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scroll_dir(container, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(container, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_set_scroll_elastic(container, false);
	switcher_attach_back_gesture(container);
	return container;
}

// A row with a name and a dim value, from the shell's own rows: a list of
// artists then reads like every other list in this player.
//
// The name is clipped by hand. settingsrow_add() has a rule for stepping a long
// name down to fit beside its value, but the rule runs from layout events, and
// a row whose value is filled in here -- after the row was made, before the
// layout that gives it a width -- keeps the full width for its name and draws it
// over the value. A server's album titles are long enough to find that every
// time, so the addon keeps them short itself.
#define ROW_NAME_CHARS 18

static const char *clip(char *out, size_t size, const char *text) {
	size_t len = text ? strlen(text) : 0;
	if (len < ROW_NAME_CHARS) {
		lv_snprintf(out, size, "%s", text ? text : "");
		return out;
	}
	// One ellipsis, three bytes of UTF-8: a byte slice would cut a Cyrillic
	// letter in half and the label would show a replacement box.
	lv_snprintf(out, size, "%.*s…", ROW_NAME_CHARS - 3, text);
	return out;
}

static void row(lv_obj_t *parent, const char *name, const char *value, lv_event_cb_t cb, void *user) {
	char short_name[SUBSONIC_TITLE_MAX];
	lv_obj_t *value_label = NULL;
	settingsrow_add(parent, clip(short_name, sizeof(short_name), name), &value_label, cb, user);
	if (value_label && value) {
		lv_label_set_text(value_label, value);
	}
}

// ---------------------------------------------------------------------------
// Playing a track
//
// The track becomes a radio station, because that is the player's one public
// way to hand a URL to the audio path: radio.c owns the transport, the codec
// detection and the now-playing line, and an addon is not going to grow a
// second one. What the user sees is a station called "artist - title".
// ---------------------------------------------------------------------------

static void play_track(const subsonic_track_t *track) {
	radio_station_t station;
	memset(&station, 0, sizeof(station));

	if (track->artist[0] && track->title[0]) {
		lv_snprintf(station.name, sizeof(station.name), "%s - %s", track->artist, track->title);
	} else {
		lv_snprintf(station.name, sizeof(station.name), "%s", track->title);
	}
	// No uuid: this station is not in radio-browser's directory, and radio.c
	// reports every uuid it is given back to it as a listen. An empty one is
	// how a station from anywhere else says "not yours".
	station.bitrate = track->bit_rate;
	subsonic_stream_url(track->id, station.url, sizeof(station.url));

	const char *problem = radio_station_problem(&station);
	if (problem) {
		gui_notify_popup(problem);
		return;
	}
	if (!radio_play(&station)) {
		gui_notify_popup("the player could not start the stream");
	}
}

// ---------------------------------------------------------------------------
// The list screen
// ---------------------------------------------------------------------------

static void list_show_level(void);

static void list_show_message(const char *text) {
	lv_obj_clean(list_container);
	lv_obj_t *label = lv_label_create(list_container);
	lv_label_set_text(label, text);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

static void heading_set(const char *text) {
	lv_obj_t *heading = settingsrow_page_title(list_screen);
	if (heading) {
		lv_label_set_text(heading, text && text[0] ? text : "Subsonic");
	}
}

static void start_job(job_kind_t kind, const char *id, const char *query);

static void row_artist_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	const subsonic_artist_t *a = &artists[index];
	if (level_count >= LEVELS_MAX) {
		return;
	}
	level_t *l = &levels[level_count++];
	l->kind = JOB_ALBUMS;
	lv_snprintf(l->id, sizeof(l->id), "%s", a->id);
	lv_snprintf(l->title, sizeof(l->title), "%s", a->name);
	heading_set(l->title);
	start_job(JOB_ALBUMS, a->id, NULL);
}

static void row_album_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	const subsonic_album_t *a = &albums[index];
	if (level_count >= LEVELS_MAX) {
		return;
	}
	level_t *l = &levels[level_count++];
	l->kind = JOB_TRACKS;
	lv_snprintf(l->id, sizeof(l->id), "%s", a->id);
	lv_snprintf(l->title, sizeof(l->title), "%s", a->title);
	heading_set(l->title);
	start_job(JOB_TRACKS, a->id, NULL);
}

static void row_track_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	play_track(&tracks[index]);
}

static void row_playlist_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	const subsonic_playlist_t *p = &playlists[index];
	if (level_count >= LEVELS_MAX) {
		return;
	}
	level_t *l = &levels[level_count++];
	l->kind = JOB_PLAYLIST_TRACKS;
	lv_snprintf(l->id, sizeof(l->id), "%s", p->id);
	lv_snprintf(l->title, sizeof(l->title), "%s", p->name);
	heading_set(l->title);
	start_job(JOB_PLAYLIST_TRACKS, p->id, NULL);
}

static void list_show_level(void) {
	if (result_count < 0) {
		list_show_message(result_error[0] ? result_error : "the server did not answer");
		return;
	}
	if (result_count == 0) {
		list_show_message("nothing here");
		return;
	}

	lv_obj_clean(list_container);
	char sub[TEXT_MAX];
	for (int i = 0; i < result_count; i++) {
		switch (result_kind) {
		case JOB_ARTISTS:
			lv_snprintf(sub, sizeof(sub), "%d", artists[i].album_count);
			row(list_container, artists[i].name, artists[i].album_count ? sub : "", row_artist_cb,
				(void *)(intptr_t)i);
			break;
		case JOB_ALBUMS:
		case JOB_NEWEST:
			if (albums[i].year) {
				lv_snprintf(sub, sizeof(sub), "%d", albums[i].year);
			} else if (albums[i].song_count) {
				lv_snprintf(sub, sizeof(sub), "%d", albums[i].song_count);
			} else {
				sub[0] = '\0';
			}
			row(list_container, albums[i].title, sub, row_album_cb, (void *)(intptr_t)i);
			break;
		case JOB_TRACKS:
		case JOB_PLAYLIST_TRACKS:
		case JOB_RANDOM:
		case JOB_SEARCH:
			// The duration, not the artist: a name and a four-character value fit
			// side by side, and the artist is already in the now-playing line.
			lv_snprintf(sub, sizeof(sub), "%d:%02d", tracks[i].duration / 60, tracks[i].duration % 60);
			row(list_container, tracks[i].title, sub, row_track_cb, (void *)(intptr_t)i);
			break;
		case JOB_PLAYLISTS:
			lv_snprintf(sub, sizeof(sub), "%d", playlists[i].song_count);
			row(list_container, playlists[i].name, playlists[i].song_count ? sub : "", row_playlist_cb,
				(void *)(intptr_t)i);
			break;
		default:
			break;
		}
	}
}

static void home_build(void);

static void job_finished(void *user) {
	int generation = (int)(intptr_t)user;
	pthread_mutex_lock(&gen_lock);
	bool stale = generation != request_generation;
	int kind = (int)result_kind;
	int count = result_count;
	pthread_mutex_unlock(&gen_lock);
	if (stale) {
		return; // the user has asked for something else since
	}

	if (kind == JOB_LOGIN) {
		memset(pending_password, 0, sizeof(pending_password));
		home_build();
		switch_screen(home_screen);
		gui_notify_popup(count >= 0 ? "signed in" : (result_error[0] ? result_error : "sign-in failed"));
		return;
	}
	list_show_level();
}

// ---------------------------------------------------------------------------
// The worker
// ---------------------------------------------------------------------------

typedef struct {
	job_kind_t kind;
	char id[SUBSONIC_ID_MAX];
	char query[SUBSONIC_TITLE_MAX];
	int generation;
} job_t;

static void *worker(void *arg) {
	job_t *job = arg;
	int count = -1;
	const char *error = NULL;

	// Where a request is when the page says "loading…" and stays there: one
	// line before and one after, with how long it took.
	struct timespec began, ended;
	clock_gettime(CLOCK_MONOTONIC, &began);
	fprintf(stderr, "subsonic: job %d started\n", (int)job->kind);

	// One worker fills the arrays at a time; the ones that follow wait their
	// turn rather than interleaving half of a list with half of another.
	pthread_mutex_lock(&data_lock);
	switch (job->kind) {
	case JOB_ARTISTS:
		count = subsonic_artists(0, artists, LIST_MAX);
		break;
	case JOB_ALBUMS:
		count = subsonic_artist_albums(job->id, 0, albums, LIST_MAX);
		break;
	case JOB_TRACKS:
		count = subsonic_album_tracks(job->id, 0, tracks, LIST_MAX);
		break;
	case JOB_PLAYLISTS:
		count = subsonic_playlists(0, playlists, LIST_MAX);
		break;
	case JOB_PLAYLIST_TRACKS:
		count = subsonic_playlist_tracks(job->id, 0, tracks, LIST_MAX);
		break;
	case JOB_NEWEST:
		count = subsonic_album_list("newest", 0, albums, LIST_MAX);
		break;
	case JOB_RANDOM:
		count = subsonic_random_tracks(tracks, LIST_MAX);
		break;
	case JOB_SEARCH:
		count = subsonic_search_tracks(job->query, 0, tracks, LIST_MAX);
		break;
	case JOB_LOGIN:
		count = subsonic_login(pending_server, pending_user, pending_password) ? 0 : -1;
		break;
	default:
		break;
	}
	clock_gettime(CLOCK_MONOTONIC, &ended);
	fprintf(stderr, "subsonic: job %d done, %d items, %ld ms%s%s\n", (int)job->kind, count,
			(long)((ended.tv_sec - began.tv_sec) * 1000 + (ended.tv_nsec - began.tv_nsec) / 1000000),
			count < 0 ? ", " : "", count < 0 ? subsonic_last_error() : "");

	if (count < 0) {
		error = subsonic_last_error();
	}
	result_kind = job->kind;
	result_count = count;
	lv_snprintf(result_error, sizeof(result_error), "%s", error ? error : "");
	pthread_mutex_unlock(&data_lock);

	gui_post(job_finished, (void *)(intptr_t)job->generation);
	free(job);
	return NULL;
}

static void start_job(job_kind_t kind, const char *id, const char *query) {
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		return;
	}
	job->kind = kind;
	if (id) {
		lv_snprintf(job->id, sizeof(job->id), "%s", id);
	}
	if (query) {
		lv_snprintf(job->query, sizeof(job->query), "%s", query);
	}
	pthread_mutex_lock(&gen_lock);
	job->generation = ++request_generation;
	pthread_mutex_unlock(&gen_lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, job) != 0) {
		free(job);
		list_show_message("could not start a worker thread");
		return;
	}
	pthread_detach(thread);
}

static void job_start_visual(job_kind_t kind, const char *id, const char *query) {
	list_show_message("loading…");
	start_job(kind, id, query);
}

// Going back inside the list pops one level; at the top there is nothing left
// to pop and the chevron leaves the addon as it always did.
static bool list_back_guard(void) {
	if (level_count > 1) {
		level_count--;
		// The level below was overwritten by the level above, so it has to be
		// asked for again.
		const level_t *level = &levels[level_count - 1];
		heading_set(level->title);
		job_start_visual(level->kind, level->id[0] ? level->id : NULL, NULL);
		return true;
	}
	return false;
}

// Opens the list screen on a fresh stack of one level.
static void list_open(job_kind_t kind, const char *title, const char *query) {
	level_count = 0;
	level_t *l = &levels[level_count++];
	l->kind = kind;
	l->id[0] = '\0';
	lv_snprintf(l->title, sizeof(l->title), "%s", title ? title : "");

	heading_set(l->title);
	switch_screen(list_screen);
	job_start_visual(kind, NULL, query);
}

// ---------------------------------------------------------------------------
// Text entry
// ---------------------------------------------------------------------------

static void (*entry_done)(const char *text);

static void entry_ok_cb(lv_event_t *e) {
	(void)e;
	char text[TEXT_MAX];
	lv_snprintf(text, sizeof(text), "%s", lv_textarea_get_text(entry_field));
	void (*done)(const char *) = entry_done;
	entry_done = NULL;
	// The keyboard is *not* hidden here: a step that rejects what was typed
	// (an empty server, say) leaves its screen up, and a keyboard that vanished
	// under the message would have to be tapped back into existence. The steps
	// that leave the screen hide it themselves.
	if (done) {
		done(text);
	}
}

static void entry_open(const char *title, const char *initial, bool password, void (*done)(const char *)) {
	lv_label_set_text(entry_title, title);
	lv_textarea_set_text(entry_field, initial ? initial : "");
	lv_textarea_set_password_mode(entry_field, password);
	keyboard_set_field(entry_keyboard, entry_field);
	keyboard_set_visible(entry_keyboard, true);
	entry_done = done;
	switch_screen(entry_screen);
}

// ---------------------------------------------------------------------------
// The sign-in flow: server, then user, then password, then the server's answer
//
// Three entry screens in a row, each one's callback the next step. The name of
// each callback is the field it just read -- ask_user() is handed the server --
// which is the one thing that is easy to get wrong here and silently: a step
// that stores what it was given under the wrong name passes the next test with
// an empty field.
// ---------------------------------------------------------------------------

// Spaces around a server or a name are a phone keyboard's, not the user's.
static void trim_copy(char *dst, size_t size, const char *src) {
	if (!src) {
		src = "";
	}
	while (*src == ' ' || *src == '\t') {
		src++;
	}
	lv_snprintf(dst, size, "%s", src);
	size_t len = strlen(dst);
	while (len > 0 && (dst[len - 1] == ' ' || dst[len - 1] == '\t')) {
		dst[--len] = '\0';
	}
}

// The password arrived: everything is in hand, ask the server.
static void sign_in_finish(const char *password) {
	trim_copy(pending_password, sizeof(pending_password), password);
	if (!pending_server[0] || !pending_user[0]) {
		gui_notify_popup("server and user name are needed");
		return;
	}
	keyboard_set_visible(entry_keyboard, false);
	switch_screen(home_screen);
	start_job(JOB_LOGIN, NULL, NULL);
}

// The user name arrived: ask for the password.
static void ask_password(const char *user) {
	trim_copy(pending_user, sizeof(pending_user), user);
	if (!pending_user[0]) {
		gui_notify_popup("the user name is empty");
		return; // the entry screen stays up
	}
	entry_open("Password", "", true, sign_in_finish);
}

// The server arrived: ask for the user name.
static void ask_user(const char *server) {
	trim_copy(pending_server, sizeof(pending_server), server);
	if (!pending_server[0]) {
		gui_notify_popup("the server is empty");
		return;
	}
	entry_open("User name", pending_user, false, ask_password);
}

static void ask_server(void) { entry_open("Server", pending_server, false, ask_user); }

// ---------------------------------------------------------------------------
// Home
// ---------------------------------------------------------------------------

static void account_cb(lv_event_t *e) {
	(void)e;
	ask_server();
}

static void artists_cb(lv_event_t *e) {
	(void)e;
	list_open(JOB_ARTISTS, "Artists", NULL);
}

static void newest_cb(lv_event_t *e) {
	(void)e;
	list_open(JOB_NEWEST, "Newest", NULL);
}

static void random_cb(lv_event_t *e) {
	(void)e;
	list_open(JOB_RANDOM, "Random", NULL);
}

static void playlists_cb(lv_event_t *e) {
	(void)e;
	list_open(JOB_PLAYLISTS, "Playlists", NULL);
}

static void search_done(const char *query) {
	if (!query || !query[0]) {
		keyboard_set_visible(entry_keyboard, false);
		switch_screen(home_screen);
		return;
	}
	keyboard_set_visible(entry_keyboard, false);
	level_count = 0;
	level_t *l = &levels[level_count++];
	l->kind = JOB_SEARCH;
	l->id[0] = '\0';
	lv_snprintf(l->title, sizeof(l->title), "Search: %.60s", query);
	heading_set(l->title);
	switch_screen(list_screen);
	job_start_visual(JOB_SEARCH, NULL, query);
}

static void search_cb(lv_event_t *e) {
	(void)e;
	entry_open("Search", "", false, search_done);
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	subsonic_logout();
	home_build();
	gui_notify_popup("signed out");
}

static void home_build(void) {
	static lv_obj_t *page;
	if (!page) {
		home_screen = lv_obj_create(NULL);
		page = scrolling_page(home_screen, "Subsonic");
	}
	lv_obj_clean(page);

	if (subsonic_logged_in()) {
		char value[TEXT_MAX];
		subsonic_display_name(value, sizeof(value));
		row(page, "Account", value, account_cb, NULL);
		row(page, "Artists", "", artists_cb, NULL);
		row(page, "Newest", "", newest_cb, NULL);
		row(page, "Random", "", random_cb, NULL);
		row(page, "Playlists", "", playlists_cb, NULL);
		row(page, "Search", "", search_cb, NULL);
		row(page, "Sign out", "", sign_out_cb, NULL);
	} else {
		row(page, "Sign in", "server, user, password", account_cb, NULL);
	}
}

// ---------------------------------------------------------------------------
// The addon
// ---------------------------------------------------------------------------

static lv_obj_t *subsonic_init(const sonix_addon_host_t *host) {
	config = host->cfg;
	subsonic_load(host->dir);

	// What is already known fills the form: signing in again after a token
	// expired is then one field, not three.
	subsonic_server(pending_server, sizeof(pending_server));
	subsonic_user_name(pending_user, sizeof(pending_user));

	home_build();

	list_screen = lv_obj_create(NULL);
	list_container = scrolling_page(list_screen, "Subsonic");
	switcher_set_back_guard(list_screen, list_back_guard);

	entry_screen = lv_obj_create(NULL);
	lv_obj_add_style(entry_screen, &theme_style_screen, 0);
	entry_title = settingsrow_title(entry_screen, config, "");
	int top = settingsrow_content_top(config);
	entry_field = lv_textarea_create(entry_screen);
	lv_textarea_set_one_line(entry_field, true);
	lv_obj_set_size(entry_field, config->screen_width - 2 * config->padding, SC(62));
	lv_obj_set_scrollbar_mode(entry_field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(entry_field, LV_ALIGN_TOP_LEFT, config->padding, top + SC(10));
	lv_obj_add_style(entry_field, &theme_style_card, 0);
	lv_obj_set_style_radius(entry_field, SC(12), 0);
	lv_obj_set_style_border_width(entry_field, 0, 0);
	lv_obj_set_style_shadow_width(entry_field, 0, 0);
	lv_obj_set_style_pad_all(entry_field, SC(14), 0);
	lv_obj_set_style_text_font(entry_field, &font_ui_24, 0);
	keyboard_style_caret(entry_field);
	entry_keyboard =
		keyboard_create(entry_screen, config->screen_width, SC(316), entry_field, NULL, "ok", entry_ok_cb, NULL);
	keyboard_set_visible(entry_keyboard, false);

	return home_screen;
}

static const sonix_addon_t subsonic = {
	.api = SONIX_ADDON_API,
	.title = "Subsonic",
	.init = subsonic_init,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &subsonic : NULL;
}
