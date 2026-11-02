#ifdef SONIX_ZVUK

#include "zvuksync.h"
#include "zvukplay.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "src/system/core/utils.h"
#include "src/system/streaming/zvukcache.h"
#include "src/system/streaming/zvukdl.h"
#include "src/system/streaming/zvukstop.h"

// See zvuksync.h.

typedef enum { JOB_LOAD, JOB_LIKE } job_kind_t;

typedef struct {
	job_kind_t kind;
	zvuk_item_t item;
	char id[ZVUK_ID_MAX];
	bool want;
	zvuksync_done_cb done;
	void (*changed)(void);
	void *user;
} job_t;

#define QUEUE_MAX 8
#define LOAD_RETRY_SECS 60

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER;
static job_t queue[QUEUE_MAX];
static int queue_head, queue_len;
static bool worker_started, load_queued;
static long load_retry_after;

// The one worker, and the flag that tells it to leave: see zvukstop.h.
static zvuk_stop_t threads = ZVUK_STOP_INIT;
static bool stopping;

static long monotonic_secs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec;
}

static void *worker_main(void *arg);

static void submit(const job_t *job) {
	pthread_mutex_lock(&lock);
	if (stopping) {
		// The library is being unloaded: a write accepted now would be one the
		// worker below never makes.
		pthread_mutex_unlock(&lock);
		return;
	}
	if (!worker_started) {
		pthread_t thread;
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 256 * 1024);
		// Counted before it exists, uncounted if it never started, under `lock`:
		// zvuksync_stop() holds the same lock to set `stopping`, so no thread
		// can appear after its wait began.
		zvuk_stop_enter(&threads);
		if (pthread_create(&thread, &attr, worker_main, NULL) == 0) {
			pthread_detach(thread);
			worker_started = true;
		} else {
			zvuk_stop_leave(&threads);
		}
		pthread_attr_destroy(&attr);
	}
	if (queue_len == QUEUE_MAX) {
		// Eight writes waiting: the service is not answering, and the newest
		// tap is the one that matters.
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
	if (job->kind == JOB_LOAD) {
		if (zvuk_liked_known(ZVUK_ITEM_TRACK) || !zvuk_logged_in()) {
			return;
		}
		if (zvuk_collection_refresh()) {
			// Taps still in the queue win over the list just read (see
			// yandexsync.c).
			pthread_mutex_lock(&lock);
			for (int i = 0; i < queue_len; i++) {
				const job_t *q = &queue[(queue_head + i) % QUEUE_MAX];
				if (q->kind == JOB_LIKE) {
					zvuk_liked_mark(q->item, q->id, q->want);
				}
			}
			pthread_mutex_unlock(&lock);
			if (job->changed) {
				job->changed();
			}
		} else {
			fprintf(stderr, "zvuksync: the collection did not load (%s); next try in %d s\n", zvuk_last_error(),
					LOAD_RETRY_SECS);
			pthread_mutex_lock(&lock);
			load_retry_after = monotonic_secs() + LOAD_RETRY_SECS;
			pthread_mutex_unlock(&lock);
		}
		return;
	}
	bool ok = zvuk_like(job->item, job->id, job->want);
	if (!ok) {
		zvuk_liked_mark(job->item, job->id, !job->want); // refused: put back
	}
	if (job->done) {
		job->done(ok, ok ? "" : zvuk_last_error(), job->user);
	}
}

static void *worker_main(void *arg) {
	(void)arg;
	// Counted by submit(), under the lock that decides whether it may start.
	// Nice ten, as yandexsync.c explains: a like is a tap waiting.
	thread_be_low_priority("zvuk sync");
	for (;;) {
		pthread_mutex_lock(&lock);
		while (queue_len == 0 && !stopping) {
			pthread_cond_wait(&ready, &lock);
		}
		if (stopping) {
			pthread_mutex_unlock(&lock);
			break;
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
	zvuk_stop_leave(&threads);
	return NULL;
}

void zvuksync_like(zvuk_item_t kind, const char *id, bool want, zvuksync_done_cb done, void *user) {
	if (!id || !id[0]) {
		return;
	}
	zvuk_liked_mark(kind, id, want);
	job_t job = {.kind = JOB_LIKE, .item = kind, .want = want, .done = done, .user = user};
	snprintf(job.id, sizeof(job.id), "%s", id);
	submit(&job);
}

void zvuksync_likes_refresh(void (*done)(void)) {
	if (zvuk_liked_known(ZVUK_ITEM_TRACK) || !zvuk_logged_in()) {
		return;
	}
	pthread_mutex_lock(&lock);
	bool queued = load_queued || monotonic_secs() < load_retry_after;
	pthread_mutex_unlock(&lock);
	if (!queued) {
		job_t job = {.kind = JOB_LOAD, .changed = done};
		submit(&job);
	}
}

// The service table the player asks about files it did not create is not
// here: it is zvukservice.c, filled from these functions the way
// addons/qobuz fills its own (docs/ADDONS.md, "A service rather than an
// app"). zvuksync.c is the writes.

// ---------------------------------------------------------------------------
// stopping
// ---------------------------------------------------------------------------

bool zvuksync_stop(void) {
	pthread_mutex_lock(&lock);
	stopping = true;
	// Writes still in the queue are not made: a like is a request of its own
	// and the mirror that answered the tap at once goes with this library
	// anyway. The one in flight -- a like is a short POST -- finishes or hits
	// the wait below.
	queue_head = 0;
	queue_len = 0;
	load_queued = false;
	load_retry_after = 0;
	pthread_cond_broadcast(&ready);
	pthread_mutex_unlock(&lock);

	bool gone = zvuk_stop_wait(&threads, "sync worker");
	if (gone) {
		pthread_mutex_lock(&lock);
		worker_started = false;
		stopping = false; // a second init() starts it again
		pthread_mutex_unlock(&lock);
	}
	return gone;
}

#endif /* SONIX_ZVUK */
