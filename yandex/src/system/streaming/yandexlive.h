#ifndef YANDEXLIVE_H
#define YANDEXLIVE_H

#include <stdbool.h>

// How many of this addon's threads are inside it right now, and the wait that
// stop() needs from that number.
//
// addon.h's contract: stop() may return only once no thread of the library is
// still executing its code, because the player dlclose()s the mapping straight
// after. The workers here are detached (they are started on first use, by
// whichever module needs them, and nothing holds their pthread_t), so they count
// themselves instead: a thread calls yandexlive_enter() as its first statement
// and yandexlive_leave() as its last, and each module's stop function waits for
// the count to fall to zero before it frees what those threads read.
//
// The wait is bounded, and a thread that outlives it is logged. Nothing here can
// interrupt a thread already parked in a socket -- http.h wakes a stream reader,
// not http_request() -- so the bound is what keeps a 15-second HTTP timeout from
// freezing the interface, and the answer travels all the way up: each module's
// stop() returns it, and the addon's stop hook returns the conjunction to the
// player, which keeps the library mapped instead of dlclose()ing it (addon.h,
// sonix_addon_stop_t::stop).

// How long a module gives its threads before it gives up on them. The interface
// is stopped at that point (the card may be gone), so this is also how long a
// tap on the tile can hang.
#define YANDEXLIVE_WAIT_MS 2000

void yandexlive_enter(void);
void yandexlive_leave(void);

// Waits up to `budget_ms` for the last counted thread to leave; `who` names the
// module in the log line. False when the count is still above zero -- the thread
// has been told to stop and will go, but not within the budget.
bool yandexlive_wait(const char *who, int budget_ms);

#endif /* YANDEXLIVE_H */
