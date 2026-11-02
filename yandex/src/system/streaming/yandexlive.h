#ifndef YANDEXLIVE_H
#define YANDEXLIVE_H

#include <stdbool.h>

// How many of this addon's threads are inside it right now, and the waits that
// stop() needs from those numbers.
//
// addon.h's contract: stop() may return only once no thread of the library is
// still executing its code, because the player dlclose()s the mapping straight
// after. The workers here are detached (they are started on first use, by
// whichever module needs them, and nothing holds their pthread_t), so they count
// themselves instead: a thread calls yandexlive_enter() as its first statement
// and yandexlive_leave() as its last, and the module's stop function waits for
// its own count to fall to zero before it frees what those threads read.
//
// PER MODULE, and that is the point of the owner argument: a single shared
// counter makes every stop() wait for everybody else's idle workers, and a wait
// that fails keeps the library mapped for the rest of the run. Each module owns
// one counter, and a module's stop only ever waits on its own.
//
// The wait is bounded, and a thread that outlives it is logged. Nothing here can
// interrupt a thread already parked in a socket -- http.h wakes a stream reader,
// not http_request() -- so the bound is what keeps a 15-second HTTP timeout from
// freezing the interface, and the answer travels all the way up: each module's
// stop() returns it, and the addon's stop hook returns the conjunction to the
// player, which keeps the library mapped instead of dlclose()ing it.

// How long a module gives its threads before it gives up on them. The interface
// is stopped at that point (the card may be gone), so this is also how long a
// tap on the tile can hang.
#define YANDEXLIVE_WAIT_MS 2000

typedef enum {
	YANDEXLIVE_PAGE = 0, // the page's own request workers
	YANDEXLIVE_PLAY,
	YANDEXLIVE_WAVE,
	YANDEXLIVE_SYNC,
	YANDEXLIVE_DL,
	YANDEXLIVE_CACHE,
	YANDEXLIVE_OWNERS
} yandexlive_owner_t;

void yandexlive_enter(yandexlive_owner_t owner);
void yandexlive_leave(yandexlive_owner_t owner);

// Waits up to `budget_ms` for this owner's last thread to leave; `who` names the
// module in the log line. False when the count is still above zero -- the thread
// has been told to stop and will go, but not within the budget.
bool yandexlive_wait(yandexlive_owner_t owner, const char *who, int budget_ms);

// The same for every owner, for the addon's own stop hook: after the modules
// have each waited, anything still counted belongs to a module whose stop did
// not run.
bool yandexlive_wait_all(const char *who, int budget_ms);

#endif /* YANDEXLIVE_H */
