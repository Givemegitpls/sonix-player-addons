#ifndef SUBSONICSTOP_H
#define SUBSONICSTOP_H

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
// The shape is zvuk's (addons/zvuk/src/system/streaming/zvukstop.h): each
// module counts its own workers, and every module's stop() sets its own "leave
// the loop" flag, broadcasts whatever the loop waits on, and then waits.
#define SUBSONIC_STOP_WAIT_MS 2000

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t changed;
	int live;
} subsonic_stop_t;

#define SUBSONIC_STOP_INIT                                                                                            \
	{                                                                                                                 \
		.lock = PTHREAD_MUTEX_INITIALIZER, .changed = PTHREAD_COND_INITIALIZER, .live = 0                            \
	}

static inline void subsonic_stop_enter(subsonic_stop_t *s) {
	pthread_mutex_lock(&s->lock);
	s->live++;
	pthread_mutex_unlock(&s->lock);
}

static inline void subsonic_stop_leave(subsonic_stop_t *s) {
	pthread_mutex_lock(&s->lock);
	s->live--;
	pthread_cond_broadcast(&s->changed);
	pthread_mutex_unlock(&s->lock);
}

// Waits up to SUBSONIC_STOP_WAIT_MS for every thread that entered to leave.
// True when the count reached zero; false is a thread still inside the library,
// which is said out loud and then left alone -- a false here means the caller
// must not free what that thread may still write.
static inline bool subsonic_stop_wait(subsonic_stop_t *s, const char *what) {
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += SUBSONIC_STOP_WAIT_MS / 1000;
	deadline.tv_nsec += (long)(SUBSONIC_STOP_WAIT_MS % 1000) * 1000000L;
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
		fprintf(stderr, "subsonic: %s: %d thread(s) still running after %d ms\n", what, left, SUBSONIC_STOP_WAIT_MS);
	}
	return gone;
}

#endif /* SUBSONICSTOP_H */
