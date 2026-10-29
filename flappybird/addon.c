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

static lv_obj_t *flappybird_addon_init(const sonix_addon_host_t *host) {
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
	return flappybird_load() ? flappybird_screen : NULL;
}

static const sonix_addon_t flappybird = {
	.api = SONIX_ADDON_API,
	.title = "Flappy Bird",
	.init = flappybird_addon_init,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &flappybird : NULL;
}
