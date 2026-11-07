#include "subsoniccache.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/cardpace.h"

#include "src/system/core/lang.h"
#include "src/system/core/utils.h"
#include "src/system/decode/growfile.h"
#include "src/system/net/http.h"
#include "src/system/streaming/streamturn.h"

#include "subsonicstop.h"

// See subsoniccache.h. The shape is zvuk's cache; what is particular to
// Subsonic is said where it matters:
//
//   * the file name is a hash of (server, song id) -- subsoniccache_key() calls
//     subsonic_track_key() -- and the id itself lives in the sidecar. A
//     Subsonic id is "al-123", "so-45" or a UUID, and a UUID is not a file name
//     on a FAT card;
//   * there is nothing to unwrap: the server sends the file as it is
//     (format=raw) or the transcode that was asked for, so the download is a
//     plain copy;
//   * the cover is part of the download: "<stem>.jpg" beside the track is where
//     the player's own cover loader looks, which is what gives the sheet real
//     artwork rather than a grey note.

#define BUF_SIZE (64 * 1024)
#define MARKER ".incompleto"
#define COVER_MAX_BYTES (4 * 1024 * 1024)
#define COVER_TIMEOUT_SECS 15
#define STREAM_TIMEOUT_SECS 20

static char cache_dir[400];	  // the one in use, "" while the card is gone
static char addon_cache[400]; // "<addon>/cache", the path init() gave: the card coming back re-arms it
static __thread char last_error[256];

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static unsigned generation; // a card change or a cancellation invalidates a slow result

typedef struct {
	char song_id[SUBSONIC_ID_MAX];
	char path[SUBSONICCACHE_PATH_MAX];
	char marker[SUBSONICCACHE_PATH_MAX + 16];
	FILE *f;
	long written;
	bool finished, ok;
	volatile bool abandon;
	char error[256];

	http_stream_t st;
	bool st_open;

	// Card pacing: the clock cardpace_note() holds this download to -- see
	// common/cardpace.h.
	cardpace_t pace;
} download_t;

static download_t *current;

// The cache's download thread, and the flag that tells it to stop. See
// subsonicstop.h for what the count is for.
static subsonic_stop_t threads = SUBSONIC_STOP_INIT;
static bool stopping;

static void set_error(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(last_error, sizeof(last_error), fmt, ap);
	va_end(ap);
}

const char *subsoniccache_last_error(void) { return last_error; }

void subsoniccache_set_addon_dir(const char *cache) {
	pthread_mutex_lock(&lock);
	generation++;
	snprintf(addon_cache, sizeof(addon_cache), "%s", cache && cache[0] ? cache : "");
	snprintf(cache_dir, sizeof(cache_dir), "%s", addon_cache);
	pthread_mutex_unlock(&lock);
	if (!addon_cache[0]) {
		fprintf(stderr, "subsoniccache: no cache directory on the card; nothing can be played\n");
	}
}

void subsoniccache_set_root(const char *sd_root) {
	pthread_mutex_lock(&lock);
	generation++;
	snprintf(cache_dir, sizeof(cache_dir), "%s", sd_root && sd_root[0] ? addon_cache : "");
	pthread_mutex_unlock(&lock);
}

bool subsoniccache_ready(void) { return cache_dir[0] != '\0'; }

const char *subsoniccache_dir(void) { return cache_dir[0] ? cache_dir : NULL; }

// The key, and the two places it is used: the file's name and the directory.
void subsoniccache_key(const char *song_id, char *out, size_t size) {
	subsonic_track_key(song_id, out, size);
}

void subsoniccache_path(const char *song_id, const char *suffix, char *out, size_t size) {
	char key[32];
	subsoniccache_key(song_id, key, sizeof(key));
	snprintf(out, size, "%s/%s.%s", cache_dir, key, suffix && suffix[0] ? suffix : "flac");
}

static bool complete(const char *path) {
	struct stat st;
	char marker[SUBSONICCACHE_PATH_MAX + 16];
	snprintf(marker, sizeof(marker), "%s" MARKER, path);
	return stat(path, &st) == 0 && st.st_size > 0 && !growfile_is_growing(path) && access(marker, F_OK) != 0;
}

bool subsoniccache_find(const subsonic_track_t *track, char *out, size_t size) {
	if (!cache_dir[0] || !track || !track->id[0]) {
		return false;
	}
	// The expected suffix is the quality in force now; the track's own is what
	// a raw download was called; the rest is the set of audio suffixes a
	// Subsonic server sends. A handful of stats, no directory scan: this is
	// asked once per row when a list is built, and the cache may hold hundreds
	// of files.
	static const char *const OTHERS[] = {"flac", "mp3", "opus", "m4a", "aac", "ogg", "wav"};
	const char *tried[sizeof(OTHERS) / sizeof(OTHERS[0]) + 2];
	int n = 0;
	tried[n++] = subsonic_expected_suffix(track);
	if (track->suffix[0]) {
		tried[n++] = track->suffix;
	}
	for (size_t i = 0; i < sizeof(OTHERS) / sizeof(OTHERS[0]); i++) {
		tried[n++] = OTHERS[i];
	}
	char path[SUBSONICCACHE_PATH_MAX];
	for (int i = 0; i < n; i++) {
		subsoniccache_path(track->id, tried[i], path, sizeof(path));
		if (complete(path)) {
			if (out) {
				snprintf(out, size, "%s", path);
			}
			return true;
		}
	}
	return false;
}

bool subsoniccache_owns(const char *path) {
	size_t n = strlen(cache_dir);
	return n && path && strncmp(path, cache_dir, n) == 0 && path[n] == '/';
}

bool subsoniccache_complete(const char *path) { return path && path[0] && complete(path); }

bool subsoniccache_is_track(const char *path) {
	if (!subsoniccache_owns(path)) {
		return false;
	}
	const char *name = path + strlen(cache_dir) + 1;
	if (strchr(name, '/')) {
		return false;
	}
	// "<key>.<suffix>", one dot and nothing else: the key holds no dot, and a
	// sidecar ("<key>.mp3.tags"), a marker ("<key>.mp3.incompleto") and a cover
	// ("<key>.jpg") all have a second dot or an image extension.
	const char *dot = strchr(name, '.');
	if (!dot || dot == name) {
		return false;
	}
	const char *ext = dot + 1;
	if (!ext[0] || strchr(ext, '.') || strcmp(ext, "tags") == 0) {
		return false;
	}
	return true;
}

// The sidecar's "key=value", for the two facts the player needs back from a
// path: which song it is, and which album it came off.
static bool tag_read(const char *path, const char *key, char *out, size_t size) {
	out[0] = '\0';
	if (!subsoniccache_is_track(path)) {
		return false;
	}
	char tags[SUBSONICCACHE_PATH_MAX + 16];
	snprintf(tags, sizeof(tags), "%s.tags", path);
	FILE *f = fopen(tags, "r");
	if (!f) {
		return false;
	}
	size_t key_len = strlen(key);
	char line[320];
	while (fgets(line, sizeof(line), f)) {
		if (strncmp(line, key, key_len) != 0 || line[key_len] != '=') {
			continue;
		}
		line[strcspn(line, "\r\n")] = '\0';
		snprintf(out, size, "%s", line + key_len + 1);
		break;
	}
	fclose(f);
	return out[0] != '\0';
}

void subsoniccache_song_id(const char *path, char *out, size_t size) { tag_read(path, "id", out, size); }

bool subsoniccache_album_id(const char *path, char *out, size_t size) { return tag_read(path, "album_id", out, size); }

void subsoniccache_downloading_id(char *out, size_t size) {
	pthread_mutex_lock(&lock);
	snprintf(out, size, "%s", current && !current->finished ? current->song_id : "");
	pthread_mutex_unlock(&lock);
}

bool subsoniccache_busy(void) {
	pthread_mutex_lock(&lock);
	bool busy = current && !current->finished;
	pthread_mutex_unlock(&lock);
	return busy;
}

static volatile bool network_wanted;
void subsoniccache_set_network_wanted(bool wanted) { network_wanted = wanted; }
bool subsoniccache_network_wanted(void) { return network_wanted; }

// The one download in flight, stopped. `wake` shuts the socket down: a recv()
// on a stalled connection has no other interruption.
static void abandon_locked(void) {
	generation++;
	if (current) {
		current->abandon = true;
		if (current->st_open) {
			http_stream_wake(&current->st);
		}
	}
}

void subsoniccache_abandon_all(void) {
	pthread_mutex_lock(&lock);
	abandon_locked();
	pthread_mutex_unlock(&lock);
}

void subsoniccache_fetch_cancel(void) { subsoniccache_abandon_all(); }

// ---------------------------------------------------------------------------
// the source: one GET, from the first byte
// ---------------------------------------------------------------------------

static void src_close(download_t *d) {
	if (d->st_open) {
		http_stream_close(&d->st);
		d->st_open = false;
	}
}

// The player's http.h opens a stream from the first byte and takes no caller
// headers, so there is no Range and therefore no resume: a connection that
// drops mid-file ends the download (see src_read) instead of splicing a second
// copy of the file into the first.
static bool src_open(download_t *d, const char *url) {
	if (http_stream_open(&d->st, url, STREAM_TIMEOUT_SECS)) {
		d->st_open = true;
		return true;
	}
	const char *why = http_last_error();
	snprintf(d->error, sizeof(d->error), "%s", why && why[0] ? why : tr("download_failed"));
	return false;
}

static long src_read(download_t *d, uint8_t *buf, long n) {
	if (d->abandon) {
		return -1;
	}
	if (!d->st_open) {
		return -1;
	}
	int got = http_stream_read(&d->st, buf, (int)(n > BUF_SIZE ? BUF_SIZE : n));
	if (got > 0) {
		return got;
	}
	if (got == 0) {
		return 0;
	}
	snprintf(d->error, sizeof(d->error), "%s", tr("download_failed"));
	return -1;
}

// ---------------------------------------------------------------------------
// writing
// ---------------------------------------------------------------------------

static void write_error(download_t *d) {
	int saved_errno = errno ? errno : EIO;
	// A pulled card is the ordinary way this happens, and the player has to
	// hear "failed" rather than wait for bytes that will never come.
	snprintf(d->error, sizeof(d->error), "%s: %s", tr("download_failed"), strerror(saved_errno));
}

static bool write_chunk(download_t *d, const uint8_t *buf, long n) {
	if (fwrite(buf, 1, (size_t)n, d->f) != (size_t)n) {
		write_error(d);
		return false;
	}
	// Flushed per chunk on purpose: the reader is another file descriptor on
	// the same file, and growfile hands out only what has reached the card.
	if (fflush(d->f) != 0) {
		write_error(d);
		return false;
	}
	return true;
}

static void note_written(download_t *d, long written) {
	pthread_mutex_lock(&lock);
	d->written = written;
	pthread_cond_broadcast(&changed);
	pthread_mutex_unlock(&lock);
	growfile_progress(d->path, written);
	// The one place a download's byte count moves, so the one place the card
	// rate is held to; covers are a few tens of KB and are not paced.
	cardpace_note(&d->pace, written);
}

static bool transfer(download_t *d, const char *url) {
	if (!src_open(d, url)) {
		return false;
	}
	uint8_t *buf = malloc(BUF_SIZE);
	if (!buf) {
		snprintf(d->error, sizeof(d->error), "%s", tr("download_failed"));
		return false;
	}
	long written = 0;
	bool ok = true;
	// Before the first write: the first cardpace_note() below starts the clock.
	cardpace_reset(&d->pace);
	for (;;) {
		long got = src_read(d, buf, BUF_SIZE);
		if (got == 0) {
			break;
		}
		if (got < 0) {
			ok = false;
			break;
		}
		if (!write_chunk(d, buf, got)) {
			ok = false;
			break;
		}
		written += got;
		note_written(d, written);
	}
	// A short file is not a file: the decoder would sit at its end waiting for
	// the rest of a stream that has already finished. The server states the
	// length for everything Subsonic sends, so this is a real check.
	if (ok && d->st.content_length > 0 && written < d->st.content_length) {
		printf("subsonic: %s ended at %ld of %ld bytes\n", d->song_id, written, d->st.content_length);
		snprintf(d->error, sizeof(d->error), "%s", tr("download_failed"));
		ok = false;
	}
	free(buf);
	return ok;
}

static void *download_main(void *arg) {
	download_t *d = arg;
	// Counted by subsoniccache_start() before this thread existed: see
	// subsonicstop.h.
	thread_be_low_priority("subsonic download");
	streamturn_acquire();

	// The address is asked for here, on the worker, not on the thread that
	// tapped: build_url() is cheap, but the session lock is held by whoever is
	// signing in.
	char url[SUBSONIC_URL_MAX];
	subsonic_stream_url(d->song_id, url, sizeof(url));
	bool ok = url[0] && !d->abandon && transfer(d, url);

	streamturn_release();
	src_close(d);
	if (fclose(d->f) != 0) {
		if (!d->error[0]) {
			write_error(d);
		}
		ok = false;
	}
	ok = ok && !d->abandon;
	if (!ok) {
		remove(d->path);
		fprintf(stderr, "subsonic: download of %s failed: %s\n", d->song_id, d->abandon ? "abandoned"
																						 : d->error[0] ? d->error
																									   : tr("download_failed"));
	} else {
		printf("subsonic: %s complete (%ld bytes)\n", d->song_id, d->written);
		long took_ms = cardpace_elapsed_ms(&d->pace);
		if (took_ms > 0 && d->written > 0) {
			printf("subsonic: %s took %ld s, %ld KB/s (the card is paced to %ld KB/s)\n", d->song_id, took_ms / 1000,
				   d->written / 1024 * 1000 / took_ms, CARDPACE_BYTES_PER_SEC / 1024);
		}
	}
	remove(d->marker);
	growfile_finish(d->path, ok);

	pthread_mutex_lock(&lock);
	d->finished = true;
	d->ok = ok;
	pthread_cond_broadcast(&changed);
	if (current == d) {
		current = NULL;
	}
	pthread_mutex_unlock(&lock);
	free(d);
	subsonic_stop_leave(&threads);
	return NULL;
}

static void streamturn_hook(void) { subsoniccache_abandon_all(); }

// Whether this module's hook is in the player's abandon table (streamturn.h).
// Registered once, on the first download, and taken back in subsoniccache_stop():
// the hook is code in this library and the table is walked by every service
// that is about to download.
static bool abandon_hook_installed;

static void prune(long room);
static void clear_files(bool (*keep)(const char *stem));

bool subsoniccache_start(const subsonic_track_t *track, const char *path) {
	last_error[0] = '\0';
	if (stopping) {
		return false;
	}
	if (!track || !track->id[0] || !path || !path[0]) {
		set_error("%s", tr("download_failed"));
		return false;
	}
	if (!subsoniccache_ready()) {
		set_error("%s", tr("download_failed"));
		return false;
	}
	if (!abandon_hook_installed) {
		streamturn_register_abandon(streamturn_hook);
		abandon_hook_installed = true;
	}
	// The directory is made at init, but a card that went away and came back (or
	// a folder the user tidied up) can leave the path without its directory:
	// mkdir here is one syscall and saves the track from failing on an error
	// that says nothing about the real cause.
	mkdir(cache_dir, 0777);
	if (complete(path)) {
		return true;
	}

	// The same track coming down already (the prefetch got there first, or the
	// player asked twice): wait behind it rather than start a second copy.
	pthread_mutex_lock(&lock);
	if (current && !current->finished && strcmp(current->song_id, track->id) == 0) {
		download_t *d = current;
		while (current == d && d->written < STREAM_PREBUFFER && !d->finished) {
			pthread_cond_wait(&changed, &lock);
		}
		bool same_name = strcmp(d->path, path) == 0;
		pthread_mutex_unlock(&lock);
		if (same_name) {
			return true;
		}
		// Another name for the same song: the quality changed under the queue.
		// The queue's path is what the player will open, so this one cannot be
		// answered -- the track is skipped rather than played from a name the
		// player does not hold.
		set_error("%s", tr("download_failed"));
		return false;
	}
	// Another track: waited for, never abandoned from here. Whoever starts
	// playback abandons first (subsonicplay.c) -- the shared turn may well be
	// held by another service's track, and waiting on it here is what keeps
	// this thread from cutting that one off.
	while (current && !current->finished) {
		pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);

	download_t *d = calloc(1, sizeof(*d));
	if (!d) {
		set_error("%s", tr("download_failed"));
		return false;
	}
	snprintf(d->song_id, sizeof(d->song_id), "%s", track->id);
	snprintf(d->path, sizeof(d->path), "%s", path);
	snprintf(d->marker, sizeof(d->marker), "%s" MARKER, d->path);

	prune(SUBSONICCACHE_PRUNE_HEADROOM);
	FILE *marker = fopen(d->marker, "w");
	if (marker) {
		fclose(marker);
	}
	d->f = fopen(d->path, "wb");
	if (!d->f) {
		int saved_errno = errno;
		remove(d->marker);
		set_error("%s: %s", tr("download_failed"), strerror(saved_errno));
		free(d);
		return false;
	}

	pthread_mutex_lock(&lock);
	current = d;
	pthread_mutex_unlock(&lock);
	// Before the thread exists: a reader that opens the file in the next instant
	// must be told it is growing, not finished.
	growfile_announce(d->path, 0);

	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 160 * 1024);
	// Counted before it exists and uncounted when it never started. Every
	// caller -- subsonicplay's worker -- is stopped and waited for before
	// subsoniccache_stop(), so nothing can be created after its wait began.
	subsonic_stop_enter(&threads);
	int thread_error = pthread_create(&th, &attr, download_main, d);
	if (thread_error != 0) {
		subsonic_stop_leave(&threads);
		set_error("%s: %s", tr("download_failed"), strerror(thread_error));
		pthread_attr_destroy(&attr);
		growfile_finish(d->path, false);
		fclose(d->f);
		remove(d->path);
		remove(d->marker);
		pthread_mutex_lock(&lock);
		current = NULL;
		pthread_cond_broadcast(&changed);
		pthread_mutex_unlock(&lock);
		free(d);
		return false;
	}
	pthread_attr_destroy(&attr);

	// Enough to start the decoder: the format headers, plus margin so that
	// playback does not immediately catch up with the download (streamturn.h).
	pthread_mutex_lock(&lock);
	bool ok;
	for (;;) {
		if (current != d) {
			// Finished (or abandoned) before the prebuffer: the answer is
			// whether the file is there and complete.
			ok = d->ok && complete(path);
			break;
		}
		if (d->written >= STREAM_PREBUFFER) {
			ok = true;
			break;
		}
		pthread_cond_wait(&changed, &lock);
	}
	pthread_mutex_unlock(&lock);
	if (!ok) {
		set_error("%s", d->error[0] ? d->error : tr("download_failed"));
	}
	return ok;
}

// ---------------------------------------------------------------------------
// keeping the cache in bounds (the Qobuz policy, as qobuzcache.c)
// ---------------------------------------------------------------------------

static pthread_mutex_t keep_lock = PTHREAD_MUTEX_INITIALIZER;
static char protected_paths[SUBSONICCACHE_PROTECTED_MAX][SUBSONICCACHE_PATH_MAX];
static int protected_count;
#define RECENT_MAX 8
static char recent_paths[RECENT_MAX][SUBSONICCACHE_PATH_MAX];
static int recent_next;

void subsoniccache_set_protected(const char *const *paths, int count) {
	pthread_mutex_lock(&keep_lock);
	protected_count = 0;
	for (int i = 0; i < count && protected_count < SUBSONICCACHE_PROTECTED_MAX; i++) {
		if (paths[i] && paths[i][0]) {
			snprintf(protected_paths[protected_count++], sizeof(protected_paths[0]), "%s", paths[i]);
		}
	}
	pthread_mutex_unlock(&keep_lock);
}

static bool stem_of(const char *stem, const char *path) {
	size_t len = strlen(stem);
	return path[0] && strncmp(path, stem, len) == 0 && (path[len] == '.' || path[len] == '\0');
}

static bool is_kept_stem(const char *stem) {
	if (growfile_prefix_is_growing(stem)) {
		return true;
	}
	bool keep = false;
	pthread_mutex_lock(&keep_lock);
	for (int i = 0; i < protected_count && !keep; i++) {
		keep = stem_of(stem, protected_paths[i]);
	}
	for (int i = 0; i < RECENT_MAX && !keep; i++) {
		keep = stem_of(stem, recent_paths[i]);
	}
	pthread_mutex_unlock(&keep_lock);
	return keep;
}

typedef struct {
	char name[64];
	long size;
	long atime;
} entry_t;

#define PRUNE_MAX_ENTRIES 512

static int scan(entry_t *out, int max, long *total, bool (*keep)(const char *stem)) {
	*total = 0;
	int count = 0;
	DIR *dir = cache_dir[0] ? opendir(cache_dir) : NULL;
	if (!dir) {
		return 0;
	}
	struct dirent *e;
	while ((e = readdir(dir)) != NULL) {
		if (e->d_name[0] == '.' || strlen(e->d_name) >= sizeof(out[0].name)) {
			continue;
		}
		char path[SUBSONICCACHE_PATH_MAX + 128];
		snprintf(path, sizeof(path), "%s/%.64s", cache_dir, e->d_name);
		struct stat st;
		if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
			continue;
		}
		*total += (long)st.st_size;
		// The stem: the name up to its first dot ("<key>.flac.tags" -> "<key>").
		char *dot = strchr(path + strlen(cache_dir) + 1, '.');
		if (dot) {
			*dot = '\0';
		}
		if (keep && keep(path)) {
			continue;
		}
		if (out && count < max) {
			snprintf(out[count].name, sizeof(out[count].name), "%.*s", (int)sizeof(out[count].name) - 1, e->d_name);
			out[count].size = (long)st.st_size;
			out[count].atime = (long)st.st_atime;
			count++;
		}
	}
	closedir(dir);
	return count;
}

static void prune(long room) {
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return;
	}
	long total;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total, is_kept_stem);
	for (int i = 0; i < count && total + room > SUBSONICCACHE_MAX_BYTES; i++) {
		// The oldest first: a linear pick over a few hundred names, once per
		// download.
		int oldest = i;
		for (int k = i + 1; k < count; k++) {
			if (entries[k].atime < entries[oldest].atime) {
				oldest = k;
			}
		}
		entry_t tmp = entries[i];
		entries[i] = entries[oldest];
		entries[oldest] = tmp;
		char path[SUBSONICCACHE_PATH_MAX + 128];
		snprintf(path, sizeof(path), "%s/%s", cache_dir, entries[i].name);
		if (remove(path) == 0) {
			total -= entries[i].size;
			printf("subsoniccache: removed %s (%ld KB)\n", entries[i].name, entries[i].size / 1024);
		}
	}
	free(entries);
}

static void clear_files(bool (*keep)(const char *stem)) {
	entry_t *entries = malloc(sizeof(*entries) * PRUNE_MAX_ENTRIES);
	if (!entries) {
		return;
	}
	long total;
	int count = scan(entries, PRUNE_MAX_ENTRIES, &total, keep);
	int removed = 0;
	for (int i = 0; i < count; i++) {
		char path[SUBSONICCACHE_PATH_MAX + 128];
		snprintf(path, sizeof(path), "%s/%s", cache_dir, entries[i].name);
		removed += remove(path) == 0;
	}
	free(entries);
	printf("subsoniccache: cleared (%d files)\n", removed);
}

void subsoniccache_note_played(const char *path) {
	if (!subsoniccache_ready() || !path || !path[0]) {
		return;
	}
	pthread_mutex_lock(&keep_lock);
	snprintf(recent_paths[recent_next], sizeof(recent_paths[0]), "%s", path);
	recent_next = (recent_next + 1) % RECENT_MAX;
	pthread_mutex_unlock(&keep_lock);
}

static bool is_growing_stem(const char *stem) { return growfile_prefix_is_growing(stem); }

void subsoniccache_clear(void) {
	if (subsoniccache_ready()) {
		printf("subsoniccache: clearing the cache on request\n");
		clear_files(is_growing_stem);
	}
}

void subsoniccache_clear_on_exit(void) {
	if (subsoniccache_ready()) {
		printf("subsoniccache: shutting down, clearing the cache\n");
		subsoniccache_abandon_all();
		clear_files(NULL);
	}
}

// ---------------------------------------------------------------------------
// sidecars: the tags and the cover
//
// No card descriptor is held during HTTP: the card can be pulled mid-request,
// and the answer to "is this still the card I started on" is the generation,
// re-read after the socket came back.
// ---------------------------------------------------------------------------

static bool sidecar_card_ok(const char *path, unsigned started_at) {
	pthread_mutex_lock(&lock);
	bool same = started_at == generation;
	char dir[400];
	snprintf(dir, sizeof(dir), "%s", cache_dir);
	pthread_mutex_unlock(&lock);
	size_t dir_len = strlen(dir);
	return same && dir_len > 0 && strncmp(path, dir, dir_len) == 0 && path[dir_len] == '/';
}

static void write_tags(const subsonic_track_t *track, const char *path) {
	char tags[SUBSONICCACHE_PATH_MAX + 16];
	snprintf(tags, sizeof(tags), "%s.tags", path);
	char tmp[SUBSONICCACHE_PATH_MAX + 24];
	snprintf(tmp, sizeof(tmp), "%s.tmp", tags);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return;
	}
	// The format the player's metadata.c already reads (metadata.c, "Sidecar
	// files"), plus the two facts only this service knows: which song this is
	// (the star writes by id) and which album it came off ("show album").
	bool ok = fprintf(f, "id=%s\ntitle=%s\nartist=%s\nalbum=%s\nalbum_id=%s\ntrack=%d\nsuffix=%s\n", track->id,
					  track->title, track->artist, track->album, track->album_id, track->track_number,
					  track->suffix) > 0;
	if (fclose(f) != 0) {
		ok = false;
	}
	// Renamed into place: a half-written sidecar read by the player at the
	// instant of a track change would show an empty title, and a rename within
	// one filesystem is indivisible.
	if (!ok || rename(tmp, tags) != 0) {
		remove(tmp);
	}
}

static bool looks_like_image(const char *body, size_t len) {
	if (!body || len < 8) {
		return false;
	}
	unsigned char jpeg[] = {0xFF, 0xD8, 0xFF};
	unsigned char png[] = {0x89, 'P', 'N', 'G'};
	return memcmp(body, jpeg, sizeof(jpeg)) == 0 || memcmp(body, png, sizeof(png)) == 0;
}

static void write_cover(const subsonic_track_t *track, const char *path) {
	if (!track->cover_id[0]) {
		return;
	}
	char jpg[SUBSONICCACHE_PATH_MAX];
	const char *dot = strrchr(path, '.');
	snprintf(jpg, sizeof(jpg), "%.*s.jpg", (int)(dot ? dot - path : (long)strlen(path)), path);
	struct stat st;
	if (stat(jpg, &st) == 0 && st.st_size > 0) {
		return; // already there
	}
	char url[SUBSONIC_URL_MAX];
	subsonic_cover_url(track->cover_id, 600, url, sizeof(url));
	if (!url[0]) {
		return;
	}
	pthread_mutex_lock(&lock);
	unsigned started_at = generation;
	pthread_mutex_unlock(&lock);
	char *body = NULL;
	size_t len = 0;
	if (!http_get_ex(url, NULL, false, &body, &len, COVER_MAX_BYTES, COVER_TIMEOUT_SECS, NULL)) {
		free(body);
		return;
	}
	if (looks_like_image(body, len) && sidecar_card_ok(path, started_at)) {
		char tmp[SUBSONICCACHE_PATH_MAX + 8];
		snprintf(tmp, sizeof(tmp), "%s.tmp", jpg);
		FILE *f = fopen(tmp, "wb");
		if (f) {
			bool ok = fwrite(body, 1, len, f) == len;
			if (fclose(f) != 0) {
				ok = false;
			}
			if (!ok || rename(tmp, jpg) != 0) {
				remove(tmp);
			}
		}
	}
	free(body);
}

void subsoniccache_write_sidecars(const subsonic_track_t *track, const char *path) {
	if (!track || !path || !path[0] || !subsoniccache_ready() || !subsoniccache_owns(path)) {
		return;
	}
	// Only for a file this addon is about to play or keep: a path outside the
	// cache would put a .tags beside the user's own music.
	write_tags(track, path);
	write_cover(track, path);
}

// ---------------------------------------------------------------------------
// stopping
// ---------------------------------------------------------------------------

bool subsoniccache_stop(void) {
	// The one hook this module leaves in the player goes first, before anything
	// is waited for: the table is walked by every service that is about to
	// download, and the function it holds is code in this library.
	if (abandon_hook_installed) {
		streamturn_unregister_abandon(streamturn_hook);
		abandon_hook_installed = false;
	}

	// The download next: it is the longest-lived thread here and the only one
	// holding a file open. The abandon flag and the socket wake are what end a
	// transfer; they leave the part file and its marker cleaned up by the thread
	// itself.
	pthread_mutex_lock(&lock);
	stopping = true;
	abandon_locked();
	pthread_mutex_unlock(&lock);

	// A thread that did not come back still owns `current` and the file it has
	// open: the pointers are left for it rather than cleared under it, and
	// `stopping` stays up so no new download starts in what is left of this
	// library's life. The false is what stops the player from unmapping.
	if (!subsonic_stop_wait(&threads, "cache download")) {
		return false;
	}

	pthread_mutex_lock(&lock);
	current = NULL;
	stopping = false; // a second init() is a fresh start
	pthread_mutex_unlock(&lock);

	// The card, and the two lists of paths on it: init() puts the root back from
	// the host it is given, and a protected path from the previous card would
	// keep a file of that card from being swept.
	cache_dir[0] = '\0';
	addon_cache[0] = '\0';
	network_wanted = false;
	pthread_mutex_lock(&keep_lock);
	protected_count = 0;
	recent_next = 0;
	pthread_mutex_unlock(&keep_lock);
	return true;
}
