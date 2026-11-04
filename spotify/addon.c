#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/keyboard.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/streaming/radio.h"

#include "lvgl/lvgl.h"

// Spotify, as an addon.
//
// The addon is the interface half. The protocol half is a separate *process*:
// `assets/spotify-engine-mips`, a static MIPS binary built from engine-mips/
// (librespot, tokio, rustls), which this addon starts with fork()/execv() the
// first time its page is opened and speaks to over a unix socket, one line of
// JSON per request.
//
// Why a process and not a library -- which is what this addon used to dlopen():
//
//   * the device's glibc is 2.16 and every modern Rust library asks for 2.17,
//     so a cdylib could only ever be built against a custom target. A static
//     mipsel-musl binary has no dynamic loader and no libc.so.6 to disagree
//     with (engine-mips/README.md).
//   * librespot wants to own the network, TLS, timers and its threads. In a
//     process of its own it is allowed to; inside the player it would compete
//     with LVGL for a single thread with no OS layer.
//   * the account lives in the engine, so a card carrying a newer engine than
//     this addon keeps working: the contract is the JSON protocol, not a header
//     with an ABI number in it.
//
// Two faces of the engine, for two callers:
//
//   unix socket  this addon: status / login / logout / search / playlists /
//                playlist_tracks / album_tracks / track.
//   loopback     the player itself, which only knows how to play a URL:
//                http://127.0.0.1:<port>/stream/<track id>. That is what
//                radio_play() is handed -- Sonix keeps owning the decoder, the
//                queue, the DSP and the DAC, exactly as for an internet station.
//
// What is on the card, and why:
//
//   <card>/Addons/spotify/addon.so                     this library
//   <card>/Addons/spotify/assets/spotify-engine-mips   the engine, if it was built
//   <card>/Addons/spotify/credentials.ini              `access_token = ...`, and nothing else
//   <card>/Addons/spotify/state/                       librespot's own cache
//
// Everything is inside the addon's own folder: the credentials beside addon.so,
// the engine's cache in its `state/` subdirectory. That last one is declared in
// addon.json -- owns_paths and cache_path -- because the player has to know it is
// this addon's before it opens this library: the factory reset empties it from
// the manifest alone, and nothing else on the card claims it. Nothing is written
// to the card's root or to the shared .local/ tree (addons/common/addonfs.h).
//
// The account, and why there is no password form here:
//
// Spotify has withdrawn the username/password flow for the client librespot
// imitates. `Session::connect` with a password answers LoginFailed on a real
// account (engine-mips/src/session.rs, "A token is tried instead of the pair"),
// so a form with two fields and a Sign in button would be a page whose one
// button cannot work. This addon therefore has no such form, and must not grow
// one back. The credential that does work is an `access_token = ...` line in
// credentials.ini, which the engine reads on login; after the first success it
// caches the session in `state/` (librespot's own Cache), so the file is needed
// once and again only after a Sign out.
//
// The addon never opens credentials.ini: the token is the engine's secret, its
// reader scrubs it out of every answer, and a copy on this side would only be a
// second place for it to reach a label or a log. The one thing this side writes
// is the commented template a fresh card gets, once, so the file the user has to
// edit exists and says what goes in it.
//
// The socket cannot live on the card -- vfat has no unix sockets -- so it goes
// to /tmp, named after the player's pid so that two player processes on one
// machine (a simulator beside a device) do not meet.

#ifdef HOST_BUILD
#define ENGINE_BINARY "spotify-engine-host"
#define ENGINE_BINARY_ALT "spotify-engine-mips"
#else
#define ENGINE_BINARY "spotify-engine-mips"
#define ENGINE_BINARY_ALT "spotify-engine-host"
#endif

// The loopback port for the audio pipe: the first of ten that nothing else on
// the device has bound. The engine binds 127.0.0.1 and no interface at all.
#define ENGINE_PORT_FIRST 34567
#define ENGINE_PORTS 10

// How long the engine may take to create its socket. It logs `listening` after
// about a second on the device; three seconds is slack, and past it the process
// is killed rather than left half-started.
#define ENGINE_SOCKET_WAIT_MS 3000
#define ENGINE_SOCKET_STEP_MS 100

// The largest answer read in one go. A page of thirty tracks with their titles
// is a few kilobytes; the engine caps a title at 160 characters, so this has
// room for a page and for its escaping.
#define ENGINE_REPLY_MAX 16384
#define ENGINE_REQUEST_MAX 512
// The answers of the account ops (`status`, `login`, `logout`) are one short
// line: state, premium, and at most one sentence of why. A buffer that size is
// enough for the resume below, and small enough for a worker's stack.
#define ENGINE_AUTH_REPLY_MAX 1024

// The engine gives an op ten seconds (RESPONSE_TIMEOUT in engine-mips/src/
// main.rs); a search also spends one metadata request per row. Wait a little
// longer than that, so its own `timeout` answer arrives instead of ours.
#define ENGINE_REPLY_TIMEOUT_MS 12000

// Rows drawn per list, and the text one row carries.
#define LIST_MAX 30
#define ITEM_TEXT_MAX 160
#define ITEM_ID_MAX 32
#define ITEM_KIND_MAX 12

// The status poll: one short line of JSON, and it is what notices that the
// engine died, that a login finished on the engine's side, or that its stream
// is no longer playing.
#define TICK_PERIOD_MS 1000

// The longest stop() lets a request finish before it says so in the log and
// joins it anyway. Every wait inside a job watches `stopping`, so this is a
// fallback and not the normal case: a worker in poll() or in the engine's
// start-up window gives up within one step of its own.
#define STOP_JOB_WAIT_MS 2000

// The longest one poll() may block while a request waits for its answer. The
// reply timeout is twelve seconds and stop() runs on the GUI thread: a single
// poll of that length would outlive the unload and leave a thread executing
// code that has been unmapped.
#define ENGINE_POLL_SLICE_MS 100

// Requests in flight at once. One is the normal number -- the tick skips while a
// job runs -- and the rest are the taps that arrive while an answer is on its
// way. Bounded because each one is a thread stop() has to join.
#define WORKER_MAX 8

// Characters of a row's name that are kept; the rest is an ellipsis.
#define ROW_NAME_CHARS 20

// ---------------------------------------------------------------------------
// What a list row is
// ---------------------------------------------------------------------------

// The fields the engine's catalog ops really answer (engine-mips/src/catalog.rs,
// `struct Item`): id, kind, title, artist, album, duration_ms, cover_url. The
// cover is ignored here -- a row has no artwork, and fetching one per row
// through listart would cost memory this device does not have to spare.
typedef struct {
	char id[ITEM_ID_MAX];
	char kind[ITEM_KIND_MAX];
	char title[ITEM_TEXT_MAX + 1];
	char artist[ITEM_TEXT_MAX + 1];
	int duration_ms;
} item_t;

typedef enum {
	JOB_STATUS = 0,
	JOB_LOGOUT,
	// logout, then login: the cached session has to go before the file is read
	// again, or a stale cache answers for the token the user just edited.
	JOB_SIGN_IN,
	// OAuth: one job asks the engine for the authorization URL (which lands in a
	// file on the card), the other hands it the code the browser was redirected
	// with. Two jobs and not one, because the user leaves the player in between.
	JOB_OAUTH_START,
	JOB_OAUTH_FINISH,
	JOB_SEARCH,
	JOB_PLAYLISTS,
	JOB_PLAYLIST_TRACKS,
	JOB_SHUTDOWN,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char request[ENGINE_REQUEST_MAX];
	int generation;
	int slot; // where this worker's thread is recorded, for stop() to join
	char title[80];
} job_t;

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static gui_config_t *config;
static char addon_dir[ADDON_PATH_MAX];
static char credentials_path[ADDON_PATH_MAX];
static char state_dir[ADDON_PATH_MAX];
static char engine_binary[ADDON_PATH_MAX + 32];
static bool engine_binary_present;

// The engine, as a process. `engine_lock` serialises everything that touches it
// -- fork, the socket, the SIGTERM. A request holds it for as long as the
// engine takes to answer, which is exactly why the interface never takes it.
static pthread_mutex_t engine_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile pid_t engine_pid;
static volatile int engine_port;
static char engine_socket[sizeof(((struct sockaddr_un *)0)->sun_path)];

// A freshly started engine holds no session: librespot keeps one in the process,
// and the addon stops the engine when the page is left (40 MB of RSS is not
// something to leave behind on this device). What makes that harmless is
// `state/credentials.json`, librespot's own cache, written by every successful
// login; this flag says the one status job that meets a new engine should try it
// instead of reporting "signed_out" to somebody who signed in an hour ago.
// Cleared when that attempt is made, so a stale cache is tried once per engine,
// not once a second.
static volatile bool resume_pending;

// An engine that still ends itself over the account: librespot calls exit(1)
// from `Session::check_catalogue` for anything that is not a Premium account
// (librespot-core 0.8.0, "TODO: logout instead of exiting"). This build does not:
// vendor/librespot-core.patch keeps the session so a free account can browse the
// catalogue. What remains here is the guard for an engine binary built before
// that patch -- a card can carry an older one -- where a login that answered
// `signed_in` is followed by a process that is gone, and every request after it
// answers "not_signed_in". Nothing is resumed for such an account until the user
// pastes another token or signs out.
static volatile bool account_refused;
#define ACCOUNT_REFUSED_REASON                                                                                         \
	"this account is not Premium: Spotify ends a free account's session, and streaming needs Premium"

// What the interface draws, and the last page of the catalog. A worker writes
// these under `data_lock` and then posts itself to the UI thread; the UI thread
// reads them only from that post or from its own timer.
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static char engine_note[64] = "engine not built";
static char account_state[32] = "unknown";
static char account_error[160];
// Reported with every status: whether the card carries an application id, and
// whether an authorization has been done. Both are what the two OAuth rows say.
static bool account_client_id;
static bool account_authorized;
// 1 premium, 0 free, -1 the engine has not said. Only a premium account can
// stream at all, so the page has to be able to say which one this is.
static int account_premium = -1;
static char list_error[160];
static item_t items[LIST_MAX];
static int item_count;

// Which catalog answer still matters. Only those are gated: a status poll that
// lands while a search is in flight must not throw the search away.
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static int list_latest;
static int jobs_running;

// Set by stop(), never cleared until it has joined everything: no new job
// starts, and every wait inside a job -- the engine's start-up window, the poll
// for its answer -- gives up promptly instead of living past the unload.
// Volatile because those two loops read it outside job_lock, on a worker.
static volatile bool stopping;

// The workers, kept joinable. `jobs_running` says how many are busy; these say
// *which threads they are*, because stop() has to wait for the last instruction
// of each, and a counter decremented a few instructions early is not that. A
// slot is only handed to a new job once its thread has been joined.
typedef struct {
	pthread_t thread;
	bool used;
	bool done; // the worker's last act before it leaves the function
} worker_slot_t;
static worker_slot_t workers[WORKER_MAX];

static lv_obj_t *home_screen;
static lv_obj_t *home_page;
static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *search_screen;
static lv_obj_t *search_field;
static keyboard_t *search_keyboard;

static lv_obj_t *row_engine_value;
static lv_obj_t *row_account_value;
static lv_obj_t *row_premium_value;
static lv_obj_t *row_token_value;
static lv_obj_t *row_client_value;
// Set when the authorization URL has been written and the code has not arrived:
// the tick looks for oauth-code.txt a few times a second... no, every five
// seconds, which is often enough for a file drop and rare enough for the card.
static bool oauth_waiting;
static int oauth_ticks;
// The one sentence a free account needs: Spotify will not stream audio to it.
// Hidden unless the engine has said premium = false.
static lv_obj_t *account_hint;

// The one-second status poll, this run's own: created in init(), deleted in
// stop(). Kept for that reason alone.
static lv_timer_t *tick_timer;

// The id the host gave this addon, copied in init(). Every top-level screen the
// addon makes is registered with sonix_addon_own_screen() under it, so the shell
// knows what to switch off and delete when the library is unloaded -- the screen
// init() returns and the two the pages switch to by themselves.
static char addon_id[ADDON_ID_MAX];

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static void copy_text(char *dst, size_t size, const char *src) {
	if (!src) {
		src = "";
	}
	snprintf(dst, size, "%s", src);
}

static void trim_text(char *text) {
	char *start = text;
	while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r') {
		start++;
	}
	if (start != text) {
		memmove(text, start, strlen(start) + 1);
	}
	size_t len = strlen(text);
	while (len > 0) {
		char last = text[len - 1];
		if (last != ' ' && last != '\t' && last != '\n' && last != '\r') {
			break;
		}
		text[--len] = '\0';
	}
}

// One ellipsis, three bytes of UTF-8, rather than a byte slice through the
// middle of a Cyrillic letter (the same rule as subsonic/page.c).
static const char *clip(char *out, size_t size, const char *text, int chars) {
	size_t len = text ? strlen(text) : 0;
	if (len < (size_t)chars) {
		lv_snprintf(out, size, "%s", text ? text : "");
		return out;
	}
	lv_snprintf(out, size, "%.*s…", chars - 3, text);
	return out;
}

static void format_duration(char *out, size_t size, int duration_ms) {
	if (duration_ms <= 0) {
		out[0] = '\0';
		return;
	}
	int seconds = duration_ms / 1000;
	lv_snprintf(out, size, "%d:%02d", seconds / 60, seconds % 60);
}

// A JSON string literal for a user-typed query: the two characters that would
// otherwise end the string early, and control characters, which have no place
// in a search term and would break the line protocol.
static void json_escape(char *out, size_t size, const char *text) {
	size_t at = 0;
	if (!size) {
		return;
	}
	for (const char *p = text; *p && at + 7 < size; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '"' || c == '\\') {
			out[at++] = '\\';
			out[at++] = (char)c;
		} else if (c < 0x20) {
			out[at++] = ' ';
		} else {
			out[at++] = (char)c;
		}
	}
	out[at] = '\0';
}

// ---------------------------------------------------------------------------
// A JSON reader, small enough to keep here
//
// The engine answers one object per line, and the addon needs three things out
// of it: whether "ok" is true, the string "state", and the objects in "items".
// A real parser would be another library on the card for that; this is a
// scanner that knows the grammar it will meet -- strings with the escapes
// serde writes, numbers, literals, and objects and arrays nested to any depth --
// and never runs past the end of what it was given. A field that is cut short
// is dropped rather than fatal: a truncated answer still fills the rows that
// made it into the buffer.
// ---------------------------------------------------------------------------

static const char *json_ws(const char *p, const char *end) {
	while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
		p++;
	}
	return p;
}

static void utf8_put(char *out, size_t *at, size_t size, unsigned code) {
	if (code < 0x80) {
		if (*at + 1 < size) {
			out[(*at)++] = (char)code;
		}
	} else if (code < 0x800) {
		if (*at + 2 < size) {
			out[(*at)++] = (char)(0xC0 | (code >> 6));
			out[(*at)++] = (char)(0x80 | (code & 0x3F));
		}
	} else {
		if (*at + 3 < size) {
			out[(*at)++] = (char)(0xE0 | (code >> 12));
			out[(*at)++] = (char)(0x80 | ((code >> 6) & 0x3F));
			out[(*at)++] = (char)(0x80 | (code & 0x3F));
		}
	}
}

// Copies the string at `p` (the opening quote) into `out` -- or only walks past
// it when `out` is NULL -- and returns the position after its closing quote.
// NULL means the string never closed inside `end`.
static const char *json_string(const char *p, const char *end, char *out, size_t size) {
	if (p >= end || *p != '"') {
		return NULL;
	}
	size_t at = 0;
	if (out && size) {
		out[0] = '\0';
	}
	p++;
	while (p < end) {
		unsigned char c = (unsigned char)*p;
		if (c == '"') {
			if (out && size) {
				out[at < size ? at : size - 1] = '\0';
			}
			return p + 1;
		}
		if (c == '\\' && p + 1 < end) {
			p++;
			char literal = 0;
			switch (*p) {
			// A control character has no place in a title; a space says the same
			// thing and cannot break the line the row is drawn on.
			case 'n':
			case 't':
			case 'r':
			case 'b':
			case 'f': literal = ' '; break;
			case '/': literal = '/'; break;
			case '"': literal = '"'; break;
			case '\\': literal = '\\'; break;
			case 'u': {
				unsigned code = 0;
				int digits = 0;
				while (digits < 4 && p + 1 < end) {
					unsigned char h = (unsigned char)p[1];
					unsigned value;
					if (h >= '0' && h <= '9') {
						value = (unsigned)(h - '0');
					} else if (h >= 'a' && h <= 'f') {
						value = (unsigned)(h - 'a' + 10);
					} else if (h >= 'A' && h <= 'F') {
						value = (unsigned)(h - 'A' + 10);
					} else {
						break;
					}
					code = (code << 4) | value;
					p++;
					digits++;
				}
				// Half of a surrogate pair: one replacement mark, no table and
				// no crash. Titles out of Spotify are UTF-8 already.
				if (code >= 0xD800 && code <= 0xDFFF) {
					literal = '?';
				} else if (out) {
					utf8_put(out, &at, size, code);
				}
				break;
			}
			default: literal = *p; break;
			}
			if (literal && out && at + 1 < size) {
				out[at++] = literal;
			}
			p++;
			continue;
		}
		if (out && at + 1 < size) {
			out[at++] = (char)c;
		}
		p++;
	}
	if (out && size) {
		out[at < size ? at : size - 1] = '\0';
	}
	return NULL;
}

// Past one value of any kind.
static const char *json_skip(const char *p, const char *end) {
	p = json_ws(p, end);
	if (p >= end) {
		return end;
	}
	if (*p == '"') {
		const char *after = json_string(p, end, NULL, 0);
		return after ? after : end;
	}
	if (*p == '{' || *p == '[') {
		char open = *p;
		char close = open == '{' ? '}' : ']';
		int depth = 0;
		while (p < end) {
			if (*p == '"') {
				const char *after = json_string(p, end, NULL, 0);
				if (!after) {
					return end;
				}
				p = after;
				continue;
			}
			if (*p == open) {
				depth++;
			} else if (*p == close) {
				depth--;
				if (depth == 0) {
					return p + 1;
				}
			}
			p++;
		}
		return end;
	}
	while (p < end && *p != ',' && *p != '}' && *p != ']') {
		p++;
	}
	return p;
}

// The value of `key` in the object at `object`, with the end of that value in
// `value_end` when the caller needs to bound a nested read.
static const char *json_member(const char *object, const char *end, const char *key, const char **value_end) {
	const char *p = json_ws(object, end);
	if (p >= end || *p != '{') {
		return NULL;
	}
	p++;
	for (;;) {
		p = json_ws(p, end);
		if (p >= end || *p == '}') {
			return NULL;
		}
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p != '"') {
			return NULL;
		}
		char name[40];
		const char *after = json_string(p, end, name, sizeof(name));
		if (!after) {
			return NULL;
		}
		p = json_ws(after, end);
		if (p >= end || *p != ':') {
			return NULL;
		}
		p = json_ws(p + 1, end);
		const char *next = json_skip(p, end);
		if (strcmp(name, key) == 0) {
			if (value_end) {
				*value_end = next;
			}
			return p;
		}
		p = next;
	}
}

static bool json_field_string(const char *object, const char *end, const char *key, char *out, size_t size) {
	const char *value_end = NULL;
	const char *value = json_member(object, end, key, &value_end);
	if (!value || *value != '"') {
		return false;
	}
	return json_string(value, value_end, out, size) != NULL;
}

static bool json_field_int(const char *object, const char *end, const char *key, int *out) {
	const char *value_end = NULL;
	const char *value = json_member(object, end, key, &value_end);
	if (!value) {
		return false;
	}
	long number = 0;
	bool digit = false;
	for (const char *p = value; p < value_end; p++) {
		if (*p >= '0' && *p <= '9') {
			number = number * 10 + (*p - '0');
			digit = true;
		} else if (*p == '-') {
			continue;
		} else {
			break;
		}
		if (number > 1000000000L) {
			return false;
		}
	}
	if (!digit) {
		return false;
	}
	*out = (int)number;
	return true;
}

static bool json_ok(const char *json) {
	const char *value = json_member(json, json + strlen(json), "ok", NULL);
	return value && strncmp(value, "true", 4) == 0;
}

static bool json_top_string(const char *json, const char *key, char *out, size_t size) {
	return json_field_string(json, json + strlen(json), key, out, size);
}

// A boolean field of a status answer: 1 true, 0 false, -1 absent.
static int json_flag(const char *json, const char *key) {
	const char *value = json_member(json, json + strlen(json), key, NULL);
	if (!value) {
		return -1;
	}
	return strncmp(value, "true", 4) == 0 ? 1 : 0;
}

// The account's `premium` flag: 1 for a premium account, 0 for a free one, -1
// when the engine did not say (signed out, or Spotify answered nothing). Only a
// premium account can stream, and the engine only puts the field in when it
// knows (engine-mips/src/session.rs, `Inner::status`).
static int json_premium(const char *json) {
	const char *value = json_member(json, json + strlen(json), "premium", NULL);
	if (!value) {
		return -1;
	}
	if (strncmp(value, "true", 4) == 0) {
		return 1;
	}
	if (strncmp(value, "false", 5) == 0) {
		return 0;
	}
	return -1;
}

static int parse_items(const char *json, item_t *out, int max) {
	const char *end = NULL;
	const char *value = json_member(json, json + strlen(json), "items", &end);
	if (!value || *value != '[') {
		return -1;
	}
	const char *p = value + 1;
	int count = 0;
	while (p < end && count < max) {
		p = json_ws(p, end);
		if (p >= end || *p == ']') {
			break;
		}
		if (*p != '{') {
			p++;
			continue;
		}
		const char *object_end = json_skip(p, end);
		item_t *item = &out[count];
		memset(item, 0, sizeof(*item));
		json_field_string(p, object_end, "id", item->id, sizeof(item->id));
		json_field_string(p, object_end, "kind", item->kind, sizeof(item->kind));
		json_field_string(p, object_end, "title", item->title, sizeof(item->title));
		json_field_string(p, object_end, "artist", item->artist, sizeof(item->artist));
		json_field_int(p, object_end, "duration_ms", &item->duration_ms);
		if (item->id[0]) {
			count++;
		}
		p = object_end;
	}
	return count;
}

// ---------------------------------------------------------------------------
// The engine, as a process
// ---------------------------------------------------------------------------

static bool file_readable(const char *path) { return path && path[0] && access(path, R_OK) == 0; }

static bool locate_engine(void) {
	// The packer's place first, then the two a hand-copied card tends to have,
	// then the other build's name: a simulator pointed at a card that carries
	// the device's engine, or the other way round.
	static const char *const places[] = {
		"assets/" ENGINE_BINARY,
		ENGINE_BINARY,
		"../" ENGINE_BINARY,
		"assets/" ENGINE_BINARY_ALT,
		ENGINE_BINARY_ALT,
		"../" ENGINE_BINARY_ALT,
	};
	for (size_t index = 0; index < sizeof(places) / sizeof(places[0]); index++) {
		char path[ADDON_PATH_MAX + 32];
		snprintf(path, sizeof(path), "%s/%s", addon_dir, places[index]);
		if (file_readable(path)) {
			copy_text(engine_binary, sizeof(engine_binary), path);
			return true;
		}
	}
	return false;
}

// A port the engine can bind: the first of ten that nothing else has. Bind and
// close rather than connect and close, because binding is exactly the question
// the engine asks next.
//
// EADDRNOTAVAIL is not "taken": this device boots with the loopback interface
// administratively down, and the engine raises it before it binds. Without this
// the probe answers "no free port" for all ten, the engine never starts, and the
// one-second status tick repeats the failure for as long as the page is open.
static int free_loopback_port(void) {
	for (int offset = 0; offset < ENGINE_PORTS; offset++) {
		int port = ENGINE_PORT_FIRST + offset;
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			return 0;
		}
		struct sockaddr_in address;
		memset(&address, 0, sizeof(address));
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		address.sin_port = htons((uint16_t)port);
		bool free_now = bind(fd, (struct sockaddr *)&address, sizeof(address)) == 0;
		int error = errno;
		close(fd);
		if (free_now || error == EADDRNOTAVAIL) {
			return port;
		}
	}
	return 0;
}

static void set_note(const char *text) {
	pthread_mutex_lock(&data_lock);
	copy_text(engine_note, sizeof(engine_note), text);
	pthread_mutex_unlock(&data_lock);
}

static void set_account(const char *state, const char *error) {
	pthread_mutex_lock(&data_lock);
	copy_text(account_state, sizeof(account_state), state);
	copy_text(account_error, sizeof(account_error), error ? error : "");
	// Whatever premium was, it was about the session that has just gone: an
	// engine that cannot answer must not leave "premium: yes" on the page.
	account_premium = -1;
	pthread_mutex_unlock(&data_lock);
}

// Whether the page was last told the account is signed in. Read by the one
// decision that needs it: an engine that died by itself right after a login.
static bool account_is_signed_in(void) {
	bool signed_in;
	pthread_mutex_lock(&data_lock);
	signed_in = strcmp(account_state, "signed_in") == 0;
	pthread_mutex_unlock(&data_lock);
	return signed_in;
}

// The sentence and the flags for an account librespot ended the process over
// (account_refused, ACCOUNT_REFUSED_REASON). Called wherever the death is
// noticed: when a later job finds the engine gone, and by the sign-in job itself
// when the verdict arrives while it is still there to report it.
static void mark_account_refused(void) {
	account_refused = true;
	resume_pending = false;
	set_note("engine exited");
	set_account("failed", ACCOUNT_REFUSED_REASON);
}

// fork + execv, never system(): the addon is a library inside the player, and a
// shell in between would be a second process to reap and a command line to
// quote. `engine_lock` is held by the caller.
static bool engine_spawn_locked(char *why, size_t why_size) {
	if (!engine_binary_present) {
		snprintf(why, why_size, "engine not built");
		return false;
	}
	if (mkdir(state_dir, 0700) != 0 && errno != EEXIST) {
		snprintf(why, why_size, "state dir: %s", strerror(errno));
		return false;
	}

	int port = free_loopback_port();
	if (port == 0) {
		snprintf(why, why_size, "no free loopback port in %d..%d", ENGINE_PORT_FIRST, ENGINE_PORT_FIRST + ENGINE_PORTS - 1);
		return false;
	}

	// vfat carries no permission bits of its own and the mount may have left
	// the binary not-executable.
	chmod(engine_binary, 0755);
	unlink(engine_socket);

	char port_text[8];
	snprintf(port_text, sizeof(port_text), "%d", port);
	char *const argv[] = {
		engine_binary,
		"--socket",
		engine_socket,
		"--http-port",
		port_text,
		"--credentials",
		credentials_path,
		"--state-dir",
		state_dir,
		NULL,
	};

	pid_t pid = fork();
	if (pid < 0) {
		snprintf(why, why_size, "fork: %s", strerror(errno));
		return false;
	}
	if (pid == 0) {
		// The engine's stderr is the player's log, prefixed `spotify-engine:`;
		// its stdin is nothing at all.
		//
		// No PR_SET_PDEATHSIG here, deliberately: the parent prctl() watches is
		// the *thread* that forked, not the process, and this fork happens on a
		// worker that exits a moment later -- the engine would be killed by its
		// own request. The engine's life is managed explicitly instead: the
		// tick below, and spotify_stop().
		int null_fd = open("/dev/null", O_RDONLY);
		if (null_fd >= 0) {
			dup2(null_fd, STDIN_FILENO);
			if (null_fd > STDERR_FILENO) {
				close(null_fd);
			}
		}
		execv(engine_binary, argv);
		fprintf(stderr, "spotify: exec %s: %s\n", engine_binary, strerror(errno));
		_exit(127);
	}

	engine_pid = pid;
	engine_port = port;
	fprintf(stderr, "spotify: engine pid %d, socket %s, http port %d\n", (int)pid, engine_socket, port);

	int waited = 0;
	// `stopping` as well as the clock: a stop() that arrives while the engine is
	// still coming up must not wait out the whole three seconds before it can
	// join this worker -- the failure path below kills what was started.
	while (waited < ENGINE_SOCKET_WAIT_MS && !stopping) {
		if (access(engine_socket, F_OK) == 0) {
			char note[48];
			snprintf(note, sizeof(note), "engine loaded (%d)", port);
			set_note(note);
			snprintf(why, why_size, "listening");
			// A new process, an empty account: the first status that meets it
			// signs it in from the cache (resume_locked).
			resume_pending = true;
			return true;
		}
		int status = 0;
		if (waitpid(pid, &status, WNOHANG) == pid) {
			engine_pid = 0;
			engine_port = 0;
			snprintf(why, why_size, "engine exited while starting (%d)",
					 WIFEXITED(status) ? WEXITSTATUS(status) : -1);
			set_note("engine exited");
			return false;
		}
		usleep(ENGINE_SOCKET_STEP_MS * 1000);
		waited += ENGINE_SOCKET_STEP_MS;
	}

	kill(pid, SIGTERM);
	int status = 0;
	for (int i = 0; i < 10 && waitpid(pid, &status, WNOHANG) != pid; i++) {
		usleep(100000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	engine_pid = 0;
	engine_port = 0;
	unlink(engine_socket);
	snprintf(why, why_size, "engine did not listen within %d ms", ENGINE_SOCKET_WAIT_MS);
	set_note("engine did not start");
	return false;
}

// The engine is up, or it is started now. `engine_lock` is held by the caller.
static bool engine_ensure_locked(char *why, size_t why_size) {
	if (engine_pid > 0) {
		int status = 0;
		if (waitpid(engine_pid, &status, WNOHANG) != engine_pid) {
			return true;
		}
		fprintf(stderr, "spotify: engine %d exited on its own\n", (int)engine_pid);
		engine_pid = 0;
		engine_port = 0;
		unlink(engine_socket);
		set_note("engine exited");
		// Nothing here ends the engine without asking -- every stop is an
		// explicit SIGTERM -- so an engine that went away by itself went away
		// inside librespot. The one place it does that is `check_catalogue`,
		// which calls exit(1) for an account that is not Premium
		// (librespot-core 0.8.0, "TODO: logout instead of exiting"). A second
		// ago the page was told "signed_in", and every request after it answered
		// "not_signed_in"; that is worth a sentence, and a sign-in that is not
		// attempted again.
		if (account_is_signed_in()) {
			mark_account_refused();
		}
	}
	return engine_spawn_locked(why, why_size);
}

// SIGTERM, then SIGKILL for a process that ignored it, then the socket file a
// killed engine cannot remove itself. `engine_lock` is held by the caller.
static void engine_stop_locked(void) {
	if (engine_pid > 0) {
		pid_t pid = engine_pid;
		kill(pid, SIGTERM);
		bool reaped = false;
		int status = 0;
		// The engine removes its socket and exits on SIGTERM within a few
		// milliseconds; three seconds is where waiting stops being a shutdown
		// and becomes a hang. Half a second once the addon is being unloaded,
		// whenever the flag arrives: SIGKILL follows, and the GUI thread is
		// inside stop() waiting for this call.
		for (int i = 0; i < 30; i++) {
			if (waitpid(pid, &status, WNOHANG) == pid) {
				reaped = true;
				break;
			}
			if (stopping && i >= 5) {
				break;
			}
			usleep(100000);
		}
		if (!reaped) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			fprintf(stderr, "spotify: engine %d ignored SIGTERM\n", (int)pid);
		}
		engine_pid = 0;
		engine_port = 0;
		fprintf(stderr, "spotify: engine stopped\n");
	}
	resume_pending = false;
	unlink(engine_socket);
}

// ---------------------------------------------------------------------------
// The stop half
//
// There is no atexit() any more, and there must not be: the library is
// dlopen()ed from the card and dlclose()d when the card leaves or its files
// change, and a handler registered with the process would later call into a page
// that is no longer mapped. What the handler did -- kill the engine and remove
// its socket -- is what stop() does now, at the only moment it is safe.
//
// Running the interface without an engine to talk to is the price of an unload:
// the engine is a child process whose lifetime is this library's, and a library
// that goes away while its child runs leaves a process nobody can ever name
// (its socket is gone with the library). So the engine is stopped here, and a
// stream that was playing from it stops with it: the alternative is a decoder
// reading a pipe whose other end belongs to a library that no longer exists.
// ---------------------------------------------------------------------------

// One request line in, one answer line out: the engine speaks exactly one
// exchange per connection and then closes it.
static bool engine_exchange_locked(const char *request, char *reply, size_t size, char *why, size_t why_size) {
	if (size < 2) {
		snprintf(why, why_size, "reply buffer too small");
		return false;
	}
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		snprintf(why, why_size, "socket: %s", strerror(errno));
		return false;
	}
	struct sockaddr_un address;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	snprintf(address.sun_path, sizeof(address.sun_path), "%s", engine_socket);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		snprintf(why, why_size, "engine: %s", strerror(errno));
		close(fd);
		return false;
	}

	size_t length = strlen(request);
	if (write(fd, request, length) != (ssize_t)length || write(fd, "\n", 1) != 1) {
		snprintf(why, why_size, "engine: %s", strerror(errno));
		close(fd);
		return false;
	}

	size_t used = 0;	int waited_ms = 0;
	for (;;) {
		if (stopping) {
			// stop() is on the GUI thread waiting to join this worker: give up
			// now rather than leave a thread in a library that is going away.
			snprintf(why, why_size, "the addon is stopping");
			break;
		}
		// In slices, not one twelve-second wait. The whole reply budget is still
		// ENGINE_REPLY_TIMEOUT_MS, but `stopping` is looked at every slice.
		int slice = ENGINE_REPLY_TIMEOUT_MS - waited_ms;
		if (slice > ENGINE_POLL_SLICE_MS) {
			slice = ENGINE_POLL_SLICE_MS;
		}
		struct pollfd watching = {.fd = fd, .events = POLLIN, .revents = 0};
		int ready = poll(&watching, 1, slice);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			snprintf(why, why_size, "poll: %s", strerror(errno));
			break;
		}
		if (ready == 0) {
			waited_ms += slice;
			if (waited_ms < ENGINE_REPLY_TIMEOUT_MS) {
				continue;
			}
			snprintf(why, why_size, "the engine did not answer");
			break;
		}
		ssize_t taken = read(fd, reply + used, size - 1 - used);
		if (taken < 0) {
			if (errno == EINTR) {
				continue;
			}
			snprintf(why, why_size, "read: %s", strerror(errno));
			break;
		}
		if (taken == 0) {
			break;
		}
		char *newline = memchr(reply + used, '\n', (size_t)taken);
		used += (size_t)taken;
		if (newline) {
			used = (size_t)(newline - reply);
			break;
		}
		if (used + 1 >= size) {
			// Longer than the buffer: keep what arrived and let the reader drop
			// the item the cut lands in.
			break;
		}
	}
	close(fd);
	reply[used] = '\0';
	if (used == 0) {
		if (!why[0]) {
			snprintf(why, why_size, "the engine answered nothing");
		}
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// The session an engine restart does not have
//
// librespot keeps the account in the process: stop the engine, and the session
// goes with it. It does write `state/credentials.json` on every successful
// login, and that file is what the next engine can sign in from without the
// card's token -- but only if somebody asks it to. Nobody did, so leaving the
// addon and coming back (or the engine being killed under memory pressure) left
// a page that said "signed_out" and a search that answered "not_signed_in" to a
// user who had signed in a minute ago.
//
// So: the first status a fresh engine answers is followed by one login, and a
// catalogue request that meets a dead session is retried behind one. Both are
// the same engine op, and both are bounded -- once per engine process, and once
// per request.
// ---------------------------------------------------------------------------

// True when the answer carries "state":"signed_out".
static bool reply_state_is(const char *reply, const char *state) {
	char got[32] = "";
	return json_top_string(reply, "state", got, sizeof(got)) && strcmp(got, state) == 0;
}

// True when the answer is a refusal whose error is exactly `error`.
static bool reply_error_is(const char *reply, const char *error) {
	if (json_ok(reply)) {
		return false;
	}
	char got[160] = "";
	return json_top_string(reply, "error", got, sizeof(got)) && strcmp(got, error) == 0;
}

// librespot's own cache of the last login: what `login` reads before it ever
// looks at the card's file.
static bool cached_login(void) {
	if (!state_dir[0]) {
		return false;
	}
	char path[sizeof(state_dir) + 20];
	snprintf(path, sizeof(path), "%s/credentials.json", state_dir);
	return file_readable(path);
}

// Signs the engine in from the cache, and from the card's file when the cached
// token has gone stale (Spotify's tokens last about an hour and librespot saves
// the one it was given; a logout is what makes it read the file again, and it
// is safe here because the cache has just been proved useless).
//
// True when the engine answered at all -- `reply` then holds the last answer,
// signed in or refused, so the page can say why. False only when there was
// nothing to resume from, and then nothing was asked. `engine_lock` is held by
// the caller.
static bool resume_locked(char *reply, size_t size, char *why, size_t why_size) {
	if (!cached_login()) {
		return false;
	}
	if (!engine_exchange_locked("{\"op\":\"login\"}", reply, size, why, why_size)) {
		return false;
	}
	if (reply_state_is(reply, "signed_in")) {
		return true;
	}
	fprintf(stderr, "spotify: the cached login was refused, trying the card's file\n");
	if (!engine_exchange_locked("{\"op\":\"logout\"}", reply, size, why, why_size)) {
		return false;
	}
	return engine_exchange_locked("{\"op\":\"login\"}", reply, size, why, why_size);
}

// ---------------------------------------------------------------------------
// The answers
// ---------------------------------------------------------------------------

// {"ok":true,"protocol":1,"state":"signed_out","premium":true}
// {"ok":false,"protocol":1,"error":"not_signed_in"}
static void take_status(const char *reply, const char *why) {
	char state[32];
	char error[160];
	state[0] = '\0';
	error[0] = '\0';
	if (!reply[0]) {
		set_account("unreachable", why);
		return;
	}
	if (json_top_string(reply, "state", state, sizeof(state))) {
		json_top_string(reply, "error", error, sizeof(error));
		int premium = json_premium(reply);
		int client_id = json_flag(reply, "client_id");
		int authorized = json_flag(reply, "authorized");
		// An account librespot ended the process over stays ended: the fresh
		// engine that replaced it answers "signed_out", and letting that replace
		// the sentence would put back the very state this explains.
		if (account_refused && strcmp(state, "signed_out") == 0) {
			return;
		}
		pthread_mutex_lock(&data_lock);
		copy_text(account_state, sizeof(account_state), state);
		copy_text(account_error, sizeof(account_error), error);
		account_premium = premium;
		account_client_id = client_id == 1;
		account_authorized = authorized == 1;
		pthread_mutex_unlock(&data_lock);
		return;
	}
	if (!json_ok(reply)) {
		if (!json_top_string(reply, "error", error, sizeof(error))) {
			copy_text(error, sizeof(error), "the engine refused the request");
		}
		set_account("failed", error);
		return;
	}
	set_account("unknown", "");
}

// The caller holds data_lock: this is the one place the rows are replaced.
static void take_items(const char *reply, char *why, size_t why_size) {
	int count = parse_items(reply, items, LIST_MAX);
	if (count < 0) {
		char error[160];
		error[0] = '\0';
		if (!json_ok(reply) && json_top_string(reply, "error", error, sizeof(error))) {
			snprintf(why, why_size, "%s", error);
		} else {
			snprintf(why, why_size, "the engine answered without a list");
		}
		item_count = 0;
		copy_text(list_error, sizeof(list_error), why);
		return;
	}
	item_count = count;
	list_error[0] = '\0';
}

// ---------------------------------------------------------------------------
// The worker
//
// One thread per request, and every request serialised on engine_lock: the
// engine speaks one line per connection, so a second connection while the first
// waits would only queue behind it inside the engine anyway. The UI thread
// never blocks on any of this -- it gets the answer through gui_post().
// ---------------------------------------------------------------------------

static void job_finished(void *user);

static bool is_list_job(job_kind_t kind) {
	return kind == JOB_SEARCH || kind == JOB_PLAYLISTS || kind == JOB_PLAYLIST_TRACKS;
}

static const char *job_name(job_kind_t kind) {
	switch (kind) {
	case JOB_STATUS: return "status";
	case JOB_LOGOUT: return "logout";
	case JOB_SIGN_IN: return "sign in from the card";
	case JOB_OAUTH_START: return "sign in with Spotify";
	case JOB_OAUTH_FINISH: return "finish sign-in";
	case JOB_SEARCH: return "search";
	case JOB_PLAYLISTS: return "playlists";
	case JOB_PLAYLIST_TRACKS: return "playlist_tracks";
	case JOB_SHUTDOWN: return "shutdown";
	}
	return "?";
}

static void *worker(void *argument) {
	job_t *job = argument;
	// Read before anything can free the job: the last thing this function does
	// is report to the slot that the thread is leaving.
	int slot = job->slot;
	char *reply = calloc(1, ENGINE_REPLY_MAX);
	char why[160];
	why[0] = '\0';
	bool post = true;
	// The sign-in below can see the engine end under a login that read
	// `signed_in`; then the answer is stale and the page keeps the sentence that
	// explains it (mark_account_refused).
	bool refused = false;

	if (!reply) {
		set_account("failed", "out of memory");
		goto finish;
	}

	// A catalog answer that a newer request replaced is not worth the engine's
	// time, let alone the screen's.
	if (is_list_job(job->kind)) {
		pthread_mutex_lock(&job_lock);
		bool stale = job->generation != list_latest;
		pthread_mutex_unlock(&job_lock);
		if (stale) {
			post = false;
			goto finish;
		}
	}

	pthread_mutex_lock(&engine_lock);
	if (job->kind == JOB_SHUTDOWN) {
		engine_stop_locked();
		pthread_mutex_unlock(&engine_lock);
		post = false;
		goto finish;
	}
	bool up = engine_ensure_locked(why, sizeof(why));
	bool answered = false;
	if (up && job->kind == JOB_SIGN_IN) {
		// The cache first. `login` reads credentials.json before the INI file
		// (session.rs, `connect`), so a token the user has just edited would be
		// ignored while a cached session from the previous one still answers.
		// `logout` is the engine's op that drops both the session and that file;
		// only then is `login` guaranteed to read the file.
		char discarded[256];
		char why_logout[160];
		why_logout[0] = '\0';
		resume_pending = false;
		// A new token, or the same one again: a new question for Spotify, and
		// the refusal of the previous attempt must not outlive it.
		account_refused = false;
		if (!engine_exchange_locked("{\"op\":\"logout\"}", discarded, sizeof(discarded), why_logout,
									sizeof(why_logout))) {
			fprintf(stderr, "spotify: sign in: the logout before it did not answer: %s\n", why_logout);
		}
		answered = engine_exchange_locked(job->request, reply, ENGINE_REPLY_MAX, why, sizeof(why));

		// A login that answers `signed_in` and an engine that then ends itself
		// look the same from here for about a second: librespot ends the process,
		// from its own dispatch task, for an account that is not Premium. So when
		// the answer did not say premium, wait for that verdict here rather than
		// raising a "signed in" the next status would have to take back. A
		// Premium answer skips the wait: it is the one case librespot does not
		// end.
		if (answered && reply_state_is(reply, "signed_in") && json_premium(reply) != 1) {
			int status = 0;
			for (int i = 0; i < 15; i++) {
				if (engine_pid > 0 && waitpid(engine_pid, &status, WNOHANG) == engine_pid) {
					fprintf(stderr, "spotify: the engine ended right after the login (status %d)\n",
							WIFEXITED(status) ? WEXITSTATUS(status) : -1);
					engine_pid = 0;
					engine_port = 0;
					unlink(engine_socket);
					mark_account_refused();
					// The login's own answer is stale now: the page keeps the
					// sentence mark_account_refused() put in its place.
					refused = true;
					break;
				}
				usleep(100000);
			}
		}
	} else if (up) {
		answered = engine_exchange_locked(job->request, reply, ENGINE_REPLY_MAX, why, sizeof(why));
	}

	// The account, before the page is told anything: a fresh engine has no
	// session and answers `signed_out`, which is not news to somebody who signed
	// in already. One resume per engine process -- `resume_pending` is set by the
	// spawn that made it fresh -- so a cache that really is stale costs one
	// attempt, not one a second.
	if (answered && job->kind == JOB_STATUS && resume_pending) {
		resume_pending = false;
		if (!account_refused && reply_state_is(reply, "signed_out")) {
			char resumed[ENGINE_AUTH_REPLY_MAX];
			if (resume_locked(resumed, sizeof(resumed), why, sizeof(why))) {
				snprintf(reply, ENGINE_REPLY_MAX, "%s", resumed);
			}
		}
	}
	if (answered && job->kind == JOB_LOGOUT) {
		// A sign-out is a decision: the next engine must not quietly sign back in.
		resume_pending = false;
		account_refused = false;
	}

	// A catalogue request answered by a session that died under it (the engine's
	// connection to Spotify dropped, or a newer engine took the socket): sign in
	// again from what is on the card and ask the same question once more. A list
	// that says "not_signed_in" to a signed-in user is never the right answer.
	if (answered && is_list_job(job->kind) && !account_refused && reply_error_is(reply, "not_signed_in")) {
		char resumed[ENGINE_AUTH_REPLY_MAX];
		if (resume_locked(resumed, sizeof(resumed), why, sizeof(why)) && reply_state_is(resumed, "signed_in")) {
			answered = engine_exchange_locked(job->request, reply, ENGINE_REPLY_MAX, why, sizeof(why));
		}
	}
	pthread_mutex_unlock(&engine_lock);

	if (!answered) {
		fprintf(stderr, "spotify: %s: %s\n", job_name(job->kind), why);
		if (is_list_job(job->kind)) {
			pthread_mutex_lock(&data_lock);
			item_count = 0;
			copy_text(list_error, sizeof(list_error), why);
			pthread_mutex_unlock(&data_lock);
		} else if (!engine_binary_present) {
			// A card with no engine is a state of the card, not a failure of
			// the account: the row above already says "engine not built".
			set_account("no engine", why);
		} else {
			set_account("failed", why);
		}
	} else if (job->kind == JOB_STATUS || job->kind == JOB_LOGOUT || job->kind == JOB_SIGN_IN ||
			   job->kind == JOB_OAUTH_FINISH) {
		// All of these lock data_lock themselves: it is not recursive.
		if (!refused) {
			take_status(reply, why);
		}
	} else if (job->kind == JOB_OAUTH_START) {
		// The answer is not a status: `{"ok":true,"state":"waiting_for_authorization"}`.
		// Only a failure has something to say, and it is worth saying loudly --
		// the usual one is a card with no `client_id` in credentials.ini.
		if (!json_ok(reply)) {
			char error[160] = "";
			if (!json_top_string(reply, "error", error, sizeof(error))) {
				copy_text(error, sizeof(error), "the engine refused");
			}
			set_account("failed", error);
		}
	} else if (is_list_job(job->kind)) {
		if (account_refused && reply_error_is(reply, "not_signed_in")) {
			// The list cannot say more than the account already did: an engine
			// that was refused will answer this way to every request.
			pthread_mutex_lock(&data_lock);
			item_count = 0;
			copy_text(list_error, sizeof(list_error), ACCOUNT_REFUSED_REASON);
			pthread_mutex_unlock(&data_lock);
		} else {
			pthread_mutex_lock(&data_lock);
			take_items(reply, why, sizeof(why));
			pthread_mutex_unlock(&data_lock);
		}
	}

finish:
	free(reply);
	pthread_mutex_lock(&job_lock);
	jobs_running--;
	bool going = stopping;
	pthread_mutex_unlock(&job_lock);
	if (going) {
		// The addon is being unloaded: the answer has no interface left to go
		// to, and the job must not outlive the join below.
		free(job);
	} else if (!post || !gui_post(job_finished, job)) {
		free(job);
	}
	// The last statement, and the reason the interface keeps these threads
	// joinable: stop() waits on it to know this thread has left every line of
	// this library, which is more than `jobs_running` can say.
	pthread_mutex_lock(&job_lock);
	workers[slot].done = true;
	pthread_mutex_unlock(&job_lock);
	return NULL;
}

// Joins the workers that have said they are done, and only those: a live one is
// still working and start_job() has no business blocking on it. Called from the
// GUI thread, never under job_lock -- a thread being joined takes that lock on
// its way out.
static void workers_reap(void) {
	pthread_t finished[WORKER_MAX];
	int count = 0;
	pthread_mutex_lock(&job_lock);
	for (int i = 0; i < WORKER_MAX; i++) {
		if (workers[i].used && workers[i].done) {
			finished[count++] = workers[i].thread;
			workers[i].used = false;
			workers[i].done = false;
		}
	}
	pthread_mutex_unlock(&job_lock);
	for (int i = 0; i < count; i++) {
		pthread_join(finished[i], NULL);
	}
}

// Whether every worker has reported that it is done -- which is one statement
// before the thread leaves, so it is the count the join can be built on.
static bool workers_all_done(void) {
	bool done = true;
	pthread_mutex_lock(&job_lock);
	for (int i = 0; i < WORKER_MAX; i++) {
		if (workers[i].used && !workers[i].done) {
			done = false;
			break;
		}
	}
	pthread_mutex_unlock(&job_lock);
	return done;
}

// Joins every worker still recorded, finished or not. stop() has already asked
// them to give up, so the wait here is the tail of a poll() or of a usleep().
static void workers_join_all(void) {
	pthread_t joining[WORKER_MAX];
	int count = 0;
	pthread_mutex_lock(&job_lock);
	for (int i = 0; i < WORKER_MAX; i++) {
		if (!workers[i].used) {
			continue;
		}
		if (!workers[i].done) {
			fprintf(stderr, "spotify: stop: a worker had not finished on its own; joining it\n");
		}
		joining[count++] = workers[i].thread;
		// Nobody can take the slot while this runs: stop() clears `stopping`
		// only after the joins below.
		workers[i].used = false;
	}
	pthread_mutex_unlock(&job_lock);
	for (int i = 0; i < count; i++) {
		pthread_join(joining[i], NULL);
	}
	// After the joins, not before: the worker's last act is a write into its own
	// slot, and it has now left.
	memset(workers, 0, sizeof(workers));
}

static void start_job(job_kind_t kind, const char *request, const char *title) {
	pthread_mutex_lock(&job_lock);
	bool going = stopping;
	pthread_mutex_unlock(&job_lock);
	if (going) {
		// stop() has begun: nothing new may touch the engine or the screens.
		return;
	}
	// The threads of the requests that have answered are collected before a new
	// one is made, so that a session of a thousand status polls leaves a handful
	// of slots rather than a thousand unreaped threads.
	workers_reap();

	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		gui_notify_popup("out of memory");
		return;
	}
	job->kind = kind;
	copy_text(job->request, sizeof(job->request), request);
	copy_text(job->title, sizeof(job->title), title);

	int slot = -1;
	pthread_mutex_lock(&job_lock);
	if (!stopping) {
		for (int i = 0; i < WORKER_MAX; i++) {
			if (!workers[i].used) {
				workers[i].used = true;
				workers[i].done = false;
				job->slot = i;
				slot = i;
				break;
			}
		}
	}
	if (slot >= 0) {
		if (is_list_job(kind)) {
			job->generation = ++list_latest;
		}
		jobs_running++;
	}
	pthread_mutex_unlock(&job_lock);

	if (slot < 0) {
		// Either stop() began between the two checks, or every slot is taken by
		// a request still waiting for its answer.
		fprintf(stderr, "spotify: %s [%s] dropped: no worker free\n", job_name(kind), title);
		free(job);
		return;
	}

	fprintf(stderr, "spotify: job %s [%s] %s\n", job_name(kind), job->title, job->request);

	// Joinable, not detached: stop() has to be able to wait for the thread, and
	// a detached one can be a few instructions from returning when dlclose()
	// unmaps the code it is returning through.
	if (pthread_create(&workers[slot].thread, NULL, worker, job) != 0) {
		pthread_mutex_lock(&job_lock);
		jobs_running--;
		memset(&workers[slot], 0, sizeof(workers[slot]));
		pthread_mutex_unlock(&job_lock);
		free(job);
		gui_notify_popup("could not start a worker thread");
		return;
	}
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

// Every top-level screen this addon makes is registered the moment it exists.
// The shell keeps the list and, on unload, moves the display off whichever of
// them is up, forgets them in the back-stack and in the player sheet, and
// deletes them after stop(); without the list it knows only the screen init()
// returned, and a page the addon opened by itself would be left on the display
// with its callbacks in a library that is being unmapped.
static void own_screen(lv_obj_t *screen) {
	if (screen && addon_id[0]) {
		sonix_addon_own_screen(addon_id, screen);
	}
}

// The same three screens again, from the first time the shell actually shows
// home. Not redundant in this build: lib_open() takes init()'s return value as
// the first entry of its own list *after* init() returned, and resets the count
// to one as it does -- so what the three calls above registered during init() is
// gone by the time the tile is on screen. Switching to home is the first moment
// that bookkeeping is finished, and the call is a set (a screen already known is
// ignored), so this costs three strcmps and works whether or not that reset stays.
static bool screens_owned;

static void own_all_screens(void) {
	own_screen(home_screen);
	own_screen(list_screen);
	own_screen(search_screen);
}

static void home_shown_cb(lv_event_t *e) {
	(void)e;
	if (screens_owned) {
		return;
	}
	screens_owned = true;
	own_all_screens();
}

// A page that scrolls, unlike settingsrow_page(): a list of search results is
// taller than the panel.
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

static void heading_set(const char *text) {
	lv_obj_t *heading = settingsrow_page_title(list_screen);
	if (heading) {
		lv_label_set_text(heading, text && text[0] ? text : "Spotify");
	}
}

static void list_show_message(const char *text) {
	lv_obj_clean(list_container);
	lv_obj_t *label = lv_label_create(list_container);
	lv_label_set_text(label, text);
	lv_obj_add_style(label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(label, &font_ui_24, 0);
	lv_obj_set_width(label, lv_pct(100));
	lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}

// ---------------------------------------------------------------------------
// Playing a track
//
// The track becomes a radio station, because that is the player's one public
// way to hand a URL to the audio path: radio.c owns the transport, the codec
// detection and the now-playing line, and an addon is not going to grow a
// second one. What the user sees is a station called "artist - title". The URL
// is the engine's loopback pipe, and the decoder Sonix picks from the bytes
// (FLAC, from `/stream/<id>` at the default 320 kbps, for an account that can
// have it) works like it does for an internet station.
// ---------------------------------------------------------------------------

// Whether the account was last reported as premium by the engine: 1 premium, 0
// free, -1 not said. A free account browses the catalogue (the engine keeps the
// session, see vendor/librespot-core.patch) and cannot play anything, because
// Spotify refuses the audio key -- so a tap has to say that rather than start a
// stream that would only end in "the track could not be played".
static int account_premium_now(void) {
	int premium;
	pthread_mutex_lock(&data_lock);
	premium = account_premium;
	pthread_mutex_unlock(&data_lock);
	return premium;
}

static void play_item(int index) {
	if (index < 0 || index >= item_count) {
		return;
	}
	if (account_premium_now() == 0) {
		gui_notify_popup("Spotify Premium is required to play");
		return;
	}
	if (engine_pid <= 0 || engine_port == 0) {
		gui_notify_popup("the engine is not running");
		return;
	}
	radio_station_t station;
	memset(&station, 0, sizeof(station));
	if (items[index].artist[0] && items[index].title[0]) {
		lv_snprintf(station.name, sizeof(station.name), "%s - %s", items[index].artist, items[index].title);
	} else if (items[index].title[0]) {
		lv_snprintf(station.name, sizeof(station.name), "%s", items[index].title);
	} else {
		lv_snprintf(station.name, sizeof(station.name), "Spotify");
	}
	// No uuid: this station is not in radio-browser's directory, and radio.c
	// would report every uuid it was given back to it as a listen.
	station.bitrate = 320;
	lv_snprintf(station.url, sizeof(station.url), "http://127.0.0.1:%d/stream/%s", (int)engine_port, items[index].id);

	const char *problem = radio_station_problem(&station);
	if (problem) {
		gui_notify_popup(problem);
		return;
	}
	fprintf(stderr, "spotify: play %s\n", station.url);
	if (!radio_play(&station)) {
		gui_notify_popup("the player could not start the stream");
	}
}

// ---------------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------------

static void open_list(const char *title, const char *request, job_kind_t kind);

static void row_item_cb(lv_event_t *e) {
	int index = (int)(intptr_t)lv_event_get_user_data(e);
	if (index < 0 || index >= item_count) {
		return;
	}
	const item_t *item = &items[index];
	if (strcmp(item->kind, "playlist") == 0 || strcmp(item->kind, "album") == 0) {
		// A playlist or an album replaces the list with its tracks: one page,
		// one level, and back leaves the addon the way it always does. A stack
		// of levels would be the nicety, but the engine pages by offset and
		// this page does not, so there is nothing to come back to.
		char request[ENGINE_REQUEST_MAX];
		char heading[96];
		lv_snprintf(request, sizeof(request), "{\"op\":\"%s\",\"id\":\"%s\",\"offset\":0,\"limit\":%d}",
					strcmp(item->kind, "album") == 0 ? "album_tracks" : "playlist_tracks", item->id, LIST_MAX);
		lv_snprintf(heading, sizeof(heading), "%.70s", item->title);
		heading_set(heading);
		list_show_message("loading…");
		start_job(JOB_PLAYLIST_TRACKS, request, heading);
		return;
	}
	if (strcmp(item->kind, "artist") == 0) {
		gui_notify_popup("artists cannot be opened here");
		return;
	}
	play_item(index);
}

// The engine's failure codes in words a user can act on. Everything else passes
// through as it came: a wrong sentence is worse than a code that names itself.
static const char *error_text(const char *error) {
	if (strcmp(error, "token_expired") == 0) {
		return "the access_token has expired (they last about an hour): paste a new one into the token file and "
			   "tap \"Sign in from the card\"";
	}
	if (strcmp(error, "catalog_failed") == 0) {
		return "Spotify did not answer with that list";
	}
	if (strcmp(error, "no_client_id") == 0) {
		return "put client_id = <your Spotify app Client ID> into Addons/spotify/credentials.ini";
	}
	if (strcmp(error, "no_code_file") == 0) {
		return "no Addons/spotify/oauth-code.txt yet: authorize in the browser, then save the redirect URL "
			   "(or just the code) into that file";
	}
	if (strcmp(error, "no_code_in_file") == 0 || strcmp(error, "no_login_in_progress") == 0) {
		return "tap \"Sign in with Spotify\" first: a code only works with the URL it was issued for";
	}
	if (strcmp(error, "oauth_refused") == 0) {
		return "Spotify refused the code (it lives a minute or two): tap \"Sign in with Spotify\" again";
	}
	return error;
}

static void list_show(void) {
	pthread_mutex_lock(&data_lock);
	int count = item_count;
	char error[160];
	copy_text(error, sizeof(error), list_error);
	pthread_mutex_unlock(&data_lock);

	if (count <= 0) {
		list_show_message(error[0] ? error_text(error) : "nothing here");
		return;
	}

	lv_obj_clean(list_container);
	for (int i = 0; i < count; i++) {
		char name[ROW_NAME_CHARS * 4];
		char value[48];
		lv_obj_t *value_label = NULL;
		clip(name, sizeof(name), items[i].title[0] ? items[i].title : "(no title)", ROW_NAME_CHARS);
		format_duration(value, sizeof(value), items[i].duration_ms);
		if (!value[0] && items[i].artist[0]) {
			// A playlist or an album has no length: the owner is what the right
			// column can carry instead, and it is what tells two playlists apart.
			clip(value, sizeof(value), items[i].artist, 14);
		}
		settingsrow_add(list_container, name, &value_label, row_item_cb, (void *)(intptr_t)i);
		// Always set, even empty: LVGL gives a fresh label the text "Text", and a
		// playlist (which has no duration) would otherwise wear that word instead
		// of nothing at all.
		if (value_label) {
			lv_label_set_text(value_label, value);
		}
	}
}

static void open_list(const char *title, const char *request, job_kind_t kind) {
	heading_set(title);
	list_show_message("loading…");
	switch_screen(list_screen);
	start_job(kind, request, title);
}

// ---------------------------------------------------------------------------
// The home page
// ---------------------------------------------------------------------------

static void refresh_home(void) {
	if (!home_screen) {
		return;
	}
	char note[64];
	char state[32];
	char error[160];
	int premium = -1;
	pthread_mutex_lock(&data_lock);
	copy_text(note, sizeof(note), engine_note);
	copy_text(state, sizeof(state), account_state);
	copy_text(error, sizeof(error), account_error);
	premium = account_premium;
	pthread_mutex_unlock(&data_lock);

	if (row_engine_value) {
		lv_label_set_text(row_engine_value, note);
	}
	if (row_account_value) {
		// The state, or the engine's own sentence when it gave one: a `failed`
		// status, an engine that did not answer, and a login with no token all
		// carry a reason, and that reason is the useful half. The value label
		// sits beside the name and is not clipped by the row itself, and those
		// sentences are long ("Permission denied { Login failed with reason: Bad
		// credentials }"): they would be drawn over the name, so they are cut by
		// hand. The whole sentence is in the popup that follows a login.
		char value[64];
		if (error[0]) {
			clip(value, sizeof(value), error, 18);
		} else {
			copy_text(value, sizeof(value), state);
		}
		lv_label_set_text(row_account_value, value);
	}
	if (row_client_value) {
		lv_label_set_text(row_client_value, account_client_id ? "set" : "not set");
	}
	if (row_premium_value) {
		// Only the engine knows, and only while signed in: `premium` is in its
		// status answer. A free account cannot stream at all (session.rs), so
		// "no" here is the answer to "why does every track fail".
		const char *text = "unknown";
		if (strcmp(state, "signed_in") == 0) {
			text = premium == 1 ? "yes" : premium == 0 ? "no" : "unknown";
		}
		lv_label_set_text(row_premium_value, text);
	}
	if (account_hint) {
		if (strcmp(state, "signed_in") == 0 && premium == 0) {
			lv_obj_set_hidden(account_hint, false);
		} else {
			lv_obj_set_hidden(account_hint, true);
		}
	}
}

// What a finished login or logout says, in the words the player uses elsewhere.
static void popup_account(void) {
	char state[32];
	char error[160];
	pthread_mutex_lock(&data_lock);
	copy_text(state, sizeof(state), account_state);
	copy_text(error, sizeof(error), account_error);
	pthread_mutex_unlock(&data_lock);
	if (strcmp(state, "signed_in") == 0) {
		gui_notify_popup("signed in");
	} else if (strcmp(state, "failed") == 0) {
		gui_notify_popup(error[0] ? error : "sign in failed");
	} else if (strcmp(state, "signed_out") == 0) {
		gui_notify_popup("signed out");
	} else if (state[0]) {
		gui_notify_popup(state);
	}
}

static void job_finished(void *user) {
	job_t *job = user;
	if (is_list_job(job->kind)) {
		list_show();
	} else if (job->kind == JOB_LOGOUT || job->kind == JOB_SIGN_IN || job->kind == JOB_OAUTH_FINISH) {
		refresh_home();
		popup_account();
	} else if (job->kind == JOB_OAUTH_START) {
		refresh_home();
		char state[32] = "";
		pthread_mutex_lock(&data_lock);
		copy_text(state, sizeof(state), account_state);
		char error[160];
		copy_text(error, sizeof(error), account_error);
		pthread_mutex_unlock(&data_lock);
		if (strcmp(state, "failed") == 0 && error[0]) {
			gui_notify_popup(error_text(error));
		} else {
			// The URL is far too long for a 320-pixel toast: the row that made it
			// points at the file it went into.
			gui_notify_popup("open the URL from Addons/spotify/oauth-url.txt");
		}
	} else {
		refresh_home();
	}
	free(job);
}

// ---------------------------------------------------------------------------
// The credentials file
//
// One key that works: `access_token = ...`. Spotify has withdrawn the
// username/password flow for the client librespot imitates, and a login with a
// password answers LoginFailed on a real account (engine-mips/src/session.rs),
// which is why there is no form here to type one into.
//
// The addon does not read this file. The engine does, on login, and its reader
// skips `#` lines -- so the template below is inert until the user fills it in,
// and a hand-added token works whether it sits under the comment or replaces it.
// Reading it here would put a secret in this process's memory for no purpose,
// and the engine already scrubs it out of every sentence it answers with.
// ---------------------------------------------------------------------------

// The file a fresh card gets, once. Every line is a comment: read_credentials()
// in the engine sees nothing in it, which is a login that fails with
// "credentials file has no username and password" -- the honest answer for a
// card nobody has put a token on yet.
static const char CREDENTIALS_TEMPLATE[] =
	"# Spotify: two ways in, and only one of them still works.\n"
	"#\n"
	"# 1. Your own application (https://developer.spotify.com/dashboard), which is\n"
	"#    the one that works: put its Client ID below, and set the application's\n"
	"#    redirect URI to exactly  http://127.0.0.1:5588/login\n"
	"#    Then use \"Sign in with Spotify\" on the page.\n"
	"# client_id =\n"
	"#\n"
	"# 2. An access token copied out of the web player. Spotify's Web API answers\n"
	"#    429 to those since 2025-12-15, so the lists will not load; playback may.\n"
	"# access_token =\n";

// O_EXCL, so a card that already has a token in it is never overwritten by a
// page open, and stop()/init() in one process cannot truncate what the user
// just wrote. Not common/store: store_save() rewrites the file from the keys it
// parsed, which would drop every comment the template is made of.
static void write_credentials_template(void) {
	if (access(credentials_path, F_OK) == 0) {
		return;
	}
	int fd = open(credentials_path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		if (errno != EEXIST) {
			fprintf(stderr, "spotify: credentials template %s: %s\n", credentials_path, strerror(errno));
		}
		return;
	}
	const char *text = CREDENTIALS_TEMPLATE;
	size_t length = strlen(text);
	ssize_t wrote = write(fd, text, length);
	close(fd);
	if (wrote != (ssize_t)length) {
		fprintf(stderr, "spotify: credentials template: short write to %s\n", credentials_path);
		return;
	}
	// vfat keeps no mode bits, so this is a nod to a card formatted otherwise:
	// the file is about to become a token.
	chmod(credentials_path, 0600);
	fprintf(stderr, "spotify: wrote the credentials template to %s\n", credentials_path);
}

// ---------------------------------------------------------------------------
// Signing in
// ---------------------------------------------------------------------------

// Both ways in are one row each: there is nothing to type, only a file to read
// (Sign in from the card) and an engine op to drop the session (Sign out).
// The two halves of the OAuth sign-in. The first writes the authorization URL
// into a file on the card (it cannot be shown, let alone typed, on this screen);
// the second hands the engine whatever code that browser left behind.
static void oauth_start_cb(lv_event_t *e) {
	(void)e;
	oauth_waiting = true;
	oauth_ticks = 0;
	start_job(JOB_OAUTH_START, "{\"op\":\"oauth_start\"}", "Sign in with Spotify");
}

static void oauth_finish_cb(lv_event_t *e) {
	(void)e;
	oauth_waiting = false;
	start_job(JOB_OAUTH_FINISH, "{\"op\":\"oauth_finish\"}", "Finish sign-in");
}

static void sign_in_from_card_cb(lv_event_t *e) {
	(void)e;
	// The row says "authenticating" for the length of the engine's attempt: a
	// login with no token still takes a round trip, and a page that says
	// signed_out until the popup arrives reads as a dead button.
	set_account("authenticating", "");
	refresh_home();
	start_job(JOB_SIGN_IN, "{\"op\":\"login\"}", "Sign in from the card");
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	// The engine's logout is what drops the session and state/credentials.json;
	// the token in credentials.ini stays where the user put it, or the next sign
	// in would have nothing to read.
	set_account("signed_out", "");
	refresh_home();
	start_job(JOB_LOGOUT, "{\"op\":\"logout\"}", "Sign out");
}

// ---------------------------------------------------------------------------
// Searching
// ---------------------------------------------------------------------------

static void search_run_cb(lv_event_t *e) {
	(void)e;
	char query[128];
	copy_text(query, sizeof(query), lv_textarea_get_text(search_field));
	trim_text(query);
	keyboard_set_visible(search_keyboard, false);
	if (!query[0]) {
		switch_screen(home_screen);
		return;
	}
	char escaped[256];
	char request[ENGINE_REQUEST_MAX];
	char title[96];
	json_escape(escaped, sizeof(escaped), query);
	lv_snprintf(request, sizeof(request), "{\"op\":\"search\",\"query\":\"%s\",\"offset\":0,\"limit\":%d}", escaped, LIST_MAX);
	lv_snprintf(title, sizeof(title), "Search: %.60s", query);
	open_list(title, request, JOB_SEARCH);
}

static void search_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(search_field, "");
	keyboard_reset(search_keyboard);
	keyboard_set_visible(search_keyboard, true);
	switch_screen(search_screen);
}

static void playlists_cb(lv_event_t *e) {
	(void)e;
	char request[ENGINE_REQUEST_MAX];
	lv_snprintf(request, sizeof(request), "{\"op\":\"playlists\",\"offset\":0,\"limit\":%d}", LIST_MAX);
	open_list("Playlists", request, JOB_PLAYLISTS);
}

// ---------------------------------------------------------------------------
// The pages that are built once
// ---------------------------------------------------------------------------

// The credentials file, as a card rather than a one-line row: the path is
// thirty characters and does not share a line with the name on a 320-pixel
// panel. The name sits where every row's name sits, the path underneath in the
// dim value style.
static void token_row_build(lv_obj_t *parent) {
	lv_obj_t *card = lv_obj_create(parent);
	lv_obj_set_width(card, lv_pct(100));
	lv_obj_set_height(card, SC(100));
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, SC(12), 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
	lv_obj_set_style_pad_hor(card, SC(20), 0);
	lv_obj_set_style_pad_ver(card, SC(14), 0);
	lv_obj_set_scrollable(card, false);
	// Presses reach the page container, as on any other row, so the swipe-back
	// gesture can start here too.
	lv_obj_set_event_bubble(card, true);

	lv_obj_t *name = lv_label_create(card);
	lv_label_set_text(name, "Token file");
	lv_obj_add_style(name, &theme_style_text, 0);
	lv_obj_set_style_text_font(name, &font_ui_24, 0);
	lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);

	row_token_value = lv_label_create(card);
	lv_obj_add_style(row_token_value, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(row_token_value, &font_ui_16, 0);
	lv_label_set_long_mode(row_token_value, LV_LABEL_LONG_DOT);
	lv_obj_set_width(row_token_value, lv_pct(100));
	lv_label_set_text(row_token_value, "Addons/spotify/credentials.ini");
	lv_obj_align(row_token_value, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void home_build(void) {
	home_screen = lv_obj_create(NULL);
	own_screen(home_screen);
	// The moment the shell has finished its own list, it shows this page: that is
	// where the other two screens are registered a second time (own_all_screens).
	lv_obj_add_event_cb(home_screen, home_shown_cb, LV_EVENT_SCREEN_LOADED, NULL);
	home_page = scrolling_page(home_screen, "Spotify");

	// The engine first: everything below it is only meaningful if this row says
	// the engine is there.
	settingsrow_add(home_page, "Engine", &row_engine_value, NULL, NULL);
	// The account, in the engine's own words. There is no password form: Spotify
	// withdrew that flow, so the only way in is a token in the file below.
	settingsrow_add(home_page, "Account", &row_account_value, NULL, NULL);
	settingsrow_add(home_page, "Premium", &row_premium_value, NULL, NULL);
	settingsrow_add(home_page, "Client ID", &row_client_value, NULL, NULL);
	account_hint = lv_label_create(home_page);
	lv_label_set_long_mode(account_hint, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(account_hint, lv_pct(100));
	lv_obj_add_style(account_hint, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(account_hint, &font_ui_18, 0);
	lv_label_set_text(account_hint, "a free account cannot stream: Spotify will not serve the audio");
	lv_obj_set_hidden(account_hint, true);
	token_row_build(home_page);

	settingsrow_action(home_page, "Sign in from the card", sign_in_from_card_cb, NULL);
	settingsrow_action(home_page, "Sign in with Spotify", oauth_start_cb, NULL);
	settingsrow_action(home_page, "Finish sign-in", oauth_finish_cb, NULL);
	settingsrow_action(home_page, "Search", search_cb, NULL);
	settingsrow_action(home_page, "Playlists", playlists_cb, NULL);
	settingsrow_action(home_page, "Sign out", sign_out_cb, NULL);

	refresh_home();
}

static lv_obj_t *make_field(lv_obj_t *parent, const char *placeholder, int y) {
	lv_obj_t *field = lv_textarea_create(parent);
	lv_textarea_set_one_line(field, true);
	lv_textarea_set_placeholder_text(field, placeholder);
	lv_obj_set_size(field, config->screen_width - 2 * config->padding, SC(62));
	lv_obj_set_scrollbar_mode(field, LV_SCROLLBAR_MODE_OFF);
	lv_obj_align(field, LV_ALIGN_TOP_LEFT, config->padding, y);
	lv_obj_add_style(field, &theme_style_card, 0);
	lv_obj_set_style_radius(field, SC(12), 0);
	lv_obj_set_style_border_width(field, 0, 0);
	lv_obj_set_style_shadow_width(field, 0, 0);
	lv_obj_set_style_pad_all(field, SC(14), 0);
	lv_obj_set_style_text_font(field, &font_ui_24, 0);
	keyboard_style_caret(field);
	return field;
}

static void build_list_screen(void) {
	list_screen = lv_obj_create(NULL);
	own_screen(list_screen);
	list_container = scrolling_page(list_screen, "Spotify");
}

static void build_search_screen(void) {
	search_screen = lv_obj_create(NULL);
	own_screen(search_screen);
	lv_obj_add_style(search_screen, &theme_style_screen, 0);
	settingsrow_title(search_screen, config, "Spotify search");
	search_field = make_field(search_screen, "what to look for", settingsrow_content_top(config) + SC(10));
	search_keyboard =
		keyboard_create(search_screen, config->screen_width, SC(316), search_field, NULL, "ok", search_run_cb, NULL);
	keyboard_set_visible(search_keyboard, false);
	switcher_attach_back_gesture(search_screen);
}

// ---------------------------------------------------------------------------
// The timer: is the addon on screen, and is the engine it needs alive
// ---------------------------------------------------------------------------

static bool addon_on_screen(void) {
	lv_obj_t *active = lv_screen_active();
	return active && (active == home_screen || active == list_screen || active == search_screen);
}

// Whether the player is playing something this engine serves. A stream that is
// running holds the engine open after the page is left: killing it would cut
// the audio, which is the one thing leaving an addon must not do.
static bool our_stream_loaded(void) {
	if (engine_port == 0 || !radio_is_active()) {
		return false;
	}
	radio_station_t station;
	if (!radio_current_station(&station)) {
		return false;
	}
	char prefix[48];
	lv_snprintf(prefix, sizeof(prefix), "http://127.0.0.1:%d/stream/", (int)engine_port);
	return strncmp(station.url, prefix, strlen(prefix)) == 0;
}

static void tick_cb(lv_timer_t *timer) {
	(void)timer;
	if (!engine_binary_present) {
		// Nothing can change on this card: the rows already say so.
		refresh_home();
		return;
	}
	pthread_mutex_lock(&job_lock);
	bool busy = jobs_running > 0;
	pthread_mutex_unlock(&job_lock);
	if (busy) {
		return;
	}
	// An authorization URL is out: look for the file the browser's redirect was
	// meant to leave behind. Every fifth tick -- a file drop is picked up within
	// seconds, and the card is not read once a second for a file that may never
	// come.
	if (oauth_waiting && ++oauth_ticks >= 5) {
		oauth_ticks = 0;
		char path[ADDON_PATH_MAX + 16];
		lv_snprintf(path, sizeof(path), "%s/oauth-code.txt", addon_dir);
		if (access(path, F_OK) == 0) {
			oauth_waiting = false;
			start_job(JOB_OAUTH_FINISH, "{\"op\":\"oauth_finish\"}", "Finish sign-in");
			return;
		}
	}
	if (addon_on_screen()) {
		// The status op is also what starts the engine if it is not running:
		// the first one after the page opens, and the first one after the addon
		// is opened again later.
		start_job(JOB_STATUS, "{\"op\":\"status\"}", "Status");
	} else if (engine_pid > 0 && !our_stream_loaded()) {
		// Left the addon, nothing of ours playing: the engine has no reason to
		// hold 40 MB of RSS. It is started again the next time the page is on
		// screen.
		start_job(JOB_SHUTDOWN, "", "Stop");
	}
}

// ---------------------------------------------------------------------------
// The addon
// ---------------------------------------------------------------------------

static lv_obj_t *spotify_init(const sonix_addon_host_t *host) {
	config = host->cfg;
	// Before the first screen exists: own_screen() names the addon to the shell.
	copy_text(addon_id, sizeof(addon_id), host->id);
	copy_text(addon_dir, sizeof(addon_dir), host->dir);
	lv_snprintf(credentials_path, sizeof(credentials_path), "%s/credentials.ini", addon_dir);
	lv_snprintf(state_dir, sizeof(state_dir), "%s/state", addon_dir);
	// vfat has no unix sockets, so the socket is in /tmp, named after the
	// player that owns it.
	lv_snprintf(engine_socket, sizeof(engine_socket), "/tmp/spotify-engine-%d.sock", (int)getpid());

	engine_binary_present = locate_engine();
	if (engine_binary_present) {
		set_note("engine starting");
		fprintf(stderr, "spotify: engine %s\n", engine_binary);
	} else {
		set_note("engine not built");
		fprintf(stderr, "spotify: no %s in %s: the addon runs without a protocol half\n", ENGINE_BINARY, addon_dir);
	}

	// The template goes down before the page that points at it exists: a card
	// with no credentials.ini gets the commented file, and a card that has one
	// (a token the user put there) is never touched.
	write_credentials_template();
	fprintf(stderr, "spotify: credentials %s (%s)\n", credentials_path,
			access(credentials_path, R_OK) == 0 ? "present" : "none");

	// The screens are built once per load: the shell unloads the library rather
	// than calling init() again, and stop() below is what makes a second run of
	// this function in one process a clean start.
	home_build();
	build_list_screen();
	build_search_screen();

	// The status poll, this run's own: stop() deletes it, and a run that somehow
	// begins with one already there replaces it rather than leaving two ticks
	// asking the same engine the same question.
	if (tick_timer) {
		lv_timer_delete(tick_timer);
	}
	tick_timer = lv_timer_create(tick_cb, TICK_PERIOD_MS, NULL);

	// The engine is started by this first status job rather than by init()
	// itself: fork/execv and the wait for the socket are on the worker thread,
	// so opening the tile never stalls the interface for three seconds.
	start_job(JOB_STATUS, "{\"op\":\"status\"}", "Status");

	return home_screen;
}

// The widgets of this run. The screens themselves are the shell's: every one of
// them was registered with sonix_addon_own_screen(), and the shell deletes them
// after stop() returns -- with the search keyboard inside one of them, which
// keyboard.c now unlinks and frees on LV_EVENT_DELETE.
static void forget_widgets(void) {
	home_screen = NULL;
	home_page = NULL;
	list_screen = NULL;
	list_container = NULL;
	search_screen = NULL;
	search_field = NULL;
	search_keyboard = NULL;
	row_engine_value = NULL;
	row_account_value = NULL;
	row_premium_value = NULL;
	row_token_value = NULL;
	account_hint = NULL;
}

static bool stop_with_workers_running(void);

// The stop half of the interface (src/addon/addon.h). Runs on the GUI thread;
// the shell deletes the three registered screens and, when this returns true,
// dlclose()s this library right after -- so nothing below may leave a thread, a
// timer, a child or a static behind.
//
// The answer is the contract's: true means the library is safe to unmap, false
// means a worker is still inside it. Every wait inside a job watches `stopping`,
// so false should not happen -- but a socket the kernel will not wake, or a
// thread stuck behind one of the mutexes, is exactly the case the flag cannot
// paper over, and saying so is better than being unmapped under the thread.
//
// What there is to undo, in the order it has to happen:
//
//   1. the flag and the tick: no new request, and no timer that would start one;
//   2. the workers: asked to give up, waited for (bounded, and answered false
//      past the bound) and then joined -- the join, not the counter, is what
//      says the thread has left this library's code;
//   3. the engine process, and the socket file beside it;
//   4. the pointers: three screens and a keyboard that belong to the shell from
//      here (it deletes them next -- they are registered with
//      sonix_addon_own_screen(), so it has all three), and every piece of per-run
//      state a second init() would inherit.
//
// There is no atexit() in this library, and there must never be one again: the
// handler would be a call into a page that is no longer mapped, from a process
// that outlives the library by hours.
static bool spotify_stop(void) {
	// 1. Nothing new starts. Every wait inside a job looks at this flag -- the
	// engine's three-second start-up and the twelve-second poll for its answer
	// -- so the wait in step 2 is the tail of one slice, not the whole of one.
	pthread_mutex_lock(&job_lock);
	stopping = true;
	pthread_mutex_unlock(&job_lock);

	if (tick_timer) {
		lv_timer_delete(tick_timer);
		tick_timer = NULL;
	}

	// 2. The workers.
	int waited = 0;
	while (!workers_all_done()) {
		if (waited >= STOP_JOB_WAIT_MS) {
			return stop_with_workers_running();
		}
		usleep(50 * 1000);
		waited += 50;
	}
	workers_join_all();

	// 3. The child. `engine_lock` as everywhere else that touches its pid, its
	// port or its socket; no worker is left to hold it by now.
	pthread_mutex_lock(&engine_lock);
	engine_stop_locked();
	pthread_mutex_unlock(&engine_lock);
	// Whatever way the process ended, the socket is a file this addon made.
	unlink(engine_socket);

	// 4. The pointers. The screens are the shell's to delete after this returns;
	// the search keyboard lives inside its screen, so a stored keyboard_t* would
	// be a dangling pointer in the next run.
	forget_widgets();

	config = NULL;
	addon_id[0] = '\0';
	addon_dir[0] = '\0';
	credentials_path[0] = '\0';
	state_dir[0] = '\0';
	engine_binary[0] = '\0';
	engine_binary_present = false;
	engine_socket[0] = '\0';
	screens_owned = false;

	pthread_mutex_lock(&data_lock);
	copy_text(engine_note, sizeof(engine_note), "engine not built");
	copy_text(account_state, sizeof(account_state), "unknown");
	account_error[0] = '\0';
	account_premium = -1;
	list_error[0] = '\0';
	memset(items, 0, sizeof(items));
	item_count = 0;
	pthread_mutex_unlock(&data_lock);

	pthread_mutex_lock(&job_lock);
	list_latest = 0;
	stopping = false; // init() is a fresh start after this
	pthread_mutex_unlock(&job_lock);

	return true;
}

// The false answer, and the little that can still be done while a worker is
// inside the library: an engine whose lock is free is taken down, and the widget
// pointers go because the shell deletes the screens whether or not this addon
// asked for another chance. The rest is left alone on purpose -- the library
// stays mapped, and its worker keeps writing state this run will never read
// again, but nothing it can touch has been freed.
static bool stop_with_workers_running(void) {
	fprintf(stderr, "spotify: stop: a worker is still inside the library after %d ms; not safe to unload\n",
			STOP_JOB_WAIT_MS);

	if (pthread_mutex_trylock(&engine_lock) == 0) {
		engine_stop_locked();
		pthread_mutex_unlock(&engine_lock);
		unlink(engine_socket);
	} else {
		// The worker that holds it is the one that is not finished. SIGTERM behind
		// its back would race its own bookkeeping of engine_pid, so the child is
		// left to the process exit.
		fprintf(stderr, "spotify: stop: the engine is left running; a worker holds engine_lock\n");
	}

	forget_widgets();
	// `stopping` stays set: no new request, no new screen, no second init.
	return false;
}

static const sonix_addon_t spotify = {
	.api = SONIX_ADDON_API,
	.title = "Spotify",
	.init = spotify_init,
};

static const sonix_addon_stop_t spotify_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = spotify_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &spotify : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &spotify_lifecycle : NULL;
}

// The service half.
//
// Spotify is a service by intent: one day it will own a cache on the card, put
// its tracks in the player's queue and answer the player's questions about the
// files in it. None of that exists yet -- the engine streams over a loopback
// pipe and downloads nothing -- so there is nothing to star, nothing to export
// and no cache to empty. The table is exported anyway, with the id and nothing
// else, for one reason: the manifest says "service", and a service addon
// without this symbol makes the loader log "says it is a service but exports no
// table" on every open. A NULL callback is the interface's own way of saying
// "this service does not do that yet" (addon_service.h).
static const sonix_addon_service_t spotify_service = {
	.id = "spotify",
};

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &spotify_service : NULL;
}
