#include "src/addon/addon.h"

#include "src/gui/streaming/tidalpage.h"
#include "src/system/streaming/tidal.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalconf.h"
#include "src/system/streaming/tidalservice.h"
#include "src/system/streaming/tidalsync.h"

// Tidal, as a service addon -- the same shape as addons/qobuz, which is the one
// to read first: the player knows nothing about either of them beyond the table
// in src/system/streaming/service.h.
//
// Two entry points: sonix_addon_entry() is the page (init() on the first tap),
// sonix_addon_service_entry() is the table the player asks about files it did
// not create. The loader registers the table as soon as the library is open,
// before init() runs.
//
// What the player answers before this library is opened -- is the path Tidal's,
// may the device sleep, is there a cache to empty -- comes from the manifest's
// owns_paths/cache_path.
//
// Tidal's playlists are addressed by UUID rather than Qobuz's numbers, which is
// why the service table's playlist id is text; nothing else differs.
//
// And a third entry point, sonix_addon_stop_entry(): the stop hook is the price
// of being unloadable. A dlopen()ed .so is a mapping of a file on the card, and
// a mapping keeps the filesystem alive after the card is pulled; addon.h says
// what stop() owes the player, and it is obeyed in the modules this file calls
// -- the page's workers, the cache's download thread and the account's queue are
// all told to leave and waited for.

static lv_obj_t *tidal_addon_init(const sonix_addon_host_t *host) {
	// The card first: the keys and the session are the addon's own files in
	// host->dir (tidalconf.h), which is also where the old place they came from
	// is migrated once, and so is everything the cache does.
	tidalconf_init(host->dir, host->sd_root);
	tidalcache_set_root(host->sd_root);
	tidal_init();

	// The page builds its screens and fills in the two interface objects the
	// service table carries: the mark over the artwork and "show album". It is
	// given the host's name for this addon because every screen it builds is
	// registered with the shell under that name (sonix_addon_own_screen) -- that
	// list is what an unload takes off the display and deletes.
	tidalpage_init(host->cfg, host->id);

	return tidal_screen;
}

static const sonix_addon_t tidal_addon = {
	.api = SONIX_ADDON_API,
	.title = "Tidal",
	.init = tidal_addon_init,
};

// The way out, in the order the pieces depend on each other: the page first,
// because its prefetch worker is what starts downloads and what hands jobs to
// the sync queue; then the cache, whose download thread is the last thing
// running in this library; then the queue that writes to the account. Each
// hands back what the player remembers of it -- timers, hooks; the service
// table is the loader's own -- and leaves no thread behind (addon.h, "the stop
// half"). The card's files need no flushing: every write here is one file,
// closed before the call returns.
//
// All three are called even when one of them fails: an unload that leaves a
// worker running elsewhere in the library is still better off with the other
// two stopped, and the answer the loader wants is the conjunction.
static bool tidal_addon_stop(void) {
	bool page_gone = tidalpage_stop();
	bool cache_gone = tidalcache_stop();
	bool sync_gone = tidalsync_stop();
	return page_gone && cache_gone && sync_gone;
}

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &tidal_addon : NULL;
}

static const sonix_addon_stop_t tidal_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = tidal_addon_stop,
};

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &tidal_lifecycle : NULL;
}

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? tidal_service_table() : NULL;
}
