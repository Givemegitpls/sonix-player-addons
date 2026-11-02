#ifndef ZVUKSYNC_H
#define ZVUKSYNC_H

#include <stdbool.h>

#include "src/system/streaming/zvuk.h"

// What the player writes back to Zvuk off the UI thread: the writes are likes
// -- of a track (the player's star, a row's star), a release, an artist, a
// playlist -- each flipping the mirror at once and put back if the service
// refuses. One thread, started on the first use. Zvuk as the rest of the
// program sees it is zvukservice.c.
//
// Callbacks run on the worker: the receiver bounces them with gui_post().

typedef void (*zvuksync_done_cb)(bool ok, const char *error, void *user);

void zvuksync_like(zvuk_item_t kind, const char *id, bool want, zvuksync_done_cb done, void *user);

// Reads the collection's ids if they are not known yet (the star, the
// sheets' "Like"/"Remove like"). `done` (may be NULL) on the worker, only
// when they arrived.
void zvuksync_likes_refresh(void (*done)(void));

// Stops the module for good: the worker leaves, the writes still queued are
// dropped, and a later init starts it again. False when the worker was still
// inside the library at the end of the wait. src/addon/addon.h, "the stop
// half".
bool zvuksync_stop(void);

#endif /* ZVUKSYNC_H */