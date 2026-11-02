#include "src/addon/addon.h"

#include "src/gui/streaming/qobuzpage.h"
#include "src/system/streaming/qobuz.h"
#include "src/system/streaming/qobuzcache.h"
#include "src/system/streaming/qobuzconf.h"
#include "src/system/streaming/qobuzservice.h"
#include "src/system/streaming/qobuzsync.h"

// Qobuz, as a service addon.
//
// It was a page and four modules of the binary (qobuz.c, qobuzcache.c,
// qobuzsync.c, qobuzkey.c and the page), and the rest of the player knew it by
// name: the star, the badge, the playlist picker, USB, the sleep timer and the
// four exit paths. That knowledge is the registry's now
// (src/system/streaming/service.h), and this addon is what fills it in.
//
// Three entry points, which is what being a service costs:
//
//   sonix_addon_entry()          the page: init() runs on the first tap and
//                                returns the screen the shell switches to;
//                                stop() runs when the library is unloaded.
//   sonix_addon_service_entry()  the table the player asks about files it did
//                                not create. The loader registers it as soon as
//                                the library is open, before init().
//   sonix_addon_stop_entry()     how this one leaves: the card can be pulled or
//                                an update copied over, and the library is
//                                dlclose()d then (addon.h, "Unloading").
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

	// The page builds its screens -- all five registered with the shell under
	// host->id as they are created -- and fills in the two interface objects the
	// service table carries: the mark over the artwork and "show album".
	qobuzpage_init(host->cfg, host->id);

	return qobuz_screen;
}

// The page first, and it brings the transfers down with it: its prefetch thread
// is parked in the shared download turn, and only an abandoned download lets it
// out -- so the cache is stopped from inside qobuzpage_stop(), before it waits
// for its own two threads. Then the account worker, which is the one thing here
// that can still be in the middle of an HTTP request, and last what the session
// held in memory.
//
// False when either wait found a thread still in the library: the shell then
// keeps the mapping instead of taking the code away under a running thread
// (addon.h, sonix_addon_stop_t::stop). Both are asked even when the first has
// already answered, so that neither is left half taken apart.
static bool qobuz_addon_stop(void) {
	bool page_ok = qobuzpage_stop();
	bool sync_ok = qobuzsync_stop();
	qobuz_forget();
	return page_ok && sync_ok;
}

static const sonix_addon_t qobuz_addon = {
	.api = SONIX_ADDON_API,
	.title = "Qobuz",
	.init = qobuz_addon_init,
};

static const sonix_addon_stop_t qobuz_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = qobuz_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &qobuz_addon : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &qobuz_lifecycle : NULL;
}

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? qobuz_service_table() : NULL;
}
