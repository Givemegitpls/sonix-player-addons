#include "src/addon/addon.h"

#include "src/gui/games/gstate.h"
#include "src/gui/games/t2048.h"

// 2048, as an addon.
//
// The chrome (header, score, field, overlay) is the shared games frame from
// Dino; this library only owns the board, the swipe rules and the slide/spawn
// animations. Best score lives on the card beside the addon (gstate).

static lv_obj_t *t2048_addon_init(const sonix_addon_host_t *host) {
	gstate_init(host->dir);

	t2048_screen = lv_obj_create(NULL);
	t2048_init(host->cfg);
	return t2048_screen;
}

static bool t2048_addon_stop(void) {
	t2048_stop();
	return true;
}

static const sonix_addon_t t2048_addon = {
	.api = SONIX_ADDON_API,
	.title = "2048",
	.init = t2048_addon_init,
};

static const sonix_addon_stop_t t2048_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = t2048_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &t2048_addon : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &t2048_lifecycle : NULL;
}
