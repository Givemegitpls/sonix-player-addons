#ifndef CASSETTE_CPAGE_H
#define CASSETTE_CPAGE_H

#include "src/addon/addon.h"

// The page: one cassette, the whole panel, nothing else. The reels turn while
// something plays, the tape moves from one hub to the other as the track runs,
// and the sleeve's colour decides what the shell is made of.

// Builds the screen and returns it; the shell switches to it.
lv_obj_t *cpage_build(const sonix_addon_host_t *host);

// Drops every pointer and deletes what the page owns. The shell deletes the
// screen itself, so this runs before the library is unmapped.
void cpage_stop(void);

#endif // CASSETTE_CPAGE_H
