#include "subsonicplay.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"

#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/gui.h"
#include "src/system/core/lang.h"
#include "src/system/decode/growfile.h"
#include "src/system/playback/device_state.h"
#include "src/system/playback/playlist.h"
#include "src/system/streaming/streamturn.h"

#include "subsoniccache.h"
#include "subsonicstop.h"

// See subsonicplay.h. The shape of the queue and of the worker is zvuk's
// (addons/zvuk/src/system/streaming/zvukplay.c); what differs is said where it
// differs. There is no list module here: the page hands over an array of
// subsonic_track_t, and that is the whole catalogue this addon needs (a hundred
// rows at most).

// One list of rows at a time, and the pages never build more than LIST_MAX
// (page.c). The extra room is for "play next" additions after the list that
// started the queue.
#define QUEUE_MAX 128
#define SKIPS_MAX 3

static subsonicplay_event_fn listener;

static void tell(bool started, const char *text) {
	if (listener) {
		listener(started, text ? text : "");
	}
}

// ---------------------------------------------------------------------------
// the table of tracks behind the queue's paths
//
// The player's queue holds paths and knows nothing else: a path is a hash and a
// suffix. Everything the fetch needs -- the server's id, the title for the
// sidecar, the cover id -- lives here, looked up by the path the player asks
// about or by the song id the queue was built from.
// ---------------------------------------------------------------------------

typedef struct {
	subsonic_track_t track;
	char path[SUBSONICCACHE_PATH_MAX];
} queued_t;

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static queued_t *queue;
static int queue_count;
static unsigned queue_generation;

// Whether this library is between init() and stop(). Both run on the UI thread,
// and so do the two calls that consult it (the row menu's queue actions).
static bool alive;

static void queue_replace(queued_t *fresh, int count) {
	pthread_mutex_lock(&queue_lock);
	queued_t *old = queue;
	queue = fresh;
	queue_count = count;
	queue_generation++;
	pthread_mutex_unlock(&queue_lock);
	free(old);
}

static void queue_clear(void) { queue_replace(NULL, 0); }

// A copy: the caller must not hold the lock while it touches the card or the
// network, and a replaced queue must not be read a moment later.
static bool queue_by_path(const char *path, queued_t *out) {
	bool found = false;
	pthread_mutex_lock(&queue_lock);
	for (int i = 0; i < queue_count && !found; i++) {
		if (queue && strcmp(queue[i].path, path) == 0) {
			if (out) {
				*out = queue[i];
			}
			found = true;
		}
	}
	pthread_mutex_unlock(&queue_lock);
	return found;
}

static unsigned queue_generation_now(void) {
	pthread_mutex_lock(&queue_lock);
	unsigned generation = queue_generation;
	pthread_mutex_unlock(&queue_lock);
	return generation;
}

// Adopts one track into the table, so that a path added by "play next" or
// "enqueue" can be fetched when the player reaches it. The table is not the
// player's queue: this is only what a path means.
static void queue_adopt(const subsonic_track_t *track, const char *path) {
	if (!alive || !track || !track->id[0] || !path || !path[0] || queue_by_path(path, NULL)) {
		return;
	}
	pthread_mutex_lock(&queue_lock);
	queued_t *grown = realloc(queue, (size_t)(queue_count + 1) * sizeof(*grown));
	if (grown) {
		queue = grown;
		queue[queue_count].track = *track;
		snprintf(queue[queue_count].path, sizeof(queue[0].path), "%s", path);
		queue_count++;
	}
	pthread_mutex_unlock(&queue_lock);
}

void subsonicplay_path(const subsonic_track_t *track, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	if (!track || !track->id[0]) {
		return;
	}
	if (!subsoniccache_ready()) {
		return; // no card: no path, and the callers refuse the action
	}
	if (subsoniccache_find(track, out, size)) {
		return;
	}
	// Not on the card: the name the download will produce. The player's queue
	// keeps it, and the prepare callback fetches into exactly that name.
	subsoniccache_path(track->id, subsonic_expected_suffix(track), out, size);
}

// ---------------------------------------------------------------------------
// the worker: the tapped track, and the one after it
// ---------------------------------------------------------------------------

typedef enum { JOB_NONE = 0, JOB_PLAY, JOB_FETCH } job_kind_t;

typedef struct {
	job_kind_t kind;
	subsonic_track_t track;
	char path[SUBSONICCACHE_PATH_MAX];
	unsigned generation;
} job_t;

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_cond = PTHREAD_COND_INITIALIZER;
static job_t play_pending, fetch_pending;
static job_kind_t running_kind;
static bool worker_started;

// The worker, the flag that tells it to leave, and whether the prepare callback
// is in the player's table: see subsonicstop.h and subsonicplay_stop().
static subsonic_stop_t threads = SUBSONIC_STOP_INIT;
static bool stopping;
static bool prepare_installed;

typedef struct {
	job_kind_t kind;
	unsigned generation;
	bool ok;
	char path[SUBSONICCACHE_PATH_MAX];
	char error[192];
} result_t;

static void play_done_async(void *user);
static void fetch_done_async(void *user);

// The download about to start is what the user is waiting for, so whatever else
// is coming down gives way -- unless it is already this track, which abandoning
// would restart from zero, over and over.
static void abandon_unless(const char *song_id) {
	char busy[SUBSONIC_ID_MAX];
	subsoniccache_downloading_id(busy, sizeof(busy));
	if (strcmp(busy, song_id) != 0) {
		streamturn_abandon_all();
	}
}

static bool fetch_track(const subsonic_track_t *track, const char *path, char *error, size_t error_size) {
	error[0] = '\0';
	if (subsoniccache_complete(path)) {
		return true;
	}
	if (!subsoniccache_start(track, path)) {
		const char *why = subsoniccache_last_error();
		snprintf(error, error_size, "%s", why && why[0] ? why : tr("download_failed"));
		return false;
	}
	// The tags and the cover before the path is handed over: the player reads
	// the tags when it loads a track, and the cover beside the file is what the
	// sheet draws -- both have to be there by then.
	subsoniccache_write_sidecars(track, path);
	subsoniccache_note_played(path);
	return true;
}

static void *worker_main(void *arg) {
	(void)arg;
	// Counted by submit() before this thread existed: see subsonicstop.h.
	for (;;) {
		pthread_mutex_lock(&job_lock);
		running_kind = JOB_NONE;
		while (play_pending.kind == JOB_NONE && fetch_pending.kind == JOB_NONE && !stopping) {
			pthread_cond_wait(&job_cond, &job_lock);
		}
		if (stopping) {
			pthread_mutex_unlock(&job_lock);
			break;
		}
		job_t job;
		if (play_pending.kind != JOB_NONE) {
			job = play_pending;
			play_pending.kind = JOB_NONE;
		} else {
			job = fetch_pending;
			fetch_pending.kind = JOB_NONE;
		}
		running_kind = job.kind;
		pthread_mutex_unlock(&job_lock);

		result_t *r = calloc(1, sizeof(*r));
		if (!r) {
			continue;
		}
		r->kind = job.kind;
		r->generation = job.generation;
		snprintf(r->path, sizeof(r->path), "%s", job.path);
		// A job for a queue that has been replaced is not worth the network: the
		// paths in it are already somebody else's.
		if (job.generation == queue_generation_now()) {
			if (job.kind == JOB_PLAY) {
				abandon_unless(job.track.id);
			}
			r->ok = fetch_track(&job.track, job.path, r->error, sizeof(r->error));
		}
		if (!r->ok && r->error[0]) {
			fprintf(stderr, "subsonic: fetching %s failed: %s\n", job.track.id, r->error);
		}
		if (!gui_post(job.kind == JOB_PLAY ? play_done_async : fetch_done_async, r)) {
			free(r);
		}
	}
	subsonic_stop_leave(&threads);
	return NULL;
}

static void submit(job_kind_t kind, const subsonic_track_t *track, const char *path) {
	pthread_mutex_lock(&job_lock);
	if (stopping) {
		pthread_mutex_unlock(&job_lock);
		return;
	}
	if (!worker_started) {
		pthread_t th;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&attr, 224 * 1024);
		// Counted before it exists, uncounted when it never started, and both
		// under job_lock -- the same lock stop() takes to set `stopping`, so a
		// thread cannot be created between its wait and the unload.
		subsonic_stop_enter(&threads);
		worker_started = pthread_create(&th, &attr, worker_main, NULL) == 0;
		if (!worker_started) {
			subsonic_stop_leave(&threads);
		}
		pthread_attr_destroy(&attr);
	}
	job_t job;
	memset(&job, 0, sizeof(job));
	job.kind = kind;
	job.track = *track;
	job.generation = queue_generation_now();
	if (path) {
		snprintf(job.path, sizeof(job.path), "%s", path);
	}
	if (kind == JOB_PLAY) {
		play_pending = job;
	} else {
		fetch_pending = job;
	}
	pthread_cond_signal(&job_cond);
	pthread_mutex_unlock(&job_lock);
}

static bool worker_busy(void) {
	pthread_mutex_lock(&job_lock);
	bool busy = play_pending.kind != JOB_NONE || fetch_pending.kind != JOB_NONE || running_kind != JOB_NONE;
	pthread_mutex_unlock(&job_lock);
	return busy;
}

// ---------------------------------------------------------------------------
// the watch: a timer that exists only while something of ours is playing
// ---------------------------------------------------------------------------

static lv_timer_t *watch_timer;
static void watch_cb(lv_timer_t *timer);

static void watch_start(void) {
	// Nothing new on the way out: stop() deletes the timer, and a worker still
	// finishing a job must not put one back.
	if (!watch_timer && !stopping) {
		watch_timer = lv_timer_create(watch_cb, 1000, NULL);
	}
}

static void watch_start_async(void *user) {
	(void)user;
	watch_start();
}

static void downloading_note_async(void *user) {
	(void)user;
	gui_notify_popup(tr("downloading"));
}

// ---------------------------------------------------------------------------
// the two results, on the UI thread
// ---------------------------------------------------------------------------

// A prefetch that failed is not retried for ever: a card that is gone (or a
// server that refuses) would otherwise have the watch submit the same doomed
// download once a second, filling the log and the network with nothing.
#define FETCH_FAIL_MAX 3
static char fetch_failed_path[SUBSONICCACHE_PATH_MAX];
static int fetch_failed_count;

static void fetch_result(const char *path, bool ok) {
	if (ok) {
		fetch_failed_path[0] = '\0';
		fetch_failed_count = 0;
		return;
	}
	if (strcmp(path, fetch_failed_path) != 0) {
		snprintf(fetch_failed_path, sizeof(fetch_failed_path), "%s", path);
		fetch_failed_count = 1;
		return;
	}
	fetch_failed_count++;
}


// How many tracks in a row may fail before the queue is left where it is: a
// server that refuses everything (signed out, card full) must not turn a tap
// into a walk through the whole album.
static int skips_in_a_row;

static void play_done_async(void *user) {
	result_t *r = user;
	if (r->generation != queue_generation_now()) {
		free(r);
		return; // the user has started something else since
	}
	device_state_t state;
	device_state_get(&state);
	if (r->ok) {
		skips_in_a_row = 0;
		// The player stopped at the missing file (prepare said so); now that it
		// is there, the same entry starts again -- unless the user has pressed
		// play or moved in the meantime, in which case there is nothing to fix.
		if (state.status != AUDIO_STATUS_PLAYING) {
			char now[SUBSONICCACHE_PATH_MAX];
			if (playlist_current_path(now, sizeof(now)) && strcmp(now, r->path) == 0) {
				device_state_play_queue_index(playlist_current_index());
			}
		}
		tell(true, NULL);
	} else if (state.status != AUDIO_STATUS_PLAYING) {
		if (++skips_in_a_row <= SKIPS_MAX && !playlist_at_end()) {
			char next[SUBSONICCACHE_PATH_MAX];
			tell(false, r->error[0] ? r->error : tr("download_failed"));
			device_state_next(next, sizeof(next));
		} else {
			skips_in_a_row = 0;
			tell(false, r->error[0] ? r->error : tr("download_failed"));
		}
	}
	free(r);
}

// A prefetched track has arrived before its turn: the queue's name is already
// the file's name (subsonicplay_path() produced it), so there is nothing to
// correct -- only the note that the file is worth keeping and that the sheet
// may now have a cover it did not have.
static void fetch_done_async(void *user) {
	result_t *r = user;
	fetch_result(r->path, r->ok);
	if (r->ok && r->generation == queue_generation_now()) {
		char now[SUBSONICCACHE_PATH_MAX];
		if (playlist_current_path(now, sizeof(now)) && strcmp(now, r->path) == 0) {
			player_refresh_now_playing();
		}
	}
	free(r);
}

// ---------------------------------------------------------------------------
// starting a list
// ---------------------------------------------------------------------------

bool subsonicplay_list(const subsonic_track_t *tracks, int count, int start_index) {
	if (!tracks || count <= 0 || start_index < 0 || start_index >= count) {
		return false;
	}
	if (!subsoniccache_ready()) {
		// No card, no cache: a Subsonic track is a file, not a stream, so there
		// is nothing to hand the player. Said once, by the caller's toast.
		fprintf(stderr, "subsonic: no cache directory, nothing can be played\n");
		return false;
	}
	int kept = count < QUEUE_MAX ? count : QUEUE_MAX;
	queued_t *table = calloc((size_t)kept, sizeof(*table));
	const char **paths = calloc((size_t)kept, sizeof(*paths));
	if (!table || !paths) {
		free(table);
		free(paths);
		return false;
	}
	for (int i = 0; i < kept; i++) {
		table[i].track = tracks[i];
		subsonicplay_path(&tracks[i], table[i].path, sizeof(table[i].path));
		paths[i] = table[i].path;
	}
	if (start_index >= kept) {
		// Past the table: the queue is still right (the player got all the
		// paths), but this addon cannot fetch what it did not keep.
		start_index = kept - 1;
	}
	// The table first: device_state_play_list() calls the prepare callback from
	// inside its own call, and that callback looks the path up here.
	queue_replace(table, kept);
	fetch_failed_path[0] = '\0'; // a new queue: the old failures say nothing
	fetch_failed_count = 0;
	printf("subsonic: queue of %d tracks from %d\n", kept, start_index);
	// The player's queue: shuffle, repeat, the queue page and the star all come
	// from here, and the tapped track starts as soon as its file is there.
	device_state_play_list(paths, kept, start_index);
	free(paths);
	watch_start();
	subsoniccache_set_network_wanted(true);
	return true;
}

bool subsonicplay_play_next(const subsonic_track_t *track) {
	char path[SUBSONICCACHE_PATH_MAX];
	subsonicplay_path(track, path, sizeof(path));
	if (!path[0]) {
		return false;
	}
	queue_adopt(track, path);
	if (!playlist_insert_next(path)) {
		return false;
	}
	// The addition is the queue's now, and the players that write it down do so
	// on a track change: a queue that survives a reboot has to be written here.
	device_state_queue_changed();
	watch_start();
	return true;
}

bool subsonicplay_enqueue(const subsonic_track_t *track) {
	char path[SUBSONICCACHE_PATH_MAX];
	subsonicplay_path(track, path, sizeof(path));
	if (!path[0]) {
		return false;
	}
	queue_adopt(track, path);
	if (!playlist_enqueue(path)) {
		return false;
	}
	device_state_queue_changed();
	watch_start();
	return true;
}

// ---------------------------------------------------------------------------
// the prepare callback
// ---------------------------------------------------------------------------

bool subsonicplay_prepare_track(const char *path) {
	if (!path || !path[0] || !subsoniccache_owns(path)) {
		return true; // not ours: the player's own file
	}
	if (growfile_is_growing(path) || subsoniccache_complete(path)) {
		return true; // coming down, or already on the card
	}
	queued_t known;
	if (!queue_by_path(path, &known)) {
		// A path of ours that this addon has no track for: a queue restored at
		// boot after the cache was swept. The name is a hash, so nothing can be
		// asked for -- and the player's own "cannot play" is the honest answer.
		fprintf(stderr, "subsonic: '%s' is not on the card and its track is unknown\n", path);
		return true;
	}
	fprintf(stderr, "subsonic: '%s' (%s) is not on the card, fetching it\n", path, known.track.id);
	// The network is needed NOW: the Wi-Fi idle parking is told before playback
	// is stopped, or the radio goes down in the gap between the two.
	subsoniccache_set_network_wanted(true);
	// Stopped before the download is asked for, and not out of politeness: the
	// player would otherwise hand the decoder a path to nothing, and the audio
	// thread would report "track finished" and advance to the next missing one.
	device_state_stop();
	submit(JOB_PLAY, &known.track, path);
	// The user asked for this one: whatever the prefetch made of it before, this
	// attempt counts afresh.
	fetch_failed_path[0] = '\0';
	fetch_failed_count = 0;
	gui_post(downloading_note_async, NULL);
	gui_post(watch_start_async, NULL);
	return false;
}

// ---------------------------------------------------------------------------
// the watch's tick
// ---------------------------------------------------------------------------

// Fetches the track after the playing one, one ahead and no further: the device
// has one download turn for every service together, and a queue of five
// prefetches would split the bandwidth five ways and give the card five write
// streams (streamturn.h).
static void fetch_ahead(void) {
	int current = playlist_current_index();
	char path[SUBSONICCACHE_PATH_MAX];
	if (current < 0 || !playlist_path_at(current + 1, path, sizeof(path)) || !subsoniccache_owns(path)) {
		return;
	}
	if (subsoniccache_complete(path) || growfile_is_growing(path)) {
		return;
	}
	if (subsoniccache_busy() || worker_busy()) {
		return;
	}
	if (strcmp(path, fetch_failed_path) == 0 && fetch_failed_count >= FETCH_FAIL_MAX) {
		return; // this one has failed enough times already
	}
	queued_t known;
	if (!queue_by_path(path, &known)) {
		return;
	}
	subsoniccache_set_network_wanted(true);
	submit(JOB_FETCH, &known.track, path);
}

static int idle_ticks;

static void watch_cb(lv_timer_t *timer) {
	(void)timer;
	// What the sweep must not take: the playing track and everything after it
	// in the queue that is one of ours. The order is the playback order, so
	// with shuffle on this is what will actually be needed.
	static char keep[SUBSONICCACHE_PROTECTED_MAX][SUBSONICCACHE_PATH_MAX];
	static const char *keep_ptr[SUBSONICCACHE_PROTECTED_MAX];
	int protect = 0;
	int count = playlist_count();
	for (int i = playlist_current_index(); i >= 0 && i < count && protect < SUBSONICCACHE_PROTECTED_MAX; i++) {
		if (playlist_path_at(i, keep[protect], sizeof(keep[0])) && subsoniccache_owns(keep[protect])) {
			keep_ptr[protect] = keep[protect];
			protect++;
		}
	}
	subsoniccache_set_protected(keep_ptr, protect);
	fetch_ahead();
	bool busy = subsoniccache_busy() || worker_busy();
	subsoniccache_set_network_wanted(busy);
	idle_ticks = (protect == 0 && !busy) ? idle_ticks + 1 : 0;
	if (idle_ticks >= 5) {
		// Nothing of ours is playing, queued or coming down: the timer goes,
		// and the next tap brings it back.
		lv_timer_delete(watch_timer);
		watch_timer = NULL;
		idle_ticks = 0;
	}
}

// ---------------------------------------------------------------------------
// the rest
// ---------------------------------------------------------------------------

void subsonicplay_init(subsonicplay_event_fn l) {
	listener = l;
	alive = true;
	// Once per library, not once per call: stop() takes the callback back so
	// that a reload installs a fresh one.
	if (!prepare_installed) {
		prepare_installed = true;
		device_state_add_prepare_cb(subsonicplay_prepare_track);
	}
}

void subsonicplay_forget(void) {
	// The ids belong to the server that was left, so nothing in the table can
	// be fetched any more. The player's queue is the player's and is left as it
	// is: its cached tracks still play.
	subsoniccache_fetch_cancel();
	pthread_mutex_lock(&job_lock);
	play_pending.kind = JOB_NONE;
	fetch_pending.kind = JOB_NONE;
	pthread_mutex_unlock(&job_lock);
	queue_clear();
}

bool subsonicplay_stop(void) {
	// The prepare callback first: it is the player's, it is code in this
	// library, and it runs from whatever thread starts a track -- including one
	// the player starts while this is happening.
	if (prepare_installed) {
		device_state_remove_prepare_cb(subsonicplay_prepare_track);
		prepare_installed = false;
	}

	// The watch is the only LVGL object here. Left behind, the player's timer
	// loop would call watch_cb() in an unmapped page a second later.
	if (watch_timer) {
		lv_timer_delete(watch_timer);
		watch_timer = NULL;
	}
	idle_ticks = 0;

	// Whatever is in flight gives way before the wait: a fetch is minutes of
	// network, and the wait below is two seconds.
	subsoniccache_abandon_all();

	pthread_mutex_lock(&job_lock);
	stopping = true;
	play_pending.kind = fetch_pending.kind = JOB_NONE;
	running_kind = JOB_NONE;
	pthread_cond_broadcast(&job_cond);
	pthread_mutex_unlock(&job_lock);

	bool gone = subsonic_stop_wait(&threads, "playback worker");
	pthread_mutex_lock(&job_lock);
	if (gone) {
		worker_started = false;
		stopping = false; // a second init() starts a worker again
	}
	pthread_mutex_unlock(&job_lock);

	queue_clear();
	alive = false;
	listener = NULL;
	skips_in_a_row = 0;
	return gone;
}
