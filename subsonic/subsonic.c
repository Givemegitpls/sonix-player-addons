#include "subsonic.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "src/system/core/json.h"
#include "src/system/core/lang.h"
#include "src/system/core/md5.h"
#include "common/gunzip.h"
#include "common/store.h"
#include "src/system/net/http.h"

#define API_VERSION "1.16.1"
#define CLIENT_NAME "sonix"

// A big library's getArtists or a three-thousand-song playlist runs to a few
// megabytes of JSON; past this the reply is refused rather than eating the
// device's memory.
#define BODY_LIMIT (6 * 1024 * 1024)
#define TIMEOUT_SECS 20

// The session. Written by the worker that signs in, read by every thread that
// builds a URL (the page's worker, the prefetch thread, the sync worker), so it
// sits behind a lock and is copied out whole.
typedef struct {
	char server[SUBSONIC_SERVER_MAX]; // no trailing slash
	char user[SUBSONIC_NAME_MAX];
	char salt[24];
	char token[40];		// md5(password + salt), hex
	char password_hex[256]; // only for servers that refuse tokens
} session_t;

static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
static session_t session;
static store_t settings; // the same, on the card; guarded by session_lock
static int quality = SUBSONIC_QUALITY_ORIGINAL;

static __thread char last_error[192];

const char *subsonic_last_error(void) { return last_error; }

static void set_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

static void session_copy(session_t *out) {
	pthread_mutex_lock(&session_lock);
	*out = session;
	pthread_mutex_unlock(&session_lock);
}

static void normalise_server(const char *in, char *out, size_t size);

// The session goes to the addon's own file on the card, <dir>/session.ini, and
// nothing goes to the player's configuration: that file is the player's, it is
// rewritten whole on every save, and a worker thread writing to it races the
// interface for it. The card is also where the addon itself lives, so a card
// that is pulled takes the session with it and a fresh card starts signed out,
// which is what the second reading of "an addon is a package on a card" means.
static void persist_locked(void) {
	store_set(&settings, "server", session.server);
	store_set(&settings, "user", session.user);
	store_set(&settings, "salt", session.salt);
	store_set(&settings, "token", session.token);
	store_set(&settings, "password_hex", session.password_hex);
	store_save(&settings);
}

// `dir` is the addon's folder on the card: host->dir in init(). Read once.
void subsonic_load(const char *dir) {
	char path[STORE_PATH_MAX];
	if (dir && dir[0]) {
		snprintf(path, sizeof(path), "%s/session.ini", dir);
	} else {
		path[0] = '\0';
	}

	pthread_mutex_lock(&session_lock);
	store_init(&settings, path);
	store_load(&settings);
	memset(&session, 0, sizeof(session));
	// Through the same normaliser the sign-in form uses: a server written once
	// without a scheme, or edited by hand, arrives with one.
	normalise_server(store_get(&settings, "server"), session.server, sizeof(session.server));
	snprintf(session.user, sizeof(session.user), "%s", store_get(&settings, "user"));
	snprintf(session.salt, sizeof(session.salt), "%s", store_get(&settings, "salt"));
	snprintf(session.token, sizeof(session.token), "%s", store_get(&settings, "token"));
	snprintf(session.password_hex, sizeof(session.password_hex), "%s", store_get(&settings, "password_hex"));
	quality = store_get_int(&settings, "quality", SUBSONIC_QUALITY_ORIGINAL);
	pthread_mutex_unlock(&session_lock);

	if (quality < 0 || quality >= SUBSONIC_QUALITY_COUNT) {
		quality = SUBSONIC_QUALITY_ORIGINAL;
	}

	printf("addon subsonic: %s\n", subsonic_logged_in() ? "saved session" : "no session");
}

bool subsonic_logged_in(void) {
	pthread_mutex_lock(&session_lock);
	bool in = session.server[0] && session.user[0] && (session.token[0] || session.password_hex[0]);
	pthread_mutex_unlock(&session_lock);
	return in;
}

void subsonic_display_name(char *out, size_t size) {
	session_t s;
	session_copy(&s);
	const char *host = strstr(s.server, "://");
	host = host ? host + 3 : s.server;
	if (s.user[0]) {
		snprintf(out, size, "%s @ %s", s.user, host);
	} else if (size) {
		out[0] = '\0';
	}
}

void subsonic_user_name(char *out, size_t size) {
	pthread_mutex_lock(&session_lock);
	snprintf(out, size, "%s", session.user);
	pthread_mutex_unlock(&session_lock);
}

void subsonic_server(char *out, size_t size) {
	pthread_mutex_lock(&session_lock);
	snprintf(out, size, "%s", session.server);
	pthread_mutex_unlock(&session_lock);
}

// FNV-1a, 64 bits: a dozen lines, no table, and at a few thousand cached files
// the chance of two keys colliding is around one in a billion billion.
void subsonic_track_key(const char *song_id, char *out, size_t size) {
	session_t s;
	session_copy(&s);
	uint64_t h = 0xcbf29ce484222325ULL;
	const char *parts[] = {s.server, "\n", song_id ? song_id : ""};
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
		for (const unsigned char *p = (const unsigned char *)parts[i]; *p; p++) {
			h ^= *p;
			h *= 0x100000001b3ULL;
		}
	}
	snprintf(out, size, "%016llx", (unsigned long long)h);
}

// ---------------------------------------------------------------------------
// quality
// ---------------------------------------------------------------------------

int subsonic_get_quality(void) { return quality; }

void subsonic_set_quality(int q) {
	if (q < 0 || q >= SUBSONIC_QUALITY_COUNT) {
		return;
	}
	quality = q;
	pthread_mutex_lock(&session_lock);
	store_set_int(&settings, "quality", q);
	store_save(&settings);
	pthread_mutex_unlock(&session_lock);
}

const char *subsonic_expected_suffix(const subsonic_track_t *track) {
	switch (quality) {
	case SUBSONIC_QUALITY_MP3_320:
	case SUBSONIC_QUALITY_MP3_192:
		return "mp3";
	case SUBSONIC_QUALITY_OPUS_128:
		return "opus";
	default:
		return track && track->suffix[0] ? track->suffix : "flac";
	}
}

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

// "<server>/rest/<method>?u=..&t=..&s=..&v=..&c=..&f=json" plus `extra`
// (already encoded, starting with '&' or empty).
static bool build_url(const session_t *s, const char *method, const char *extra, char *out, size_t size) {
	if (!s->server[0] || !s->user[0]) {
		if (size) {
			out[0] = '\0';
		}
		return false;
	}
	char user[SUBSONIC_NAME_MAX * 3];
	http_url_encode(s->user, user, sizeof(user));
	int n;
	if (s->token[0]) {
		n = snprintf(out, size, "%s/rest/%s?u=%s&t=%s&s=%s&v=" API_VERSION "&c=" CLIENT_NAME "&f=json%s", s->server,
					 method, user, s->token, s->salt, extra ? extra : "");
	} else {
		n = snprintf(out, size, "%s/rest/%s?u=%s&p=enc:%s&v=" API_VERSION "&c=" CLIENT_NAME "&f=json%s", s->server,
					 method, user, s->password_hex, extra ? extra : "");
	}
	return n > 0 && (size_t)n < size;
}

void subsonic_stream_url(const char *song_id, char *out, size_t size) {
	session_t s;
	session_copy(&s);
	char id[SUBSONIC_ID_MAX * 3];
	http_url_encode(song_id ? song_id : "", id, sizeof(id));
	char extra[SUBSONIC_ID_MAX * 3 + 64];
	switch (quality) {
	case SUBSONIC_QUALITY_MP3_320:
		snprintf(extra, sizeof(extra), "&id=%s&format=mp3&maxBitRate=320", id);
		break;
	case SUBSONIC_QUALITY_MP3_192:
		snprintf(extra, sizeof(extra), "&id=%s&format=mp3&maxBitRate=192", id);
		break;
	case SUBSONIC_QUALITY_OPUS_128:
		snprintf(extra, sizeof(extra), "&id=%s&format=opus&maxBitRate=128", id);
		break;
	default:
		// raw: the file untouched, which is what makes Range resumption
		// possible (a transcode has no fixed length to resume into).
		snprintf(extra, sizeof(extra), "&id=%s&format=raw", id);
		break;
	}
	build_url(&s, "stream", extra, out, size);
}

void subsonic_cover_url(const char *cover_id, int px, char *out, size_t size) {
	if (!cover_id || !cover_id[0]) {
		if (size) {
			out[0] = '\0';
		}
		return;
	}
	session_t s;
	session_copy(&s);
	char id[SUBSONIC_ID_MAX * 3];
	http_url_encode(cover_id, id, sizeof(id));
	char extra[SUBSONIC_ID_MAX * 3 + 32];
	snprintf(extra, sizeof(extra), "&id=%s&size=%d", id, px);
	build_url(&s, "getCoverArt", extra, out, size);
}

// ---------------------------------------------------------------------------
// the call
// ---------------------------------------------------------------------------

// Every reply is {"subsonic-response":{"status":"ok"|"failed", "error":{...}}}.
// On success `*resp` is the subsonic-response object; the caller frees with
// call_done() once it has read the document (which points into the body).
// `resp` is the index of the "subsonic-response" object inside `doc`, not the
// HTTP status: a server that answers 426 with a plain-text body never reaches
// JSON parsing at all, so the status needs its own way out. `http_status` is
// optional and is set as soon as a reply was read, whatever it said.
static bool call_with(const session_t *s, const char *method, const char *extra, char **body, json_doc_t *doc,
					  int *resp, int *error_code, int *http_status) {
	last_error[0] = '\0';
	if (error_code) {
		*error_code = 0;
	}
	char url[SUBSONIC_URL_MAX + 512];
	if (!build_url(s, method, extra, url, sizeof(url))) {
		set_error("%s", "not signed in");
		return false;
	}

	// One retry of anything that does not parse. A close-delimited body -- the
	// server framing it by closing the socket, which is what an HTTP/1.0
	// request gets -- can arrive cut short, and guessing that from the bytes is
	// unreliable: a document cut at a chunk boundary can end in '}' by luck.
	// A second connection usually delivers the whole thing, and a request that
	// fails twice is reported with the bytes it got.
	int attempt = 0;
	int transport = 0;

again:;
	int status = 0;
	size_t len = 0;
	*body = NULL;
	bool read = http_get_ex(url, NULL, true, body, &len, BODY_LIMIT, TIMEOUT_SECS, &status);
	if (http_status) {
		*http_status = read ? status : 0;
	}
	if (!read) {
		// A server that closes before the body is complete -- or a connection
		// dropped mid-reply -- fails here rather than parsing as something
		// short. One more connection before saying so: it is a different fault
		// from a server that answered with something else, and the same second
		// attempt tends to get it.
		const char *why = http_last_error();
		char said[192];
		snprintf(said, sizeof(said), "%s", why && why[0] ? why : "cannot reach the server");
		if (transport++ == 0) {
			fprintf(stderr, "subsonic: %s: %s, asking again\n", method, said);
			goto again;
		}
		set_error("%s", said);
		return false;
	}

	// A proxy in front of the server may compress the answer whatever the
	// request asked for; what arrives is then gzip and not JSON, and without
	// this the only thing that could be said is "the server answered 200".
	if (body_is_gzip((const uint8_t *)*body, len)) {
		size_t plain_len = 0;
		uint8_t *plain = gunzip_body((const uint8_t *)*body, len, &plain_len, BODY_LIMIT);
		free(*body);
		*body = (char *)plain;
		len = plain_len;
		if (!plain) {
			set_error("%s", "the server's answer was compressed and could not be unpacked");
			return false;
		}
	}

	if (!json_parse(*body, doc)) {
		// The one line that makes the next report actionable: what came back
		// instead of JSON, and the first bytes of it.
		fprintf(stderr, "subsonic: %s answered %d, %zu bytes:", method, status, len);
		for (size_t i = 0; i < 24 && i < len; i++) {
			fprintf(stderr, " %02x", (unsigned char)(*body)[i]);
		}
		fprintf(stderr, " |");
		for (size_t i = 0; i < 60 && i < len; i++) {
			unsigned char c = (unsigned char)(*body)[i];
			fputc(c >= 32 && c < 127 ? (int)c : '.', stderr);
		}
		fprintf(stderr, "| last:");
		for (size_t i = len > 16 ? len - 16 : 0; i < len; i++) {
			unsigned char c = (unsigned char)(*body)[i];
			fputc(c >= 32 && c < 127 ? (int)c : '.', stderr);
		}
		// A JSON document ends in '}'. When it does not, the body was cut
		// short -- a close-delimited response over TLS that ended early -- and
		// that is a different fault from a server answering something else.
		size_t tail = len;
		while (tail > 0 && ((*body)[tail - 1] == '\n' || (*body)[tail - 1] == '\r' || (*body)[tail - 1] == ' ')) {
			tail--;
		}
		bool truncated = !(tail > 0 && (*body)[tail - 1] == '}');
		fprintf(stderr, "| %s\n", truncated ? "TRUNCATED" : "complete");

		// The sentence on screen carries the first printable characters of what
		// did arrive: "not JSON" alone means a trip to the card to read the log,
		// and this is the difference between one more question and the answer.
		bool xml = len > 0 && ((*body)[0] == '<');
		char preview[48];
		size_t at = 0;
		for (size_t i = 0; i < len && at + 1 < sizeof(preview); i++) {
			unsigned char c = (unsigned char)(*body)[i];
			preview[at++] = (c >= 32 && c < 127) ? (char)c : '.';
		}
		preview[at] = '\0';
		free(*body);
		*body = NULL;
		if (attempt++ == 0) {
			fprintf(stderr, "subsonic: %s: the answer arrived cut short, asking again\n", method);
			goto again;
		}
		if (xml) {
			set_error("the server answered XML, not JSON: %s", preview);
		} else {
			set_error("not JSON (%zu bytes, %s): %s", len, truncated ? "cut short" : "complete", preview);
		}
		return false;
	}
	*resp = json_get(doc, json_root(doc), "subsonic-response");
	char state[16] = "";
	json_obj_str(doc, *resp, "status", state, sizeof(state));
	if (*resp < 0 || strcmp(state, "ok") != 0) {
		int err = json_get(doc, *resp, "error");
		char message[160] = "";
		json_obj_str(doc, err, "message", message, sizeof(message));
		long code = json_obj_long(doc, err, "code", 0);
		if (error_code) {
			*error_code = (int)code;
		}
		// 40 is the one every user meets; the server's own sentence for the
		// rest, which is written for a human.
		if (code == 40) {
			set_error("%s", "the server refused the user name or password");
		} else if (message[0]) {
			set_error("%s", message);
		} else {
			set_error("the server answered %d", status);
		}
		json_free(doc);
		free(*body);
		*body = NULL;
		return false;
	}
	return true;
}

static bool call(const char *method, const char *extra, char **body, json_doc_t *doc, int *resp) {
	session_t s;
	session_copy(&s);
	return call_with(&s, method, extra, body, doc, resp, NULL, NULL);
}

static void call_done(char *body, json_doc_t *doc) {
	json_free(doc);
	free(body);
}

// ---------------------------------------------------------------------------
// sign-in
// ---------------------------------------------------------------------------

static void make_salt(char *out, size_t size) {
	unsigned char raw[8] = {0};
	int fd = open("/dev/urandom", O_RDONLY);
	bool got = false;
	if (fd >= 0) {
		got = read(fd, raw, sizeof(raw)) == (ssize_t)sizeof(raw);
		close(fd);
	}
	if (!got) {
		// No urandom (it is always there on Linux, but): time and pid. A salt
		// only has to differ from other clients', not be secret.
		unsigned long v = (unsigned long)time(NULL) ^ ((unsigned long)getpid() << 16);
		memcpy(raw, &v, sizeof(v) < sizeof(raw) ? sizeof(v) : sizeof(raw));
	}
	size_t n = 0;
	for (size_t i = 0; i < sizeof(raw) && n + 2 < size; i++) {
		n += (size_t)snprintf(out + n, size - n, "%02x", raw[i]);
	}
}

// "192.168.1.10:4533", "http://box:4533/", "https://music.example.org" -- all
// three to a base URL with a scheme and no trailing slash, because "/rest/ping"
// is appended to it.
//
// An empty field stays empty. It used to become "http://", whose trailing-slash
// strip then left "http:", and a bare OK on the sign-in form asked the server
// "http://http:". Nothing typed is nothing stored.
static void normalise_server(const char *in, char *out, size_t size) {
	if (!size) {
		return;
	}
	out[0] = '\0';
	if (!in) {
		return;
	}
	while (*in == ' ' || *in == '\t') {
		in++;
	}
	char work[SUBSONIC_SERVER_MAX];
	snprintf(work, sizeof(work), "%s", in);

	size_t len = strlen(work);
	while (len > 0 && (work[len - 1] == ' ' || work[len - 1] == '\t' || work[len - 1] == '\n')) {
		work[--len] = '\0';
	}
	bool https = strncmp(work, "https://", 8) == 0;
	bool http = !https && strncmp(work, "http://", 7) == 0;
	size_t floor = https ? 8 : http ? 7 : 0;
	// Trailing slashes go, but never the scheme's own "//".
	while (len > floor && work[len - 1] == '/') {
		work[--len] = '\0';
	}
	if (len <= floor) {
		return; // "http://" alone, or nothing at all
	}
	// Copied by hand rather than snprintf("%s%s"): a server longer than the
	// buffer is truncated to it, not warned about.
	const char *prefix = floor ? "" : "http://";
	size_t room = size - 1;
	size_t plen = strlen(prefix);
	if (plen > room) {
		plen = room;
	}
	memcpy(out, prefix, plen);
	size_t n = len < room - plen ? len : room - plen;
	memcpy(out + plen, work, n);
	out[plen + n] = '\0';
}

bool subsonic_login(const char *server, const char *user, const char *password) {
	if (!server || !server[0] || !user || !user[0] || !password) {
		set_error("%s", "server, user name and password are needed");
		return false;
	}
	session_t s;
	memset(&s, 0, sizeof(s));
	normalise_server(server, s.server, sizeof(s.server));
	snprintf(s.user, sizeof(s.user), "%s", user);
	make_salt(s.salt, sizeof(s.salt));

	char salted[512];
	snprintf(salted, sizeof(salted), "%s%s", password, s.salt);
	md5_hex(salted, strlen(salted), s.token);
	memset(salted, 0, sizeof(salted));

	char *body = NULL;
	json_doc_t doc;
	int resp = -1;
	int code = 0;
	int status = 0;
	bool ok = call_with(&s, "ping", NULL, &body, &doc, &resp, &code, &status);
	if (!ok && code == 41) {
		// "Token authentication not supported for LDAP users": the old form,
		// and the password has to be kept for it.
		printf("addon subsonic: the server refuses tokens, falling back to the password\n");
		s.token[0] = '\0';
		s.salt[0] = '\0';
		size_t n = 0;
		for (const unsigned char *p = (const unsigned char *)password; *p && n + 2 < sizeof(s.password_hex); p++) {
			n += (size_t)snprintf(s.password_hex + n, sizeof(s.password_hex) - n, "%02x", *p);
		}
		ok = call_with(&s, "ping", NULL, &body, &doc, &resp, &code, &status);
	}
	// A server behind a reverse proxy that terminates TLS answers a plain-HTTP
	// request with 426 Upgrade Required and no Location, so there is nothing for
	// the player's redirect handling to follow. Ask the same question over TLS
	// once, and keep the scheme that answered.
	if (!ok && status == 426 && strncmp(s.server, "http://", 7) == 0) {
		char previous[SUBSONIC_SERVER_MAX];
		snprintf(previous, sizeof(previous), "%s", s.server);
		snprintf(s.server, sizeof(s.server), "https://%s", previous + 7);
		printf("addon subsonic: %s wants TLS, asking again over https\n", previous);
		ok = call_with(&s, "ping", NULL, &body, &doc, &resp, &code, &status);
		if (!ok) {
			snprintf(s.server, sizeof(s.server), "%s", previous);
		}
	}
	if (!ok) {
		return false;
	}
	char kind[32] = "";
	char version[32] = "";
	json_obj_str(&doc, resp, "type", kind, sizeof(kind));
	json_obj_str(&doc, resp, "serverVersion", version, sizeof(version));
	printf("addon subsonic: signed in to %s (%s %s, API %s)\n", s.server, kind[0] ? kind : "subsonic", version,
		   json_obj_bool(&doc, resp, "openSubsonic", false) ? "OpenSubsonic" : "classic");
	call_done(body, &doc);

	pthread_mutex_lock(&session_lock);
	session = s;
	persist_locked();
	pthread_mutex_unlock(&session_lock);
	return true;
}

void subsonic_logout(void) {
	pthread_mutex_lock(&session_lock);
	memset(session.salt, 0, sizeof(session.salt));
	memset(session.token, 0, sizeof(session.token));
	memset(session.password_hex, 0, sizeof(session.password_hex));
	persist_locked();
	pthread_mutex_unlock(&session_lock);
}

// The addon is being unloaded. The session and the settings leave memory and
// nothing is written: the file on the card is the record, an unload is not a
// sign-out, and the write would land on a filesystem that may be going away with
// the card. What this buys is the library that starts again in this process --
// after a dlclose() that failed, say -- seeing the state a freshly loaded one
// would fill in from the file anyway, rather than a password sitting in a page
// nobody asked for.
void subsonic_forget(void) {
	pthread_mutex_lock(&session_lock);
	memset(&session, 0, sizeof(session));
	store_init(&settings, NULL);
	pthread_mutex_unlock(&session_lock);
	quality = SUBSONIC_QUALITY_ORIGINAL;
}

bool subsonic_ping(void) {
	char *body;
	json_doc_t doc;
	int resp;
	if (!call("ping", NULL, &body, &doc, &resp)) {
		return false;
	}
	call_done(body, &doc);
	return true;
}

// ---------------------------------------------------------------------------
// reading the entries
// ---------------------------------------------------------------------------

// An id as text whatever the server sent: OpenSubsonic says string, but older
// Ampache and Airsonic builds send numbers.
static void read_id(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	int tok = json_get(doc, obj, key);
	out[0] = '\0';
	if (tok < 0) {
		return;
	}
	if (doc->toks[tok].type == JSON_STRING) {
		json_str(doc, tok, out, size);
	} else if (doc->toks[tok].type == JSON_NUMBER) {
		int len = doc->toks[tok].end - doc->toks[tok].start;
		snprintf(out, size, "%.*s", len, doc->text + doc->toks[tok].start);
	}
}

static void read_track(const json_doc_t *doc, int obj, subsonic_track_t *t) {
	memset(t, 0, sizeof(*t));
	read_id(doc, obj, "id", t->id, sizeof(t->id));
	json_obj_str(doc, obj, "title", t->title, sizeof(t->title));
	json_obj_str(doc, obj, "artist", t->artist, sizeof(t->artist));
	json_obj_str(doc, obj, "album", t->album, sizeof(t->album));
	read_id(doc, obj, "albumId", t->album_id, sizeof(t->album_id));
	read_id(doc, obj, "coverArt", t->cover_id, sizeof(t->cover_id));
	json_obj_str(doc, obj, "suffix", t->suffix, sizeof(t->suffix));
	t->duration = (int)json_obj_long(doc, obj, "duration", 0);
	t->track_number = (int)json_obj_long(doc, obj, "track", 0);
	t->bit_rate = (int)json_obj_long(doc, obj, "bitRate", 0);
	t->starred = json_get(doc, obj, "starred") >= 0;
	if (!t->title[0]) {
		json_obj_str(doc, obj, "name", t->title, sizeof(t->title));
	}
}

static void read_album(const json_doc_t *doc, int obj, subsonic_album_t *a) {
	memset(a, 0, sizeof(*a));
	read_id(doc, obj, "id", a->id, sizeof(a->id));
	// getAlbumList2 and search3 say "name", the older calls "title".
	if (!json_obj_str(doc, obj, "name", a->title, sizeof(a->title)) || !a->title[0]) {
		json_obj_str(doc, obj, "title", a->title, sizeof(a->title));
	}
	json_obj_str(doc, obj, "artist", a->artist, sizeof(a->artist));
	read_id(doc, obj, "coverArt", a->cover_id, sizeof(a->cover_id));
	a->year = (int)json_obj_long(doc, obj, "year", 0);
	a->song_count = (int)json_obj_long(doc, obj, "songCount", 0);
}

static void read_artist(const json_doc_t *doc, int obj, subsonic_artist_t *a) {
	memset(a, 0, sizeof(*a));
	read_id(doc, obj, "id", a->id, sizeof(a->id));
	json_obj_str(doc, obj, "name", a->name, sizeof(a->name));
	read_id(doc, obj, "coverArt", a->cover_id, sizeof(a->cover_id));
	a->album_count = (int)json_obj_long(doc, obj, "albumCount", 0);
}

static void read_playlist(const json_doc_t *doc, int obj, subsonic_playlist_t *p) {
	memset(p, 0, sizeof(*p));
	read_id(doc, obj, "id", p->id, sizeof(p->id));
	json_obj_str(doc, obj, "name", p->name, sizeof(p->name));
	json_obj_str(doc, obj, "owner", p->owner, sizeof(p->owner));
	read_id(doc, obj, "coverArt", p->cover_id, sizeof(p->cover_id));
	p->song_count = (int)json_obj_long(doc, obj, "songCount", 0);
}

typedef void (*reader_fn)(const json_doc_t *doc, int obj, void *out);

// Copies elements [offset, offset + max) of array `arr`. For the calls with no
// paging of their own (getAlbum, getArtist, getPlaylist): the whole reply is
// already here, and the page just takes its slice.
static int read_slice(const json_doc_t *doc, int arr, int offset, void *out, size_t item, int max, reader_fn fn) {
	int total = json_len(doc, arr);
	int n = 0;
	for (int i = offset; i < total && n < max; i++) {
		fn(doc, json_at(doc, arr, i), (char *)out + (size_t)n * item);
		n++;
	}
	return n;
}

static void reader_track(const json_doc_t *d, int o, void *out) { read_track(d, o, out); }
static void reader_album(const json_doc_t *d, int o, void *out) { read_album(d, o, out); }
static void reader_artist(const json_doc_t *d, int o, void *out) { read_artist(d, o, out); }
static void reader_playlist(const json_doc_t *d, int o, void *out) { read_playlist(d, o, out); }

// One call, one array somewhere in the reply, one slice of it.
static int list_call(const char *method, const char *extra, const char *path, int offset, void *out, size_t item,
					 int max, reader_fn fn) {
	if (!out || max <= 0) {
		return 0;
	}
	char *body;
	json_doc_t doc;
	int resp;
	if (!call(method, extra, &body, &doc, &resp)) {
		return -1;
	}
	int n = read_slice(&doc, json_path(&doc, resp, path), offset, out, item, max, fn);
	call_done(body, &doc);
	return n;
}

static void id_param(const char *name, const char *value, char *out, size_t size) {
	char enc[SUBSONIC_ID_MAX * 3];
	http_url_encode(value ? value : "", enc, sizeof(enc));
	snprintf(out, size, "&%s=%s", name, enc);
}

// ---------------------------------------------------------------------------
// browsing
// ---------------------------------------------------------------------------

// getArtists groups the artists under index letters; the page wants one flat
// list, in the server's order.
int subsonic_artists(int offset, subsonic_artist_t *out, int max) {
	if (!out || max <= 0) {
		return 0;
	}
	char *body;
	json_doc_t doc;
	int resp;
	if (!call("getArtists", NULL, &body, &doc, &resp)) {
		return -1;
	}
	int index = json_path(&doc, resp, "artists.index");
	int seen = 0;
	int n = 0;
	for (int i = 0; i < json_len(&doc, index) && n < max; i++) {
		int arr = json_get(&doc, json_at(&doc, index, i), "artist");
		for (int k = 0; k < json_len(&doc, arr) && n < max; k++, seen++) {
			if (seen >= offset) {
				read_artist(&doc, json_at(&doc, arr, k), &out[n++]);
			}
		}
	}
	call_done(body, &doc);
	return n;
}

int subsonic_artist_albums(const char *artist_id, int offset, subsonic_album_t *out, int max) {
	char extra[SUBSONIC_ID_MAX * 3 + 8];
	id_param("id", artist_id, extra, sizeof(extra));
	return list_call("getArtist", extra, "artist.album", offset, out, sizeof(*out), max, reader_album);
}

int subsonic_album_tracks(const char *album_id, int offset, subsonic_track_t *out, int max) {
	char extra[SUBSONIC_ID_MAX * 3 + 8];
	id_param("id", album_id, extra, sizeof(extra));
	return list_call("getAlbum", extra, "album.song", offset, out, sizeof(*out), max, reader_track);
}

int subsonic_album_list(const char *type, int offset, subsonic_album_t *out, int max) {
	char extra[128];
	snprintf(extra, sizeof(extra), "&type=%s&size=%d&offset=%d", type, max, offset);
	return list_call("getAlbumList2", extra, "albumList2.album", 0, out, sizeof(*out), max, reader_album);
}

static int search(const char *query, const char *what, int offset, void *out, size_t item, int max,
				  const char *path, reader_fn fn) {
	char enc[600];
	http_url_encode(query ? query : "", enc, sizeof(enc));
	char extra[800];
	// Only the kind asked for: the other two counts at zero.
	snprintf(extra, sizeof(extra), "&query=%s&artistCount=%d&artistOffset=%d&albumCount=%d&albumOffset=%d"
								   "&songCount=%d&songOffset=%d",
			 enc, strcmp(what, "artist") == 0 ? max : 0, offset, strcmp(what, "album") == 0 ? max : 0, offset,
			 strcmp(what, "song") == 0 ? max : 0, offset);
	return list_call("search3", extra, path, 0, out, item, max, fn);
}

int subsonic_search_tracks(const char *query, int offset, subsonic_track_t *out, int max) {
	return search(query, "song", offset, out, sizeof(*out), max, "searchResult3.song", reader_track);
}

int subsonic_search_albums(const char *query, int offset, subsonic_album_t *out, int max) {
	return search(query, "album", offset, out, sizeof(*out), max, "searchResult3.album", reader_album);
}

int subsonic_search_artists(const char *query, int offset, subsonic_artist_t *out, int max) {
	return search(query, "artist", offset, out, sizeof(*out), max, "searchResult3.artist", reader_artist);
}

int subsonic_playlists(int offset, subsonic_playlist_t *out, int max) {
	return list_call("getPlaylists", NULL, "playlists.playlist", offset, out, sizeof(*out), max, reader_playlist);
}

int subsonic_playlist_tracks(const char *playlist_id, int offset, subsonic_track_t *out, int max) {
	char extra[SUBSONIC_ID_MAX * 3 + 8];
	id_param("id", playlist_id, extra, sizeof(extra));
	return list_call("getPlaylist", extra, "playlist.entry", offset, out, sizeof(*out), max, reader_track);
}

int subsonic_random_tracks(subsonic_track_t *out, int max) {
	char extra[32];
	snprintf(extra, sizeof(extra), "&size=%d", max);
	return list_call("getRandomSongs", extra, "randomSongs.song", 0, out, sizeof(*out), max, reader_track);
}

// The server card's line: a ping, timed, and the size of the library. The
// ping is the answer to "is it there"; getScanStatus only adds the count, and
// a server that does not have it (older Subsonic forks) or does not say it
// still counts as connected.
bool subsonic_status(int *tracks, int *ms) {
	*tracks = -1;
	*ms = -1;
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	if (!subsonic_ping()) {
		return false;
	}
	clock_gettime(CLOCK_MONOTONIC, &b);
	*ms = (int)((b.tv_sec - a.tv_sec) * 1000 + (b.tv_nsec - a.tv_nsec) / 1000000);
	char *body;
	json_doc_t doc;
	int resp;
	if (call("getScanStatus", NULL, &body, &doc, &resp)) {
		int status = json_get(&doc, resp, "scanStatus");
		if (status >= 0) {
			*tracks = (int)json_obj_long(&doc, status, "count", -1);
		}
		call_done(body, &doc);
	}
	return true;
}

int subsonic_starred_tracks(int offset, subsonic_track_t *out, int max) {
	return list_call("getStarred2", NULL, "starred2.song", offset, out, sizeof(*out), max, reader_track);
}

int subsonic_starred_albums(int offset, subsonic_album_t *out, int max) {
	return list_call("getStarred2", NULL, "starred2.album", offset, out, sizeof(*out), max, reader_album);
}

int subsonic_starred_ids(char (*out)[SUBSONIC_ID_MAX], int max) {
	char *body;
	json_doc_t doc;
	int resp;
	if (!call("getStarred2", NULL, &body, &doc, &resp)) {
		return -1;
	}
	int arr = json_path(&doc, resp, "starred2.song");
	int n = 0;
	for (int i = 0; i < json_len(&doc, arr) && n < max; i++) {
		read_id(&doc, json_at(&doc, arr, i), "id", out[n], SUBSONIC_ID_MAX);
		if (out[n][0]) {
			n++;
		}
	}
	call_done(body, &doc);
	return n;
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

static bool simple_call(const char *method, const char *extra) {
	char *body;
	json_doc_t doc;
	int resp;
	if (!call(method, extra, &body, &doc, &resp)) {
		return false;
	}
	call_done(body, &doc);
	return true;
}

bool subsonic_star(const char *id, bool album, bool want) {
	char extra[SUBSONIC_ID_MAX * 3 + 16];
	id_param(album ? "albumId" : "id", id, extra, sizeof(extra));
	return simple_call(want ? "star" : "unstar", extra);
}

bool subsonic_scrobble(const char *song_id, bool submission) {
	char extra[SUBSONIC_ID_MAX * 3 + 64];
	id_param("id", song_id, extra, sizeof(extra));
	size_t len = strlen(extra);
	snprintf(extra + len, sizeof(extra) - len, "&submission=%s", submission ? "true" : "false");
	return simple_call("scrobble", extra);
}

bool subsonic_playlist_add(const char *playlist_id, const char *song_id) {
	char a[SUBSONIC_ID_MAX * 3 + 16];
	char b[SUBSONIC_ID_MAX * 3 + 16];
	id_param("playlistId", playlist_id, a, sizeof(a));
	id_param("songIdToAdd", song_id, b, sizeof(b));
	char extra[sizeof(a) + sizeof(b)];
	snprintf(extra, sizeof(extra), "%s%s", a, b);
	return simple_call("updatePlaylist", extra);
}

bool subsonic_playlist_create(const char *name, const char *song_id) {
	char enc[SUBSONIC_TITLE_MAX * 3];
	http_url_encode(name ? name : "", enc, sizeof(enc));
	char b[SUBSONIC_ID_MAX * 3 + 16];
	id_param("songId", song_id, b, sizeof(b));
	char extra[sizeof(enc) + sizeof(b) + 8];
	snprintf(extra, sizeof(extra), "&name=%s%s", enc, b);
	return simple_call("createPlaylist", extra);
}
