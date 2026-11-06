// Tracks fetched to the card and then played like any other file. See pplay.h
// for why a file and not a stream, and for the two sidecars the player reads.
//
// The shape is zvuk's and qobuz's: one transfer at a time, the shared download
// turn, growfile so playback starts on the file that is still arriving, and a
// prepare callback for the track after the one playing. What is different here
// is the source: a track is one more /streams request to a Piped instance and
// the media URL is that instance's /videoplayback -- a plain GET with a
// Content-Length and Range support, no DASH, no separate audio URL and nothing
// to re-encapsulate.

#include "src/system/play/pplay.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/cardpace.h"

#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"

#include "src/gui/shell/gui.h"

// Enough for the MP4 header and the first seconds of AAC: itag 18 is 96 kbps,
// so 128 KB is about ten seconds of material, and the moov box ahead of mdat is
// under 100 KB for the videos the instances serve (checked: ftyp/moov/mdat).
// Waiting for the whole track instead would be half a minute of "starting" for
// every tap on a slow link.
#define YPLAY_PREBUFFER (128 * 1024)
#define YPLAY_CHUNK (64 * 1024)
#define YPLAY_TIMEOUT 20
#define YPLAY_COVER_LIMIT (2 * 1024 * 1024)
#define YPLAY_PATH_MAX 600

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prebuffer_cond = PTHREAD_COND_INITIALIZER;

static char cache_dir[YPLAY_PATH_MAX];

// The queue the player was given, kept so the prepare callback can answer "is
// this file mine, and which id is it". Copied, because the page's array lives
// on a worker's stack and is gone when the callback runs.
static pgw_item_t *queue;
static int queue_count;
static int queue_start;

// One transfer at a time, and the thread that runs it. `abandon` is read by the
// transfer loop; `active_stream` is what http_stream_wake() needs to end a read
// sitting in recv() -- without it a stop would wait out the receive timeout.
static pthread_t worker;
static bool worker_live;
static bool abandon;
static bool network_wanted;
static bool prebuffer_ready;
static http_stream_t *active_stream;

// Set by the prepare callback: when this download finishes, playback is
// restarted at `resume_index` -- the file it was waiting for is now there.
static bool resume_when_ready;
static int resume_index;
static char resume_id[PGW_ID_MAX];

static bool prepare_installed;

static bool prepare_track(const char *path);


// Bounded concatenation. Written out rather than left to snprintf because the
// compiler cannot see that a cache directory, an id and a suffix always fit,
// and a page of -Wformat-truncation warnings hides the ones that matter.
static void append(char *out, size_t size, size_t *used, const char *s) {
	while (s && *s && *used + 1 < size) {
		out[(*used)++] = *s++;
	}
	out[*used] = '\0';
}

static void join_path(char *out, size_t size, const char *a, const char *b, const char *c) {
	if (!out || size == 0) {
		return;
	}
	size_t used = 0;
	out[0] = '\0';
	append(out, size, &used, a);
	append(out, size, &used, b);
	append(out, size, &used, c);
}

static void path_for(const char *id, const char *suffix, char *out, size_t size) {
	if (!out || !size) {
		return;
	}
	if (!id || !id[0] || !cache_dir[0]) {
		out[0] = '\0';
		return;
	}
	join_path(out, size, cache_dir, "/", id);
	size_t used = strlen(out);
	append(out, size, &used, suffix);
}

// The id at the end of "<cache>/<id>.mp4", or "" for anything else.
static void id_from_path(const char *path, char *out, size_t size) {
	out[0] = '\0';
	if (!path || !cache_dir[0]) {
		return;
	}
	size_t n = strlen(cache_dir);
	if (strncmp(path, cache_dir, n) != 0 || path[n] != '/') {
		return;
	}
	const char *base = path + n + 1;
	size_t len = strcspn(base, ".");
	if (len == 0 || len >= size) {
		return;
	}
	memcpy(out, base, len);
	out[len] = '\0';
}

void pplay_init(const char *addon_dir, const char *sd_root) {
	(void)sd_root;
	pthread_mutex_lock(&lock);
	if (addon_dir && addon_dir[0]) {
		snprintf(cache_dir, sizeof(cache_dir), "%s/cache", addon_dir);
	} else {
		cache_dir[0] = '\0';
	}
	pthread_mutex_unlock(&lock);

	// Once per library, and taken back in stop(): a reload installs a fresh
	// callback and the loader never keeps one pointing into a mapping it has
	// dropped.
	if (!prepare_installed) {
		prepare_installed = true;
		device_state_add_prepare_cb(prepare_track);
		streamturn_register_abandon(pplay_abandon);
	}
}

const char *pplay_cache_dir(void) { return cache_dir; }

void pplay_path(const char *id, char *out, size_t size) { path_for(id, ".mp4", out, size); }

// Complete when the file is there and the ".part" marker -- written before the
// first byte and removed after the last -- is not. Without the marker a
// download cut by a card pull would look like a finished track and play as
// silence followed by a stop.
bool pplay_ready(const char *id) {
	char path[YPLAY_PATH_MAX];
	char part[YPLAY_PATH_MAX];
	path_for(id, ".mp4", path, sizeof(path));
	path_for(id, ".mp4.part", part, sizeof(part));
	if (!path[0] || access(path, R_OK) != 0) {
		return false;
	}
	return access(part, F_OK) != 0;
}

// ---------------------------------------------------------------------------
// sidecars
// ---------------------------------------------------------------------------

// metadata.c reads "<file>.tags" after the file's own tags and lets it win;
// this is the only place the title and the artist can come from, because an
// itag 18 stream carries neither.
static void write_tags(const char *path, const pgw_track_t *track) {
	char tags[YPLAY_PATH_MAX];
	char tmp[YPLAY_PATH_MAX];
	join_path(tags, sizeof(tags), path, ".tags", "");
	join_path(tmp, sizeof(tmp), tags, ".tmp", "");
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	fprintf(f, "title=%s\n", track->title);
	fprintf(f, "artist=%s\n", track->uploader);
	bool ok = fclose(f) == 0;
	if (!ok || rename(tmp, tags) != 0) {
		remove(tmp);
	}
}

// albumart.c looks for "<name>.jpg" beside the file, so the cover needs no tag.
// The instance's image proxy answers WebP for the sizes the app asks for, and
// the player decodes JPEG, PNG and GIF: a WebP is dropped here rather than
// written and then ignored by every reader.
static void fetch_cover(const char *path, const pgw_track_t *track) {
	if (!track->thumbnail[0]) {
		return;
	}
	char *body = NULL;
	size_t len = 0;
	if (!http_get(track->thumbnail, &body, &len, YPLAY_COVER_LIMIT, 10) || !body || len < 12) {
		free(body);
		return;
	}
	bool jpeg = (unsigned char)body[0] == 0xff && (unsigned char)body[1] == 0xd8;
	bool png = memcmp(body, "\x89PNG", 4) == 0;
	bool gif = memcmp(body, "GIF8", 4) == 0;
	if (!jpeg && !png && !gif) {
		free(body);
		return;
	}
	size_t n = strlen(path);
	if (n < 5) {
		free(body);
		return;
	}
	char base[YPLAY_PATH_MAX];
	snprintf(base, sizeof(base), "%.*s", (int)(n - 4), path);
	char cover[YPLAY_PATH_MAX];
	join_path(cover, sizeof(cover), base, ".jpg", "");
	char tmp[YPLAY_PATH_MAX];
	join_path(tmp, sizeof(tmp), cover, ".tmp", "");
	FILE *f = fopen(tmp, "wb");
	if (f) {
		bool ok = fwrite(body, 1, len, f) == len;
		if (fclose(f) != 0) {
			ok = false;
		}
		if (!ok || rename(tmp, cover) != 0) {
			remove(tmp);
		}
	}
	free(body);
}

// ---------------------------------------------------------------------------
// handing the queue to the player
// ---------------------------------------------------------------------------

typedef struct {
	char **paths;
	char *names;
	int count;
	int start;
} queue_args_t;

static void queue_args_free(queue_args_t *args) {
	if (!args) {
		return;
	}
	free(args->names);
	free(args->paths);
	free(args);
}

// Copies the current queue into the paths the player takes. Called from a
// worker; the player is called later, on the UI thread, from queue_start_cb().
static queue_args_t *queue_args_build(int start) {
	pthread_mutex_lock(&lock);
	int count = queue_count;
	char **paths = count > 0 ? calloc((size_t)count, sizeof(char *)) : NULL;
	char *names = count > 0 ? calloc((size_t)count, 512) : NULL;
	queue_args_t *args = NULL;
	if (paths && names) {
		for (int i = 0; i < count; i++) {
			paths[i] = names + (size_t)i * 512;
			path_for(queue[i].id, ".mp4", paths[i], 512);
		}
		args = calloc(1, sizeof(*args));
		if (args) {
			args->paths = paths;
			args->names = names;
			args->count = count;
			args->start = start;
		}
	}
	pthread_mutex_unlock(&lock);
	if (!args) {
		free(paths);
		free(names);
	}
	return args;
}

static void queue_start_cb(void *user) {
	queue_args_t *args = user;
	if (!args) {
		return;
	}
	device_state_play_list((const char *const *)args->paths, args->count, args->start);
	queue_args_free(args);
}

static void queue_post(int start) {
	queue_args_t *args = queue_args_build(start);
	if (!args) {
		return;
	}
	if (!gui_post(queue_start_cb, args)) {
		queue_args_free(args);
	}
}

// ---------------------------------------------------------------------------
// the transfer
// ---------------------------------------------------------------------------

static void *download_main(void *arg) {
	char id[PGW_ID_MAX];
	snprintf(id, sizeof(id), "%s", (const char *)arg);

	thread_signal_stack();
	thread_be_low_priority("piped download");

	char path[YPLAY_PATH_MAX];
	char part[YPLAY_PATH_MAX];
	path_for(id, ".mp4", path, sizeof(path));
	path_for(id, ".mp4.part", part, sizeof(part));

	pgw_track_t track;
	pgw_err_t err = pgw_track(id, &track);
	bool ok = err == PGW_OK && track.url[0];
	http_stream_t *st = NULL;
	FILE *out = NULL;
	long total = 0;
	long done = 0;
	bool turn = false;

	if (ok) {
		out = fopen(path, "wb");
		ok = out != NULL;
		if (ok) {
			// The marker comes first: from here on the file exists but is not
			// a track yet, and pplay_ready() says so.
			FILE *marker = fopen(part, "w");
			if (marker) {
				fclose(marker);
			}
		}
	}

	if (ok) {
		streamturn_acquire();
		turn = true;
		st = malloc(sizeof(*st));
		ok = st && http_stream_open(st, track.url, YPLAY_TIMEOUT);
		if (ok) {
			pthread_mutex_lock(&lock);
			active_stream = st;
			pthread_mutex_unlock(&lock);
			total = st->content_length;
			growfile_announce(path, total);
		}
	}

	char *buf = ok ? malloc(YPLAY_CHUNK) : NULL;
	// The card times out under writes faster than it retires them, and a video
	// here is tens of megabytes -- more than a lossless track. See
	// common/cardpace.h.
	cardpace_t pace;
	cardpace_reset(&pace);
	while (ok && buf) {
		pthread_mutex_lock(&lock);
		bool stop = abandon;
		pthread_mutex_unlock(&lock);
		if (stop) {
			ok = false;
			break;
		}
		int got = http_stream_read(st, buf, YPLAY_CHUNK);
		if (got < 0) {
			ok = false;
			break;
		}
		if (got == 0) {
			break;
		}
		if (fwrite(buf, 1, (size_t)got, out) != (size_t)got) {
			ok = false;
			break;
		}
		done += got;
		growfile_progress(path, done);
		cardpace_note(&pace, done);
		pthread_mutex_lock(&lock);
		if (!prebuffer_ready && done >= YPLAY_PREBUFFER) {
			prebuffer_ready = true;
			pthread_cond_broadcast(&prebuffer_cond);
		}
		pthread_mutex_unlock(&lock);
	}
	free(buf);

	if (out && fclose(out) != 0) {
		ok = false;
	}
	if (st) {
		http_stream_close(st);
		pthread_mutex_lock(&lock);
		active_stream = NULL;
		pthread_mutex_unlock(&lock);
		free(st);
	}
	if (turn) {
		streamturn_release();
	}

	growfile_finish(path, ok);

	if (ok) {
		remove(part);
		write_tags(path, &track);
		fetch_cover(path, &track);
		long took_ms = cardpace_elapsed_ms(&pace);
		if (took_ms > 0 && done > 0) {
			printf("piped: %s took %ld s, %ld KB/s (the card is paced to %ld KB/s)\n", id, took_ms / 1000,
				   done / 1024 * 1000 / took_ms, CARDPACE_BYTES_PER_SEC / 1024);
		}
	} else {
		remove(path);
		remove(part);
	}

	pthread_mutex_lock(&lock);
	prebuffer_ready = true; // a failed start must not leave a waiter behind
	pthread_cond_broadcast(&prebuffer_cond);
	bool resume = resume_when_ready && ok && strcmp(resume_id, id) == 0;
	int index = resume_index;
	resume_when_ready = false;
	resume_id[0] = '\0';
	network_wanted = false;
	worker_live = false;
	pthread_mutex_unlock(&lock);

	if (resume) {
		// The player was told "not yet" and stopped; now the file is there.
		// Handing it the same queue again is what zvuk does, and it keeps the
		// order and the position.
		queue_post(index);
	}
	return NULL;
}

// Starts one transfer. The caller must not hold `lock`.
static void start_transfer(const char *id) {
	static char id_copy[PGW_ID_MAX];

	pthread_mutex_lock(&lock);
	if (worker_live || !id[0]) {
		pthread_mutex_unlock(&lock);
		return;
	}
	snprintf(id_copy, sizeof(id_copy), "%s", id);
	worker_live = true;
	abandon = false;
	prebuffer_ready = false;
	network_wanted = true;
	pthread_mutex_unlock(&lock);

	if (pthread_create(&worker, NULL, download_main, id_copy) != 0) {
		pthread_mutex_lock(&lock);
		worker_live = false;
		prebuffer_ready = true;
		network_wanted = false;
		pthread_cond_broadcast(&prebuffer_cond);
		pthread_mutex_unlock(&lock);
	}
}

// Joins the transfer thread when one is live. The caller must not hold `lock`.
static void wait_for_transfer(void) {
	pthread_mutex_lock(&lock);
	bool live = worker_live;
	pthread_mutex_unlock(&lock);
	if (live) {
		pthread_join(worker, NULL);
	}
}

void pplay_abandon(void) {
	pthread_mutex_lock(&lock);
	abandon = true;
	http_stream_t *st = active_stream;
	pthread_mutex_unlock(&lock);
	if (st) {
		// Ends the recv() holding the transfer thread; the loop sees `abandon`
		// on its next turn and the file goes.
		http_stream_wake(st);
	}
}

// ---------------------------------------------------------------------------
// the queue
// ---------------------------------------------------------------------------

bool pplay_queue(const pgw_item_t *items, int count, int start,
				 void (*done)(bool ok, const char *error, void *user), void *user) {
	if (!items || count <= 0 || start < 0 || start >= count || !cache_dir[0]) {
		if (done) {
			done(false, cache_dir[0] ? "Nothing to play" : "No cache folder on the card", user);
		}
		return false;
	}

	// The queue is remembered before the first byte: the prepare callback
	// answers questions about it while the transfer runs.
	pthread_mutex_lock(&lock);
	if (!queue) {
		queue = calloc(PGW_ITEMS_MAX, sizeof(*queue));
	}
	int kept = 0;
	if (queue) {
		for (int i = 0; i < count && kept < PGW_ITEMS_MAX; i++) {
			if (items[i].id[0]) {
				queue[kept++] = items[i];
			}
		}
		queue_count = kept;
		queue_start = start < kept ? start : 0;
	}
	pthread_mutex_unlock(&lock);
	if (!queue) {
		if (done) {
			done(false, "Out of memory", user);
		}
		return false;
	}

	// A tap on a new track belongs to it, not to the previous one.
	pplay_abandon();
	wait_for_transfer();

	// Index in the kept array is not necessarily the caller's index: empty ids
	// are dropped. The started track is the one at the caller's `start`, found
	// by its id.
	char id[PGW_ID_MAX];
	snprintf(id, sizeof(id), "%s", items[start].id);

	if (!pplay_ready(id)) {
		start_transfer(id);
		pthread_mutex_lock(&lock);
		while (!prebuffer_ready && worker_live) {
			pthread_cond_wait(&prebuffer_cond, &lock);
		}
		pthread_mutex_unlock(&lock);
	}

	char path[YPLAY_PATH_MAX];
	path_for(id, ".mp4", path, sizeof(path));
	if (access(path, R_OK) != 0) {
		if (done) {
			done(false, "The track did not arrive", user);
		}
		return false;
	}

	pthread_mutex_lock(&lock);
	int start_index = queue_start;
	pthread_mutex_unlock(&lock);

	// The queue is handed to the player on the UI thread: playlist.c is not
	// thread-safe and this runs on a worker.
	queue_post(start_index);

	if (done) {
		done(true, NULL, user);
	}
	return true;
}

// ---------------------------------------------------------------------------
// the prepare callback
// ---------------------------------------------------------------------------

static bool prepare_track(const char *path) {
	if (!path || !cache_dir[0]) {
		return true;
	}
	char id[PGW_ID_MAX];
	id_from_path(path, id, sizeof(id));
	if (!id[0]) {
		return true; // not ours: someone else's file is not our business
	}
	if (growfile_is_growing(path) || pplay_ready(id)) {
		return true;
	}

	int index = -1;
	pthread_mutex_lock(&lock);
	for (int i = 0; i < queue_count; i++) {
		if (strcmp(queue[i].id, id) == 0) {
			index = i;
			break;
		}
	}
	bool busy = worker_live;
	if (index >= 0 && !busy) {
		resume_when_ready = true;
		resume_index = index;
		snprintf(resume_id, sizeof(resume_id), "%s", id);
		network_wanted = true;
	}
	pthread_mutex_unlock(&lock);

	if (index < 0) {
		// Not a queued track: nothing will ever arrive for it, so let the
		// player fail the way it does for any missing file instead of waiting.
		return true;
	}
	if (busy) {
		// Another transfer is running. Saying "not yet" and starting a second
		// one would fight for the turn; the running one is the track the user
		// asked for, so this one is let go.
		return true;
	}

	start_transfer(id);
	return false;
}

// ---------------------------------------------------------------------------
// the service table
// ---------------------------------------------------------------------------

static const char *svc_cache_dir(void) { return cache_dir; }

static bool svc_owns_path(const char *path) {
	if (!path || !cache_dir[0]) {
		return false;
	}
	size_t n = strlen(cache_dir);
	return strncmp(path, cache_dir, n) == 0 && path[n] == '/';
}

// The interface asks for a number, and this service's ids are eleven characters
// of base64-ish text. 1 means "a track of this cache" for everything the player
// uses the answer for -- the star, the sleep timer, the factory reset -- and
// the real id stays in the file name, where track_id_text() below reads it.
static int svc_track_id(const char *path) {
	char id[PGW_ID_MAX];
	id_from_path(path, id, sizeof(id));
	return id[0] ? 1 : 0;
}

static bool svc_downloading(void) {
	pthread_mutex_lock(&lock);
	bool live = worker_live;
	pthread_mutex_unlock(&lock);
	return live;
}

static bool svc_network_wanted(void) {
	pthread_mutex_lock(&lock);
	bool want = network_wanted;
	pthread_mutex_unlock(&lock);
	return want;
}

static void svc_clear_on_exit(void) {
	// A half-downloaded track is not worth keeping across a power cycle: the
	// URL it came from has expired by then.
	DIR *d = cache_dir[0] ? opendir(cache_dir) : NULL;
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t n = strlen(e->d_name);
		if (n > 5 && strcmp(e->d_name + n - 5, ".part") == 0) {
			char path[YPLAY_PATH_MAX + 260];
			snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name);
			remove(path);
		}
	}
	closedir(d);
}

static void svc_wipe(void) {
	DIR *d = cache_dir[0] ? opendir(cache_dir) : NULL;
	if (!d) {
		return;
	}
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}
		char path[YPLAY_PATH_MAX + 260];
		snprintf(path, sizeof(path), "%s/%s", cache_dir, e->d_name);
		remove(path);
	}
	closedir(d);
}

static const sonix_addon_service_t service = {
	.id = "piped",
	.cache_dir = svc_cache_dir,
	.owns_path = svc_owns_path,
	.track_id = svc_track_id,
	.downloading = svc_downloading,
	.network_wanted = svc_network_wanted,
	.clear_on_exit = svc_clear_on_exit,
	.wipe = svc_wipe,
};

const sonix_addon_service_t *pplay_service_table(void) { return &service; }

// ---------------------------------------------------------------------------
// stop
// ---------------------------------------------------------------------------

bool pplay_stop(void) {
	pplay_abandon();
	wait_for_transfer();

	pthread_mutex_lock(&lock);
	bool live = worker_live;
	pthread_mutex_unlock(&lock);
	if (live) {
		// The transfer is parked in a read the wake has not reached: the
		// library must not be unmapped under it.
		return false;
	}

	if (prepare_installed) {
		prepare_installed = false;
		device_state_remove_prepare_cb(prepare_track);
		streamturn_unregister_abandon(pplay_abandon);
	}

	pthread_mutex_lock(&lock);
	free(queue);
	queue = NULL;
	queue_count = 0;
	queue_start = 0;
	network_wanted = false;
	prebuffer_ready = false;
	abandon = false;
	resume_when_ready = false;
	resume_index = 0;
	resume_id[0] = '\0';
	pthread_mutex_unlock(&lock);
	return true;
}
