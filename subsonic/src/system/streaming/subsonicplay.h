#ifndef SUBSONICPLAY_H
#define SUBSONICPLAY_H

#include <stdbool.h>
#include <stddef.h>

#include "../../../subsonic.h"

// Subsonic tracks played through the player's own queue.
//
// The first version of this addon made every track a radio station: radio_play()
// was the only public way an addon could hand audio to the transport, so a tap
// built a `radio_station_t` and fed the visible rows to radio_set_list(). A
// live stream has no order, no end and no queue -- shuffle, repeat, the queue
// page, "play next" and the star all stop making sense -- and the sheet has no
// cover beyond the station's icon.
//
// So a track is downloaded and then played as a file of the card, which is what
// the player already does for Qobuz, Tidal, Yandex and Zvuk: the queue is
// playlist.c's, the prepare callback (device_state.h) turns a path that is not
// there yet into a download, and the sheet gets shuffle, repeat, the queue, the
// star and the real cover for free.
//
// What this module adds on top:
//
//   * the addon's track list kept beside the paths, so that a path -- which is
//     a hash of the song id -- can still say which track it is when the player
//     asks about it an hour later;
//   * one worker: a tap that has to wait for a download waits there, and the
//     next track is fetched one step ahead;
//   * "Play next" and "Add to queue", which are playlist_insert_next() and
//     playlist_enqueue() over the path the same naming rules produce.
//
// Threads: everything here except subsonicplay_list(), subsonicplay_path(),
// subsonicplay_play_next() and subsonicplay_enqueue() may be called from any
// thread; the four are the UI thread's. The prepare callback runs on whatever
// thread starts a track, so it touches no LVGL object (gui_post is the way back
// to the interface), exactly as qobuzpage.c's does.

// What the page is told, on the UI thread: whether playback started (the sheet
// should refresh) or why it did not.
typedef void (*subsonicplay_event_fn)(bool started, const char *text);

// Idempotent, as init() has to be: installs the prepare callback once and keeps
// the listener for the page's own notifications.
void subsonicplay_init(subsonicplay_event_fn listener);

// Starts the whole list from `start_index`: the player's queue becomes these
// tracks, and the one tapped starts as soon as its file is there.
bool subsonicplay_list(const subsonic_track_t *tracks, int count, int start_index);

// The file a track plays from: the cached one when there is one, otherwise the
// name the download will produce. No network, no card write.
void subsonicplay_path(const subsonic_track_t *track, char *out, size_t size);

// The row menu's two queue actions. False when the player refused the addition.
bool subsonicplay_play_next(const subsonic_track_t *track);
bool subsonicplay_enqueue(const subsonic_track_t *track);

// Another server was picked: nothing in the queue can be fetched any more (the
// ids belong to the server that was left), so the download is cancelled and the
// table of tracks dropped. The player's queue is left alone -- it is the
// player's, and its cached tracks still play.
void subsonicplay_forget(void);

// The prepare callback itself, exposed for the log line and for a caller that
// wants to ask the same question. True when the file is there or coming down.
bool subsonicplay_prepare_track(const char *path);

// The stop half: the timer, the prepare callback, the worker and its jobs.
// False when a thread is still inside the library (subsonicstop.h).
bool subsonicplay_stop(void);

#endif /* SUBSONICPLAY_H */
