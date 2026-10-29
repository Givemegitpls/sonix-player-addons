#include "src/addon/addon.h"

#include "src/gui/streaming/tidalpage.h"
#include "src/system/streaming/tidal.h"
#include "src/system/streaming/tidalcache.h"
#include "src/system/streaming/tidalservice.h"

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

static lv_obj_t *tidal_addon_init(const sonix_addon_host_t *host) {
	// The card first: the session and the keys are read from it (streamkeys.h),
	// and so is everything the cache does.
	tidalcache_set_root(host->sd_root);
	tidal_init();

	// The page builds its screens and fills in the two interface objects the
	// service table carries: the mark over the artwork and "show album".
	tidalpage_init(host->cfg);

	return tidal_screen;
}

static const sonix_addon_t tidal_addon = {
	.api = SONIX_ADDON_API,
	.title = "Tidal",
	.init = tidal_addon_init,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &tidal_addon : NULL;
}

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? tidal_service_table() : NULL;
}
