#include "yandexstate.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/gui.h"
#include "src/gui/shell/settingsrow.h"

#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexconf.h"
#include "src/system/streaming/yandexplay.h"
#include "src/system/streaming/yandexwave.h"

#include "lvgl/lvgl.h"

// The Yandex Music addon's screen for this stage: the keys, the account, and
// the actions that change them. See yandexstate.h.
//
// Nothing that talks to the network runs on the UI thread: the sign-in is a
// detached worker that posts its answer back with gui_post(), as
// docs/ADDONS.md asks. Opening the page itself never touches the network, so a
// card with no keys and no token still opens it and says why the service is off.

lv_obj_t *yandexstate_screen;

static char card_root[400];

static lv_obj_t *keys_value;
static lv_obj_t *account_value;
static lv_obj_t *token_value;
static lv_obj_t *quality_value;

// The code the device flow shows: what to type, where to type it, and how long
// it has left.
static lv_obj_t *code_value;
static lv_obj_t *where_value;
static lv_obj_t *wait_value;

// The strings here are English, not tr(): an addon's strings are its own and
// are not in the player's language files (docs/ADDONS.md, "Languages"). The
// keys the service modules themselves pass through tr() (an error from the
// downloader, for instance) come back as the tag when there is no line for
// them, which is why the page does not show those.

static void token_path(char *out, size_t size) {
	if (!card_root[0]) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s/%s", card_root, YANDEX_TOKEN_FILE);
}

// The first line of the card's token file that has anything on it. A whole
// "Authorization: OAuth ..." line pasted in is one line too, and
// yandex_token_clean() takes the token out of it.
static bool read_token_file(char *out, size_t size) {
	char path[520];
	out[0] = '\0';
	token_path(path, sizeof(path));
	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) {
		return false;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		const char *start = line;
		// A UTF-8 BOM, as an editor on Windows leaves it.
		if ((unsigned char)start[0] == 0xef && (unsigned char)start[1] == 0xbb && (unsigned char)start[2] == 0xbf) {
			start += 3;
		}
		while (*start == ' ' || *start == '\t') {
			start++;
		}
		if (*start) {
			// The line may be longer than a token: yandex_token_clean() keeps
			// what is a token and stops at the first character that is not.
			snprintf(out, size, "%s", start);
			break;
		}
	}
	fclose(f);
	return out[0] != '\0';
}

static void refresh(void) {
	if (!keys_value) {
		return;
	}
	lv_label_set_text(keys_value, yandex_configured() ? "present" : "no keys");
	if (yandex_logged_in()) {
		const char *name = yandex_display_name();
		lv_label_set_text(account_value, name && name[0] ? name : "signed in");
	} else {
		lv_label_set_text(account_value, "not signed in");
	}
	char path[520];
	token_path(path, sizeof(path));
	bool token = false;
	if (path[0]) {
		FILE *f = fopen(path, "r");
		if (f) {
			token = true;
			fclose(f);
		}
	}
	lv_label_set_text(token_value, token ? "found on the card" : "not on the card");
	static const char *const QUALITY[] = {"Lossless", "High", "Normal"};
	yandex_quality_t q = yandex_get_quality();
	lv_label_set_text(quality_value, QUALITY[(unsigned)q < YANDEX_QUALITY_COUNT ? (unsigned)q : 0]);
}

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef struct {
	char message[256];
	bool ok;
} answer_t;

static void answer_async(void *user) {
	answer_t *a = user;
	if (a->message[0]) {
		gui_notify_popup(a->message);
	}
	refresh();
	free(a);
}

static void *sign_in_main(void *arg) {
	(void)arg;
	answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	char token[1024];
	if (!read_token_file(token, sizeof(token))) {
		snprintf(a->message, sizeof(a->message), "no %s at the card's root", YANDEX_TOKEN_FILE);
		gui_post(answer_async, a);
		return NULL;
	}
	a->ok = yandex_import_token(token, a->message, sizeof(a->message));
	memset(token, 0, sizeof(token));
	if (!a->message[0]) {
		snprintf(a->message, sizeof(a->message), "%s", a->ok ? "signed in" : "the token was not accepted");
	}
	if (!gui_post(answer_async, a)) {
		free(a);
	}
	return NULL;
}

static void start_worker(void *(*main_fn)(void *)) {
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024); // TLS: as the other workers
	if (pthread_create(&th, &attr, main_fn, NULL) != 0) {
		gui_notify_popup("could not start a worker");
	}
	pthread_attr_destroy(&attr);
}

// ---------------------------------------------------------------------------
// signing in with a code (OAuth device flow)
//
// The backend for this has been in yandex.c from the start -- yandex_login_begin()
// asks oauth.yandex.ru for a code and yandex_login_poll() trades it for tokens,
// saving the session and fetching the account on the way -- but there was no
// page to drive it, so this stage only took a token out of a file. This is the
// page: a code is shown, the user types it at ya.ru/device on a phone, and the
// code is polled until the account says yes.
//
// The polls are made by the worker, one at a time, and paced by an LVGL timer on
// the GUI thread: a sleep-and-retry loop inside one job would hold the worker
// for the whole life of the code, which is minutes. Polling stops when the page
// is left, or it would be one request every few seconds forever -- and sooner or
// later a sign-in succeeding on a page nobody is looking at.
// ---------------------------------------------------------------------------

static yandex_login_t login;
static lv_timer_t *login_timer;
static int login_elapsed;  // seconds since the code arrived
static bool login_active;  // the page is waiting for a code to be typed
static bool login_polling; // a poll is in flight: never two at once

static void login_clear(void) {
	if (code_value) {
		lv_label_set_text(code_value, "");
	}
	if (where_value) {
		lv_label_set_text(where_value, "");
	}
	if (wait_value) {
		lv_label_set_text(wait_value, "");
	}
}

static void login_stop(void) {
	login_active = false;
	login_polling = false;
	if (login_timer) {
		lv_timer_delete(login_timer);
		login_timer = NULL;
	}
}

// The flow ended without signing in: say what happened and clear the code, so
// an expired one cannot be typed in again from a screen that still shows it.
static void login_ended(const char *message) {
	login_stop();
	login_clear();
	if (message && message[0]) {
		gui_notify_popup(message);
	}
}

typedef struct {
	bool ok;
	char message[256];
} code_answer_t;

typedef struct {
	yandex_login_state_t state;
	char message[256];
} poll_answer_t;

static void code_async(void *user);
static void poll_async(void *user);

static void *code_main(void *arg) {
	(void)arg;
	code_answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	if (!yandex_login_begin(&login)) {
		const char *why = yandex_last_error();
		snprintf(a->message, sizeof(a->message), "%s", why && why[0] ? why : "the code was not given out");
	} else {
		a->ok = true;
	}
	if (!gui_post(code_async, a)) {
		free(a);
	}
	return NULL;
}

static void *poll_main(void *arg) {
	(void)arg;
	poll_answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	a->state = yandex_login_poll(&login);
	if (a->state == YANDEX_LOGIN_ERROR) {
		const char *why = yandex_last_error();
		snprintf(a->message, sizeof(a->message), "%s", why && why[0] ? why : "sign in failed");
	}
	if (!gui_post(poll_async, a)) {
		free(a);
	}
	return NULL;
}

static void login_tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!login_active) {
		return;
	}
	login_elapsed++;

	// The code has a lifetime after which the service stops recognising it;
	// polling past that point is pure traffic.
	if (login.expires_secs > 0 && login_elapsed >= login.expires_secs) {
		login_ended("the code has expired");
		return;
	}

	char left[64];
	if (login.expires_secs > 0) {
		snprintf(left, sizeof(left), "waiting (%ds left)", login.expires_secs - login_elapsed);
	} else {
		snprintf(left, sizeof(left), "waiting (%ds)", login_elapsed);
	}
	if (wait_value) {
		lv_label_set_text(wait_value, left);
	}

	int interval = login.interval_secs > 0 ? login.interval_secs : 5;
	if (login_elapsed % interval != 0 || login_polling) {
		return;
	}
	login_polling = true;
	start_worker(poll_main);
}

// The code arrived: show it, and start the timer that paces the polls.
static void code_async(void *user) {
	code_answer_t *a = user;
	if (!login_active) {
		free(a); // the page stopped waiting meanwhile
		return;
	}
	if (!a->ok) {
		login_ended(a->message);
		free(a);
		return;
	}
	const char *url = login.verification_url[0] ? login.verification_url : "https://ya.ru/device";
	const char *scheme = strstr(url, "://");
	if (code_value) {
		lv_label_set_text(code_value, login.user_code);
	}
	if (where_value) {
		lv_label_set_text(where_value, scheme ? scheme + 3 : url);
	}
	if (wait_value) {
		lv_label_set_text(wait_value, "waiting for the code");
	}
	login_elapsed = 0;
	if (!login_timer) {
		login_timer = lv_timer_create(login_tick_cb, 1000, NULL);
	}
	free(a);
}

static void poll_async(void *user) {
	poll_answer_t *a = user;
	login_polling = false;
	// A successful answer counts even if the page stopped waiting meanwhile: the
	// tokens are already written and the account is signed in, and dropping it
	// would leave the screen saying "not signed in" to someone who typed in time.
	if (!login_active && a->state != YANDEX_LOGIN_OK) {
		free(a);
		return;
	}
	switch (a->state) {
	case YANDEX_LOGIN_PENDING:
		break; // nobody has typed it yet: keep polling
	case YANDEX_LOGIN_OK:
		login_stop();
		login_clear();
		refresh();
		gui_notify_popup("signed in");
		break;
	case YANDEX_LOGIN_EXPIRED:
		login_ended("the code has expired");
		break;
	case YANDEX_LOGIN_ERROR:
	default:
		login_ended(a->message[0] ? a->message : "sign in failed");
		break;
	}
	free(a);
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

// Playing
//
// The engine is yandexplay.c and it was already here: a list of ids becomes the
// player's queue, the track playback reaches is fetched into
// <card>/.local/yandex-cache/ and played, and the ones after it are fetched
// ahead. What it never had is a caller -- the page that used to call it is
// yandexpage.c, and that page is not ported. Two buttons are the smallest
// caller that gets music out of the device: My Wave (a station, endless) and
// the liked tracks (a list, finite).
//
// yandexplay_watch() runs on a one-second GUI timer: it fetches ahead, keeps the
// queued files from the cache's sweep and tells My Wave what is playing. The
// timer is created with the page and never deleted with it -- leaving the screen
// does not stop the music, and the wave would run dry without it.
// ---------------------------------------------------------------------------

static lv_timer_t *play_timer;
static bool play_busy; // one list is being built: the buttons do nothing meanwhile

typedef struct {
	yandexlist_t *list; // built on the worker, started on the GUI thread
	bool wave;
	char message[256];
} play_answer_t;

static void play_async(void *user);

static void liked_each(const char *track_id, const char *album_id, const yandex_track_t *track, void *user) {
	(void)track;
	yandexlist_t *list = user;
	if (track_id && track_id[0]) {
		yandexlist_add_id(list, track_id, album_id);
	}
}

static void *liked_main(void *arg) {
	(void)arg;
	play_answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	a->list = yandexlist_new();
	if (a->list) {
		int n = yandex_liked_ids(liked_each, a->list);
		if (n <= 0) {
			const char *why = yandex_last_error();
			snprintf(a->message, sizeof(a->message), "%s",
					 n == 0 ? "no liked tracks" : (why && why[0] ? why : "the liked list was not read"));
		}
	}
	if (!gui_post(play_async, a)) {
		yandexlist_free(a->list);
		free(a);
	}
	return NULL;
}

static void *wave_main(void *arg) {
	(void)arg;
	play_answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	yandex_track_t *tracks = calloc(YANDEXWAVE_BATCH_MAX, sizeof(*tracks));
	if (!tracks) {
		free(a);
		return NULL;
	}
	int n = yandex_wave_start(NULL, 0, tracks, YANDEXWAVE_BATCH_MAX);
	a->wave = true;
	if (n > 0) {
		a->list = yandexlist_new();
	}
	for (int i = 0; a->list && i < n; i++) {
		if (tracks[i].id[0]) {
			yandexlist_add_track(a->list, &tracks[i]);
		}
	}
	if (!a->list || yandexlist_count(a->list) == 0) {
		const char *why = yandex_last_error();
		snprintf(a->message, sizeof(a->message), "%s", why && why[0] ? why : "the station said nothing");
	}
	free(tracks);
	if (!gui_post(play_async, a)) {
		yandexlist_free(a->list);
		free(a);
	}
	return NULL;
}

static void play_async(void *user) {
	play_answer_t *a = user;
	play_busy = false;
	// yandexplay_list() takes a copy of the list, so ours is dropped either way.
	if (a->list && yandexlist_count(a->list) > 0 && yandexplay_list(a->list, 0, a->wave)) {
		yandexlist_free(a->list);
		free(a);
		return;
	}
	yandexlist_free(a->list);
	gui_notify_popup(a->message[0] ? a->message : "could not start the queue");
	free(a);
}

static void play_tick_cb(lv_timer_t *timer) {
	(void)timer;
	yandexplay_watch();
}

// Both buttons want the same three things: keys, a session, and no list being
// built already.
static bool play_ready(void) {
	if (!yandex_configured()) {
		gui_notify_popup("no keys: the addon's settings.ini has none");
		return false;
	}
	if (!yandex_logged_in()) {
		gui_notify_popup("not signed in");
		return false;
	}
	if (play_busy) {
		gui_notify_popup("a list is being built");
		return false;
	}
	return true;
}

static void play_wave_cb(lv_event_t *e) {
	(void)e;
	if (!play_ready()) {
		return;
	}
	play_busy = true;
	gui_notify_popup("asking the station...");
	start_worker(wave_main);
}

static void play_liked_cb(lv_event_t *e) {
	(void)e;
	if (!play_ready()) {
		return;
	}
	play_busy = true;
	gui_notify_popup("reading the liked tracks...");
	start_worker(liked_main);
}

static void sign_in_cb(lv_event_t *e) {
	(void)e;
	if (!yandex_configured()) {
		gui_notify_popup("no keys: the addon's settings.ini has none");
		return;
	}
	start_worker(sign_in_main);
}

static void reload_cb(lv_event_t *e) {
	(void)e;
	// The card may have been written while the player ran: the keys, the token,
	// a hand-edited settings.ini. Nothing here talks to the network.
	yandex_reload();
	refresh();
	gui_notify_popup("read again from the card");
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	login_stop();
	login_clear();
	yandex_logout();
	yandexplay_signed_out();
	refresh();
	gui_notify_popup("signed out");
}

// Ask for a code: one request, in the worker, answered on the GUI thread.
static void code_sign_in_cb(lv_event_t *e) {
	(void)e;
	if (!yandex_configured()) {
		gui_notify_popup("no keys: the addon's settings.ini has none");
		return;
	}
	if (login_active) {
		gui_notify_popup("a code is already waiting");
		return;
	}
	login_stop();
	memset(&login, 0, sizeof(login));
	login_clear();
	if (wait_value) {
		lv_label_set_text(wait_value, "asking for a code...");
	}
	login_active = true;
	start_worker(code_main);
}

static void stop_waiting_cb(lv_event_t *e) {
	(void)e;
	if (!login_active) {
		gui_notify_popup("nothing is waiting");
		return;
	}
	login_stop();
	login_clear();
	gui_notify_popup("stopped");
}

static void quality_cb(lv_event_t *e) {
	(void)e;
	yandex_quality_t q = yandex_get_quality();
	q = (yandex_quality_t)(((unsigned)q + 1) % YANDEX_QUALITY_COUNT);
	yandex_set_quality(q);
	refresh();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

// The page is off screen: stop polling, or it is one request every few seconds
// for a code nobody is looking at. A poll already in flight finishes and is
// dropped by poll_async().
static void unloaded_cb(lv_event_t *e) {
	(void)e;
	if (login_active) {
		login_stop();
	}
	login_clear();
}

void yandexstate_init(gui_config_t *cfg, const char *sd_root) {
	snprintf(card_root, sizeof(card_root), "%s", sd_root ? sd_root : "");
	yandexstate_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(yandexstate_screen, cfg, "Yandex Music");

	settingsrow_add(container, "Keys", &keys_value, NULL, NULL);
	settingsrow_add(container, "Account", &account_value, NULL, NULL);
	settingsrow_add(container, "Code", &code_value, NULL, NULL);
	settingsrow_add(container, "Where", &where_value, NULL, NULL);
	settingsrow_add(container, "Waiting", &wait_value, NULL, NULL);
	// A value label is created with LVGL's own "Text" until something writes to
	// it, and these three are empty until a code arrives.
	login_clear();
	settingsrow_add(container, "Token file", &token_value, NULL, NULL);
	settingsrow_add(container, "Quality", &quality_value, quality_cb, NULL);

	settingsrow_action(container, "Play My Wave", play_wave_cb, NULL);
	settingsrow_action(container, "Play liked tracks", play_liked_cb, NULL);
	settingsrow_action(container, "Sign in with a code", code_sign_in_cb, NULL);
	settingsrow_action(container, "Stop waiting", stop_waiting_cb, NULL);
	settingsrow_action(container, "Sign in from the card", sign_in_cb, NULL);
	settingsrow_action(container, "Read again from the card", reload_cb, NULL);
	settingsrow_action(container, "Sign out", sign_out_cb, NULL);

	refresh();

	// The playback watch: fetching ahead, the cache's ceiling, My Wave's next
	// batch. One second, for the life of the program, whether the page is up or
	// not -- the music outlives the screen that started it.
	play_timer = lv_timer_create(play_tick_cb, 1000, NULL);

	lv_obj_add_event_cb(yandexstate_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(yandexstate_screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
