#include "yandexlive.h"

#include <pthread.h>
#include <stdio.h>
#include <time.h>

// See yandexlive.h: one counter per module, because a stop() that waits on
// another module's idle worker fails and keeps the library mapped.

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t idle = PTHREAD_COND_INITIALIZER;
static int live[YANDEXLIVE_OWNERS];

static bool owner_valid(yandexlive_owner_t owner) { return owner >= 0 && owner < YANDEXLIVE_OWNERS; }

void yandexlive_enter(yandexlive_owner_t owner) {
	if (!owner_valid(owner)) {
		return;
	}
	pthread_mutex_lock(&lock);
	live[owner]++;
	pthread_mutex_unlock(&lock);
}

void yandexlive_leave(yandexlive_owner_t owner) {
	if (!owner_valid(owner)) {
		return;
	}
	pthread_mutex_lock(&lock);
	if (live[owner] > 0) {
		live[owner]--;
	}
	if (live[owner] == 0) {
		pthread_cond_broadcast(&idle);
	}
	pthread_mutex_unlock(&lock);
}

static struct timespec deadline_after(int budget_ms) {
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
	return deadline;
}

bool yandexlive_wait(yandexlive_owner_t owner, const char *who, int budget_ms) {
	if (!owner_valid(owner)) {
		return true;
	}

	struct timespec deadline = deadline_after(budget_ms);

	pthread_mutex_lock(&lock);
	while (live[owner] > 0) {
		if (pthread_cond_timedwait(&idle, &lock, &deadline) != 0) {
			break;
		}
	}
	int left = live[owner];
	pthread_mutex_unlock(&lock);

	if (left > 0) {
		// Not an error of this module's: a thread that was handed a socket
		// cannot be recalled, and the only thing left to say is how many. The
		// caller passes the false up, and the player keeps the library mapped.
		fprintf(stderr, "yandex: %s: %d thread%s still in the library after %d ms\n", who, left, left == 1 ? "" : "s", budget_ms);
	}
	return left == 0;
}

bool yandexlive_wait_all(const char *who, int budget_ms) {
	struct timespec deadline = deadline_after(budget_ms);
	int left = 0;

	pthread_mutex_lock(&lock);
	for (;;) {
		int total = 0;
		for (int i = 0; i < YANDEXLIVE_OWNERS; i++) {
			total += live[i];
		}
		if (total == 0) {
			break;
		}
		// One deadline for the whole sweep: the modules have each had their
		// turn by now, so this is only the tail.
		if (pthread_cond_timedwait(&idle, &lock, &deadline) != 0) {
			left = total;
			break;
		}
	}
	pthread_mutex_unlock(&lock);

	if (left > 0) {
		fprintf(stderr, "yandex: %s: %d thread%s still in the library after %d ms\n", who, left, left == 1 ? "" : "s", budget_ms);
	}
	return left == 0;
}
