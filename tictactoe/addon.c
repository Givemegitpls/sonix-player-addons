#include "src/addon/addon.h"

#include "src/gui/games/gstate.h"
#include "src/gui/games/ttt.h"
#include "src/gui/games/tttsettings.h"

// Tic Tac Toe: games frame + a settings screen for bot difficulty.

static lv_obj_t *ttt_addon_init(const sonix_addon_host_t *host) {
	gstate_init(host->dir);

	ttt_screen = lv_obj_create(NULL);
	tttsettings_screen = lv_obj_create(NULL);

	ttt_init(host->cfg);
	tttsettings_init(host->cfg, host->id);

	return ttt_screen;
}

static bool ttt_addon_stop(void) {
	tttsettings_stop();
	ttt_stop();
	tttsettings_screen = NULL;
	return true;
}

static const sonix_addon_t tictactoe = {
	.api = SONIX_ADDON_API,
	.title = "Tic Tac Toe",
	.init = ttt_addon_init,
};

static const sonix_addon_stop_t tictactoe_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = ttt_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &tictactoe : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &tictactoe_lifecycle : NULL;
}
