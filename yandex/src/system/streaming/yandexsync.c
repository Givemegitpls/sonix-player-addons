#include "yandexsync.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/core/utils.h"
#include "src/system/streaming/yandex.h"

// See yandexsync.h.

static long monotonic_secs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec;
}

// ---------------------------------------------------------------------------
// the worker and its queue
//
// A queue of a few jobs, not one slot, for the reason subsonicsync.c gives: a
// like tapped while the mirror is still loading must not replace the load, or
// the other way round. Started on first use, so a player never signed in to
// Yandex never has the thread.
// ---------------------------------------------------------------------------

typedef enum { JOB_LOAD, JOB_LIKE } job_kind_t;

typedef struct {
	job_kind_t kind;
	char track_id[YANDEX_ID_MAX];
	char album_id[YANDEX_ID_MAX];
	bool want;
	yandexsync_done_cb done;
	void (*changed)(void);
	void *user;
} job_t;

#define QUEUE_MAX 8

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static job_t queue[QUEUE_MAX];
static int queue_head;
static int queue_len;
static bool worker_started;
static bool load_queued; // the star asks on every redraw: one load in the queue is enough
// A load that failed is not asked for again before this (monotonic seconds):
// the star asks on every redraw, and a list that cannot be had right now
// (no network, the rate limit) would otherwise be asked for on every one.
static long load_retry_after;
#define LOAD_RETRY_SECS 60

static void *worker_main(void *arg);

static void submit(const job_t *job) {
	pthread_mutex_lock(&lock);
	if (!worker_started) {
		pthread_t thread;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024); // TLS: as the page's worker
		if (pthread_create(&thread, &attr, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		}
		pthread_attr_destroy(&attr);
	}
	if (queue_len == QUEUE_MAX) {
		// Full: the oldest goes. Eight writes waiting means Yandex has stopped
		// answering (or is making us wait, see throttle() in yandex.c), and the
		// newest tap is the one that matters.
		if (queue[queue_head].kind == JOB_LOAD) {
			load_queued = false;
		}
		queue_head = (queue_head + 1) % QUEUE_MAX;
		queue_len--;
	}
	if (job->kind == JOB_LOAD) {
		load_queued = true;
	}
	queue[(queue_head + queue_len) % QUEUE_MAX] = *job;
	queue_len++;
	pthread_cond_signal(&ready);
	pthread_mutex_unlock(&lock);
}

static void run_job(const job_t *job) {
	switch (job->kind) {
	case JOB_LOAD:
		if (yandex_liked_known() || !yandex_logged_in()) {
			break;
		}
		if (yandex_liked_refresh()) {
			// A star tapped while the list was on its way is in the queue,
			// not on the account yet: the list just installed does not have
			// it, and showing that would flip the star back under the
			// user's finger. Their taps win until they are sent.
			pthread_mutex_lock(&lock);
			for (int i = 0; i < queue_len; i++) {
				const job_t *q = &queue[(queue_head + i) % QUEUE_MAX];
				if (q->kind == JOB_LIKE) {
					yandex_liked_mark(q->track_id, q->want);
				}
			}
			pthread_mutex_unlock(&lock);
			if (job->changed) {
				job->changed();
			}
		} else {
			// Without the list the star reads every track as not liked, and
			// a tap on a liked one adds it again instead of removing it.
			fprintf(stderr, "yandexsync: the liked tracks did not load (%s); next try in %d s\n", yandex_last_error(), LOAD_RETRY_SECS);
			pthread_mutex_lock(&lock);
			load_retry_after = monotonic_secs() + LOAD_RETRY_SECS;
			pthread_mutex_unlock(&lock);
		}
		break;
	case JOB_LIKE: {
		printf("yandexsync: %s %s (album %s)\n", job->want ? "like" : "unlike", job->track_id, job->album_id[0] ? job->album_id : "none");
		bool ok = yandex_like_track(job->track_id, job->album_id, job->want);
		if (!ok) {
			yandex_liked_mark(job->track_id, !job->want); // refused: put it back
			fprintf(stderr, "yandexsync: like of %s failed: %s\n", job->track_id, yandex_last_error());
		}
		if (job->done) {
			job->done(ok, ok ? "" : yandex_last_error(), job->user);
		}
		break;
	}
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	// Nice ten, not SCHED_IDLE (which the other services' sync threads use).
	// The likes are the one thing here, and a like is a tap waiting for its
	// answer. With Bluetooth LDAC encoding and a track decoding, the device
	// log shows the core 0 to 20 % idle; an idle-class thread then gets well
	// under one percent of it (see thread_be_low_priority), and the TLS
	// handshake plus the liked list a like may queue behind took minutes --
	// long enough for the star to flip back, for the player to sleep, for
	// the like never to leave. The lists, on the page's own worker, were at
	// normal priority all along, which is why everything but the likes
	// worked.
	thread_be_low_priority("yandex sync");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (queue_len == 0) {
			pthread_cond_wait(&ready, &lock);
		}
		job_t job = queue[queue_head];
		queue_head = (queue_head + 1) % QUEUE_MAX;
		queue_len--;
		if (job.kind == JOB_LOAD) {
			load_queued = false;
		}
		pthread_mutex_unlock(&lock);
		run_job(&job);
	}
	return NULL;
}

// ---------------------------------------------------------------------------
// the calls
// ---------------------------------------------------------------------------

void yandexsync_like(const char *track_id, const char *album_id, bool want, yandexsync_done_cb done, void *user) {
	if (!track_id || !track_id[0]) {
		return;
	}
	yandex_liked_mark(track_id, want);
	job_t job = {.kind = JOB_LIKE, .want = want, .done = done, .user = user};
	snprintf(job.track_id, sizeof(job.track_id), "%s", track_id);
	snprintf(job.album_id, sizeof(job.album_id), "%s", album_id ? album_id : "");
	submit(&job);
}

void yandexsync_likes_refresh(void (*done)(void)) {
	// Called on every redraw of the star: the common case -- already known,
	// or not signed in -- costs two flag reads and no lock held for long.
	if (yandex_liked_known() || !yandex_logged_in()) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool queued = load_queued || monotonic_secs() < load_retry_after;
	pthread_mutex_unlock(&lock);
	if (queued) {
		return;
	}
	job_t job = {.kind = JOB_LOAD, .changed = done};
	submit(&job);
}

// ---------------------------------------------------------------------------
// the service table is not here
//
// The fork ended this file with the stream_provider_t it registered with the
// player's provider registry: the cache hooks, the star, "show album". That is
// sonix_addon_service_t in this tree, and it lives in yandexservice.c, which
// takes the same functions from yandexcache.c, yandexdl.c, yandexsaved.c and
// from this file. Nothing of it is registered from here: an addon's table is
// returned by sonix_addon_service_entry() and the player registers it when the
// library is opened.
// ---------------------------------------------------------------------------
