#include "src/addon/addon.h"

#include "src/gui/games/dino.h"
#include "src/gui/games/gstate.h"

// Dino, as an addon.
//
// From the TempoTec V1 port (mr-f0xx/sonix-player-tempotec-v1, PR #56, GPL-3.0),
// where it was a page of the binary that opened from the More menu. The game and
// the chrome it is built out of (`src/gui/games/games.c`: header, score line,
// field, card, clock) are the same code; what changed is where the page comes
// from and where the best score lives.
//
// It has no artwork and no sound: every sprite is a set of coloured rectangles
// moved by a 16 ms timer, and `games.h` says why a game here does not touch the
// audio at all -- a run leaves the music playing.
//
// Their PR was reverted the same day it landed, because the V1 froze when the
// theme was changed while a game had been opened. The theme hook is the same
// (`theme_register_refresh`), so that is the first thing to try here: open the
// game, change the theme, come back.

static lv_obj_t *dino_addon_init(const sonix_addon_host_t *host) {
	// The best score, on the card beside the addon.
	gstate_init(host->dir);

	// The game draws into its own screen, and init() returns it: the shell does
	// the switching (docs/ADDONS.md), which is also what makes the page's
	// SCREEN_LOADED -- the one that hides the status bar and starts the clock --
	// fire exactly once per visit.
	dino_screen = lv_obj_create(NULL);
	dino_init(host->cfg);

	return dino_screen;
}

// The clock's timer goes with the frame, the theme hook is withdrawn, and every
// pointer into the screen is dropped: the shell deletes the screen after this
// returns, and the library is dlclose()d after that. init() is then fresh again.
//
// Always true: the clock is an LVGL timer, not a thread -- there is nothing left
// running inside the library once the frame is released.
static bool dino_addon_stop(void) {
	dino_stop();
	return true;
}

static const sonix_addon_t dino = {
	.api = SONIX_ADDON_API,
	.title = "Dino",
	.init = dino_addon_init,
};

static const sonix_addon_stop_t dino_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = dino_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &dino : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &dino_lifecycle : NULL;
}
