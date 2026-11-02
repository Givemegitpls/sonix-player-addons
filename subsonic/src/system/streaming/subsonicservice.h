#ifndef SUBSONICSERVICE_H
#define SUBSONICSERVICE_H

#include <stdbool.h>
#include <stddef.h>

#include "src/addon/addon_service.h"

// Subsonic as the rest of the player sees it.
//
// The player asks two kinds of question about a streaming service, and this is
// the second one (docs/ADDONS.md, "A service rather than an app"):
//
//   * before the library is opened -- "is this path Subsonic's, may the device
//     sleep, is a download running" -- answered from the addon's manifest
//     (owns_paths/cache_path) and, once it is open, from this table;
//   * about the account -- the star, "show album" -- which only the service can
//     answer.
//
// The table is the shape qobuzservice.c fills in; exported through
// sonix_addon_service_entry(), which the loader calls as soon as the library is
// open -- before init(), which is why every function here has to survive the
// cache not being set up yet.
//
// Threads, as addon_service.h says: the cache hooks are asked from any thread
// and never touch LVGL; the account hooks are called on the UI thread and their
// `done` arrives on this module's worker.

const sonix_addon_service_t *subsonicservice_table(void);

// The one piece only the page can supply: "show album" from the player's track
// menu. Called from the page's init(), long after the table was registered.
// NULL for the badge: this player's icon set has Qobuz's and Tidal's marks and
// no Subsonic one, so the artwork keeps none (the sheet's cover is the real
// one -- it comes from the cached file, not from the badge).
void subsonicservice_attach_gui(void *(*open_album)(const char *album_id, const char *title));

// The mirror of the account's starred songs: fetched on a worker and kept in
// memory, so that the star can answer without a network round trip. Called at
// init, after a sign-in and after another server is picked -- the ids belong to
// one server and mean nothing on the next.
void subsonicservice_refresh_stars(void);

// Signs the mirror out with the account: signing out does not stop a cached
// track from playing, and its star must not keep answering for an account that
// is gone.
void subsonicservice_forget_stars(void);

// The row menu's Star/Unstar. The mirror flips at once (so the sheet's star and
// the row agree) and the write goes out on a worker; `done`, if given, is called
// from there with the server's answer.
bool subsonicservice_star(const char *song_id, bool want, sonix_addon_service_done_cb done, void *user);

// The mirror's answer for a song, false when the song is unknown or the mirror
// has not landed yet.
bool subsonicservice_is_starred(const char *song_id);

// The stop half: the worker threads that write, and the mirror.
bool subsonicservice_stop(void);

#endif /* SUBSONICSERVICE_H */
