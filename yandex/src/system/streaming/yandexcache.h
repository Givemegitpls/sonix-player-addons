#ifndef YANDEXCACHE_H
#define YANDEXCACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "src/system/streaming/yandex.h"

// Yandex Music tracks downloaded to the card and played like any other file.
//
// The same idea as tidalcache.h and qobuzcache.h -- the decoder wants a path,
// seeking wants to go backwards, and with the file on disk everything else in
// the player works unchanged -- with what is particular to Yandex:
//
//   * the download is one GET, from the first byte to the last. In the fork it
//     went in ranged windows of 4 MB, because Yandex's storage hosts drop long
//     connections and a window lost cost four megabytes rather than the track;
//     this player's http.h cannot ask for a range (no http_stream_open_range,
//     no http_stream_resume), so the window is gone and a connection the host
//     drops costs the track. The signed address is still renewed when it is
//     refused (get-file-info), because those expire;
//   * "encraw" files come AES-128-CTR encrypted (zero IV, key from
//     get-file-info) and are decrypted on the way to the card, so what lands
//     there is an ordinary .flac or .mp3;
//   * flac-mp4 is unwrapped into a plain .flac as it arrives (mp4flac.c), one
//     fragment at a time, like Tidal's hi-res segments.
//
// A directory of its own, so that a path tells which service it came from
// (yandexcache_owns) and two catalogues' ids never meet -- and inside this
// addon's folder, not the card's ".local" tree, which is the player's.
//
// Thread contract: yandexcache_start() blocks for the prebuffer and must run
// on a worker thread; the rest is cheap and callable from anywhere.

// The cache's directory, "<addon>/cache", as addon_cache_dir() made it. NULL or
// empty means the card would not have it (read-only, or gone): the cache then
// answers "no cache" and tracks stream without being kept.
void yandexcache_set_addon_dir(const char *cache);

// The card, as the service registry hands it. It no longer says where the cache
// is -- that follows the addon's folder, above -- only whether there is a card
// at all: NULL or empty disables the cache.
void yandexcache_set_root(const char *sd_root);
bool yandexcache_ready(void);

// The path a track would have with this extension ("flac", "mp3", "m4a").
void yandexcache_path(const char *track_id, const char *ext, char *out, size_t size);
// The path of the track if it is fully downloaded, under any extension.
bool yandexcache_find(const char *track_id, char *out, size_t size);
// The extension the cache gives a codec of get-file-info.
const char *yandexcache_extension(const char *codec);

// Starts downloading and returns once enough is on the card to start playing
// (or, for an MP4 container, once all of it is). `out` gets the path.
bool yandexcache_start(const yandex_track_t *track, const yandex_file_t *file, char *out, size_t size);
const char *yandexcache_last_error(void);

// Downloads `file` into `out` (the caller's, left open) on the caller's own
// thread: decrypted, and unwrapped into plain FLAC when it is flac-mp4 --
// exactly what the cache would hold, for a copy kept elsewhere (yandexdl.h).
// Gives way to everything being listened to: while the cache or another
// service downloads, it drops its connection and waits; without a Range request
// it cannot resume, so the save fails and the track is fetched again when it is
// asked for (see the note above). One at a time. `progress` (may be NULL) hears the bytes written
// and the source size (0 while unknown). `samples` (may be NULL) gets a
// flac-mp4's counted length, 0 otherwise. False with `err` set on failure or
// after yandexcache_fetch_cancel().
bool yandexcache_fetch(const char *track_id, const yandex_file_t *file, FILE *out, void (*progress)(long written, long total, void *user), void *user, uint64_t *samples, char *err, size_t err_size);
void yandexcache_fetch_cancel(void);

// The tags beside the file ("<file>.tags") and the cover ("<stem>.jpg").
void yandexcache_write_sidecars(const yandex_track_t *track, const char *path);

// Whether `path` is in this cache, and which track it is (empty when not).
bool yandexcache_owns(const char *path);
void yandexcache_track_id(const char *path, char *out, size_t size);
// The track downloading right now, or an empty string.
void yandexcache_downloading_id(char *out, size_t size);

// Gives up every download in flight (registered with streamturn as well, and
// unregistered when the library is unloaded: yandexcache_stop).
void yandexcache_abandon_all(void);

// --- what the rest of the player asks (service.h, through yandexservice.c) ---

// The cache directory, NULL without a card.
const char *yandexcache_dir(void);
// A download is in progress right now.
bool yandexcache_busy(void);
// One of the cache's audio files (not a sidecar, not a half-written .part).
bool yandexcache_is_track(const char *path);
// The album the track came off, from its sidecar; false when it does not say.
bool yandexcache_album_id(const char *path, char *out, size_t size);

// "Yandex needs the network", with nothing downloading this instant: between
// a track's end and the next one's first bytes sit the stop and a
// get-file-info request, and the Wi-Fi parking (power.c) must not read that
// gap as idle. Raised by the page as soon as it asks for a track, lowered by
// its once-a-second watch. See qobuzcache_network_wanted() for the long story.
void yandexcache_set_network_wanted(bool wanted);
bool yandexcache_network_wanted(void);

// --- keeping it in bounds: the Qobuz policy (qobuzcache.h) ---------------

// The ceiling, the free space a new download makes sure of first, and how
// many tracks between sweeps.
#define YANDEXCACHE_MAX_BYTES (1024L * 1024L * 1024L)
#define YANDEXCACHE_PRUNE_HEADROOM (96L * 1024 * 1024)
#define YANDEXCACHE_CLEAR_EVERY 10
#define YANDEXCACHE_PROTECTED_MAX 64

// Every track fetched, with its path: counts towards the next sweep and is
// spared by it for a while (the queue may not know about it yet).
void yandexcache_note_played(const char *path);
// The tracks the queue still expects: the current one and those after it. The
// sweep and the ceiling skip them. GUI thread.
void yandexcache_set_protected(const char *const *paths, int count);
// "Clear cache" on the Downloads page: every file but the one coming down
// right now, which playback may be reading. Any thread.
void yandexcache_clear(void);
// Shutdown, restart, update, factory reset: the cache does not survive a power
// cycle.
void yandexcache_clear_on_exit(void);

// The addon being unloaded: the downloads are abandoned, the download thread is
// waited for and the module forgets the card (the files stay). Called from the
// addon's stop hook, before the library is unmapped and before libcrypto is
// closed.
bool yandexcache_stop(void);

#endif /* YANDEXCACHE_H */
