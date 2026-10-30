#ifndef YANDEXCOMPAT_H
#define YANDEXCOMPAT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "src/system/library/mp4flac.h"

// The handful of player functions rekkod's fork had and this player does not.
//
// Yandex Music was a service of that fork, and its eight modules call the player
// in its own vocabulary. Almost all of it is here already -- http_get_ex,
// http_stream_*, the playlist reader, device_state, the card helpers -- but a
// few calls have no counterpart at all, and every one of them is written out
// below with what this player has instead and what the substitution costs. They
// carry a yandex_ prefix rather than the player's names on purpose: if the
// player ever grows the real function, a local definition of the same name would
// shadow it silently.
//
// addons/zvuk/src/system/streaming/zvukcompat.{h,c} is the same file for the
// other service that came from that tree; the two differ only in what their
// modules happen to call.
//
// What is missing, and why it is not simply added to the player:
//
//   playlist_replace_path()              the queue cannot rename an entry in
//                                        place; rebuilt through the public
//                                        playlist_load_paths()
//   device_state_note_metadata_available no metadata revision in device_state:
//                                        the tags appear at the next track
//   sleeptimer_expiration_serial()       no serial on the sleep timer
//   storage_pin_card()                   no mount pin: an fd on the card root
//   http_set_thread_timeout_cap()        no per-thread cap; the timeout is
//                                        passed per call
//   gui_post_or_free()                   gui_post() answers false when the
//                                        queue is full; the caller frees
//   mp4flac_payload()                    the player unwraps DASH segments, not
//                                        a streamed mdat
//   mp4flac_total_samples()              no sample counting (see above)
//   mp4flac_patch_total_samples()        not in this player's mp4flac.c; the
//                                        five bytes are rewritten here
//
// One thing this player's http.h has no answer for at all, and it is not a shim
// but a loss, is written out where it bites: a ranged request
// (http_stream_open_range/http_stream_resume in the fork's http.h). The Yandex
// cache's ranged windows and its resume-after-a-drop are gone with it; see
// "the source" in yandexcache.c.
//
// All of them are safe from any thread unless a comment says otherwise; the
// ones that touch the interface say so.

// Renames every queue entry that is `old_path` to `new_path`, and returns how
// many. GUI thread.
//
// The player's queue is a black box: playlist_replace_path() does not exist, and
// the entry is the guess "<id>.flac" that yandexplay.c puts in the queue before
// the file is fetched (the real extension is only known once the stream
// answers). So the queue is rebuilt from its own paths with
// playlist_load_paths(), keeping the current position. What that costs: the
// appended/library bookkeeping of the queue is dropped and a shuffle deals the
// upcoming order again -- the same tracks, possibly in another order. Called
// once per fetched track, on the UI thread.
int yandex_playlist_replace_path(const char *old_path, const char *new_path);

// The file `path` now has tags to read. This player reads a track's metadata
// once, when the track is loaded (device_state.h), and has no way to ask for it
// again; the streaming queue loads a track only after its file is there, so the
// call is a no-op that keeps the module's flow unchanged.
void yandex_device_state_note_metadata_available(const char *path);

// The sleep timer's serial in the fork: a job that started before the timer was
// reset must not start playback after it. This player's sleeptimer.h has no
// serial, so the guard degenerates to "nothing was reset meanwhile" -- a track
// fetched just as the timer fires may still start.
unsigned yandex_sleeptimer_expiration_serial(void);

// The card's root, and an fd on it: fstatat() through that fd is how yandexdl.c
// checks that the folder it is about to write into is still on the same card it
// admitted. -1 and errno when there is no card. The caller closes the fd.
int yandex_storage_pin_card(char *out, size_t size);

// A cap on the thread's HTTP timeouts. This player's http.h takes the timeout
// per call, so the cap is not recorded.
void yandex_http_set_thread_timeout_cap(int secs);

// gui_post() for a heap-allocated `user`: this player's gui_post() says whether
// the message was queued, so a caller that allocated its argument has to free it
// when the answer is no. The fork had one call for both.
void yandex_gui_post_or_free(void (*cb)(void *), void *user);

// The mdat payload of a non-fragmented MP4 as it comes: FLAC frames back to
// back, which is what the file already holds after mp4flac_begin() wrote the
// signature and the STREAMINFO.
bool yandex_mp4flac_payload(mp4flac_t *m, const uint8_t *data, size_t size);

// The length in samples. Not counted here: mp4flac_t in this player has no
// timing fields, and a Yandex FLAC arrives with its STREAMINFO filled in.
uint64_t yandex_mp4flac_total_samples(const mp4flac_t *m);

// Writes `samples` into the STREAMINFO of the finished .flac when it says 0
// (unknown) there. Five bytes rewritten in place; nothing else is touched.
bool yandex_mp4flac_patch_total_samples(const char *path, uint64_t samples);

#endif /* YANDEXCOMPAT_H */
