#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dlfcn.h>

#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"

#include "common/store.h"
#include "spotify_engine.h"

#include "lvgl/lvgl.h"

// Spotify, as an addon.
//
// The addon is the interface half: a page with the engine's state on it, a
// field to paste a token into, and the three rows that drive the engine. The
// protocol half is a Rust cdylib beside it (engine/), which this opens with
// dlopen() when the page is built -- see spotify_engine.h for the contract and
// engine/README.md for why it is a library of its own.
//
// The addon does not talk to Spotify. Everything it knows about the account
// comes from the engine, as one line of JSON per event, and everything it says
// about the account goes back the same way. That is deliberate: the engine is
// the part that will be replaced when Spotify changes something, and a card
// that carries a newer engine than this addon has to keep working.
//
// What is on the card, and why:
//
//   <card>/Addons/spotify/addon.so                   this library
//   <card>/Addons/spotify/assets/spotify_engine.so   the engine, if it was built
//   <card>/Addons/spotify/session.ini                the settings, through common/store
//   <card>/Addons/spotify/token.txt                  a token too long for session.ini
//
// The session is the addon's, not the player's: an addon keeps nothing in
// device_config.ini (docs/ADDONS.md, "Where an addon keeps its settings"), and
// a card taken out of the player takes the token with it.

#define ENGINE_NAME "spotify_engine.so"
#define POLL_PERIOD_MS 250
#define EVENT_MAX 512
#define NOTE_MAX 96
#define ROW_NAME_CHARS 22
// The longest token the engine accepts (spotify_engine.h), and the room a
// buffer needs to carry one into a JSON string: every byte may need escaping.
#define TOKEN_MAX 4096
#define TOKEN_JSON_MAX (TOKEN_MAX * 2 + 64)

// ---------------------------------------------------------------------------
// The engine, as loaded
// ---------------------------------------------------------------------------

typedef struct {
	void *handle;
	int32_t (*abi)(void);
	int32_t (*init)(const char *config_json);
	int32_t (*login_start)(const char *request_json);
	int32_t (*poll)(char *buffer, uint32_t capacity);
	int32_t (*shutdown)(void);
} engine_api_t;

static gui_config_t *config;
static char addon_dir[ADDON_PATH_MAX];

static store_t session;
static char session_path[STORE_PATH_MAX];

static engine_api_t engine;
static bool engine_open;   // the five pointers are all there
static char engine_note[NOTE_MAX] = "engine not built";  // what the Engine row says
static char engine_state[32] = "unknown";
static char engine_last[192];  // the last event, for the row at the bottom

static lv_obj_t *home_screen;
static lv_obj_t *home_page;
static lv_obj_t *entry_screen;
static lv_obj_t *entry_field;
static keyboard_t *entry_keyboard;
static lv_timer_t *poll_timer;

// The labels the rows carry, kept so an event can move them without the page
// being built again: rebuilding a page from the timer would delete the row
// whose callback is running.
static lv_obj_t *row_engine_value;
static lv_obj_t *row_state_value;
static lv_obj_t *row_account_value;
static lv_obj_t *row_last_value;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// One ellipsis, three bytes of UTF-8, rather than a byte slice through the
// middle of a Cyrillic letter (the same rule as subsonic/page.c).
static const char *clip(char *out, size_t size, const char *text) {
	size_t len = text ? strlen(text) : 0;
	if (len < ROW_NAME_CHARS) {
		lv_snprintf(out, size, "%s", text ? text : "");
		return out;
	}
	lv_snprintf(out, size, "%.*s…", ROW_NAME_CHARS - 3, text);
	return out;
}

// A JSON string literal for a path. Paths on the card have no quotes in them,
// but a backslash or a quote in one would otherwise make the config unparseable
// and the engine would refuse to start with no explanation.
static void json_string(char *out, size_t size, const char *text) {
	size_t at = 0;
	if (size < 3) {
		return;
	}
	out[at++] = '"';
	for (const char *p = text; *p && at + 7 < size; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\') {
			out[at++] = '\\';
			out[at++] = (char)c;
		} else if (c < 0x20) {
			at += (size_t)lv_snprintf(out + at, size - at, "\\u%04x", c);
		} else {
			out[at++] = (char)c;
		}
	}
	out[at++] = '"';
	out[at] = '\0';
}

// ---------------------------------------------------------------------------
// The token: the one thing here that is a credential
//
// It goes through common/store like every other setting when it fits, and into
// a file of its own when it does not: store.h caps one value at
// STORE_VALUE_MAX bytes (256), and a Spotify token is routinely longer than
// that. Either way it lives in the addon's own folder on the card, never in
// device_config.ini and never in the log.
// ---------------------------------------------------------------------------

static void token_path(char *out, size_t size) {
	lv_snprintf(out, size, "%s/token.txt", addon_dir);
}

static bool token_load(char *out, size_t size) {
	char path[ADDON_PATH_MAX];
	token_path(path, sizeof(path));
	FILE *file = fopen(path, "r");
	if (file) {
		size_t count = fread(out, 1, size - 1, file);
		fclose(file);
		while (count > 0 && (out[count - 1] == '\n' || out[count - 1] == '\r')) {
			count--;
		}
		out[count] = '\0';
		return count > 0;
	}
	const char *stored = store_get(&session, "token");
	if (!stored[0]) {
		out[0] = '\0';
		return false;
	}
	lv_snprintf(out, size, "%s", stored);
	return true;
}

static void token_clear(void) {
	char path[ADDON_PATH_MAX];
	token_path(path, sizeof(path));
	unlink(path);
	store_set(&session, "token", "");
	store_set(&session, "signed_in", "");
	store_save(&session);
}

static bool token_save(const char *token) {
	size_t len = strlen(token);
	if (len < STORE_VALUE_MAX) {
		store_set(&session, "token", token);
		store_set(&session, "signed_in", "1");
		bool saved = store_save(&session);
		if (saved) {
			char path[ADDON_PATH_MAX];
			token_path(path, sizeof(path));
			unlink(path);  // a long token from before is now stale
		}
		return saved;
	}

	// Too long for the store: a file of its own, written the way store_save()
	// writes -- temporary, flushed, renamed -- so a card pulled mid-write
	// leaves the previous token whole rather than half of a new one.
	char path[ADDON_PATH_MAX];
	char tmp[ADDON_PATH_MAX];
	token_path(path, sizeof(path));
	lv_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		fprintf(stderr, "spotify: cannot write %s\n", tmp);
		return false;
	}
	bool written = write(fd, token, len) == (ssize_t)len && fsync(fd) == 0;
	close(fd);
	if (!written || rename(tmp, path) != 0) {
		unlink(tmp);
		fprintf(stderr, "spotify: cannot replace %s\n", path);
		return false;
	}
	store_set(&session, "token", "");
	store_set(&session, "signed_in", "1");
	return store_save(&session);
}

// ---------------------------------------------------------------------------
// Loading the engine
// ---------------------------------------------------------------------------

static void *open_engine_library(void) {
	// Where the packer puts it first, then the two places a hand-copied card
	// tends to have it.
	static const char *const relative[] = {"assets/" ENGINE_NAME, ENGINE_NAME, "engine/" ENGINE_NAME};
	char path[ADDON_PATH_MAX];
	bool found = false;
	for (size_t index = 0; index < sizeof(relative) / sizeof(relative[0]); index++) {
		lv_snprintf(path, sizeof(path), "%s/%s", addon_dir, relative[index]);
		if (access(path, R_OK) == 0) {
			found = true;
			break;
		}
	}
	if (!found) {
		// The ordinary case on a card whose owner never built the engine: the
		// addon still opens, and says so instead of failing to start.
		lv_snprintf(engine_note, sizeof(engine_note), "engine not built");
		fprintf(stderr, "spotify: no %s in %s: the addon runs without a protocol half\n", ENGINE_NAME, addon_dir);
		return NULL;
	}

	void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!handle) {
		const char *said = dlerror();
		lv_snprintf(engine_note, sizeof(engine_note), "dlopen: %.60s", said ? said : "failed");
		fprintf(stderr, "spotify: dlopen %s: %s\n", path, said ? said : "failed");
		return NULL;
	}
	fprintf(stderr, "spotify: engine %s\n", path);
	return handle;
}

static bool take_symbol(void *handle, const char *name, void **out) {
	*out = dlsym(handle, name);
	if (!*out) {
		lv_snprintf(engine_note, sizeof(engine_note), "engine has no %s", name);
		fprintf(stderr, "spotify: %s is not a symbol of this engine\n", name);
		return false;
	}
	return true;
}

static void engine_load(void) {
	engine.handle = open_engine_library();
	if (!engine.handle) {
		return;
	}
	bool complete = take_symbol(engine.handle, "spotify_engine_abi", (void **)&engine.abi) &&
					take_symbol(engine.handle, "spotify_engine_init", (void **)&engine.init) &&
					take_symbol(engine.handle, "spotify_engine_login_start", (void **)&engine.login_start) &&
					take_symbol(engine.handle, "spotify_engine_poll", (void **)&engine.poll) &&
					take_symbol(engine.handle, "spotify_engine_shutdown", (void **)&engine.shutdown);
	if (!complete) {
		dlclose(engine.handle);
		memset(&engine, 0, sizeof(engine));
		return;
	}

	int32_t abi = engine.abi();
	if (abi != SPOTIFY_ENGINE_ABI) {
		// Both halves are C structures and one-line JSON: an engine built
		// against another number is refused with a sentence, not a call
		// through a signature that moved.
		lv_snprintf(engine_note, sizeof(engine_note), "engine ABI %d, addon wants %d", (int)abi, SPOTIFY_ENGINE_ABI);
		fprintf(stderr, "spotify: %s\n", engine_note);
		dlclose(engine.handle);
		memset(&engine, 0, sizeof(engine));
		return;
	}

	// What the engine is told about itself. `bitrate` is one of 96, 160, 320:
	// the engine refuses anything else rather than quietly choosing, so the
	// value here comes from the store and defaults to the middle one.
	char dir_json[ADDON_PATH_MAX + 16];
	char config_json[ADDON_PATH_MAX + 160];
	json_string(dir_json, sizeof(dir_json), addon_dir);
	lv_snprintf(config_json, sizeof(config_json), "{\"session_dir\":%s,\"device_name\":\"Sonix\",\"bitrate\":%d}", dir_json,
				store_get_int(&session, "bitrate", 160));
	int32_t started = engine.init(config_json);
	if (started != SPOTIFY_ENGINE_OK) {
		lv_snprintf(engine_note, sizeof(engine_note), "engine refused its config (%d)", (int)started);
		fprintf(stderr, "spotify: %s\n", engine_note);
		engine.shutdown();
		dlclose(engine.handle);
		memset(&engine, 0, sizeof(engine));
		return;
	}

	engine_open = true;
	lv_snprintf(engine_note, sizeof(engine_note), "loaded");
}

// ---------------------------------------------------------------------------
// What the engine says
//
// One line per event, printed as `spotify: <line>` -- the same prefix the rest
// of the addons use, so the log of a session is one `grep spotify:` away. The
// line is also looked at, shallowly, for the two fields the page shows: an
// unknown event is logged and ignored rather than being an error, which is what
// lets the engine grow without this file changing.
// ---------------------------------------------------------------------------

// The value of "key":"..." in a flat event line, unescaped only for the two
// escapes that can appear in a state or a message.
static bool event_field(const char *line, const char *key, char *out, size_t size) {
	char pattern[32];
	lv_snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
	const char *at = strstr(line, pattern);
	if (!at) {
		return false;
	}
	at += strlen(pattern);
	size_t written = 0;
	while (*at && *at != '"' && written + 1 < size) {
		if (*at == '\\' && at[1]) {
			at++;
			switch (*at) {
			case 'n': out[written++] = ' '; break;
			case 't': out[written++] = ' '; break;
			default: out[written++] = *at; break;
			}
			at++;
			continue;
		}
		out[written++] = *at++;
	}
	out[written] = '\0';
	return true;
}

static void remember(const char *line) {
	char state[32];
	char message[192];
	if (event_field(line, "state", state, sizeof(state))) {
		lv_snprintf(engine_state, sizeof(engine_state), "%s", state);
	}
	if (event_field(line, "message", message, sizeof(message))) {
		lv_snprintf(engine_last, sizeof(engine_last), "%s", message);
	} else {
		lv_snprintf(engine_last, sizeof(engine_last), "%s", line);
	}
}

static void refresh_labels(void) {
	char clipped[64];
	if (row_engine_value) {
		lv_label_set_text(row_engine_value, clip(clipped, sizeof(clipped), engine_note));
	}
	if (row_state_value) {
		lv_label_set_text(row_state_value, engine_state);
	}
	if (row_last_value) {
		lv_label_set_text(row_last_value, clip(clipped, sizeof(clipped), engine_last));
	}
	if (row_account_value) {
		// The whole token, not the store's 256: see token_save().
		static char token[TOKEN_MAX + 1];
		if (token_load(token, sizeof(token))) {
			char text[48];
			lv_snprintf(text, sizeof(text), "token, %u bytes", (unsigned)strlen(token));
			lv_label_set_text(row_account_value, text);
		} else {
			lv_label_set_text(row_account_value, "signed out");
		}
	}
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;
	if (!engine_open) {
		return;
	}
	char line[EVENT_MAX];
	bool changed = false;
	for (int guard = 0; guard < 16; guard++) {
		int32_t count = engine.poll(line, sizeof(line));
		if (count == SPOTIFY_ENGINE_ERR_TOO_LONG) {
			fprintf(stderr, "spotify: an event did not fit %d bytes and was dropped\n", (int)sizeof(line));
			changed = true;
			continue;
		}
		if (count <= 0) {
			break;
		}
		fprintf(stderr, "spotify: %s\n", line);
		remember(line);
		changed = true;
	}
	if (changed) {
		refresh_labels();
	}
}

// ---------------------------------------------------------------------------
// The rows that drive the engine
// ---------------------------------------------------------------------------

// Sends one request and says on the log whether the engine took it. The answer
// itself arrives through poll_cb(): this is the same split the rest of the
// player uses, where a call returns at once and the work reports later.
static void request(const char *json, const char *what) {
	if (!engine_open) {
		gui_notify_popup("engine not built");
		return;
	}
	int32_t result = engine.login_start(json);
	fprintf(stderr, "spotify: %s -> %d\n", what, (int)result);
	if (result != SPOTIFY_ENGINE_OK) {
		gui_notify_popup("the engine refused the request");
	}
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	token_clear();
	request("{\"kind\":\"signout\"}", "sign out");
	lv_snprintf(engine_state, sizeof(engine_state), "signed out");
	refresh_labels();
	gui_notify_popup("signed out");
}

static void selftest_cb(lv_event_t *e) {
	(void)e;
	request("{\"kind\":\"selftest\"}", "self-test");
}

// The token is typed or pasted into the field on the entry screen and handed
// over here. It is not echoed to the log: only its length is, which is what
// tells a failed attempt apart from a typo.
static void entry_accept_cb(lv_event_t *e) {
	(void)e;
	const char *text = lv_textarea_get_text(entry_field);
	while (*text == ' ' || *text == '\n' || *text == '\t') {
		text++;
	}
	keyboard_set_visible(entry_keyboard, false);
	if (!text[0]) {
		switch_screen(home_screen);
		return;
	}

	if (!token_save(text)) {
		gui_notify_popup("the card refused the token");
		switch_screen(home_screen);
		return;
	}
	fprintf(stderr, "spotify: token saved, %u bytes\n", (unsigned)strlen(text));

	// Static rather than on the stack: an LVGL callback runs on the UI thread,
	// and two buffers of eight kilobytes each there is a risk with nothing on
	// the other side of it.
	static char request_json[TOKEN_JSON_MAX];
	static char escaped[TOKEN_JSON_MAX];
	size_t at = 0;
	for (const char *p = text; *p && at + 2 < sizeof(escaped); p++) {
		if (*p == '"' || *p == '\\') {
			escaped[at++] = '\\';
		}
		escaped[at++] = *p;
	}
	escaped[at] = '\0';
	lv_snprintf(request_json, sizeof(request_json), "{\"kind\":\"token\",\"token\":\"%s\"}", escaped);

	lv_textarea_set_text(entry_field, "");
	if (engine_open) {
		request(request_json, "login");
	} else {
		// The token is on the card and nothing here can use it. Saying that is
		// better than a field that silently swallowed it.
		gui_notify_popup("token saved; engine not built");
	}
	switch_screen(home_screen);
	refresh_labels();
}

// The field is offered whether or not the engine is there: the token belongs to
// the card either way, and a build that gains an engine later should not make
// its owner type a 200-character token again.
static void sign_in_cb(lv_event_t *e) {
	(void)e;
	keyboard_reset(entry_keyboard);
	lv_textarea_set_text(entry_field, "");
	keyboard_set_visible(entry_keyboard, true);
	switch_screen(entry_screen);
}

static void home_build(void) {
	if (!home_screen) {
		home_screen = lv_obj_create(NULL);
		home_page = settingsrow_page(home_screen, config, "Spotify");
	}
	lv_obj_clean(home_page);

	// The engine first: everything below it is only meaningful if this row
	// says a library was loaded.
	settingsrow_add(home_page, "Engine", &row_engine_value, NULL, NULL);
	settingsrow_add(home_page, "State", &row_state_value, NULL, NULL);
	settingsrow_add(home_page, "Account", &row_account_value, NULL, NULL);
	settingsrow_add(home_page, "Sign in", NULL, sign_in_cb, NULL);
	settingsrow_action(home_page, "Engine self-test", selftest_cb, NULL);
	settingsrow_action(home_page, "Sign out", sign_out_cb, NULL);
	settingsrow_add(home_page, "Last event", &row_last_value, NULL, NULL);

	refresh_labels();
}

// ---------------------------------------------------------------------------
// The addon
// ---------------------------------------------------------------------------

static lv_obj_t *spotify_init(const sonix_addon_host_t *host) {
	config = host->cfg;
	lv_snprintf(addon_dir, sizeof(addon_dir), "%s", host->dir);

	// The addon's own file on the card. Nothing of it goes into the player's
	// configuration: that file is the player's and is rewritten whole.
	lv_snprintf(session_path, sizeof(session_path), "%s/session.ini", addon_dir);
	store_init(&session, session_path);
	bool known = store_load(&session);
	fprintf(stderr, "spotify: session %s (%s)\n", session_path, known ? "read" : "new");

	engine_load();
	if (!engine_open) {
		// Nothing will ever move this row on a card without an engine, and
		// "unknown" would read as "the engine has not said yet".
		lv_snprintf(engine_state, sizeof(engine_state), "no engine");
	}

	// The entry screen: a field, a keyboard, and the page's own chevron for
	// backing out. Built once, like every other page here -- init() runs once.
	entry_screen = lv_obj_create(NULL);
	lv_obj_add_style(entry_screen, &theme_style_screen, 0);
	settingsrow_title(entry_screen, config, "Spotify token");
	int top = settingsrow_content_top(config);
	entry_field = lv_textarea_create(entry_screen);
	lv_textarea_set_one_line(entry_field, true);
	// A token is a credential: bullets on screen, and the tracking the bullets
	// need in this typeface (keyboard.h).
	lv_textarea_set_password_mode(entry_field, true);
	keyboard_style_password(entry_field, 0);
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
		keyboard_create(entry_screen, config->screen_width, SC(316), entry_field, NULL, "ok", entry_accept_cb, NULL);
	keyboard_set_visible(entry_keyboard, false);

	home_build();

	// The engine's events are drained from a timer rather than a callback: LVGL
	// has no OS layer here, so a worker thread that called into the interface
	// would be a SIGSEGV. Nothing is created when there is no engine, which is
	// also the cheapest way to say that a card without one costs nothing.
	if (engine_open) {
		poll_timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);
	}
	return home_screen;
}

static const sonix_addon_t spotify = {
	.api = SONIX_ADDON_API,
	.title = "Spotify",
	.init = spotify_init,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &spotify : NULL;
}

// The service half.
//
// Spotify is a service by intent: one day it will own a cache on the card, put
// its tracks in the player's queue and answer the player's questions about the
// files in it. None of that exists yet -- the transport is not built (see
// engine/README.md), so there is nothing to download, nothing to star and no
// album page. The table is exported anyway, with the id and nothing else, for
// two reasons: the manifest says "service", and a service addon without this
// symbol makes the loader log "says it is a service but exports no table" on
// every open. A NULL callback is the interface's own way of saying "this
// service does not do that yet" (addon_service.h).
static const sonix_addon_service_t spotify_service = {
	.id = "spotify",
};

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &spotify_service : NULL;
}
