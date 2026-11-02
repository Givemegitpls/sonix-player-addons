#include "src/addon/addon.h"

#include "common/addonfs.h"

#include "src/gui/streaming/zvukpage.h"
#include "src/system/streaming/streamturn.h"
#include "src/system/streaming/zvuk.h"
#include "src/system/streaming/zvukcache.h"
#include "src/system/streaming/zvukconf.h"
#include "src/system/streaming/zvukdl.h"
#include "src/system/streaming/zvukplay.h"
#include "src/system/streaming/zvukservice.h"
#include "src/system/streaming/zvuksync.h"

// Zvuk (zvuk.com), as a service addon.
//
// The system half of the service came from the rekkod-mod tree
// (Rekkod-Tree/sonix-player-rekkod-mod, GPLv3, as this tree), where it was six
// modules of the binary: zvuk.c (the API), zvukcache.c (the download cache),
// zvukdl.c (saving into the library), zvukplay.c (the queue), zvuklist.c (a long
// list's ids and their pages) and zvuksync.c (the writes). The page is
// zvukpage.c, the same page ported off Graphite 2 onto this player's widgets
// (docs/ADDONS.md, "Porting a service page").
//
// Three entry points, which is what being a service costs:
//
//   sonix_addon_entry()          the page: init() runs on the first tap and
//                                returns the screen the shell switches to.
//   sonix_addon_service_entry()  the table the player asks about files it did
//                                not create (zvukservice.c).
//   sonix_addon_stop_entry()     the other end of init(), for the unload the
//                                card made necessary (src/addon/addon.h, "the
//                                stop half"). Its bool is whether every thread
//                                of this library is out.
//
// What the player answers *before* this library is opened -- is this path
// Zvuk's, may the device sleep, is there a cache to empty -- comes from the
// manifest's owns_paths/cache_path, not from here.
//
// Keys and the token: Zvuk needs none of the firmware's keys, only the user's
// own token, which the user copies out of zvuk.com/api/tiny/profile into
// token.txt in this addon's folder. The addon reads that file itself (there is
// no cardimport here) and keeps the session it gets beside it; its settings --
// the address, the User-Agent, the quality ceiling -- are
// <card>/Addons/zvuk/settings.ini. Nothing of it is in device_config.ini.
//
// The rule for everything below: an addon writes under its own folder and
// nowhere else -- settings beside addon.so, caches and indexes under it too.
// What earlier builds left at the card's root and in the card's shared ".local"
// tree is moved in once, at init() (zvuk_migrate_card_files). The saved music
// is the exception that proves it: <music>/Zvuk/ is the user's library, not
// this addon's storage, and it stays where the scanner finds it.

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
static void zvuk_migrate_card_files(const sonix_addon_host_t *host) {
	if (!host || !host->dir || !host->dir[0] || !host->sd_root || !host->sd_root[0]) {
		return;
	}
	char old_path[512], new_path[512];

	snprintf(old_path, sizeof(old_path), "%s/zvuk_token.txt", host->sd_root);
	snprintf(new_path, sizeof(new_path), "%s/%s", host->dir, ZVUK_TOKEN_FILE);
	addon_migrate_path(old_path, new_path);

	snprintf(old_path, sizeof(old_path), "%s/.local/zvuk-saved.tsv", host->sd_root);
	snprintf(new_path, sizeof(new_path), "%s/%s", host->dir, ZVUKDL_INDEX);
	addon_migrate_path(old_path, new_path);

	snprintf(old_path, sizeof(old_path), "%s/.local/zvuk-cache", host->sd_root);
	snprintf(new_path, sizeof(new_path), "%s/cache", host->dir);
	addon_migrate_path(old_path, new_path);
}

static lv_obj_t *zvuk_addon_init(const sonix_addon_host_t *host) {
	// The card first: the settings and the session are read from it, and so is
	// everything the cache and the library index do.
	zvukconf_init(host->dir);

	// Before any module opens a file, the ones an earlier build left elsewhere
	// on the card are brought into this folder.
	zvuk_migrate_card_files(host);

	// The cache lives in <addon>/cache, made here. A card that refuses it (a
	// read-only one, or one that has gone) leaves it NULL: tracks then stream
	// and are not kept, rather than landing somewhere that is not ours.
	char cache[512];
	bool have_cache = addon_cache_dir(host, cache, sizeof(cache));
	zvukcache_set_addon_dir(have_cache ? cache : NULL);
	zvukcache_set_root(host->sd_root);
	zvukdl_set_addon_dir(host->dir);
	zvukdl_set_root(host->sd_root);
	zvuk_init();

	// The prepare callback: the player asks before it starts a file in Zvuk's
	// cache, and the answer is "not yet, it is still coming down". The page
	// calls this again with its own listener; the prepare callback is the same
	// one either way, and device_state refuses a second copy.
	zvukplay_init(NULL);

	// The page. It fills in the two interface objects the service table carries
	// (the mark over the artwork, "show album") itself, at the end of its own
	// init, and registers every screen it builds with the shell as it makes it.
	zvukpage_init(host->cfg, host->id, host->sd_root, host->dir);

	return zvukpage_screen;
}

static const sonix_addon_t zvuk_addon = {
	.api = SONIX_ADDON_API,
	.title = "Zvuk",
	.init = zvuk_addon_init,
};

// The unload: everything this library starts has to be gone before the mapping
// goes, and the order is the dependency one, from the page down.
//
// Only the module boundaries are here; what each module starts and how it ends
// is that module's own zvuk*_stop(), beside the code it stops. The answer is the
// slowest of them: false when any module's bounded wait ran out with a thread
// still inside the library, which tells the shell not to dlclose() (addon.h).
static bool zvuk_stop(void) {
	// The network first, and the shared download turn with it. A download
	// thread parked in streamturn_acquire() only leaves once it has the turn,
	// and the turn may be held by another service's track; asking everyone to
	// abandon is what makes the waits below bounded in that case rather than
	// the length of somebody else's album.
	streamturn_abandon_all();

	// Every stop runs even after one of them has said no: each still has to
	// withdraw its own hooks and close its own files, whatever is left running.
	bool safe = zvukpage_stop(); // the page's worker, its sign-in, the listrow timer
	safe = zvuksync_stop() && safe;
	safe = zvukplay_stop() && safe; // the watch timer, the prepare callback
	safe = zvukdl_stop() && safe;	// the save worker, its queue, the saved index
	safe = zvukcache_stop() && safe; // the abandon hook, the download

	// The session and the mirrors of the collection: nothing is left for init()
	// to inherit, and there is no thread here to wait for.
	zvuk_release();
	return safe;
}

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &zvuk_addon : NULL;
}

const sonix_addon_service_t *sonix_addon_service_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? zvuk_service_table() : NULL;
}

static const sonix_addon_stop_t zvuk_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = zvuk_stop,
};

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &zvuk_lifecycle : NULL;
}
