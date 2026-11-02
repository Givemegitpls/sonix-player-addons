#ifndef TIDALWORKERS_H
#define TIDALWORKERS_H

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/time.h>

// Waiting for a worker of this addon to be gone.
//
// An addon no longer lives as long as the process. The card is pulled, its .so
// is replaced, the folder is read again, and the player stops it, deletes its
// screens and dlclose()s it. A thread still inside the library at that moment
// is executing code that has just been unmapped, so stop() may not return
// before every worker it started has returned.
//
// A count, not a stored pthread_t: these workers are detached, like the rest of
// the ones in this program, and the only thing stop() has to know is whether
// code in this .so is still running. Counting has no answer for "which one",
// which is why the waiter names the worker it was waiting for in the log.
//
// The wait is bounded, and the bound is not politeness: a loop leaves at its
// next pass, but a pass can be an HTTP request, and the timeout in tidal.c is
// fifteen seconds. Blocking the GUI thread that long is worse than the problem
// it avoids -- addon_card_gone() gives the whole unload four seconds and
// unmounts the card when they run out (addon.c), which is exactly the case this
// whole interface exists for. Two seconds covers every stop that is going to
// happen, and a worker that does not make it is named on stderr, because that
// line is the only trace an unload that left something running will leave.

#define TIDALWORKERS_WAIT_MS 2000

typedef struct {
	pthread_mutex_t lock;
	pthread_cond_t returned;
	int live;
} tidalworkers_t;

// A static initialiser, like the two it is made of: these are declared at file
// scope and must be usable before main() runs.
#define TIDALWORKERS_INIT {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0}

// Counted before pthread_create(), never after: a thread that exists but is not
// counted is one stop() would not wait for.
static inline void tidalworkers_add(tidalworkers_t *w) {
	pthread_mutex_lock(&w->lock);
	w->live++;
	pthread_mutex_unlock(&w->lock);
}

// The last thing a worker does, on every way out of it.
static inline void tidalworkers_done(tidalworkers_t *w) {
	pthread_mutex_lock(&w->lock);
	w->live--;
	pthread_cond_broadcast(&w->returned);
	pthread_mutex_unlock(&w->lock);
}

// True when every counted thread has returned. `what` is the name the log line
// gives it; nothing else can say which worker was still inside.
static inline bool tidalworkers_wait(tidalworkers_t *w, const char *what) {
	struct timeval now;
	gettimeofday(&now, NULL);

	struct timespec until;
	until.tv_sec = now.tv_sec + TIDALWORKERS_WAIT_MS / 1000;
	until.tv_nsec = (long)now.tv_usec * 1000;

	pthread_mutex_lock(&w->lock);
	while (w->live > 0) {
		if (pthread_cond_timedwait(&w->returned, &w->lock, &until) == ETIMEDOUT) {
			break;
		}
	}
	int left = w->live;
	pthread_mutex_unlock(&w->lock);

	if (left > 0) {
		fprintf(stderr, "tidal: %s did not stop in %d ms (%d still inside the library)\n", what, TIDALWORKERS_WAIT_MS,
				left);
	}
	return left == 0;
}

#endif /* TIDALWORKERS_H */
