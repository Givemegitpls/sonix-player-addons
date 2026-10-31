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

#include "common/store.h"

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
//   <card>/Addons/spotify/credentials.ini              username / password
//   <card>/Addons/spotify/state/                       librespot's own cache
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
	JOB_LOGIN,
	JOB_LOGOUT,
	JOB_SEARCH,
	JOB_PLAYLISTS,
	JOB_PLAYLIST_TRACKS,
	JOB_SHUTDOWN,
} job_kind_t;

typedef struct {
	job_kind_t kind;
	char request[ENGINE_REQUEST_MAX];
	int generation;
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

// What the interface draws, and the last page of the catalog. A worker writes
// these under `data_lock` and then posts itself to the UI thread; the UI thread
// reads them only from that post or from its own timer.
static pthread_mutex_t data_lock = PTHREAD_MUTEX_INITIALIZER;
static char engine_note[64] = "engine not built";
static char account_state[32] = "unknown";
static char account_error[160];
static char list_error[160];
static item_t items[LIST_MAX];
static int item_count;

// Which catalog answer still matters. Only those are gated: a status poll that
// lands while a search is in flight must not throw the search away.
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static int list_latest;
static int jobs_running;

static store_t credentials;

static lv_obj_t *home_screen;
static lv_obj_t *home_page;
static lv_obj_t *list_screen;
static lv_obj_t *list_container;
static lv_obj_t *login_screen;
static lv_obj_t *search_screen;
static lv_obj_t *search_field;
static lv_obj_t *login_user_field;
static lv_obj_t *login_password_field;
static keyboard_t *login_keyboard;
static keyboard_t *search_keyboard;

static lv_obj_t *row_engine_value;
static lv_obj_t *row_account_value;
static lv_obj_t *row_error_value;

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
	pthread_mutex_unlock(&data_lock);
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
		// tick below and spotify_atexit().
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
	while (waited < ENGINE_SOCKET_WAIT_MS) {
		if (access(engine_socket, F_OK) == 0) {
			char note[48];
			snprintf(note, sizeof(note), "engine loaded (%d)", port);
			set_note(note);
			snprintf(why, why_size, "listening");
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
		// and becomes a hang.
		for (int i = 0; i < 30; i++) {
			if (waitpid(pid, &status, WNOHANG) == pid) {
				reaped = true;
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
	unlink(engine_socket);
}

// The deinit() the addon interface does not have (src/addon/addon.h: an addon
// is never unloaded, and the player has no shutdown path). Two things stand in
// for one: the leave-the-page check in tick_cb(), and this handler, which keeps
// an engine from outliving the player process.
static void spotify_atexit(void) {
	if (engine_pid <= 0) {
		return;
	}
	pid_t pid = engine_pid;
	kill(pid, SIGTERM);
	for (int i = 0; i < 10; i++) {
		if (waitpid(pid, NULL, WNOHANG) == pid) {
			engine_pid = 0;
			unlink(engine_socket);
			return;
		}
		usleep(50000);
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	engine_pid = 0;
	unlink(engine_socket);
}

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

	size_t used = 0;
	for (;;) {
		struct pollfd watching = {.fd = fd, .events = POLLIN, .revents = 0};
		int ready = poll(&watching, 1, ENGINE_REPLY_TIMEOUT_MS);
		if (ready < 0) {
			if (errno == EINTR) {
				continue;
			}
			snprintf(why, why_size, "poll: %s", strerror(errno));
			break;
		}
		if (ready == 0) {
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
		pthread_mutex_lock(&data_lock);
		copy_text(account_state, sizeof(account_state), state);
		copy_text(account_error, sizeof(account_error), error);
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
	case JOB_LOGIN: return "login";
	case JOB_LOGOUT: return "logout";
	case JOB_SEARCH: return "search";
	case JOB_PLAYLISTS: return "playlists";
	case JOB_PLAYLIST_TRACKS: return "playlist_tracks";
	case JOB_SHUTDOWN: return "shutdown";
	}
	return "?";
}

static void *worker(void *argument) {
	job_t *job = argument;
	char *reply = calloc(1, ENGINE_REPLY_MAX);
	char why[160];
	why[0] = '\0';
	bool post = true;

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
	bool answered = up && engine_exchange_locked(job->request, reply, ENGINE_REPLY_MAX, why, sizeof(why));
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
	} else if (job->kind == JOB_STATUS || job->kind == JOB_LOGIN || job->kind == JOB_LOGOUT) {
		// Both of these lock data_lock themselves: it is not recursive.
		take_status(reply, why);
	} else if (is_list_job(job->kind)) {
		pthread_mutex_lock(&data_lock);
		take_items(reply, why, sizeof(why));
		pthread_mutex_unlock(&data_lock);
	}

finish:
	free(reply);
	pthread_mutex_lock(&job_lock);
	jobs_running--;
	pthread_mutex_unlock(&job_lock);
	if (post && !gui_post(job_finished, job)) {
		free(job);
	} else if (!post) {
		free(job);
	}
	return NULL;
}

static void start_job(job_kind_t kind, const char *request, const char *title) {
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		gui_notify_popup("out of memory");
		return;
	}
	job->kind = kind;
	copy_text(job->request, sizeof(job->request), request);
	copy_text(job->title, sizeof(job->title), title);

	pthread_mutex_lock(&job_lock);
	if (is_list_job(kind)) {
		job->generation = ++list_latest;
	}
	jobs_running++;
	pthread_mutex_unlock(&job_lock);

	fprintf(stderr, "spotify: job %s [%s] %s\n", job_name(kind), job->title, job->request);

	pthread_t thread;
	if (pthread_create(&thread, NULL, worker, job) != 0) {
		pthread_mutex_lock(&job_lock);
		jobs_running--;
		pthread_mutex_unlock(&job_lock);
		free(job);
		gui_notify_popup("could not start a worker thread");
		return;
	}
	pthread_detach(thread);
}

// ---------------------------------------------------------------------------
// Screens
// ---------------------------------------------------------------------------

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

static void play_item(int index) {
	if (index < 0 || index >= item_count) {
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

static void list_show(void) {
	pthread_mutex_lock(&data_lock);
	int count = item_count;
	char error[160];
	copy_text(error, sizeof(error), list_error);
	pthread_mutex_unlock(&data_lock);

	if (count <= 0) {
		list_show_message(error[0] ? error : "nothing here");
		return;
	}

	lv_obj_clean(list_container);
	for (int i = 0; i < count; i++) {
		char name[ROW_NAME_CHARS * 4];
		char value[16];
		lv_obj_t *value_label = NULL;
		clip(name, sizeof(name), items[i].title[0] ? items[i].title : "(no title)", ROW_NAME_CHARS);
		format_duration(value, sizeof(value), items[i].duration_ms);
		settingsrow_add(list_container, name, &value_label, row_item_cb, (void *)(intptr_t)i);
		if (value_label && value[0]) {
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
	pthread_mutex_lock(&data_lock);
	copy_text(note, sizeof(note), engine_note);
	copy_text(state, sizeof(state), account_state);
	copy_text(error, sizeof(error), account_error);
	pthread_mutex_unlock(&data_lock);

	if (row_engine_value) {
		lv_label_set_text(row_engine_value, note);
	}
	if (row_account_value) {
		lv_label_set_text(row_account_value, state);
	}
	if (row_error_value) {
		// The row's value label sits beside the name and is not clipped by the
		// row itself: the engine's reasons are full sentences ("Permission
		// denied { Login failed with reason: Bad credentials }") and would be
		// drawn over the name.
		char short_error[64];
		clip(short_error, sizeof(short_error), error, 18);
		lv_label_set_text(row_error_value, short_error);
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
	} else if (job->kind == JOB_LOGIN || job->kind == JOB_LOGOUT) {
		refresh_home();
		popup_account();
	} else {
		refresh_home();
	}
	free(job);
}

// ---------------------------------------------------------------------------
// Credentials
//
// The engine's interface is an INI file with `username = ...` and
// `password = ...`; common/store writes exactly that shape, atomically, and
// renames over the old file so a card pulled mid-write keeps the previous
// credentials whole. Mode 0600 afterwards: the file is a password.
// ---------------------------------------------------------------------------

static bool save_credentials(const char *user, const char *password) {
	store_set(&credentials, "username", user);
	store_set(&credentials, "password", password);
	if (!store_save(&credentials)) {
		return false;
	}
	chmod(credentials_path, 0600);
	return true;
}

// ---------------------------------------------------------------------------
// Signing in
// ---------------------------------------------------------------------------

static void login_field_cb(lv_event_t *e) {
	lv_obj_t *field = lv_event_get_target(e);
	lv_obj_t *other = field == login_user_field ? login_password_field : login_user_field;
	keyboard_set_field(login_keyboard, field);
	lv_obj_add_state(field, LV_STATE_FOCUSED);
	lv_obj_remove_state(other, LV_STATE_FOCUSED);
	// Two carets blinking at once do not say which field the keys write into.
	keyboard_show_caret(field, true);
	keyboard_show_caret(other, false);
}

static void login_submit_cb(lv_event_t *e) {
	(void)e;
	char user[STORE_VALUE_MAX];
	char password[STORE_VALUE_MAX];
	copy_text(user, sizeof(user), lv_textarea_get_text(login_user_field));
	trim_text(user);
	copy_text(password, sizeof(password), lv_textarea_get_text(login_password_field));
	if (!user[0] || !password[0]) {
		gui_notify_popup("a username and a password are needed");
		return;
	}
	if (!save_credentials(user, password)) {
		gui_notify_popup("the card refused credentials.ini");
		return;
	}
	memset(password, 0, sizeof(password));
	// The password does not stay in the field a second longer than it has to.
	lv_textarea_set_text(login_password_field, "");
	keyboard_set_visible(login_keyboard, false);
	switch_screen(home_screen);
	set_account("authenticating", "");
	refresh_home();
	start_job(JOB_LOGIN, "{\"op\":\"login\"}", "Sign in");
}

static void sign_in_cb(lv_event_t *e) {
	(void)e;
	lv_textarea_set_text(login_user_field, store_get(&credentials, "username"));
	lv_textarea_set_text(login_password_field, "");
	keyboard_reset(login_keyboard);
	lv_obj_add_state(login_user_field, LV_STATE_FOCUSED);
	lv_obj_remove_state(login_password_field, LV_STATE_FOCUSED);
	keyboard_set_field(login_keyboard, login_user_field);
	keyboard_show_caret(login_user_field, true);
	keyboard_show_caret(login_password_field, false);
	keyboard_set_visible(login_keyboard, true);
	switch_screen(login_screen);
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	// The password goes off the card, the username stays: signing in again is
	// then one field. The engine drops its own cached session on logout.
	store_set(&credentials, "password", "");
	if (store_save(&credentials)) {
		chmod(credentials_path, 0600);
	}
	lv_textarea_set_text(login_password_field, "");
	set_account("signed out", "");
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

static void home_build(void) {
	home_screen = lv_obj_create(NULL);
	home_page = scrolling_page(home_screen, "Spotify");

	// The engine first: everything below it is only meaningful if this row says
	// the engine is there.
	settingsrow_add(home_page, "Engine", &row_engine_value, NULL, NULL);
	settingsrow_add(home_page, "Account", &row_account_value, NULL, NULL);
	settingsrow_action(home_page, "Sign in", sign_in_cb, NULL);
	settingsrow_action(home_page, "Search", search_cb, NULL);
	settingsrow_action(home_page, "Playlists", playlists_cb, NULL);
	settingsrow_action(home_page, "Sign out", sign_out_cb, NULL);
	settingsrow_add(home_page, "Last error", &row_error_value, NULL, NULL);

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
	list_container = scrolling_page(list_screen, "Spotify");
}

static void build_login_screen(void) {
	login_screen = lv_obj_create(NULL);
	lv_obj_add_style(login_screen, &theme_style_screen, 0);
	settingsrow_title(login_screen, config, "Spotify sign in");
	int top = settingsrow_content_top(config);

	login_user_field = make_field(login_screen, "username", top);
	login_password_field = make_field(login_screen, "password", top + SC(78));
	// Dots from the first keystroke, and never a readable character: zero show
	// time is the rule every other field in this player follows.
	lv_textarea_set_password_mode(login_password_field, true);
	keyboard_style_password(login_password_field, 0);
	lv_obj_add_event_cb(login_user_field, login_field_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(login_password_field, login_field_cb, LV_EVENT_CLICKED, NULL);
	keyboard_show_caret(login_password_field, false);

	lv_obj_t *button = lv_btn_create(login_screen);
	lv_obj_set_size(button, config->screen_width - 2 * config->padding, SC(56));
	lv_obj_align(button, LV_ALIGN_TOP_LEFT, config->padding, top + SC(156));
	lv_obj_add_style(button, &theme_style_card, 0);
	lv_obj_set_style_radius(button, SC(12), 0);
	lv_obj_set_style_border_width(button, 0, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_add_event_cb(button, login_submit_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *button_label = lv_label_create(button);
	lv_label_set_text(button_label, "Sign in");
	lv_obj_add_style(button_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(button_label, &font_ui_24, 0);
	lv_obj_center(button_label);

	lv_obj_t *note = lv_label_create(login_screen);
	lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(note, config->screen_width - 2 * config->padding);
	lv_obj_align(note, LV_ALIGN_TOP_LEFT, config->padding, top + SC(224));
	lv_obj_add_style(note, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(note, &font_ui_18, 0);
	lv_label_set_text(note, engine_binary_present
								? "credentials.ini stays on the card; the engine keeps its session in state/."
								: "the engine is not on this card (assets/spotify-engine-mips), so signing in cannot work.");

	login_keyboard =
		keyboard_create(login_screen, config->screen_width, SC(316), login_user_field, NULL, "ok", login_submit_cb, NULL);
	keyboard_set_visible(login_keyboard, true);
	switcher_attach_back_gesture(login_screen);
}

static void build_search_screen(void) {
	search_screen = lv_obj_create(NULL);
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
	return active && (active == home_screen || active == list_screen || active == login_screen || active == search_screen);
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

	store_init(&credentials, credentials_path);
	bool known = store_load(&credentials);
	fprintf(stderr, "spotify: credentials %s (%s)\n", credentials_path, known ? "read" : "none");

	// The screens are built once: init() runs once per player process.
	home_build();
	build_list_screen();
	build_login_screen();
	build_search_screen();

	// Two stand-ins for the deinit() this interface does not have.
	atexit(spotify_atexit);
	lv_timer_create(tick_cb, TICK_PERIOD_MS, NULL);

	// The engine is started by this first status job rather than by init()
	// itself: fork/execv and the wait for the socket are on the worker thread,
	// so opening the tile never stalls the interface for three seconds.
	start_job(JOB_STATUS, "{\"op\":\"status\"}", "Status");

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
