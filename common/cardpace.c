#include "common/cardpace.h"

#include <time.h>
#include <unistd.h>

static long mono_ms(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void cardpace_reset(cardpace_t *p) {
	p->started_ms = 0;
	p->at_ms = 0;
}

void cardpace_note(cardpace_t *p, long written) {
	if (CARDPACE_BYTES_PER_SEC <= 0 || written <= 0) {
		return;
	}
	long now = mono_ms();
	if (p->at_ms == 0) {
		p->at_ms = now;
		p->started_ms = now;
		return;
	}
	// 64-bit on purpose: `written * 1000` overflows a 32-bit long at 2.1 MB, and
	// a wrapped deadline lies in the past -- the pause then silently stops and
	// the rest of the file goes to the card at network speed.
	long long due = (long long)p->at_ms + (long long)written * 1000LL / CARDPACE_BYTES_PER_SEC;
	if (due > (long long)now) {
		usleep((useconds_t)(due - now) * 1000);
	}
}

long cardpace_elapsed_ms(const cardpace_t *p) {
	if (p->started_ms == 0) {
		return 0;
	}
	long now = mono_ms();
	return now > p->started_ms ? now - p->started_ms : 0;
}
