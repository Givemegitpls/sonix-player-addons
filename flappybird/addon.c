#include "src/addon/addon.h"

#include "src/gui/flappybird/flappybird.h"

#include <stdio.h>
#include <sys/stat.h>

// Flappy Bird as an addon.
//
// The game used to be a page of the binary, opened from the More section. The
// move changed two things about it and nothing else: the screen is made here,
// on the first tap, instead of at start-up, and the game is opened by this
// file rather than by a tile callback. Everything the game itself does -- the
// timer, the touch handling, the sounds through audio_external_*() -- is the
// same code, compiled against the same headers.
//
// The artwork and the five sound effects travel with the addon, in its own
// assets/ (332 KB of PNG and OGG, the same set the image used to carry for
// every board). The resource tree in the image is still read when the folder
// is not there: a card holding only addon.so then works as it did before.

static bool dir_exists(const char *path) {
	struct stat st;
	return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// The name the loader knows this addon by, for the one screen it reports
// (sonix_addon_own_screen). Copied because the host structure is the player's
// and is filled in again for the next addon it opens.
static char flappybird_id[ADDON_ID_MAX];

static lv_obj_t *flappybird_addon_init(const sonix_addon_host_t *host) {
	snprintf(flappybird_id, sizeof(flappybird_id), "%s", host->id && host->id[0] ? host->id : "");

	// The best score and the sound switch live in the addon's folder, on the
	// card, like the artwork does.
	flappybird_set_home(host->dir);

	char assets[ADDON_PATH_MAX];
	snprintf(assets, sizeof(assets), "%s/assets", host->dir);
	if (dir_exists(assets)) {
		flappybird_set_assets(assets);
	}

	flappybird_screen = lv_obj_create(NULL);
	flappybird_init(host->cfg);

	// Loads the art, starts the sounds and puts the game on its title screen.
	// It says so in a pop-up and stays where it is when a file is missing;
	// returning NULL then keeps the shell from switching to a screen that has
	// nothing on it. The switching is the shell's: see flappybird_load().
	if (!flappybird_load()) {
		return NULL;
	}

	// The screen the tile opens -- the only one this addon makes. The shell also
	// knows it by return value; reported from here as well so that every screen
	// an addon puts up is on the loader's list (addon.h,
	// sonix_addon_own_screen), and only once the game actually started: a screen
	// reported for an init() that fails would be in a list whose library is
	// about to be dlclose()d.
	sonix_addon_own_screen(flappybird_id, flappybird_screen);

	return flappybird_screen;
}

// The library is going away. `flappybird.c` knows what has to be undone and in
// which order (the mixer's thread and the PCM first, then the timer the power
// manager remembers, then the pointers); the screen is the shell's -- it is
// registered with it, and it deletes it after this returns.
static bool flappybird_addon_stop(void) { return flappybird_stop(); }

static const sonix_addon_t flappybird = {
	.api = SONIX_ADDON_API,
	.title = "Flappy Bird",
	.init = flappybird_addon_init,
};

static const sonix_addon_stop_t flappybird_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = flappybird_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &flappybird : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &flappybird_lifecycle : NULL;
}
