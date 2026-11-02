#include "src/addon/addon.h"

#include "common/addonfs.h"

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
// station's conversation). The page is this stage's own (src/gui/streaming/
// yandexstate.c: a home of sections, one list screen and a search), written for
// this player rather than ported from Graphite 2.
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
// streaming-keys.bin, which an addon cannot open. The user's token is a file in
// the same folder, token.txt, which the addon reads itself (there is no
// cardimport here) and keeps the session it gets beside it. Nothing of it is in
// device_config.ini, and nothing is compiled in.
//
// The rule for everything below: an addon writes under its own folder and
// nowhere else -- settings beside addon.so, caches and indexes under it too.
// What earlier builds left at the card's root and in the card's shared ".local"
// tree is moved in once, at init() (yandex_migrate_card_files). The saved music
// is the exception that proves it: <music>/Yandex Music/ is the user's library,
// not this addon's storage, and it stays where the scanner finds it.

// The id the host gave this library (sonix_addon_host_t::id): the shell keeps
// screens by it, so every sonix_addon_own_screen() call needs it. The manifest
// owns the string and outlives the library, so the pointer is kept as it came.
static const char *yandex_addon_id;

// The three files this addon used to keep outside its own folder: the token at
// the card's root, the index of what was saved and the audio cache in the
// card's shared ".local" tree -- which is the player's, and which a factory
// reset, the USB export and the card watcher each had to guess about.
//
// One-shot and conservative: addon_migrate_path() renames rather than copies,
// does nothing when the new path is already there (the move is done) or the old
// one is not (there was nothing to move), and on a failure -- a read-only card,
// a card just pulled -- logs and leaves both alone, so the next start tries
// again. A missing token is the normal case, not a failure: it is how a user
// signs in, and the addon runs without one.
//
// The cache's directory is moved before addon_cache_dir() creates it: an empty
// directory at the new path would read as "already migrated" and the move would
// never happen.
static void yandex_migrate_card_files(const sonix_addon_host_t *host) {
	if (!host || !host->dir || !host->dir[0] || !host->sd_root || !host->sd_root[0]) {
		return;
	}
	char old_path[512], new_path[512];

	snprintf(old_path, sizeof(old_path), "%s/yandex_token.txt", host->sd_root);
	snprintf(new_path, sizeof(new_path), "%s/%s", host->dir, YANDEX_TOKEN_FILE);
	addon_migrate_path(old_path, new_path);

	snprintf(old_path, sizeof(old_path), "%s/.local/yandex-saved.tsv", host->sd_root);
	snprintf(new_path, sizeof(new_path), "%s/%s", host->dir, YANDEXSAVED_FILE);
	addon_migrate_path(old_path, new_path);

	snprintf(old_path, sizeof(old_path), "%s/.local/yandex-cache", host->sd_root);
	snprintf(new_path, sizeof(new_path), "%s/cache", host->dir);
	addon_migrate_path(old_path, new_path);
}

static lv_obj_t *yandex_addon_init(const sonix_addon_host_t *host) {
	yandex_addon_id = host->id;

	// The card first: the settings, the keys and the session are read from it,
	// and so is everything the cache and the saved index do.
	yandexconf_init(host->dir);

	// Before any module opens a file, the ones an earlier build left elsewhere
	// on the card are brought into this folder.
	yandex_migrate_card_files(host);

	// The cache lives in <addon>/cache, made here. A card that refuses it (a
	// read-only one, or one that has gone) leaves it NULL: tracks then stream
	// and are not kept, rather than landing somewhere that is not ours.
	char cache[512];
	bool have_cache = addon_cache_dir(host, cache, sizeof(cache));
	yandexcache_set_addon_dir(have_cache ? cache : NULL);
	yandexcache_set_root(host->sd_root);
	yandexdl_set_root(host->sd_root);
	yandexsaved_set_addon_dir(host->dir);
	yandexsaved_set_root(host->sd_root);
	yandex_init();

	// The prepare callback: the player asks before it starts a file in Yandex's
	// cache, and the answer is "not yet, it is still coming down". The page
	// registers again, with the listener that shows playback's events.
	yandexplay_init(NULL);

	// The page: the home of sections, the lists behind them and the search. It
	// also fills the two interface objects the service table carries (the mark
	// over the artwork, "show album"), which is what
	// yandex_service_attach_gui() is for.
	yandexstate_init(host->cfg, host->sd_root, host->dir, yandex_addon_id);

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
	safe = yandexlive_wait_all("the addon", YANDEXLIVE_WAIT_MS) && safe;

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
