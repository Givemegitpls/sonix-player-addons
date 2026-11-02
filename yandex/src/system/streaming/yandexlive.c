#include "yandexlive.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

// See yandexlive.h.

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;
static int live;

void yandexlive_enter(void) {
	pthread_mutex_lock(&lock);
	live++;
	pthread_mutex_unlock(&lock);
}

void yandexlive_leave(void) {
	pthread_mutex_lock(&lock);
	if (live > 0) {
		live--;
	}
	if (live == 0) {
		pthread_cond_broadcast(&idle);
	}
	pthread_mutex_unlock(&lock);
}

bool yandexlive_wait(const char *who, int budget_ms) {
	// CLOCK_REALTIME, because that is what a condvar's timed wait counts in by
	// default; the deadline is computed once, so a spurious wake-up cannot
	// stretch the budget.
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += budget_ms / 1000;
	deadline.tv_nsec += (long)(budget_ms % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}

	pthread_mutex_lock(&lock);
	while (live > 0) {
		if (pthread_cond_timedwait(&idle, &lock, &deadline) != 0) {
			break;
		}
	}
	int left = live;
	pthread_mutex_unlock(&lock);

	if (left > 0) {
		// Not an error of this module's: a thread that was handed a socket
		// cannot be recalled, and the only thing left to say is how many. The
		// caller passes the false up, and the player keeps the library mapped.
		fprintf(stderr, "yandex: %s: %d thread%s still in the library after %d ms\n", who, left, left == 1 ? "" : "s", budget_ms);
	}
	return left == 0;
}
