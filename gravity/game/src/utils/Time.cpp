#include "time.h"

// Upstream took this from SDL: SDL_GetTicks-based std::chrono::system_clock for
// the time, SDL_Delay for the sleep. On the card there is neither, and the game's
// pacing (a 30 ms step, and the loading sequence's 100 ms steps) rides on both.
//
// CLOCK_MONOTONIC rather than the wall clock: everything the game compares is an
// interval, and a device whose clock is set by the user or steps at a time zone
// boundary must not stall a race or finish a timer early. Nothing is persisted --
// the records are times in hundredths, computed from intervals.
//
// The sleep is also where the game is asked to stop. Every loop in it paces
// itself here, so this is the one place a request from the interface thread is
// noticed from inside the game: `gravity::stopped` unwinds the thread instead of
// leaving the joiner waiting for the loop to look at Micro::field_249 (which the
// loading sequence never does, and which Micro::startApp() sets back to true on
// its way in anyway). See platform/stop.h.

#include <cstdint>
#include <time.h>

#include "platform/stop.h"

// Long sleeps (the three-second crash delay, a whole second of "Finished") are
// taken in slices so a stop lands promptly rather than at the end of one.
#define SLEEP_SLICE_MS 50

namespace Time {
int64_t currentTimeMillis()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void sleep(int64_t ms)
{
    if (gravity::stop_requested()) {
        throw gravity::stopped();
    }
    if (ms <= 0) {
        return;
    }

    const int64_t deadline = currentTimeMillis() + ms;
    for (;;) {
        int64_t left = deadline - currentTimeMillis();
        if (left <= 0) {
            return;
        }
        if (left > SLEEP_SLICE_MS) {
            left = SLEEP_SLICE_MS;
        }
        struct timespec ts;
        ts.tv_sec = (time_t)(left / 1000);
        ts.tv_nsec = (long)((left % 1000) * 1000000);
        nanosleep(&ts, NULL);
        if (gravity::stop_requested()) {
            throw gravity::stopped();
        }
    }
}
} // namespace Time
