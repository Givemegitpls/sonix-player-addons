#include "time.h"

// Upstream took this from SDL: SDL_GetTicks-based std::chrono::system_clock for
// the time, SDL_Delay for the sleep. On the card there is neither, and the game's
// pacing (a 30 ms step, and the loading sequence's 100 ms steps) rides on both.
//
// CLOCK_MONOTONIC rather than the wall clock: everything the game compares is an
// interval, and a device whose clock is set by the user or steps at a time zone
// boundary must not stall a race or finish a timer early. Nothing is persisted --
// the records are times in hundredths, computed from intervals.

#include <cstdint>
#include <time.h>

namespace Time {
int64_t currentTimeMillis()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep(int64_t ms)
{
    if (ms <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    nanosleep(&ts, NULL);
}
}
