#ifndef QOBUZTHREADS_H
#define QOBUZTHREADS_H

#include <pthread.h>
#include <time.h>

// How many of a module's threads are still running, and the wait for them to
// be gone.
//
// stop() runs on the GUI thread and the library may be dlclose()d the instant
// it returns (addon.h, "Unloading"): every thread this addon started has to be
// back by then, or the code it is still executing is unmapped underneath it.
// Waiting is therefore not politeness, it is the condition for being allowed to
// unload at all.
//
// Waited for, not joined. Two of the three are detached and one of them can be
// inside a twenty-second socket read (http.h has no cancel), the other inside
// the shared download turn (streamturn.h has no way out of a wait): a stop()
// that blocks until every one of those finishes freezes the interface exactly
// when the card is being made safe. So the wait is bounded and a thread that
// outlasts it is named in the log -- the caller can then decide, and a report
// saying the unload is not safe is worth more than a hang.
//
// The count goes up before pthread_create() and down on the last line of the
// thread function, so zero means "no code of ours is executing".
typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t gone;
	int live;
} qobuz_threads_t;

#define QOBUZ_THREADS_INIT                                                                                           \
	{ PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0 }

// Two seconds: long enough for a download to notice the abandoned socket and
// unwind, short enough that the four addons of a card being pulled do not add
// up to a visible freeze. Nothing in the player can interrupt a request, so a
// larger number buys nothing but a longer stall.
#define QOBUZ_STOP_WAIT_MS 2000

static inline void qobuz_threads_add(qobuz_threads_t *c) {
	pthread_mutex_lock(&c->lock);
	c->live++;
	pthread_mutex_unlock(&c->lock);
}

static inline void qobuz_threads_done(qobuz_threads_t *c) {
	pthread_mutex_lock(&c->lock);
	c->live--;
	if (c->live <= 0) {
		pthread_cond_broadcast(&c->gone);
	}
	pthread_mutex_unlock(&c->lock);
}

// Blocks until none are left or `timeout_ms` passes. Returns how many were
// still running, which is what the caller logs.
static inline int qobuz_threads_wait(qobuz_threads_t *c, int timeout_ms) {
	// CLOCK_REALTIME and not CLOCK_MONOTONIC: this is the clock
	// pthread_cond_timedwait() reads unless the condition variable was given an
	// attribute, and it is in libc on the device's glibc 2.16 while the
	// monotonic one is in librt (see glibc_compat.h).
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += timeout_ms / 1000;
	deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&c->lock);
	while (c->live > 0) {
		if (pthread_cond_timedwait(&c->gone, &c->lock, &deadline) != 0) {
			break; // timed out, or the wait was interrupted
		}
	}
	int left = c->live;
	pthread_mutex_unlock(&c->lock);
	return left;
}

#endif /* QOBUZTHREADS_H */
