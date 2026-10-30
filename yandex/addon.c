#include "src/addon/addon.h"

#include "src/gui/streaming/yandexstate.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"
#include "src/system/streaming/yandexconf.h"
#include "src/system/streaming/yandexdl.h"
#include "src/system/streaming/yandexplay.h"
#include "src/system/streaming/yandexsaved.h"
#include "src/system/streaming/yandexservice.h"

// Yandex Music, as a service addon.
//
// The system half of the service came from the rekkod-mod tree
// (Rekkod-Tree/sonix-player-rekkod-mod, GPLv3, as this tree), where it was
// eight modules of the binary: yandex.c (the API, the session, My Wave), yandexcache.c
// (the download cache), yandexdl.c (saving into the library), yandexplay.c (the
// queue), yandexlist.c (a long list's ids and their pages), yandexsaved.c (the
// index of what was saved), yandexsync.c (the likes) and yandexwave.c (the
// station's conversation). The page (yandexpage.c, Graphite 2) is not here yet:
// this stage is the service and a settings screen that shows the keys and the
// account, so the whole thing can be built and opened without one.
//
// Two entry points, which is what being a service costs:
//
//   sonix_addon_entry()          the page: init() runs on the first tap and
//                                returns the screen the shell switches to.
//   sonix_addon_service_entry()  the table the player asks about files it did
//                                not create (yandexservice.c).
//
// What the player answers *before* this library is opened -- is this path
// Yandex's, may the device sleep, is there a cache to empty -- comes from the
// manifest's owns_paths/cache_path, not from here.
//
// Keys and the token: the four application keys (client_id, client_secret,
// sign_key, client_header) and the two addresses are the addon's own, in
// <card>/Addons/yandex/settings.ini; in the fork they came out of
// streaming-keys.bin, which an addon cannot open. The user's token is a file at
// the card's root, yandex_token.txt, which the addon reads itself (there is no
// cardimport here) and keeps the session it gets in its own folder. Nothing of
// it is in device_config.ini, and nothing is compiled in.

static lv_obj_t *yandex_addon_init(const sonix_addon_host_t *host) {
	// The card first: the settings, the keys and the session are read from it,
	// and so is everything the cache and the saved index do.
	yandexconf_init(host->dir);
	yandexcache_set_root(host->sd_root);
	yandexdl_set_root(host->sd_root);
	yandexsaved_set_root(host->sd_root);
	yandex_init();

	// The prepare callback: the player asks before it starts a file in Yandex's
	// cache, and the answer is "not yet, it is still coming down". No listener
	// yet -- the page that shows the events is the next stage, and it calls this
	// again with its own.
	yandexplay_init(NULL);

	// The page. It fills in the two interface objects the service table carries
	// (the mark over the artwork, "show album") when that page exists; for now
	// it is the keys-and-account screen and the table keeps them NULL.
	yandexstate_init(host->cfg, host->sd_root);

	return yandexstate_screen;
}

static const sonix_addon_t yandex_addon = {
	.api = SONIX_ADDON_API,
	.title = "Yandex Music",
	.init = yandex_addon_init,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &yandex_addon : NULL;
}

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? yandex_service_table() : NULL;
}
