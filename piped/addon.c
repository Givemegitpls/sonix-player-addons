#include "src/addon/addon.h"

#include <stdio.h>

#include "common/addonfs.h"

#include "src/gui/ppage.h"
#include "src/system/gw/pgw.h"
#include "src/system/play/pplay.h"

// Piped, as a service addon: YouTube audio through a Piped instance, played from
// a file
// on the card.
//
// Three entry points, which is what being a service costs:
//
//   sonix_addon_entry()          the page: init() runs on the first tap and
//                                returns the screen the shell switches to;
//                                stop() runs when the library is unloaded.
//   sonix_addon_service_entry()  the table the player asks about files it did
//                                not create (the star, the sleep timer, USB,
//                                a factory reset).
//   sonix_addon_stop_entry()     how this one leaves (addon.h, "Unloading").
//
// What the player answers before this library is ever opened comes from the
// manifest's owns_paths/cache_path, not from here.

static lv_obj_t *piped_addon_init(const sonix_addon_host_t *host) {
	// The card first: settings and the cache are the addon's own files in
	// host->dir (common/addonfs.h), and nothing can be played without them.
	char cache[512];
	if (!addon_cache_dir(host, cache, sizeof(cache))) {
		fprintf(stderr, "piped: no cache folder on the card, nothing can be played\n");
	}

	pgw_init(host->dir, host->sd_root);
	pplay_init(host->dir, host->sd_root);
	ppage_init(host->cfg, host->id);

	return p_screen;
}

// The page first: its worker is the one that can be parked in an HTTP request,
// and a download in progress is abandoned from inside pplay_stop() so that the
// wait finds it on its way out.
static bool piped_addon_stop(void) {
	bool page_ok = ppage_stop();
	bool play_ok = pplay_stop();
	pgw_forget();
	return page_ok && play_ok;
}

static const sonix_addon_t piped_addon = {
	.api = SONIX_ADDON_API,
	.title = "Piped",
	.init = piped_addon_init,
};

static const sonix_addon_stop_t piped_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = piped_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &piped_addon : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &piped_lifecycle : NULL;
}

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? pplay_service_table() : NULL;
}
