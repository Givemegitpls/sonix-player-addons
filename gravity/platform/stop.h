#ifndef GRAVITY_STOP_H
#define GRAVITY_STOP_H

#include <exception>

// How the game's own loop is interrupted, and why it needs a hook of its own.
//
// The game is a thread running upstream's `Micro::run()`, and upstream knows one
// way out: `Micro::field_249` goes false and the loop's `while` ends. That works
// for a race, and it does not work for the two places the loop is not: the
// loading sequence (`Micro::init()`, three seconds of work with no test at all)
// and the delays (`Time::sleep`). Two consequences, both seen on the device:
//
//   * leaving the addon while the game loads left the interface thread blocked in
//     pthread_join() for the whole load, and `Micro::startApp()` sets field_249
//     *true* again on its way in -- so a quit that arrived first was undone and
//     the join never came back at all;
//   * the same quit arriving before `menuManager` existed went on to
//     `destroyApp()`, which dereferences it: SIGSEGV, and the player's own signal
//     handler parks the process instead of dying, which reads as "the player
//     hung".
//
// So the platform owns a stop flag of its own -- one the game cannot set back to
// false -- and `Time::sleep()`, which every loop in the game passes through, is
// where it is noticed: it throws `stopped` and the thread unwinds. `Micro.cpp`
// tests the same flag at the top of `goLoadingStep()`, because the loading
// sequence is a run of heavy steps with no sleep between them.
//
// The flag lives in platform/canvas.cpp, next to the thread it belongs to.

namespace gravity {

struct stopped : std::exception {
	const char *what() const noexcept override { return "the addon is going away"; }
};

// True between gravity_quit() and the next gravity_run().
bool stop_requested();

} // namespace gravity

#endif /* GRAVITY_STOP_H */
