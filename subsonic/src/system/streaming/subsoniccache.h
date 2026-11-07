#ifndef SUBSONICCACHE_H
#define SUBSONICCACHE_H

#include <stdbool.h>
#include <stddef.h>

#include "../../../subsonic.h"

// Subsonic tracks downloaded to the card and played like any other file.
//
// The model is zvuk's cache (addons/zvuk/src/system/streaming/zvukcache.c):
//
//   * one GET per file, written as it arrives, announced to growfile so the
//     decoder can start on a file that is still growing;
//   * the file is named after a hash of (server, song id) -- Subsonic ids are
//     the server's own strings and may hold anything -- and the real id lives
//     in the "<file>.tags" sidecar the player's metadata reader already knows;
//   * a file being written has a "<file>.incompleto" marker beside it until it
//     is complete, so a power cut in the middle leaves a file the cache knows
//     not to trust;
//   * a track also brings its tags and its cover: "<file>.tags" (title, artist,
//     album, album id, the song id) and "<stem>.jpg", which is where the
//     player's cover loader looks for artwork beside a file. That pair is what
//     gives the player sheet a real cover and a working star;
//   * the directory is this addon's own (<addon>/cache), which is what the
//     manifest's owns_paths/cache_path already say to the player.
//
// There is no resumption: this player's http.h opens a stream from the first
// byte and cannot range one, and restarting from zero would splice two copies
// of a file into one. A dropped connection fails the download, the partial file
// goes, and the track is fetched again next time -- the same answer zvuk gives.
//
// Thread contract: subsoniccache_start() blocks for the prebuffer and must run
// on a worker thread; the rest is cheap and callable from anywhere.

// The ceiling, the free space a new download makes sure of first, and how many
// tracks between sweeps (0 = no periodic sweep, on purpose: the ceiling prunes
// one track at a time, and a burst of tens of megabytes from the thread that
// also feeds playback is what puts this device's card into its 3 s timeouts).
#define SUBSONICCACHE_MAX_BYTES (1024L * 1024L * 1024L)
#define SUBSONICCACHE_PRUNE_HEADROOM (96L * 1024 * 1024)
#define SUBSONICCACHE_CLEAR_EVERY 0
#define SUBSONICCACHE_PROTECTED_MAX 64
#define SUBSONICCACHE_PATH_MAX 512

// The cache's directory, "<addon>/cache", as addon_cache_dir() made it. NULL or
// empty means the card would not have it (read-only, or gone): the cache then
// answers "no cache", and a track cannot be played at all -- a Subsonic stream
// is not a radio station, it has to be a file.
void subsoniccache_set_addon_dir(const char *cache);

// The card, as the service registry hands it. It no longer says where the cache
// is -- that follows the addon's folder, above -- only whether there is a card
// at all: NULL or empty disables the cache.
void subsoniccache_set_root(const char *sd_root);
bool subsoniccache_ready(void);
const char *subsoniccache_dir(void);

// The file name's stem for a song: a hash of the active server and the id, so
// that one library's "23" and another's "23" are two files.
void subsoniccache_key(const char *song_id, char *out, size_t size);

// "<cache>/<key>.<suffix>" -- where a track will be. Built before the download,
// because the player's queue holds paths and some of them do not exist yet.
void subsoniccache_path(const char *song_id, const char *suffix, char *out, size_t size);

// The track's file when it is complete. The name carries the suffix the file
// was downloaded at -- the quality's, or the server's own when it was fetched
// raw -- so a track cached before the quality changed is still found: the
// expected suffix, the track's own, and the handful of others a Subsonic
// server sends are tried in turn.
bool subsoniccache_find(const subsonic_track_t *track, char *out, size_t size);

// Downloads `track` to `path` (the name the queue is holding) and returns once
// enough is on the card to start playing. Worker thread.
bool subsoniccache_start(const subsonic_track_t *track, const char *path);
const char *subsoniccache_last_error(void);

// The tags and the cover beside the file. Called on the fetch worker once the
// audio has started arriving -- before the path is handed to the player, so the
// sheet already has its cover and the star has a song id to write to.
void subsoniccache_write_sidecars(const subsonic_track_t *track, const char *path);

bool subsoniccache_owns(const char *path);
// One of the cache's audio files (not a sidecar, not a marker).
bool subsoniccache_is_track(const char *path);
// True when that file is there and finished: a part file left by a power cut
// carries a marker and answers false, so the caller fetches it again rather
// than handing the decoder a track that ends in the middle.
bool subsoniccache_complete(const char *path);
// The server's own id for a cache file, from its sidecar; "" for a sidecar or
// for anything else. The path alone cannot say it -- the name is a hash.
void subsoniccache_song_id(const char *path, char *out, size_t size);
bool subsoniccache_album_id(const char *path, char *out, size_t size);
void subsoniccache_downloading_id(char *out, size_t size);
bool subsoniccache_busy(void);

// Abandons the download in flight, whoever asked for it. Registered with the
// player's shared turn (streamturn.h) and called by every service that is about
// to download -- which is how a tap on another track takes the bandwidth back.
void subsoniccache_abandon_all(void);

// The same, without going through the shared turn: for this addon's own "the
// queue this was for is gone" (another server picked, the list replaced).
void subsoniccache_fetch_cancel(void);

// Raised as soon as the queue asks for a track, lowered by its watch: the
// Wi-Fi must not be parked between "tap" and "first byte" (see the note on
// qobuzcache_set_network_wanted).
void subsoniccache_set_network_wanted(bool wanted);
bool subsoniccache_network_wanted(void);

// The Qobuz policy for keeping the cache in bounds (qobuzcache.h): the files in
// and ahead of the queue are protected, the most recently played ones too, and
// the rest goes oldest-first when the directory outgrows its budget.
void subsoniccache_note_played(const char *path);
void subsoniccache_set_protected(const char *const *paths, int count);
void subsoniccache_clear(void);
void subsoniccache_clear_on_exit(void);

// Stops the module for good: the abandon hook is taken back out of the player's
// table, the download in flight is abandoned, the thread that was running it is
// waited for, and the card and the paths held are forgotten. False when that
// wait ran out -- a thread is still inside the library. init() afterwards is a
// fresh start.
bool subsoniccache_stop(void);

#endif /* SUBSONICCACHE_H */
