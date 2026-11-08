#include "src/addon/addon.h"

#include "cpage.h"

// Cassette, as an addon: a tile in More that opens one cassette, filling the
// panel, painted in the sleeve of whatever is playing.
//
// Everything the page needs it takes from the player: device_state_get() says
// what is loaded and how far into it playback has got, player_cover_image()
// gives the sleeve already decoded, and the transport keys are the player's own
// (player_key_next/prev/play_pause), so the page moves the same queue the
// player does rather than a second one of its own.
//
// No threads: the sleeve is sampled where it already lies, in the player's own
// buffer, and the only two timers are an LVGL one for the hubs and one that
// looks at the player twice a second.

static lv_obj_t *cassette_init(const sonix_addon_host_t *host) {
	return cpage_build(host);
}

// The page owns two timers and, when the addon's assets were there, two
// FreeType faces. Both go before this returns; the shell then deletes the
// screen and dlclose()s the library, and init() is fresh again.
static bool cassette_stop(void) {
	cpage_stop();
	return true;
}

static const sonix_addon_t cassette = {
	.api = SONIX_ADDON_API,
	.title = "Cassette",
	.init = cassette_init,
};

static const sonix_addon_stop_t cassette_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = cassette_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &cassette : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &cassette_lifecycle : NULL;
}
