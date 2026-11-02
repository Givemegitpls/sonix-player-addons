#include "src/addon/addon.h"

#include "src/gui/streaming/yandexstate.h"
#include "src/system/net/crypto.h"
#include "src/system/net/openssl_runtime.h"
#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexcache.h"
#include "src/system/streaming/yandexconf.h"
#include "src/system/streaming/yandexdl.h"
#include "src/system/streaming/yandexlive.h"
#include "src/system/streaming/yandexplay.h"
#include "src/system/streaming/yandexsaved.h"
#include "src/system/streaming/yandexservice.h"
#include "src/system/streaming/yandexsync.h"
#include "src/system/streaming/yandexwave.h"

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
// and one stop hook, sonix_addon_stop_entry(): the player unmaps the library
// when the card goes or when a different addon.so arrives in its folder, so
// every thread, timer, player callback and file handle has to be given up
// first. See yandex_addon_stop() below.
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

// The id the host gave this library (sonix_addon_host_t::id): the shell keeps
// screens by it, so every sonix_addon_own_screen() call needs it. The manifest
// owns the string and outlives the library, so the pointer is kept as it came.
static const char *yandex_addon_id;

static lv_obj_t *yandex_addon_init(const sonix_addon_host_t *host) {
	yandex_addon_id = host->id;

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
	yandexstate_init(host->cfg, host->sd_root, yandex_addon_id);

	return yandexstate_screen;
}

// The library leaving the process: the card was pulled, or a different
// addon.so arrived in the addon's folder and the player is replacing this one.
// addon.h's contract is that nothing of this library is still running when this
// returns -- the mapping is dlclose()d right after -- and that init() works
// again on the copy that replaces it.
//
// The answer is the contract's other half: false means a thread was still inside
// the library when its wait ran out (a socket http.h cannot be woken out of),
// and the player keeps the mapping rather than dlclose() it. Every module's
// answer is collected, and no module is skipped for a previous one's: the state
// below has to be dropped either way, or a library kept mapped would be one
// whose half-torn state the next tap reads.
//
// The order below is the order of who calls whom, not a preference:
//
//   the page   its two LVGL timers and the one-request workers it started. With
//              those gone, nothing on the GUI thread calls a module below.
//   play       the prepare callback off device_state, its worker ended and the
//              queue dropped. It is the module that starts downloads.
//   wave, save, the three other workers, each told to finish and counted out:
//   sync        they write into yandex.c's session, the liked mirror and the
//               saved index, which are freed further down.
//   cache      the per-track download thread (abandoned by the two above), and
//              the streamturn hook, taken back once no download can be
//              abandoned through it.
//   the rest   what is left in memory, module by module: nothing in it runs.
//   OpenSSL    the handles this library opened, once the last AES has returned.
static bool yandex_addon_stop(void) {
	// Nothing new goes out from this moment, whatever thread is still in the
	// middle of a list: a page is a request, and stop() has seconds, not minutes.
	yandex_halt_requests();

	bool safe = yandexstate_stop();
	safe = yandexplay_stop() && safe;
	safe = yandexwave_stop() && safe;
	safe = yandexdl_stop() && safe;
	safe = yandexsync_stop() && safe;
	safe = yandexcache_stop() && safe;

	// One last look at the count: a thread that outlived its own module's wait
	// is still inside this library, and everything below frees memory it reads.
	// The log line names it, and its answer is what the player acts on.
	safe = yandexlive_wait("the addon", YANDEXLIVE_WAIT_MS) && safe;

	yandex_stop();
	yandexsaved_stop();
	yandexconf_stop();

	// The EVP table first, then the libraries it was resolved from: the other
	// order would leave pointers into a dlclose()d libcrypto for the next load
	// to call. Both handles are this addon's own -- it dlopen()ed them, and the
	// player's tls.c holds its own -- so closing them takes nothing away from
	// the player.
	crypto_forget();
	openssl_runtime_close();

	yandex_addon_id = NULL;
	return safe;
}

static const sonix_addon_t yandex_addon = {
	.api = SONIX_ADDON_API,
	.title = "Yandex Music",
	.init = yandex_addon_init,
};

static const sonix_addon_stop_t yandex_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = yandex_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &yandex_addon : NULL;
}

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? yandex_service_table() : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &yandex_lifecycle : NULL;
}
