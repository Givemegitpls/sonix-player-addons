// Piped: YouTube, read through a public Piped instance. See pgw.h for why an
// instance and what the two live ones actually serve.
//
// The shape of this file follows the other streaming clients: a request goes
// out with http_get_ex(), the body comes back to the caller to parse with
// json.h, and every failure is turned into one of the four answers the page can
// say something about. What is Qobuz's and Tidal's job is different in one
// respect: there is no server of ours here and no account, only a list of
// volunteer-run instances, so every call carries a failover with it.

#include "src/system/gw/pgw.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/store.h"

#include "src/system/core/json.h"
#include "src/system/net/http.h"

// The instances the addon knows about without asking anyone. Both answered on
// 2026-11-05: /search with twenty items, /streams with a proxied itag 18 and a
// working Range request on it. The public list the LibreTube and Piped projects
// publish was checked at the same time -- fifteen hosts -- and every other one
// was dead, unresolvable, or answering 502.
static const char *built_in[] = {
	"https://api.piped.private.coffee",
	"https://pipedapi.ducks.party",
};
#define BUILT_IN_COUNT ((int)(sizeof(built_in) / sizeof(built_in[0])))

// How much of a response is kept. Measured sizes: a search is 10-15 KB, a
// stream with twenty related items 26 KB, /config under 1 KB. The limits are
// two orders of magnitude above that because an instance that answers with a
// long item list is not an error, and truncating JSON would turn it into one.
#define PGW_SEARCH_LIMIT (256 * 1024)
#define PGW_STREAM_LIMIT (512 * 1024)
#define PGW_CONFIG_LIMIT (64 * 1024)

#define PGW_TIMEOUT_SECS 12
#define PGW_PROBE_SECS 5

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

// The instance that answered last, and the one the user pinned. "pinned" is
// what settings.ini holds and is never written by a failover: a card that
// remembers "whatever worked last week" would keep choosing a dead host after
// the user has picked one on purpose.
static char active[PGW_INSTANCE_MAX];
static char pinned[PGW_INSTANCE_MAX];

static store_t settings;
static bool settings_open;

static void lock(void) { pthread_mutex_lock(&state_lock); }
static void unlock(void) { pthread_mutex_unlock(&state_lock); }

static void copy_bounded(char *out, size_t size, const char *in) {
	if (!out || !size) {
		return;
	}
	snprintf(out, size, "%s", in ? in : "");
}

// ---------------------------------------------------------------------------
// URLs
// ---------------------------------------------------------------------------

// "https://host/" and "host" both become "https://host". Anything that does not
// look like http(s) is refused rather than passed to the network layer, because
// the scheme is the user's choice and a typo there should say so instead of
// failing much later as "no instance answered".
static bool normalise(const char *in, char *out, size_t size) {
	if (!in || !in[0] || !out || size < 10) {
		return false;
	}
	char buf[PGW_INSTANCE_MAX];
	copy_bounded(buf, sizeof(buf), in);
	size_t len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '/' || buf[len - 1] == ' ' || buf[len - 1] == '\n')) {
		buf[--len] = '\0';
	}
	if (len == 0) {
		return false;
	}
	const char *base = buf;
	if (strncmp(base, "http://", 7) != 0 && strncmp(base, "https://", 8) != 0) {
		if (snprintf(out, size, "https://%s", base) >= (int)size) {
			return false;
		}
	} else if (snprintf(out, size, "%s", base) >= (int)size) {
		return false;
	}
	return out[0] != '\0';
}

// Percent-encodes everything outside the unreserved set, which is what
// youtubei's search query needs: a space, an ampersand or a '#' in a track name
// would otherwise end the query or the URL itself.
static void url_encode(const char *src, char *out, size_t size) {
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	if (!out || size == 0) {
		return;
	}
	for (const unsigned char *p = (const unsigned char *)src; p && *p; p++) {
		unsigned char c = *p;
		bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
					 c == '.' || c == '~';
		size_t need = plain ? 1 : 3;
		if (o + need + 1 > size) {
			break;
		}
		if (plain) {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0xf];
		}
	}
	out[o] = '\0';
}

// ---------------------------------------------------------------------------
// Requests
// ---------------------------------------------------------------------------

// One request against one instance. `path` starts with '/'. On success *out is
// a malloc'd, NUL-terminated body the caller frees.
//
// The status is passed out rather than folded into the answer: a 404 from a
// working instance means "this video is gone" and is worth remembering while
// the failover continues, whereas a timeout means nothing at all about the
// video.
static bool fetch(const char *base, const char *path, size_t limit, int timeout_secs, char **out, size_t *out_len,
				  int *status_out) {
	char url[PGW_URL_MAX + 160];
	if (snprintf(url, sizeof(url), "%s%s", base, path) >= (int)sizeof(url)) {
		return false;
	}
	const char *headers = "Accept: application/json\r\n";
	char *body = NULL;
	size_t len = 0;
	int status = 0;
	bool ok = http_get_ex(url, headers, true, &body, &len, limit, timeout_secs, &status);
	if (status_out) {
		*status_out = status;
	}
	if (!ok || !body) {
		free(body);
		return false;
	}
	// http_get_ex() with want_error_body keeps a 4xx body, and an instance that
	// is down answers with its proxy's HTML rather than with JSON. Both are
	// failures for this client, and saying so here keeps every caller from
	// repeating the check.
	if (status < 200 || status >= 300 || len < 2 || body[0] != '{') {
		free(body);
		return false;
	}
	*out = body;
	if (out_len) {
		*out_len = len;
	}
	return true;
}

// The candidates in the order they are tried: what the user pinned (if
// anything), then the built-ins. Written to `list` as pointers into `pinned` or
// into built_in, so nothing is copied and nothing is freed.
static int candidates(const char **list, int max) {
	int n = 0;
	lock();
	if (pinned[0]) {
		list[n++] = pinned;
	}
	unlock();
	for (int i = 0; i < BUILT_IN_COUNT && n < max; i++) {
		// A pinned instance that is also in the list is not asked twice.
		bool dup = false;
		for (int j = 0; j < n; j++) {
			if (strcmp(list[j], built_in[i]) == 0) {
				dup = true;
			}
		}
		if (!dup) {
			list[n++] = built_in[i];
		}
	}
	return n;
}

// The call every public function goes through: same path against every
// candidate until one answers JSON. `not_found` is set when at least one
// instance said 404, so that "the video is gone" and "nothing answered" do not
// come back as the same sentence.
static pgw_err_t call(const char *path, size_t limit, int timeout_secs, char **body, size_t *body_len,
					  bool *not_found) {
	const char *list[PGW_INSTANCES_MAX + 1];
	int n = candidates(list, PGW_INSTANCES_MAX + 1);
	bool saw_404 = false;
	bool saw_5xx = false;

	// The instance that answered last goes first, but only if it is still one
	// of the candidates -- a pinned instance that has been changed since must
	// not keep being asked.
	char cached[PGW_INSTANCE_MAX];
	lock();
	copy_bounded(cached, sizeof(cached), active);
	unlock();
	if (cached[0]) {
		for (int i = 0; i < n; i++) {
			if (strcmp(list[i], cached) == 0) {
				const char *tmp = list[0];
				list[0] = list[i];
				list[i] = tmp;
				break;
			}
		}
	}

	for (int i = 0; i < n; i++) {
		int status = 0;
		if (fetch(list[i], path, limit, timeout_secs, body, body_len, &status)) {
			lock();
			copy_bounded(active, sizeof(active), list[i]);
			unlock();
			return PGW_OK;
		}
		if (status == 404) {
			saw_404 = true;
		} else if (status >= 500) {
			saw_5xx = true;
		}
	}
	if (not_found) {
		*not_found = saw_404;
	}
	if (saw_404) {
		return PGW_ERR_NOT_FOUND;
	}
	// 500 is what an instance answers when YouTube refuses its own IPs --
	// "SignInConfirmNotBotException: YouTube probably temporarily blocked
	// anonymous access from this IP". It is not our network and not this
	// addon, and the sentence has to say so rather than "no instance answered".
	return saw_5xx ? PGW_ERR_INSTANCE : PGW_ERR_NET;
}

// ---------------------------------------------------------------------------
// JSON helpers
// ---------------------------------------------------------------------------

// The id inside a Piped item: "url" is "/watch?v=<id>", or the item's own
// "videoId" on some endpoints. Both are accepted; the id stops at '&'.
static void item_id(const json_doc_t *doc, int obj, char *out, size_t size) {
	out[0] = '\0';
	if (json_obj_str(doc, obj, "videoId", out, size)) {
		return;
	}
	char url[PGW_URL_MAX];
	if (!json_obj_str(doc, obj, "url", url, sizeof(url))) {
		return;
	}
	const char *v = strstr(url, "v=");
	if (!v) {
		return;
	}
	v += 2;
	size_t n = strcspn(v, "&");
	if (n >= size) {
		n = size - 1;
	}
	memcpy(out, v, n);
	out[n] = '\0';
}

// An item's picture: absolute when the instance proxied it (which is what the
// two live ones do), a path on the instance otherwise.
static void item_thumb(const json_doc_t *doc, int obj, const char *key, const char *base, char *out, size_t size) {
	out[0] = '\0';
	char raw[PGW_URL_MAX];
	if (!json_obj_str(doc, obj, key, raw, sizeof(raw)) || !raw[0]) {
		return;
	}
	if (strncmp(raw, "http://", 7) == 0 || strncmp(raw, "https://", 8) == 0) {
		copy_bounded(out, size, raw);
	} else if (raw[0] == '/') {
		snprintf(out, size, "%s%s", base, raw);
	} else {
		copy_bounded(out, size, raw);
	}
}

static void read_item(const json_doc_t *doc, int obj, const char *base, pgw_item_t *out) {
	memset(out, 0, sizeof(*out));
	item_id(doc, obj, out->id, sizeof(out->id));
	json_obj_str(doc, obj, "title", out->title, sizeof(out->title));
	if (!out->title[0]) {
		json_obj_str(doc, obj, "name", out->title, sizeof(out->title));
	}
	if (!json_obj_str(doc, obj, "uploaderName", out->uploader, sizeof(out->uploader))) {
		json_obj_str(doc, obj, "uploader", out->uploader, sizeof(out->uploader));
	}
	item_thumb(doc, obj, "thumbnail", base, out->thumbnail, sizeof(out->thumbnail));
	out->duration = (int)json_obj_long(doc, obj, "duration", 0);
	out->live = json_obj_bool(doc, obj, "livestream", false);
}

// ---------------------------------------------------------------------------
// The interface
// ---------------------------------------------------------------------------

void pgw_init(const char *addon_dir, const char *sd_root) {
	(void)sd_root;
	lock();
	active[0] = '\0';
	pinned[0] = '\0';
	settings_open = false;
	unlock();

	if (!addon_dir || !addon_dir[0]) {
		return;
	}
	char path[600];
	if (snprintf(path, sizeof(path), "%s/settings.ini", addon_dir) >= (int)sizeof(path)) {
		return;
	}
	lock();
	store_init(&settings, path);
	store_load(&settings);
	copy_bounded(pinned, sizeof(pinned), store_get(&settings, "instance"));
	settings_open = true;
	unlock();
}

void pgw_forget(void) {
	lock();
	active[0] = '\0';
	settings_open = false;
	unlock();
}

// A pointer into a per-thread copy: the interface asks for this while a worker
// may be updating `active`, and handing out a pointer into shared state that
// changes under the reader is a crash waiting for a slow day.
const char *pgw_instance(void) {
	static _Thread_local char out[PGW_INSTANCE_MAX];
	lock();
	copy_bounded(out, sizeof(out), active);
	unlock();
	return out;
}

bool pgw_set_instance(const char *url) {
	char norm[PGW_INSTANCE_MAX];
	bool ok = true;
	if (url && url[0]) {
		if (!normalise(url, norm, sizeof(norm))) {
			return false;
		}
	} else {
		norm[0] = '\0';
	}

	lock();
	copy_bounded(pinned, sizeof(pinned), norm);
	active[0] = '\0'; // the next call probes rather than trusting the old one
	if (settings_open) {
		store_set(&settings, "instance", pinned);
		ok = store_save(&settings);
	}
	unlock();
	return ok;
}

int pgw_instance_count(void) { return BUILT_IN_COUNT; }

const char *pgw_instance_at(int index) { return index >= 0 && index < BUILT_IN_COUNT ? built_in[index] : ""; }

pgw_err_t pgw_health(int timeout_secs) {
	const char *list[PGW_INSTANCES_MAX + 1];
	int n = candidates(list, PGW_INSTANCES_MAX + 1);
	int wait = timeout_secs > 0 ? timeout_secs : PGW_PROBE_SECS;

	for (int i = 0; i < n; i++) {
		char *body = NULL;
		size_t len = 0;
		int status = 0;
		if (!fetch(list[i], "/config", PGW_CONFIG_LIMIT, wait, &body, &len, &status)) {
			continue;
		}
		json_doc_t doc;
		bool ok = json_parse(body, &doc);
		if (ok) {
			int root = json_root(&doc);
			// Both fields are Piped's; either one means this is an API and not
			// an instance's front page answering on the same path.
			bool api = json_get(&doc, root, "imageProxyUrl") >= 0 || json_get(&doc, root, "countries") >= 0 ||
					   json_get(&doc, root, "instanceName") >= 0;
			json_free(&doc);
			ok = api;
		}
		free(body);
		if (!ok) {
			continue;
		}
		lock();
		copy_bounded(active, sizeof(active), list[i]);
		unlock();
		return PGW_OK;
	}
	return PGW_ERR_NET;
}

pgw_err_t pgw_search(const char *query, const char *filter, pgw_item_t *out, int max, int *count) {
	if (count) {
		*count = 0;
	}
	if (!query || !query[0] || !out || max <= 0) {
		return PGW_ERR_NET;
	}

	char encoded[512];
	url_encode(query, encoded, sizeof(encoded));
	char path[PGW_URL_MAX];
	snprintf(path, sizeof(path), "/search?q=%s&filter=%s", encoded, filter && filter[0] ? filter : "music_songs");

	char *body = NULL;
	size_t len = 0;
	bool not_found = false;
	pgw_err_t err = call(path, PGW_SEARCH_LIMIT, PGW_TIMEOUT_SECS, &body, &len, &not_found);
	if (err != PGW_OK) {
		return err;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		return PGW_ERR_INSTANCE;
	}

	const char *base = pgw_instance();
	int root = json_root(&doc);
	int items = json_path(&doc, root, "items");
	int n = 0;
	int total = items >= 0 ? json_len(&doc, items) : 0;
	for (int i = 0; i < total && n < max; i++) {
		int obj = json_at(&doc, items, i);
		if (obj < 0) {
			continue;
		}
		// A search answers with channels, playlists and streams in one array.
		// Only the streams are playable, and the others are not offered: the
		// instance cannot list a playlist's contents (pgw.h says why), so a
		// row that opens an empty page is worse than no row.
		char type[32];
		json_obj_str(&doc, obj, "type", type, sizeof(type));
		if (strcmp(type, "stream") != 0) {
			continue;
		}
		read_item(&doc, obj, base, &out[n]);
		if (out[n].id[0]) {
			n++;
		}
	}
	json_free(&doc);
	free(body);

	if (count) {
		*count = n;
	}
	return n > 0 ? PGW_OK : PGW_ERR_NOT_FOUND;
}

pgw_err_t pgw_track(const char *id, pgw_track_t *out) {
	if (out) {
		memset(out, 0, sizeof(*out));
	}
	if (!id || !id[0] || !out) {
		return PGW_ERR_NOT_FOUND;
	}

	char path[PGW_ID_MAX + 16];
	snprintf(path, sizeof(path), "/streams/%s", id);

	char *body = NULL;
	size_t len = 0;
	bool not_found = false;
	pgw_err_t err = call(path, PGW_STREAM_LIMIT, PGW_TIMEOUT_SECS, &body, &len, &not_found);
	if (err != PGW_OK) {
		return err;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		return PGW_ERR_INSTANCE;
	}

	const char *base = pgw_instance();
	int root = json_root(&doc);
	copy_bounded(out->id, sizeof(out->id), id);
	json_obj_str(&doc, root, "title", out->title, sizeof(out->title));
	json_obj_str(&doc, root, "uploader", out->uploader, sizeof(out->uploader));
	item_thumb(&doc, root, "thumbnailUrl", base, out->thumbnail, sizeof(out->thumbnail));
	out->duration = (int)json_obj_long(&doc, root, "duration", 0);
	out->live = json_obj_bool(&doc, root, "livestream", false);

	// The order is the contract's: an audio-only stream if the instance has
	// one, else the muxed 360p MP4, else any MP4. The player has no WebM
	// demuxer, so a WebM entry is not a candidate at all -- and today none is
	// offered anyway, because audioStreams is empty on every live instance.
	int audio = json_path(&doc, root, "audioStreams");
	int video = json_path(&doc, root, "videoStreams");
	int audio_n = audio >= 0 ? json_len(&doc, audio) : 0;
	int video_n = video >= 0 ? json_len(&doc, video) : 0;
	int chosen = -1;
	int chosen_container = 0; // 1 audio, 2 video

	for (int i = 0; i < audio_n && chosen < 0; i++) {
		int obj = json_at(&doc, audio, i);
		char url[PGW_URL_MAX], mime[PGW_MIME_MAX];
		if (obj < 0 || !json_obj_str(&doc, obj, "url", url, sizeof(url)) || !url[0]) {
			continue;
		}
		if (!json_obj_str(&doc, obj, "mimeType", mime, sizeof(mime)) || strncmp(mime, "audio/", 6) != 0) {
			continue;
		}
		chosen = obj;
		chosen_container = 1;
	}
	if (chosen < 0) {
		for (int i = 0; i < video_n && chosen < 0; i++) {
			int obj = json_at(&doc, video, i);
			char url[PGW_URL_MAX];
			if (obj < 0 || !json_obj_str(&doc, obj, "url", url, sizeof(url)) || !url[0]) {
				continue;
			}
			if (json_obj_long(&doc, obj, "itag", 0) == 18) {
				chosen = obj;
			}
		}
	}
	if (chosen < 0) {
		for (int i = 0; i < video_n && chosen < 0; i++) {
			int obj = json_at(&doc, video, i);
			char url[PGW_URL_MAX], mime[PGW_MIME_MAX];
			if (obj < 0 || !json_obj_str(&doc, obj, "url", url, sizeof(url)) || !url[0]) {
				continue;
			}
			if (json_obj_str(&doc, obj, "mimeType", mime, sizeof(mime)) && strncmp(mime, "video/mp4", 9) == 0) {
				chosen = obj;
			}
		}
	}

	pgw_err_t answer = PGW_ERR_FORMAT;
	if (chosen >= 0) {
		char url[PGW_URL_MAX], mime[PGW_MIME_MAX];
		json_obj_str(&doc, chosen, "url", url, sizeof(url));
		json_obj_str(&doc, chosen, "mimeType", mime, sizeof(mime));
		copy_bounded(out->url, sizeof(out->url), url);
		copy_bounded(out->mime, sizeof(out->mime), mime[0] ? mime : (chosen_container == 1 ? "audio/mp4" : "video/mp4"));
		out->itag = (int)json_obj_long(&doc, chosen, "itag", 0);
		long bitrate = json_obj_long(&doc, chosen, "bitrate", 0);
		out->bitrate_kbps = bitrate > 0 ? (int)(bitrate / 1000) : 0;
		answer = PGW_OK;
	}

	json_free(&doc);
	free(body);
	return answer;
}

pgw_err_t pgw_related(const char *id, pgw_item_t *out, int max, int *count) {
	if (count) {
		*count = 0;
	}
	if (!id || !id[0] || !out || max <= 0) {
		return PGW_ERR_NOT_FOUND;
	}

	// The instance sends the related list with the video itself; there is no
	// separate endpoint (the /next route answers 404 HTML), so this is one more
	// /streams call and the JSON is read twice as far as the name goes.
	char path[PGW_ID_MAX + 16];
	snprintf(path, sizeof(path), "/streams/%s", id);

	char *body = NULL;
	size_t len = 0;
	bool not_found = false;
	pgw_err_t err = call(path, PGW_STREAM_LIMIT, PGW_TIMEOUT_SECS, &body, &len, &not_found);
	if (err != PGW_OK) {
		return err;
	}

	json_doc_t doc;
	if (!json_parse(body, &doc)) {
		free(body);
		return PGW_ERR_INSTANCE;
	}

	const char *base = pgw_instance();
	int root = json_root(&doc);
	int related = json_path(&doc, root, "relatedStreams");
	int total = related >= 0 ? json_len(&doc, related) : 0;
	int n = 0;
	for (int i = 0; i < total && n < max; i++) {
		int obj = json_at(&doc, related, i);
		if (obj < 0) {
			continue;
		}
		char type[32];
		json_obj_str(&doc, obj, "type", type, sizeof(type));
		if (type[0] && strcmp(type, "stream") != 0) {
			continue;
		}
		read_item(&doc, obj, base, &out[n]);
		if (out[n].id[0] && strcmp(out[n].id, id) != 0) {
			n++;
		}
	}
	json_free(&doc);
	free(body);

	if (count) {
		*count = n;
	}
	return n > 0 ? PGW_OK : PGW_ERR_NOT_FOUND;
}

const char *pgw_error_text(pgw_err_t err) {
	switch (err) {
	case PGW_OK:
		return "";
	case PGW_ERR_INSTANCE:
		return "The instance could not get this from YouTube";
	case PGW_ERR_NOT_FOUND:
		return "No results, or the video is not on this instance";
	case PGW_ERR_FORMAT:
		return "No playable format in this video";
	case PGW_ERR_NET:
	default:
		return "No instance answered";
	}
}
