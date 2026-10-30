#include "src/addon/addon.h"

#include "src/gui/streaming/qobuzpage.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzconf.h"
#include "src/system/streaming/qobuzservice.h"

// Qobuz, as a service addon.
//
// It was a page and four modules of the binary (qobuz.c, qobuzcache.c,
// qobuzsync.c, qobuzkey.c and the page), and the rest of the player knew it by
// name: the star, the badge, the playlist picker, USB, the sleep timer and the
// four exit paths. That knowledge is the registry's now
// (src/system/streaming/service.h), and this addon is what fills it in.
//
// Two entry points, which is what being a service costs:
//
//   sonix_addon_entry()          the page: init() runs on the first tap and
//                                returns the screen the shell switches to.
//   sonix_addon_service_entry()  the table the player asks about files it did
//                                not create. The loader registers it as soon as
//                                the library is open, before init().
//
// What the player answers *before* this library is ever opened -- is this path
// Qobuz's, may the device sleep, is there a cache to empty -- comes from the
// manifest's owns_paths/cache_path, not from here.

static lv_obj_t *qobuz_addon_init(const sonix_addon_host_t *host) {
	// The card first: the keys and the session are the addon's own files in
	// host->dir (qobuzconf.h), which is also where the old place they came from
	// is migrated once, and so is everything the cache does.
	qobuzconf_init(host->dir, host->sd_root);
	qobuzcache_set_root(host->sd_root);
	qobuz_init();

	// The page builds its screens and fills in the two interface objects the
	// service table carries: the mark over the artwork and "show album".
	qobuzpage_init(host->cfg);

	return qobuz_screen;
}

static const sonix_addon_t qobuz_addon = {
	.api = SONIX_ADDON_API,
	.title = "Qobuz",
	.init = qobuz_addon_init,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &qobuz_addon : NULL;
}

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? qobuz_service_table() : NULL;
}
