#ifndef ZVUKSTOP_H
#define ZVUKSTOP_H

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

// How many of this addon's threads are running, and the wait stop() does on
// them.
//
// src/addon/addon.h: after stop() returns the library may be dlclose()d, so no
// thread of it may still be executing code in the mapping. A library cannot
// make a thread come back -- it is blocked in a socket, in a card write, in
// another service's download turn -- so the contract is a count, a broadcast
// and a bounded wait, with a line on stderr when the bound is reached. The wait
// is bounded because the alternative is worse: the player unloads to unmount
// the card, and an unload that hangs behind a stalled download takes the
// interface down with it.
//
// Each worker calls zvuk_stop_enter() as its first statement and
// zvuk_stop_leave() on its way out; each module's stop() sets its own "leave
// the loop" flag, broadcasts whatever the loop waits on, and then calls
// zvuk_stop_wait(). The flag is the module's own because only the module knows
// what its loop waits for; the count is shared because the question -- may this
// library be unmapped -- is the same for all of them.
#define ZVUK_STOP_WAIT_MS 2000

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	int live;
} zvuk_stop_t;

#define ZVUK_STOP_INIT                                                                                                 \
	{                                                                                                                  \
		.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER, .live = 0                             \
	}

static inline void zvuk_stop_enter(zvuk_stop_t *s) {
	pthread_mutex_lock(&s->lock);
	s->live++;
	pthread_mutex_unlock(&s->lock);
}

static inline void zvuk_stop_leave(zvuk_stop_t *s) {
	pthread_mutex_lock(&s->lock);
	s->live--;
	pthread_cond_broadcast(&s->changed);
	pthread_mutex_unlock(&s->lock);
}

// Waits up to ZVUK_STOP_WAIT_MS for every thread that entered to leave. True
// when the count reached zero; false is a thread still inside the library,
// which is said out loud and then unloaded anyway -- the caller decides what it
// can still safely free, and a false here means "nothing the thread writes".
static inline bool zvuk_stop_wait(zvuk_stop_t *s, const char *what) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += ZVUK_STOP_WAIT_MS / 1000;
	deadline.tv_nsec += (long)(ZVUK_STOP_WAIT_MS % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec++;
		deadline.tv_nsec -= 1000000000L;
	}

	pthread_mutex_lock(&s->lock);
	while (s->live > 0) {
		// Anything but zero is the deadline (or a signal, which is the same
		// answer here): the loop is left and the count is read once more, so a
		// thread that left in the last instant still counts as gone.
		if (pthread_cond_timedwait(&s->changed, &s->lock, &deadline) != 0) {
			break;
		}
	}
	bool gone = s->live == 0;
	int left = s->live;
	pthread_mutex_unlock(&s->lock);

	if (!gone) {
		fprintf(stderr, "zvuk: %s: %d thread(s) still running after %d ms\n", what, left, ZVUK_STOP_WAIT_MS);
	}
	return gone;
}

#endif /* ZVUKSTOP_H */
