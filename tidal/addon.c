#include "src/addon/addon.h"

#include <stdio.h>

#include "common/addonfs.h"

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
	// host->dir (tidalconf.h), and the cache is the addon's own directory there
	// too now -- <addon>/cache, see tidalcache.h.
	tidalconf_init(host->dir, host->sd_root);

	// The move out of the .local/ tree the player owns is attempted before the
	// folder is made: addon_migrate_path() reads an existing destination as
	// "already migrated" (addonfs.h), and the empty directory addon_cache_dir()
	// would have just created looks exactly like a finished move. The old name
	// comes from tidalcache.h and the leaf is the "cache" addonfs.h makes, so
	// the directory this renames into is the one the cache opens later.
	char cache[ADDON_PATH_MAX];
	int len = snprintf(cache, sizeof(cache), "%s/" TIDALCACHE_DIRNAME, host->dir);
	if (len > 0 && (size_t)len < sizeof(cache) && host->sd_root && host->sd_root[0]) {
		char old[ADDON_PATH_MAX];
		snprintf(old, sizeof(old), "%s/" TIDALCACHE_DIR_OLD, host->sd_root);
		addon_migrate_path(old, cache);
	}

	if (addon_cache_dir(host, cache, sizeof(cache))) {
		tidalcache_set_addon_dir(host->dir, host->sd_root);
	} else {
		// A card that will not hold the folder -- read-only, or one that has
		// just gone -- leaves Tidal with no cache rather than with a cache
		// somewhere else: every track it plays is a file in there, so it will
		// say so when a track is asked for instead of writing into the player's
		// tree.
		fprintf(stderr, "tidal: no cache folder on the card, nothing can be played\n");
	}
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
