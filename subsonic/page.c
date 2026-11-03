#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "subsonic.h"

#include "common/addonfs.h"
#include "common/listrow.h"

#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/confirm.h"
#include "src/gui/shell/gui.h"
#include "src/gui/shell/icons.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/popover.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/gui/shell/toast.h"
#include "src/system/playback/device_state.h"

#include "src/gui/streaming/listart.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/subsoniccache.h"
#include "src/system/streaming/subsonicplay.h"
#include "src/system/streaming/subsonicservice.h"

#include "lvgl/lvgl.h"

// The Subsonic app: a browser for the user's own library on a Navidrome, Gonic,
// Airsonic or Ampache server, and a player for it.
//
// Four screens and no more:
//
//   home    the server in use, the account, and the five ways in: artists,
//           newest, random, playlists, search;
//   list    one list at a time, with a stack of its own -- artists -> albums ->
//           tracks, playlists -> tracks -- so that going back walks the levels
//           the user walked instead of leaving the addon;
//   entry   one line of text and a keyboard, reused for the server, the user
//           name, the password and the search;
//   servers the accounts this card holds: tap one to switch to it, add another,
//           forget the one in use.
//
// Nothing that talks to the network runs on the UI thread: a request is a
// detached worker that fills the shared arrays and posts itself back, and a
// reply that arrives after the user has moved on is dropped by its generation.
// Switching servers is the same rule taken one step further -- the ids, covers
// and quality of the server that was there before have to go, and a request
// still on the wire must not land in the new one's rows.
//
// An addon can also be unloaded -- the card leaving, the .so on it replaced by a
// newer one -- and the detached workers are what that has to survive: stop(), at
// the end of this file, stops new ones starting, waits (bounded) for the ones
// still out there, clears what they were filling, and answers whether the
// library may be unmapped.

#define LIST_MAX 100
#define LEVELS_MAX 6
#define TEXT_MAX 256

// How long stop() gives a request to come back. A request is an HTTP call with a
// twenty-second timeout of its own and no way to be interrupted, and stop() runs
// on the GUI thread of a player whose card may be leaving the slot: two seconds
// is long enough for the ordinary case -- a reply already on the wire -- and
// short enough that it does not look like the freeze this unloading exists to
// prevent.
#define STOP_WAIT_MS 2000

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
static char self_id[ADDON_ID_MAX]; // the host's name for us, for sonix_addon_own_screen()
static lv_obj_t *home_screen;
static lv_obj_t *home_page;
static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *servers_screen;
static lv_obj_t *servers_page;
static lv_obj_t *entry_screen;
static lv_obj_t *entry_field;
static lv_obj_t *entry_title;
static keyboard_t *entry_keyboard;

// The page, in listrow's sense (common/listrow.h): the address of a static here
// is a stable key that is this page's and no other page's, which is how the
// downloader knows whose rows it is serving.
static const char art_owner;

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

// Whether a stop() is under way. The shell unloads an addon when the card leaves
// or when the .so on it changed, and after stop() returns the library may be
// unmapped: from that moment no request may start and no reply may be posted,
// because both of those are code in this library.
static bool stopping;

// The workers that are alive. One detached thread per request, so there is
// nothing to join and nothing to keep: what stop() waits for is this number.
// `worker_gone` is signalled by the last statement of a worker.
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_gone = PTHREAD_COND_INITIALIZER;
static int workers_live;

// What the sign-in flow has typed so far, and which server's file it is
// writing to: the active one when the form was opened from the account row, ""
// when it was "Add server".
static char pending_server[SUBSONIC_SERVER_MAX];
static char pending_user[SUBSONIC_NAME_MAX];
static char pending_password[SUBSONIC_NAME_MAX];

// One textarea and one keyboard serve four questions: the three of a sign-in
// (server, user, password) and the search. Which one an OK belongs to is a step
// and not a function pointer, because the steps after it have to know which
// server's file they are filling in -- and a saved callback would be a pointer
// into this library that stop() has to remember to clear.
typedef enum {
	ENTRY_NONE = 0,
	ENTRY_SIGNIN,
	ENTRY_SEARCH,
} entry_kind_t;

typedef enum {
	STEP_SERVER = 0,
	STEP_USER,
	STEP_PASSWORD,
} entry_step_t;

static entry_kind_t entry_kind;
static entry_step_t entry_step;
static char entry_slug[SUBSONIC_SLUG_MAX];

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
	// A press that drags leftwards opens the player sheet over this page, the
	// same gesture every other list in the player takes: the sheet is parked
	// off the right edge and the drag pulls it in. Attached to the screen as
	// well, so the row area and the margins behave alike.
	player_sheet_attach_drag(screen, true);
	player_sheet_attach_drag(container, true);
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
// A tap starts the whole visible list, not one stream: the player's queue
// becomes these tracks (subsonicplay_list), and the tapped one plays as soon as
// its file is on the card. That is what makes shuffle, repeat, the queue page,
// "play next" and the star work -- none of them means anything for a live
// stream, which is what this addon used to hand radio_play().
//
// The row menu is the player's popover, opened from the ellipsis on a track
// row. The actions are the queue's (play, play next, add to queue) and the
// account's (star), and each runs after the popover has closed.
// ---------------------------------------------------------------------------

// The visible rows, copied out under the lock: a worker fills the arrays for
// the next list, and an action that starts the queue must not read them while
// that is happening. NULL when what is on screen is not a list of tracks.
static subsonic_track_t *tracks_snapshot(int *count) {
	*count = 0;
	pthread_mutex_lock(&data_lock);
	int n = result_count;
	bool tracks_shown = result_kind == JOB_TRACKS || result_kind == JOB_PLAYLIST_TRACKS || result_kind == JOB_RANDOM ||
						result_kind == JOB_SEARCH;
	subsonic_track_t *copy = NULL;
	if (tracks_shown && n > 0) {
		copy = malloc((size_t)n * sizeof(*copy));
		if (copy) {
			memcpy(copy, tracks, (size_t)n * sizeof(*copy));
			*count = n;
		}
	}
	pthread_mutex_unlock(&data_lock);
	return copy;
}

// What the queue module tells the page: playback started (the sheet has a new
// track and a new cover), or why it did not. On the UI thread.
static void playback_event(bool started, const char *text) {
	if (started) {
		player_refresh_now_playing();
		return;
	}
	toast_error(text && text[0] ? text : "the track could not be played");
}

static void list_play_from(int index) {
	int count = 0;
	subsonic_track_t *list = tracks_snapshot(&count);
	if (!list) {
		return;
	}
	if (!subsonicplay_list(list, count, index)) {
		toast_error("the queue could not be started");
	}
	free(list);
}

static void row_track_cb(lv_event_t *e) {
	// A gesture that ends over a row is not a tap on it: events bubble.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	list_play_from((int)(intptr_t)lv_event_get_user_data(e));
}

// The track the open popover belongs to, copied when it opens: the item's
// action runs after the popover has closed, and the rows may be rebuilt while
// it is up.
static subsonic_track_t menu_track;
static int menu_index;

static void menu_play(void *user) {
	(void)user;
	list_play_from(menu_index);
}

static void menu_play_next(void *user) {
	(void)user;
	if (!subsonicplay_play_next(&menu_track)) {
		toast_error("the queue refused the addition");
	}
}

static void menu_enqueue(void *user) {
	(void)user;
	if (!subsonicplay_enqueue(&menu_track)) {
		toast_error("the queue refused the addition");
	}
}

// The write's answer arrives on the favourites worker, never on the UI thread.
static void star_failed_async(void *user) {
	char *why = user;
	toast_error(why && why[0] ? why : "the server refused the change");
	free(why);
}

static void star_done(bool ok, const char *error, void *user) {
	(void)user;
	// The mirror has already flipped, so the star on the sheet is right either
	// way; only a refusal has anything to say.
	char *why = ok ? NULL : strdup(error && error[0] ? error : "");
	if (why) {
		if (!gui_post(star_failed_async, why)) {
			free(why);
		}
	}
}

static void menu_star(void *user) {
	bool want = user != NULL;
	subsonicservice_star(menu_track.id, want, star_done, NULL);
}

static void row_menu_cb(lv_event_t *e) {
	lv_event_stop_bubbling(e); // the tap belongs to the menu, not to the row
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	int count = 0;
	subsonic_track_t *list = tracks_snapshot(&count);
	if (!list || index < 0 || index >= count) {
		free(list);
		return;
	}
	menu_track = list[index];
	menu_index = index;
	free(list);

	bool starred = subsonicservice_is_starred(menu_track.id);
	popover_item_t items[4];
	memset(items, 0, sizeof(items));
	items[0].label = "Play";
	items[0].action = menu_play;
	items[1].label = "Play next";
	items[1].action = menu_play_next;
	items[2].label = "Add to queue";
	items[2].action = menu_enqueue;
	items[3].label = starred ? "Unstar" : "Star";
	items[3].action = menu_star;
	items[3].user = starred ? NULL : (void *)1;
	popover_show(lv_event_get_current_target(e), items, 4);
}

// ---------------------------------------------------------------------------
// The list screen
// ---------------------------------------------------------------------------

static void list_show_level(void);

static void list_show_message(const char *text) {
	// Nothing of the list above is left: the message replaces every row, so the
	// covers it had are given back rather than held for rows that no longer
	// exist.
	listrow_reset((void *)&art_owner);
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
	// A gesture that ends over a row is not a tap on it: the rows let presses
	// through so the back swipe and the sheet's drag can start on one.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
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
	// As row_artist_cb: a drag that ends here is not a tap on the album.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
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

// ---------------------------------------------------------------------------
// A list row
//
// The shell's settingsrow_add() is a name and a value and has nowhere to put a
// picture, so the lists build their rows here: a cover on the left, the name
// after it, the list's own value (an album's year, a track's length) dim on the
// right. The row keeps the shell's card -- same height, radius, padding and
// press style -- so the list still reads as part of this player.
//
// The cover is listart's, through common/listrow.h: this file says which
// picture belongs to which row and which rows are on screen, and nothing else.
// listrow keeps the rest -- the request, the slot, the decoded picture, and the
// delete handler that gives a slot back when a row goes.
// ---------------------------------------------------------------------------

#define ROW_H SC(100)		// the shell's own row height
#define ROW_THUMB SC(64)	// leaves the card's own margin above and below
// What the cover is asked for from the server. The decode is the object's
// width (listrow.h), this is only the size of the file the server sends: at
// twice the thumbnail it stays sharp without moving a large picture over wifi.
#define ROW_COVER_PX 100

static lv_obj_t *list_row(lv_obj_t *parent, const char *name, const char *value, const char *cover_id, int index,
						  lv_event_cb_t cb, bool menu) {
	lv_obj_t *r = lv_btn_create(parent);
	lv_obj_set_size(r, lv_pct(100), ROW_H);
	lv_obj_add_style(r, &theme_style_card, 0);
	lv_obj_add_style(r, &theme_style_card_pressed, LV_STATE_PRESSED);
	lv_obj_set_style_radius(r, SC(12), 0);
	lv_obj_set_style_border_width(r, 0, 0);
	lv_obj_set_style_shadow_width(r, 0, 0);
	lv_obj_set_style_pad_hor(r, SC(16), 0);
	lv_obj_set_style_pad_ver(r, 0, 0);
	lv_obj_set_style_pad_column(r, SC(14), 0);
	lv_obj_set_scrollable(r, false);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	// Gestures (the back swipe, and pulling the player sheet in) start on rows
	// too, and a button keeps the press to itself: without this neither works
	// over the list.
	lv_obj_set_event_bubble(r, true);
	lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

	// The cover, or the quiet rounded box it has until there is one: an artist
	// with no coverArt, a server that will not answer for this id, or the
	// settings' list_art turned off. The picture, when it comes, is set on this
	// object by listrow and needs no help from here.
	lv_obj_t *thumb = lv_image_create(r);
	lv_obj_set_size(thumb, ROW_THUMB, ROW_THUMB);
	lv_obj_set_style_bg_color(thumb, theme()->surface_pressed, 0);
	lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(thumb, SC(8), 0);
	lv_obj_set_style_clip_corner(thumb, true, 0);
	lv_obj_set_event_bubble(thumb, true);
	if (cover_id && cover_id[0]) {
		char url[SUBSONIC_URL_MAX];
		subsonic_cover_url(cover_id, ROW_COVER_PX, url, sizeof(url));
		// Building a URL reads the saved session and touches no network: it is
		// the request listart makes later that leaves the player.
		listrow_bind((void *)&art_owner, thumb, index, url);
	}

	// The name ellipsizes itself: unlike the shell row, whose name is laid out
	// from a resize event that arrives after the value is filled in (see
	// clip()), this label is a flex child and gets its width from the layout
	// that places it, so LV_LABEL_LONG_DOT can do the shortening and the row
	// does not have to keep titles short by hand.
	lv_obj_t *label = lv_label_create(r);
	lv_label_set_text(label, name ? name : "");
	lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
	lv_obj_set_height(label, lv_font_get_line_height(&font_ui_24));
	lv_obj_set_flex_grow(label, 1);
	lv_obj_add_style(label, &theme_style_text, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);

	if (value && value[0]) {
		lv_obj_t *right = lv_label_create(r);
		lv_label_set_text(right, value);
		lv_obj_add_style(right, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(right, &font_ui_22, 0);
	}

	if (menu) {
		// A plain object with the tap on it: no button style to undo, and the
		// handler stops the event (lv_event_stop_bubbling) so it never reaches
		// the row underneath, where a tap is "play".
		lv_obj_t *more = lv_obj_create(r);
		lv_obj_set_size(more, SC(56), SC(56));
		lv_obj_set_style_bg_opa(more, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(more, 0, 0);
		lv_obj_set_style_shadow_width(more, 0, 0);
		lv_obj_set_style_pad_all(more, 0, 0);
		lv_obj_add_event_cb(more, row_menu_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);
		lv_obj_t *dots = lv_image_create(more);
		lv_image_set_src(dots, &icon_ellipsis_vertical);
		lv_obj_add_style(dots, &theme_style_icon, 0);
		lv_obj_set_style_image_opa(dots, LV_OPA_70, 0);
		lv_obj_center(dots);
	}

	return r;
}

// The rows on screen, as a range of indices for listrow_visible(). Read from
// the row objects themselves: the container's flex layout is the only thing
// that knows where a row ended up, and a pitch guessed here would drift from
// the panel it is drawn on.
static void list_visible_range(int *first, int *last) {
	*first = 0;
	*last = -1;
	if (!list_container) {
		return;
	}
	int count = (int)lv_obj_get_child_count(list_container);
	if (count <= 0) {
		return;
	}
	int32_t top = lv_obj_get_scroll_y(list_container);
	int32_t height = lv_obj_get_height(list_container);
	int f = count, l = -1;
	for (int i = 0; i < count; i++) {
		lv_obj_t *r = lv_obj_get_child(list_container, i);
		if (!r) {
			continue;
		}
		int32_t y = lv_obj_get_y(r);
		int32_t h = lv_obj_get_height(r);
		if (y + h < top) {
			continue;
		}
		if (y > top + height) {
			break;
		}
		if (i < f) {
			f = i;
		}
		l = i;
	}
	if (l >= 0) {
		*first = f;
		*last = l;
	}
}

// The one call the downloader needs: which of this page's rows are worth a
// picture right now. The rows just outside it keep theirs (listrow.h), so
// scrolling back a screen finds the covers already there.
static void list_visible_update(void) {
	int first, last;
	list_visible_range(&first, &last);
	listrow_visible((void *)&art_owner, first, last);
}

static void list_scrolled_cb(lv_event_t *e) {
	(void)e;
	list_visible_update();
}

// A track row is the same card with the length in place of the value and an
// ellipsis on its right: the tap plays the list from this row, the ellipsis
// opens the menu.
static void track_row(lv_obj_t *parent, int index) {
	const subsonic_track_t *track = &tracks[index];
	char sub[TEXT_MAX];
	lv_snprintf(sub, sizeof(sub), "%d:%02d", track->duration / 60, track->duration % 60);
	list_row(parent, track->title, sub, track->cover_id, index, row_track_cb, true);
}

static void row_playlist_cb(lv_event_t *e) {
	// As row_artist_cb: a drag that ends here is not a tap on the playlist.
	if (switcher_back_drag_active() || player_sheet_drag_active()) {
		return;
	}
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

	// Every row goes, and every cover this page asked for goes with it: the
	// entries listrow keeps are keyed by row object, and a rebuilt list is a
	// different set of rows even when the names repeat.
	listrow_reset((void *)&art_owner);
	lv_obj_clean(list_container);
	char sub[TEXT_MAX];
	for (int i = 0; i < result_count; i++) {
		switch (result_kind) {
		case JOB_ARTISTS:
			lv_snprintf(sub, sizeof(sub), "%d", artists[i].album_count);
			list_row(list_container, artists[i].name, artists[i].album_count ? sub : "", artists[i].cover_id, i,
					 row_artist_cb, false);
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
			list_row(list_container, albums[i].title, sub, albums[i].cover_id, i, row_album_cb, false);
			break;
		case JOB_TRACKS:
		case JOB_PLAYLIST_TRACKS:
		case JOB_RANDOM:
		case JOB_SEARCH:
			track_row(list_container, i);
			break;
		case JOB_PLAYLISTS:
			lv_snprintf(sub, sizeof(sub), "%d", playlists[i].song_count);
			list_row(list_container, playlists[i].name, playlists[i].song_count ? sub : "", playlists[i].cover_id, i,
					 row_playlist_cb, false);
			break;
		default:
			break;
		}
	}

	// The rows have places only after the layout has run them through the flex
	// column, and the covers are asked for by place: this is what turns the
	// list that was just built into the first range of rows on screen.
	lv_obj_update_layout(list_container);
	list_visible_update();
}

static void home_build(void);

// Everything this page holds about the library it last showed. Switching
// servers is a switch of ids, covers and quality: the rows of the old one must
// not come back, and a request still on the wire must not land in the new one's
// rows. The generation bump is what makes an in-flight answer unwritable --
// its job_finished() finds the number it was given no longer current.
static void page_reset(void) {
	pthread_mutex_lock(&data_lock);
	level_count = 0;
	result_kind = JOB_NONE;
	result_count = 0;
	result_error[0] = '\0';
	memset(artists, 0, sizeof(artists));
	memset(albums, 0, sizeof(albums));
	memset(tracks, 0, sizeof(tracks));
	memset(playlists, 0, sizeof(playlists));
	pthread_mutex_unlock(&data_lock);

	// The covers were the last server's as much as the ids were: the rows may
	// still be on the list screen, and what they show must not outlive the
	// account it came from. The rows go with their own reset on the next
	// rebuild; this one only takes the pictures and the slots back.
	listrow_reset((void *)&art_owner);

	pthread_mutex_lock(&gen_lock);
	request_generation++;
	pthread_mutex_unlock(&gen_lock);

	// The account is (or may be) a different one: the queue's ids belong to the
	// server that was left, and the star's mirror to its account. Signing in is
	// also a page_reset(), which is what fetches the new account's stars.
	subsonicplay_forget();
	if (subsonic_logged_in()) {
		subsonicservice_refresh_stars();
	}

	entry_kind = ENTRY_NONE;
	entry_slug[0] = '\0';
	memset(pending_password, 0, sizeof(pending_password));
}

// The sign-in form starts from the active server (an expired token is then one
// field to retype), and a new server's form starts empty.
static void seed_from_host(void) {
	subsonic_server(pending_server, sizeof(pending_server));
	subsonic_user_name(pending_user, sizeof(pending_user));
	memset(pending_password, 0, sizeof(pending_password));
}

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
		// The account that answered is now the active server, and it may not be
		// the one whose rows this page was showing.
		char failure[192];
		pthread_mutex_lock(&data_lock);
		lv_snprintf(failure, sizeof(failure), "%s", result_error);
		pthread_mutex_unlock(&data_lock);

		page_reset();
		seed_from_host();
		home_build();
		switch_screen(home_screen);
		char note[192];
		subsonic_hosts_note(note, sizeof(note));
		if (count >= 0 && note[0]) {
			gui_notify_popup(note);
		} else {
			gui_notify_popup(count >= 0 ? "signed in" : (failure[0] ? failure : "sign-in failed"));
		}
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
	// JOB_LOGIN only: what was typed, and the file it goes into. Copied out of
	// the page's fields here, on the UI thread, so the worker reads nothing the
	// user can still be typing into.
	char server[SUBSONIC_SERVER_MAX];
	char user[SUBSONIC_NAME_MAX];
	char password[SUBSONIC_NAME_MAX];
	char slug[SUBSONIC_SLUG_MAX];
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
		// Slug NULL is "a server this card does not have yet"; the addon names
		// its file after the address and activates it.
		count = subsonic_login_host(job->slug[0] ? job->slug : NULL, job->server, job->user, job->password) ? 0 : -1;
		memset(job->password, 0, sizeof(job->password));
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

	// A reply for a page that is being taken away is dropped rather than posted:
	// job_finished() is a function in this library, and unmapping it is what the
	// stop() that set `stopping` is counting towards. (The loader drops whatever
	// is already in the queue by address range; this is the half that stops it
	// being queued at all.)
	pthread_mutex_lock(&worker_lock);
	bool wanted = !stopping;
	pthread_mutex_unlock(&worker_lock);

	if (wanted) {
		gui_post(job_finished, (void *)(intptr_t)job->generation);
	}
	free(job);

	// The last statement, and the one stop() is counting: after this no line of
	// this library runs on this thread again.
	pthread_mutex_lock(&worker_lock);
	workers_live--;
	pthread_cond_broadcast(&worker_gone);
	pthread_mutex_unlock(&worker_lock);
	return NULL;
}

// Whether a job may be started at all, and the count that keeps stop() honest
// about the thread it is about to create.
static bool job_launch(job_t *job) {
	// Counted here and not in the worker's first statement: between
	// pthread_create() and that statement stop() would read nought, return, and
	// leave the thread to run this library's code as its first act. start_job()
	// and stop() are both the GUI thread's, so the flag is read and the count
	// taken in one go and neither can see the other half-way.
	pthread_mutex_lock(&worker_lock);
	bool go = !stopping;
	if (go) {
		workers_live++;
	}
	pthread_mutex_unlock(&worker_lock);
	if (!go) {
		free(job);
		return false;
	}

	pthread_mutex_lock(&gen_lock);
	job->generation = ++request_generation;
	pthread_mutex_unlock(&gen_lock);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, job) != 0) {
		pthread_mutex_lock(&worker_lock);
		workers_live--;
		pthread_cond_broadcast(&worker_gone);
		pthread_mutex_unlock(&worker_lock);
		job_kind_t kind = job->kind;
		free(job);
		if (kind == JOB_LOGIN) {
			gui_notify_popup("could not start a worker thread");
		} else {
			list_show_message("could not start a worker thread");
		}
		return false;
	}
	pthread_detach(thread);
	return true;
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
	job_launch(job);
}

// The sign-in chain finished: the three answers and the file they belong to
// leave the page's fields here, and the worker talks to the network.
static void start_sign_in(void) {
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		gui_notify_popup("could not start a worker thread");
		return;
	}
	job->kind = JOB_LOGIN;
	lv_snprintf(job->slug, sizeof(job->slug), "%s", entry_slug);
	lv_snprintf(job->server, sizeof(job->server), "%s", pending_server);
	lv_snprintf(job->user, sizeof(job->user), "%s", pending_user);
	lv_snprintf(job->password, sizeof(job->password), "%s", pending_password);
	if (job_launch(job)) {
		memset(pending_password, 0, sizeof(pending_password));
	}
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

// One textarea and one keyboard serve four questions: the three of a sign-in
// (server, user, password) and the search. Which one an OK belongs to is
// `entry_kind` and `entry_step`, declared above with the fields they read.

static void ask_user(const char *server);
static void ask_password(const char *user);
static void sign_in_finish(const char *password);
static void search_done(const char *query);

static void entry_ok_cb(lv_event_t *e) {
	(void)e;
	char text[TEXT_MAX];
	lv_snprintf(text, sizeof(text), "%s", lv_textarea_get_text(entry_field));

	// Read and cleared before the step runs: a step that rejects what was typed
	// leaves the screen up and arms the next one itself, and a step that starts
	// a job leaves nothing armed at all.
	entry_kind_t kind = entry_kind;
	entry_step_t step = entry_step;
	entry_kind = ENTRY_NONE;

	// The keyboard is *not* hidden here: a step that rejects what was typed
	// (an empty server, say) leaves its screen up, and a keyboard that vanished
	// under the message would have to be tapped back into existence. The steps
	// that leave the screen hide it themselves.
	switch (kind) {
	case ENTRY_SIGNIN:
		switch (step) {
		case STEP_SERVER:
			ask_user(text);
			break;
		case STEP_USER:
			ask_password(text);
			break;
		default:
			sign_in_finish(text);
			break;
		}
		break;
	case ENTRY_SEARCH:
		search_done(text);
		break;
	default:
		break;
	}
}

static void entry_open(entry_kind_t kind, entry_step_t step, const char *title, const char *initial, bool password) {
	lv_label_set_text(entry_title, title);
	lv_textarea_set_text(entry_field, initial ? initial : "");
	lv_textarea_set_password_mode(entry_field, password);
	keyboard_set_field(entry_keyboard, entry_field);
	keyboard_set_visible(entry_keyboard, true);
	entry_kind = kind;
	entry_step = step;
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

// The password arrived: everything is in hand, ask the server. The job carries
// what was typed and the file it belongs to, so the worker never reads the
// page's fields while the user may be typing the next thing.
static void sign_in_finish(const char *password) {
	trim_copy(pending_password, sizeof(pending_password), password);
	if (!pending_server[0] || !pending_user[0]) {
		gui_notify_popup("server and user name are needed");
		return;
	}
	keyboard_set_visible(entry_keyboard, false);
	switch_screen(home_screen);
	start_sign_in();
}

// The user name arrived: ask for the password. The step stays a sign-in, and
// the target file is the one the chain was opened for.
static void ask_password(const char *user) {
	trim_copy(pending_user, sizeof(pending_user), user);
	if (!pending_user[0]) {
		gui_notify_popup("the user name is empty");
		return; // the entry screen stays up
	}
	entry_open(ENTRY_SIGNIN, STEP_PASSWORD, "Password", "", true);
}

// The server arrived: ask for the user name.
static void ask_user(const char *server) {
	trim_copy(pending_server, sizeof(pending_server), server);
	if (!pending_server[0]) {
		gui_notify_popup("the server is empty");
		return;
	}
	entry_open(ENTRY_SIGNIN, STEP_USER, "User name", pending_user, false);
}

// Starts the three questions, writing into `slug` -- "" for a server this card
// does not have yet. `seed` fills the address and the name from the server in
// use, so signing in again after a token expired is one field and not three.
static void sign_in_open(const char *slug, bool seed) {
	lv_snprintf(entry_slug, sizeof(entry_slug), "%s", slug ? slug : "");
	if (seed) {
		subsonic_server(pending_server, sizeof(pending_server));
		subsonic_user_name(pending_user, sizeof(pending_user));
	} else {
		pending_server[0] = '\0';
		pending_user[0] = '\0';
	}
	memset(pending_password, 0, sizeof(pending_password));
	entry_open(ENTRY_SIGNIN, STEP_SERVER, "Server", pending_server, false);
}

// ---------------------------------------------------------------------------
// Home
// ---------------------------------------------------------------------------

// The name of the server in use, for the row that opens the servers screen. The
// addon keeps the list in memory and refreshes it on load, sign-in, switch and
// removal, so a screen build reads no files.
static bool active_label(char *out, size_t size) {
	char active[SUBSONIC_SLUG_MAX];
	if (!out || !size) {
		return false;
	}
	out[0] = '\0';
	if (!subsonic_host_active_slug(active, sizeof(active))) {
		return false;
	}
	int n = subsonic_hosts_count();
	for (int i = 0; i < n; i++) {
		char slug[SUBSONIC_SLUG_MAX];
		char label[SUBSONIC_LABEL_MAX];
		if (subsonic_host_slug_at(i, slug, sizeof(slug)) && strcmp(slug, active) == 0 &&
			subsonic_host_at(i, label, sizeof(label), NULL, 0, NULL, 0)) {
			lv_snprintf(out, size, "%s", label);
			return true;
		}
	}
	// The file is there and the list is not: the slug at least says which one.
	lv_snprintf(out, size, "%s", active);
	return true;
}

static const char *quality_name(int q) {
	switch (q) {
	case SUBSONIC_QUALITY_MP3_320:
		return "320 kbps";
	case SUBSONIC_QUALITY_MP3_192:
		return "192 kbps";
	case SUBSONIC_QUALITY_OPUS_128:
		return "128 kbps";
	default:
		return "Original";
	}
}

static void servers_build(void);

static void account_cb(lv_event_t *e) {
	(void)e;
	char slug[SUBSONIC_SLUG_MAX];
	subsonic_host_active_slug(slug, sizeof(slug));
	sign_in_open(slug, true);
}

static void servers_cb(lv_event_t *e) {
	(void)e;
	servers_build();
	switch_screen(servers_screen);
}

// One tap, one step round the four choices: the file as it is on the server
// first -- it is the only one that keeps a lossless library lossless -- then the
// transcodes, in the order of their bitrates.
static void quality_cb(lv_event_t *e) {
	(void)e;
	subsonic_set_quality((subsonic_get_quality() + 1) % SUBSONIC_QUALITY_COUNT);
	home_build();
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
	// The step is the sign-in flow's; a search has none and ignores it.
	entry_open(ENTRY_SEARCH, STEP_SERVER, "Search", "", false);
}

// The active server, and only that one: the others keep their tokens.
static void sign_out_cb(lv_event_t *e) {
	(void)e;
	subsonic_logout();
	// A cached track still plays after this, and its star must not keep
	// answering for an account that is gone.
	subsonicservice_forget_stars();
	subsonicplay_forget();
	home_build();
	gui_notify_popup("signed out");
}

static void home_build(void) {
	if (!home_page) {
		home_screen = lv_obj_create(NULL);
		home_page = scrolling_page(home_screen, "Subsonic");
	}
	lv_obj_clean(home_page);

	char label[SUBSONIC_LABEL_MAX];
	bool have_host = active_label(label, sizeof(label));
	if (!have_host && subsonic_logged_in()) {
		// A session with no file behind it -- a card that would not take a
		// servers folder, or one from the older layout. Still a server, and
		// still the way to the servers screen.
		char server[SUBSONIC_SERVER_MAX];
		subsonic_server(server, sizeof(server));
		subsonic_server_host(server, label, sizeof(label));
		have_host = label[0] != '\0';
	}

	// Which server, before whose account: it is the row a house with two of
	// them reaches for first.
	if (have_host) {
		row(home_page, "Server", label, servers_cb, NULL);
	}

	if (subsonic_logged_in()) {
		char value[TEXT_MAX];
		subsonic_display_name(value, sizeof(value));
		row(home_page, "Account", value, account_cb, NULL);
		row(home_page, "Artists", "", artists_cb, NULL);
		row(home_page, "Newest", "", newest_cb, NULL);
		row(home_page, "Random", "", random_cb, NULL);
		row(home_page, "Playlists", "", playlists_cb, NULL);
		row(home_page, "Search", "", search_cb, NULL);
	} else {
		row(home_page, "Sign in", "server, user, password", account_cb, NULL);
	}

	// A choice that is stored per server, so it is only offered when there is
	// one to store it for.
	if (have_host) {
		row(home_page, "Quality", quality_name(subsonic_get_quality()), quality_cb, NULL);
	}
	if (subsonic_logged_in()) {
		row(home_page, "Sign out", "", sign_out_cb, NULL);
	}
}

// ---------------------------------------------------------------------------
// The servers screen
// ---------------------------------------------------------------------------

// Taps on a row make that the server in use: the session is reloaded under the
// addon, and the page forgets everything that belonged to the one before.
static void server_row_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	char slug[SUBSONIC_SLUG_MAX];
	if (!subsonic_host_slug_at(index, slug, sizeof(slug))) {
		return;
	}
	if (!subsonic_host_select(slug)) {
		gui_notify_popup("this server could not be opened");
		return;
	}
	page_reset();
	seed_from_host();
	home_build();
	switch_screen(home_screen);
}

static void add_server_cb(lv_event_t *e) {
	(void)e;
	sign_in_open("", false);
}

// After the card's own "are you sure?": the active server's file goes, and the
// page comes up for whichever server is first of those left.
static void forget_done(void *user) {
	(void)user;
	char slug[SUBSONIC_SLUG_MAX];
	if (!subsonic_host_active_slug(slug, sizeof(slug))) {
		return;
	}
	if (!subsonic_host_forget(slug)) {
		gui_notify_popup("this server could not be removed");
		return;
	}
	page_reset();
	seed_from_host();
	home_build();
	servers_build();
	gui_notify_popup("server removed");
}

static void remove_server_cb(lv_event_t *e) {
	(void)e;
	char slug[SUBSONIC_SLUG_MAX];
	if (!subsonic_host_active_slug(slug, sizeof(slug))) {
		return;
	}
	char label[SUBSONIC_LABEL_MAX];
	if (!active_label(label, sizeof(label))) {
		lv_snprintf(label, sizeof(label), "%s", slug);
	}
	char message[TEXT_MAX];
	lv_snprintf(message, sizeof(message),
				"%s will be removed from this card.\nThe account is forgotten; nothing on the server is touched.",
				label);
	confirm_show("Remove server", message, "remove", forget_done, NULL);
}

static void servers_build(void) {
	if (!servers_page) {
		return;
	}
	lv_obj_clean(servers_page);
	subsonic_hosts_refresh();

	char active[SUBSONIC_SLUG_MAX];
	bool have_active = subsonic_host_active_slug(active, sizeof(active));
	int n = subsonic_hosts_count();
	char active_name[SUBSONIC_LABEL_MAX] = "";

	for (int i = 0; i < n; i++) {
		char slug[SUBSONIC_SLUG_MAX];
		char label[SUBSONIC_LABEL_MAX];
		char server[SUBSONIC_SERVER_MAX];
		char user[SUBSONIC_NAME_MAX];
		if (!subsonic_host_slug_at(i, slug, sizeof(slug)) ||
			!subsonic_host_at(i, label, sizeof(label), server, sizeof(server), user, sizeof(user))) {
			continue;
		}
		char host[SUBSONIC_SERVER_MAX];
		subsonic_server_host(server, host, sizeof(host));
		char value[TEXT_MAX];
		if (user[0]) {
			lv_snprintf(value, sizeof(value), "%s @ %s", user, host);
		} else {
			lv_snprintf(value, sizeof(value), "%s", host);
		}
		// The one in use is marked in the name. A plain asterisk: the row's
		// font is the player's, and not every bullet is drawn in it.
		char name[SUBSONIC_LABEL_MAX + 4];
		if (have_active && strcmp(slug, active) == 0) {
			lv_snprintf(name, sizeof(name), "* %s", label);
			lv_snprintf(active_name, sizeof(active_name), "%s", label);
		} else {
			lv_snprintf(name, sizeof(name), "%s", label);
		}
		row(servers_page, name, value, server_row_cb, (void *)(intptr_t)i);
	}

	if (n == 0) {
		lv_obj_t *label = lv_label_create(servers_page);
		lv_label_set_text(label, "no servers yet");
		lv_obj_add_style(label, &theme_style_text_dim, 0);
		lv_obj_set_style_text_font(label, &font_ui_24, 0);
		lv_obj_set_width(label, lv_pct(100));
		lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
	}

	row(servers_page, "Add server", "", add_server_cb, NULL);
	if (have_active) {
		// Which one it would take is the question the row has to answer, and the
		// name is too long for this row's width: the label goes in the value.
		row(servers_page, "Remove server", active_name, remove_server_cb, NULL);
	}
}

// ---------------------------------------------------------------------------
// The addon
// ---------------------------------------------------------------------------

// "Show album" from the player's track menu: the addon's list screen, asking for
// that album's tracks, which is where the sheet's back chevron then returns
// from. The album id comes from the sidecar written when the track was
// downloaded (subsoniccache.c), so no database lookup is involved.
static void *open_album_cb(const char *album_id, const char *title) {
	if (!album_id || !album_id[0] || !list_screen || !subsonic_logged_in()) {
		return NULL;
	}
	level_count = 0;
	level_t *l = &levels[level_count++];
	l->kind = JOB_TRACKS;
	lv_snprintf(l->id, sizeof(l->id), "%s", album_id);
	lv_snprintf(l->title, sizeof(l->title), "%s", title && title[0] ? title : "Subsonic");
	heading_set(l->title);
	switch_screen(list_screen);
	job_start_visual(JOB_TRACKS, l->id, NULL);
	return list_screen;
}

static lv_obj_t *subsonic_init(const sonix_addon_host_t *host) {
	config = host->cfg;

	// The name the shell knows this addon by, for the screen list it keeps
	// (sonix_addon_own_screen): it is the key, and the pointer alone would not
	// say whose page it is.
	lv_snprintf(self_id, sizeof(self_id), "%s", host->id);

	// A second run of this library in the same process is possible -- a
	// dlclose() that failed, a worker that outlived the wait in stop() -- and it
	// has to find the state a fresh library would: nothing stopping, no request
	// numbered, nothing in the arrays that a generation could still match. The
	// locks need nothing done to them: they are statics with
	// PTHREAD_*_INITIALIZER, they are never destroyed, and a mutex that is never
	// destroyed is ready for the next run as it stands.
	pthread_mutex_lock(&worker_lock);
	stopping = false;
	pthread_mutex_unlock(&worker_lock);

	pthread_mutex_lock(&gen_lock);
	request_generation = 0;
	pthread_mutex_unlock(&gen_lock);

	pthread_mutex_lock(&data_lock);
	result_kind = JOB_NONE;
	result_count = 0;
	result_error[0] = '\0';
	pthread_mutex_unlock(&data_lock);

	level_count = 0;
	entry_kind = ENTRY_NONE;
	entry_step = STEP_SERVER;
	entry_slug[0] = '\0';

	subsonic_load(host->dir);

	// The cache and the queue: everything that plays a track as a file of the
	// card. <addon>/cache, made here; a card that refuses it (read-only, or
	// gone) leaves it NULL, and a Subsonic track then cannot be played at all --
	// it is not a stream that could be handed to radio_play() instead.
	char cache[512];
	bool have_cache = addon_cache_dir(host, cache, sizeof(cache));
	subsoniccache_set_addon_dir(have_cache ? cache : NULL);
	subsoniccache_set_root(host->sd_root);

	// The covers of the list rows come from the player's listart, which keeps
	// what it downloads in <card>/.local/listart and knows nothing about this
	// addon's folder. Somebody has to point it at the card -- every page with
	// covers does -- and without the call it has no directory at all, so the
	// rows keep their placeholder boxes for ever (listart.h).
	listart_set_root(host->sd_root);

	// The prepare callback goes in here, not on the first tap: the player
	// restores its queue at boot and can start a track of ours before this page
	// is ever opened.
	subsonicplay_init(playback_event);

	// The star's mirror: the account's starred songs, fetched once on a worker
	// so that the sheet's star answers without a network round trip.
	if (subsonic_logged_in()) {
		subsonicservice_refresh_stars();
	}

	// What is already known fills the form: signing in again after a token
	// expired is then one field, not three.
	seed_from_host();

	home_build();

	// Anything the load could not do with the card -- a servers folder it would
	// not create, a session it kept in memory only -- is said once, here: a
	// player in the hand has no log to read.
	char note[192];
	subsonic_hosts_note(note, sizeof(note));
	if (note[0]) {
		gui_notify_popup(note);
	}

	// Every screen this addon puts on the display, handed to the shell as it is
	// made: this is the list addon_unload() walks to move the display off
	// whichever one is up and to delete them all once stop() has returned. A
	// screen missing from it is a page left standing with widget callbacks
	// pointing into a library that is about to be unmapped.
	sonix_addon_own_screen(self_id, home_screen);

	list_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(self_id, list_screen);
	list_container = scrolling_page(list_screen, "Subsonic");
	switcher_set_back_guard(list_screen, list_back_guard);
	// Which rows are on screen decides which covers are asked for: the list is
	// taller than the panel, and every scroll moves the window the downloader
	// serves. Registered once, here, because the container outlives the lists.
	lv_obj_add_event_cb(list_container, list_scrolled_cb, LV_EVENT_SCROLL, NULL);

	entry_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(self_id, entry_screen);
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

	servers_screen = lv_obj_create(NULL);
	sonix_addon_own_screen(self_id, servers_screen);
	servers_page = scrolling_page(servers_screen, "Servers");

	// The two account hooks only the page can supply: "show album" opens the
	// list above. The badge stays NULL -- this player's icon set carries the
	// Qobuz and Tidal marks and no Subsonic one, so the artwork keeps none; the
	// sheet's cover is the real one, read from the cached file.
	subsonicservice_attach_gui(open_album_cb);

	return home_screen;
}

// The addon is going away: the card was pulled, or addon.so on it changed and
// the shell is unloading this copy before loading the new one. Runs on the GUI
// thread with this addon's screens already off the display, and the answer is
// whether the library may be dlclose()d: true when nothing of it is running any
// more, false when a request is still inside it and the shell has to keep the
// mapping instead.
static bool subsonic_addon_stop(void) {
	// 0. The network first, and the shared download turn with it. A download
	// thread parked in streamturn_acquire() only leaves once it has the turn,
	// and the turn may be held by another service's track; asking everyone to
	// abandon is what makes the waits below bounded in that case rather than the
	// length of somebody else's album.
	streamturn_abandon_all();

	// 1. No new work from here: start_job() reads this before it counts or
	// creates anything, and a worker reads it before it says its result came
	// back. Nothing may be posted after this point, because the callback is code
	// in a library that is about to be unmapped.
	pthread_mutex_lock(&worker_lock);
	stopping = true;
	pthread_mutex_unlock(&worker_lock);

	// 2. The workers, bounded. Each one is a request over the network with a
	// twenty-second timeout of its own and no way to interrupt it: waiting for
	// all of them is waiting twenty seconds on the GUI thread, and the card is
	// what is waiting. So two seconds, then a line in the log naming what is
	// still out there -- and a false answer, which is what keeps the loader from
	// unmapping the code that thread is standing in. (Its gui_post() is dropped
	// either way: `stopping` above, and the loader's own sweep of the queue by
	// address range.)
	pthread_mutex_lock(&worker_lock);
	if (workers_live > 0) {
		struct timespec until;
		clock_gettime(CLOCK_REALTIME, &until);
		until.tv_sec += STOP_WAIT_MS / 1000;
		until.tv_nsec += (long)(STOP_WAIT_MS % 1000) * 1000000L;
		if (until.tv_nsec >= 1000000000L) {
			until.tv_sec++;
			until.tv_nsec -= 1000000000L;
		}
		while (workers_live > 0) {
			if (pthread_cond_timedwait(&worker_gone, &worker_lock, &until) == ETIMEDOUT) {
				break;
			}
		}
	}
	int left = workers_live;
	pthread_mutex_unlock(&worker_lock);
	if (left > 0) {
		fprintf(stderr, "subsonic: stop: %d request(s) still running, keeping the library mapped\n", left);
	}

	// 2b. The modules this page started, in the order of their dependencies.
	// Each withdraws its own registration with the player -- the prepare
	// callback and the watch timer (subsonicplay_stop), the streamturn abandon
	// hook (subsoniccache_stop) -- before it waits, because all three are code
	// in this library. Every one of them runs even after another has said no.
	bool safe = subsonicplay_stop();
	safe = subsonicservice_stop() && safe;
	safe = subsoniccache_stop() && safe;

	// 3. What the last run put in the shared state. Left alone while a worker is
	// still writing it (the arrays are its, and the lock would make this wait for
	// the network all over again), and cleared otherwise, above all for the case
	// this is not the last run: a library that stopped and starts again has to
	// find the arrays and the numbering as a fresh one would.
	if (left == 0) {
		pthread_mutex_lock(&data_lock);
		result_kind = JOB_NONE;
		result_count = 0;
		result_error[0] = '\0';
		memset(artists, 0, sizeof(artists));
		memset(albums, 0, sizeof(albums));
		memset(tracks, 0, sizeof(tracks));
		memset(playlists, 0, sizeof(playlists));
		memset(levels, 0, sizeof(levels));
		pthread_mutex_unlock(&data_lock);
	}
	pthread_mutex_lock(&gen_lock);
	request_generation = 0;
	pthread_mutex_unlock(&gen_lock);

	level_count = 0;

	// The step of the sign-in flow, when one was waiting for an entry screen's
	// OK. It is an enum, but the screens and the keyboard that carried it are
	// the player's, so nothing of this library may still be armed for the next
	// tap.
	entry_kind = ENTRY_NONE;
	entry_step = STEP_SERVER;
	entry_slug[0] = '\0';

	// What was typed is the user's, and one of the three is a password.
	memset(pending_password, 0, sizeof(pending_password));
	memset(pending_user, 0, sizeof(pending_user));
	memset(pending_server, 0, sizeof(pending_server));

	// The "are you sure?" card a removal may be standing behind is the player's
	// and survives this addon: its OK button calls a function in this library,
	// so it is closed here (qobuz's stop() does the same, and for the same
	// reason). A card of the player's own would be closed too, which is why this
	// is done only when our screens are already gone.
	confirm_close();

	// 4. The session, from memory only. The files on the card are the record and
	// stop() is an unload, not a sign-out: writing here would be a write to a
	// filesystem that may be going away, and would end the session of the copy
	// that is loaded next.
	subsonic_forget();

	// 5. The one hook this addon put in the player. The list screen takes the
	// back chevron for itself (switcher_set_back_guard), and that handler is a
	// function in this library: NULL replaces it, which is what switcher.h says
	// registering the same screen again does.
	if (list_screen && gui_obj_alive(list_screen)) {
		switcher_set_back_guard(list_screen, NULL);
	}

	// 6. The rows' covers, before their rows go with the screens: listart's
	// slots are shared and a slot held by a page that is leaving starves the
	// next one. listrow_stop() then deletes the poll timer -- not a child of any
	// widget, and its callback is code in this library, so a tick after the
	// unload would jump into the mapping -- and forgets every row. The rows
	// themselves are the shell's to delete, and it does that before this
	// library is unmapped (addon.c, step 4), which is why the delete handler
	// listrow left on them finds nothing left to release.
	listrow_reset((void *)&art_owner);
	listrow_stop();

	// 7. Every pointer this file kept into LVGL, cleared: the screens are the
	// shell's from here. Each one was registered as it was made
	// (sonix_addon_own_screen), so addon_unload() moves the display off whichever
	// of them is up, forgets them in the back-stack and deletes them once this
	// returns -- with its own aliveness check, so nothing here may delete them
	// too, and the keyboard on the entry screen goes with its parent
	// (keyboard.c drops it from its own list when the tray is deleted).
	config = NULL;
	home_screen = NULL;
	home_page = NULL;
	list_screen = NULL;
	list_container = NULL;
	servers_screen = NULL;
	servers_page = NULL;
	entry_screen = NULL;
	entry_field = NULL;
	entry_title = NULL;
	entry_keyboard = NULL;

	// The answer the loader acts on: nought workers above, and every thread of
	// this library has run its last instruction. A false here is not a failure to
	// report -- the shell keeps the mapping and refuses to open this addon again
	// for the rest of the run, which is a leak instead of a jump into an unmapped
	// page.
	return left == 0 && safe;
}

static const sonix_addon_t subsonic = {
	.api = SONIX_ADDON_API,
	.title = "Subsonic",
	.init = subsonic_init,
};

// One function, one flag and one count: the whole of what this addon has to undo
// is a page that is still on the network. See addon.h for the contract.
static const sonix_addon_stop_t subsonic_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = subsonic_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &subsonic : NULL;
}

// The second symbol: what the rest of the player asks about the cache and the
// account (subsonicservice.c). The loader asks for it as soon as the library is
// open, before init(), which is why the table is a static here and every
// function in it survives the cache not being set up yet.
const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? subsonicservice_table() : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &subsonic_lifecycle : NULL;
}
