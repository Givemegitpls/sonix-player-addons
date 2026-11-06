#ifndef YPLAY_H
#define YPLAY_H

#include <stdbool.h>
#include <stddef.h>

#include "src/addon/addon_service.h"

#include "src/system/gw/pgw.h"

// Tracks fetched to the card and then played like any other file.
//
// The reason is the player's, not YouTube's: decoder_open() wants a path, the
// queue, seeking, the equalizer and ReplayGain all want a file, and a Piped
// media URL is no exception (qobuzcache.h has the full argument). So a track is
// downloaded into <addon>/cache and handed to the player as a path.
//
// The cache file is named after the videoId -- "<id>.mp4" -- with the two
// sidecars the player already understands beside it:
//
//     <id>.mp4.tags   title=/artist=/album=   (src/system/library/metadata.c)
//     <id>.jpg        the cover               (src/system/library/albumart.c)
//
// Only muxed MP4 (itag 18) exists today, and its moov box is ahead of mdat, so
// playback starts on the growing file: the download is announced with
// growfile_announce/progress/finish and the player opens it while the rest
// arrives.
//
// Threads: pplay_queue() blocks (network and card) and runs on a worker.
// Everything else is safe from either thread unless said otherwise.

// The addon's folder and the card root, as in pgw_init(). Called from init()
// after addon_cache_dir() has made the folder.
void pplay_init(const char *addon_dir, const char *sd_root);

// The cache directory, "" when there is none (a read-only card): every track is
// a file in there, so playback says so instead of writing somewhere else.
const char *pplay_cache_dir(void);

// Where `id` lives or would live. The name is derived from the id alone, so a
// queued track can be named before it has been fetched.
void pplay_path(const char *id, char *out, size_t size);

// True when that track is on the card in full.
bool pplay_ready(const char *id);

// Fetches items[start], then makes the whole list the player's queue with
// `playlist_load_paths` (on the UI thread, from gui_post) and starts it.
//
// `done` is called once, on the UI thread: `ok` false with `error` a sentence
// for the page when the instance gave no media URL or the download failed.
bool pplay_queue(const pgw_item_t *items, int count, int start,
				 void (*done)(bool ok, const char *error, void *user), void *user);

// Abandons a download in progress: the user picked another track, and the
// bandwidth belongs to it. Safe from the UI thread.
void pplay_abandon(void);

// The addon's stop(): abandons, joins the download worker, withdraws the
// prepare callback and the stream turn, deletes its timers. False when a worker
// is still inside the library -- see addon.h, "Unloading".
bool pplay_stop(void);

// The table the loader registers as sonix_addon_service_entry().
const sonix_addon_service_t *pplay_service_table(void);

#endif /* YPLAY_H */
