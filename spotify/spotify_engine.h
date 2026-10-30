#ifndef SPOTIFY_ENGINE_H
#define SPOTIFY_ENGINE_H

// The Spotify engine, as the addon sees it.
//
// The addon (`addon.c`, C, LVGL) and the engine (`engine/`, Rust, a cdylib)
// are two libraries on the same card:
//
//     <card>/Addons/spotify/addon.so                  the interface
//     <card>/Addons/spotify/assets/spotify_engine.so  the protocol
//
// The addon `dlopen()`s the engine by name when its screen is built, and takes
// the five functions below out of it. They are the whole contract: the engine
// knows nothing about LVGL and the addon knows nothing about Spotify.
//
// Why two libraries rather than one: the protocol half is Rust with a memory
// and threading model of its own, the interface half is C compiled against the
// player's headers, and the engine is the part that will be replaced when
// Spotify changes something. A card can carry a newer engine than the addon,
// which is why the version is checked before anything else is called.
//
// Threads, and this is not negotiable: every function here is called from the
// addon's UI thread. `poll()` returns in microseconds -- it copies one string
// out of a queue and nothing else. The engine's own work happens on threads it
// creates; it never calls back into the addon, so no `lv_*` is ever reached
// from the wrong thread.
//
// Events are one line of flat JSON each, no newline, e.g.
//
//     {"v":1,"event":"login","state":"failed","error":"transport_unavailable"}
//     {"v":1,"event":"log","level":"info","message":"..."}
//
// The addon prints every one of them as `spotify: <line>` and keeps the last
// on screen. Unknown events are ignored, not errors: that is what lets the
// engine grow without the addon having to be rebuilt.

#include <stdint.h>

// The ABI of this header. spotify_engine_abi() returns the number the loaded
// library was built with; the addon refuses anything else with a sentence
// instead of calling a function whose signature moved.
#define SPOTIFY_ENGINE_ABI 1

// Return codes. Anything below zero is a failure the addon reports.
#define SPOTIFY_ENGINE_OK 0
#define SPOTIFY_ENGINE_ERR_ARGUMENT (-2)  // NULL, or text that is not what it has to be
#define SPOTIFY_ENGINE_ERR_STATE (-3)     // no engine yet, or one that is closed
#define SPOTIFY_ENGINE_ERR_TOO_LONG (-4)  // one event does not fit the buffer

// The ABI the library speaks. Call this first.
int32_t spotify_engine_abi(void);

// Starts the engine. `config_json` is one flat object:
//
//     {"session_dir": "<card>/Addons/spotify", "device_name": "Sonix", "bitrate": 160}
//
// `session_dir` is where the engine may keep what is not the addon's (nothing
// today: the account is the addon's, in its own session.ini), `device_name` is
// what Spotify will show in its list of players, and `bitrate` is 96, 160 or
// 320 -- anything else is refused rather than quietly changed. Fields may be
// left out; the defaults are the ones above.
//
// Idempotent: a second call with an engine already up does nothing and returns
// OK. Returns SPOTIFY_ENGINE_ERR_ARGUMENT for text it cannot use, and then
// leaves no engine behind.
int32_t spotify_engine_init(const char *config_json);

// One login request, acted on at once and reported through poll():
//
//     {"kind": "token", "token": "..."}   a token the user pasted
//     {"kind": "signout"}                 forget the session
//     {"kind": "selftest"}                a scripted run with no account
//
// The token is not logged and not written anywhere by the engine: the addon
// keeps it on the card, and the engine only holds it for the length of the
// attempt. Returns at once.
int32_t spotify_engine_login_start(const char *request_json);

// Takes one event into `buffer` (at most `capacity` bytes, NUL-terminated) and
// returns its length, or 0 when nothing is waiting, or a negative code. One
// event longer than the buffer is dropped and ERR_TOO_LONG is returned: half
// an event is not JSON, and the caller knows to make the buffer bigger.
int32_t spotify_engine_poll(char *buffer, uint32_t capacity);

// Stops the worker and joins it. Returns 1 when there was an engine to stop
// and 0 when it was already closed or never started; both are fine. Nothing
// starts again after this, and a closed engine is not reopened by init().
int32_t spotify_engine_shutdown(void);

#endif /* SPOTIFY_ENGINE_H */
