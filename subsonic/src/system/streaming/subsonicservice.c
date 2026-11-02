#include "subsonicservice.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../subsonic.h"
#include "subsoniccache.h"
#include "subsonicstop.h"

// See subsonicservice.h. The cache hooks are one-liners over subsoniccache; the
// account half is the star, and the star needs a mirror: the player asks
// fav_get() on the UI thread at every track load, and a Subsonic API call there
// would block the interface for as long as the server takes to answer.

// ---------------------------------------------------------------------------
// the star's mirror
//
// The account's starred song ids, kept whole: getStarred2 is one reply, and
// "is this song starred" is then a walk over a few hundred short strings. A
// library with more starred songs than this keeps the first STARS_MAX -- the
// star on a track past that point answers "not starred" until the song is
// toggled, which is a smaller wrong than a lookup on the UI thread.
// ---------------------------------------------------------------------------

#define STARS_MAX 1000

static pthread_mutex_t star_lock = PTHREAD_MUTEX_INITIALIZER;
static char stars[STARS_MAX][SUBSONIC_ID_MAX];
static int star_count;
static bool stars_known;
static bool stars_fetching;
static void (*stars_done)(void); // the player's "redraw the star", waiting for the mirror

static subsonic_stop_t threads = SUBSONIC_STOP_INIT;
static bool stopping;

static int star_index_locked(const char *song_id) {
	for (int i = 0; i < star_count; i++) {
		if (strcmp(stars[i], song_id) == 0) {
			return i;
		}
	}
	return -1;
}

bool subsonicservice_is_starred(const char *song_id) {
	if (!song_id || !song_id[0]) {
		return false;
	}
	pthread_mutex_lock(&star_lock);
	bool starred = stars_known && star_index_locked(song_id) >= 0;
	pthread_mutex_unlock(&star_lock);
	return starred;
}

// Flips the mirror at once. The player has already drawn the star in its new
// face by the time the write goes out, so the two have to agree until the
// server says otherwise.
static void mirror_set(const char *song_id, bool want) {
	if (!song_id || !song_id[0]) {
		return;
	}
	pthread_mutex_lock(&star_lock);
	int at = star_index_locked(song_id);
	if (want && at < 0) {
		if (star_count < STARS_MAX) {
			snprintf(stars[star_count++], sizeof(stars[0]), "%s", song_id);
		}
	} else if (!want && at >= 0) {
		// Order does not matter here, so the last entry fills the hole.
		star_count--;
		if (at != star_count) {
			memcpy(stars[at], stars[star_count], sizeof(stars[0]));
		}
		stars[star_count][0] = '\0';
	}
	pthread_mutex_unlock(&star_lock);
}

// ---------------------------------------------------------------------------
// the workers: one per request, counted, like the page's
//
// A star write and a mirror fetch are HTTP calls with a twenty-second timeout
// of their own and no way to interrupt them, so stop() counts them the way
// subsonicstop.h describes rather than waiting for the network.
// ---------------------------------------------------------------------------

typedef enum { JOB_NONE = 0, JOB_STAR, JOB_REFRESH } job_kind_t;

typedef struct {
	job_kind_t kind;
	char song_id[SUBSONIC_ID_MAX];
	bool want;
	sonix_addon_service_done_cb done; // JOB_STAR: the caller's, called from this thread
	void *user;
} job_t;

static void mirror_landed_async(void *user) {
	(void)user;
	void (*done)(void) = NULL;
	pthread_mutex_lock(&star_lock);
	done = stars_done;
	stars_done = NULL;
	pthread_mutex_unlock(&star_lock);
	if (done) {
		done();
	}
}

static void *worker(void *arg) {
	job_t *job = arg;
	if (job->kind == JOB_REFRESH) {
		// Fetched into a heap buffer and copied in only when the whole reply
		// arrived: a half-filled mirror would answer "not starred" for songs
		// that are, and the star would quietly start writing the wrong way.
		char(*fetched)[SUBSONIC_ID_MAX] = malloc(sizeof(*fetched) * STARS_MAX);
		int count = fetched ? subsonic_starred_ids(fetched, STARS_MAX) : -1;
		if (count >= 0) {
			pthread_mutex_lock(&star_lock);
			memcpy(stars, fetched, sizeof(stars[0]) * (size_t)count);
			for (int i = count; i < star_count; i++) {
				stars[i][0] = '\0';
			}
			star_count = count;
			stars_known = true;
			pthread_mutex_unlock(&star_lock);
			printf("subsonic: %d starred songs\n", count);
		} else {
			fprintf(stderr, "subsonic: the starred list did not arrive: %s\n", subsonic_last_error());
		}
		free(fetched);
		pthread_mutex_lock(&star_lock);
		stars_fetching = false;
		pthread_mutex_unlock(&star_lock);
		if (count >= 0) {
			mirror_landed_async(NULL);
		}
	} else if (job->kind == JOB_STAR) {
		bool ok = subsonic_star(job->song_id, false, job->want);
		if (!ok) {
			// Put the mirror back the way the server has it, or the star stays
			// filled in until the next refresh says otherwise.
			mirror_set(job->song_id, !job->want);
		}
		if (job->done) {
			job->done(ok, ok ? NULL : subsonic_last_error(), job->user);
		}
	}
	free(job);
	subsonic_stop_leave(&threads);
	return NULL;
}

// Counted before the thread exists and uncounted when it never started, so that
// stop() cannot read nought in between and unload under it.
static bool spawn(job_t *job) {
	if (!job) {
		return false;
	}
	pthread_mutex_lock(&star_lock);
	bool go = !stopping;
	pthread_mutex_unlock(&star_lock);
	if (!go) {
		free(job);
		return false;
	}
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 160 * 1024);
	subsonic_stop_enter(&threads);
	if (pthread_create(&th, &attr, worker, job) != 0) {
		subsonic_stop_leave(&threads);
		pthread_attr_destroy(&attr);
		free(job);
		return false;
	}
	pthread_attr_destroy(&attr);
	return true;
}

void subsonicservice_refresh_stars(void) {
	pthread_mutex_lock(&star_lock);
	bool start = !stopping && !stars_fetching;
	if (start) {
		stars_fetching = true;
	}
	pthread_mutex_unlock(&star_lock);
	if (!start) {
		return;
	}
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		pthread_mutex_lock(&star_lock);
		stars_fetching = false;
		pthread_mutex_unlock(&star_lock);
		return;
	}
	job->kind = JOB_REFRESH;
	if (!spawn(job)) {
		pthread_mutex_lock(&star_lock);
		stars_fetching = false;
		pthread_mutex_unlock(&star_lock);
	}
}

void subsonicservice_forget_stars(void) {
	pthread_mutex_lock(&star_lock);
	star_count = 0;
	stars_known = false;
	memset(stars, 0, sizeof(stars));
	pthread_mutex_unlock(&star_lock);
}

bool subsonicservice_star(const char *song_id, bool want, sonix_addon_service_done_cb done, void *user) {	if (!song_id || !song_id[0]) {
		return false;
	}
	mirror_set(song_id, want);
	job_t *job = calloc(1, sizeof(*job));
	if (!job) {
		mirror_set(song_id, !want);
		return false;
	}
	job->kind = JOB_STAR;
	job->want = want;
	job->done = done;
	job->user = user;
	snprintf(job->song_id, sizeof(job->song_id), "%s", song_id);
	if (!spawn(job)) {
		mirror_set(song_id, !want);
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------
// the table
// ---------------------------------------------------------------------------

static void set_root(const char *sd_root) { subsoniccache_set_root(sd_root); }

static const char *cache_dir(void) { return subsoniccache_dir(); }

static bool owns_path(const char *path) { return subsoniccache_owns(path); }

// The interface numbers a track in an int and the caller only compares it with
// itself (service.h); Subsonic's own id is a string and lives in the sidecar.
// So the number is the file name's hash, truncated -- stable across restarts,
// which is all the caller needs to know that two paths are the same track.
static int track_id(const char *path) {
	if (!subsoniccache_is_track(path)) {
		return 0;
	}
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	unsigned long long key = strtoull(name, NULL, 16);
	int id = (int)(key & 0x7fffffff);
	return id;
}

static bool downloading(void) { return subsoniccache_busy(); }

static bool network_wanted(void) { return subsoniccache_network_wanted(); }

// The cache is transient, swept on the way out; a song starred on the server is
// the account's and is never touched.
static void clear_on_exit(void) { subsoniccache_clear_on_exit(); }

static void wipe(void) { subsoniccache_clear_on_exit(); }

static void song_of(const char *path, char *out, size_t size) { subsoniccache_song_id(path, out, size); }

static bool fav_get(const char *path) {
	char song_id[SUBSONIC_ID_MAX];
	song_of(path, song_id, sizeof(song_id));
	return subsonicservice_is_starred(song_id);
}

// The mirror is fetched once and then kept in step by the toggles. A second
// caller while the first fetch is in flight is remembered and answered by the
// same landing, which is why the callback lives in the module and not in a job.
//
// Nothing is called back when the mirror is already here, and that is not an
// optimisation: the player asks this on every now-playing refresh, and a `done`
// that redraws the star would ask again -- the interface would spin through its
// own posted callbacks and stop drawing (qobuzsync.c has the same rule).
static void fav_refresh(void (*done)(void)) {
	pthread_mutex_lock(&star_lock);
	bool known = stars_known;
	bool fetching = stars_fetching;
	if (known) {
		pthread_mutex_unlock(&star_lock);
		return;
	}
	if (done) {
		stars_done = done;
	}
	pthread_mutex_unlock(&star_lock);
	if (fetching) {
		return;
	}
	subsonicservice_refresh_stars();
}

static void fav_toggle(const char *path, bool want, sonix_addon_service_done_cb done, void *user) {
	char song_id[SUBSONIC_ID_MAX];
	song_of(path, song_id, sizeof(song_id));
	if (!song_id[0]) {
		if (done) {
			done(false, "not one of this service's tracks", user);
		}
		return;
	}
	subsonicservice_star(song_id, want, done, user);
}

static bool album_id(const char *path, char *out, size_t size) { return subsoniccache_album_id(path, out, size); }

// Not const: "show album" is filled in by the page, long after this table was
// registered. The badge stays NULL -- see subsonicservice.h.
static sonix_addon_service_t subsonic = {
	.id = "subsonic",
	.set_root = set_root,
	.cache_dir = cache_dir,
	.owns_path = owns_path,
	.track_id = track_id,
	.downloading = downloading,
	.network_wanted = network_wanted,
	.clear_on_exit = clear_on_exit,
	.wipe = wipe,
	.fav_get = fav_get,
	.fav_refresh = fav_refresh,
	.fav_toggle = fav_toggle,
	.album_id = album_id,
	// The picker's "add to playlist" is not offered: the API has
	// updatePlaylist/createPlaylist, but a list of the account's playlists to
	// choose from would be a page of its own, and the row menu already has the
	// server's own playlists to browse.
	.playlists_known = NULL,
};

const sonix_addon_service_t *subsonicservice_table(void) { return &subsonic; }

void subsonicservice_attach_gui(void *(*open_album)(const char *album_id, const char *title)) {
	subsonic.badge = NULL;
	subsonic.open_album = open_album;
}

bool subsonicservice_stop(void) {
	// No new work first: a refresh or a write started from here would be a
	// thread created after the wait below.
	pthread_mutex_lock(&star_lock);
	stopping = true;
	stars_done = NULL;
	pthread_mutex_unlock(&star_lock);

	bool gone = subsonic_stop_wait(&threads, "favourites worker");

	pthread_mutex_lock(&star_lock);
	if (gone) {
		stopping = false; // a second init() is a fresh start
	}
	// The mirror describes one server's account: the copy loaded next reads its
	// own, and a stale answer here would be a star on the wrong song.
	star_count = 0;
	stars_known = false;
	stars_fetching = false;
	memset(stars, 0, sizeof(stars));
	pthread_mutex_unlock(&star_lock);
	subsonic.open_album = NULL;
	return gone;
}
