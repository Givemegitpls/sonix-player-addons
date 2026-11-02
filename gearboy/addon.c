#include "src/addon/addon.h"

#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/gearboy/gearboyplay.h"
#include "src/gui/gearboy/gearboyscreens.h"
#include "src/gui/gearboy/gearboysettings.h"
#include "src/gui/nowplaying/player.h"
#include "src/gui/shell/switcher.h"
#include "src/system/gearboy/gbconf.h"
#include "src/system/gearboy/gbdb.h"

#include "common/addonfs.h"

#include <stdio.h>
#include <unistd.h>

// Gearboy as an addon.
//
// Three screens, as it had in the binary: the list of games on the card, the
// emulator itself, and its settings page. They are built on the first tap
// here instead of at start-up, which is the whole point of the move -- a
// player who never opens a game does not pay for the core, the database and
// the interface of an emulator.
//
// What the player used to hand it, and is handed here:
//
//   gbdb_set_root()   where Games/GB and Games/GBC are. main.c and system.c
//                     called it with the card's root; the host tells us the
//                     same path, and the cartridge database rescans it every
//                     time the list is opened, so a card that changed under
//                     the player needs nothing more.
//   gbdb_set_cache_dir()
//                     where the ROM index goes. That used to be a directory of
//                     its own in the shared .local/ tree; it is this addon's
//                     cache now, <addon>/cache (below).
//   panel_touch_*()   the touch panel, which the emulator takes over for
//                     multitouch (it reads the evdev node itself) and gives
//                     back on the way out. Those live in the player's main.c
//                     and are exported to the addons.
//
// The ROMs, the saves and the BIOS files are the user's own, on the card, and
// were never part of the binary. The saves stay in Games/Saves beside the ROMs
// for that reason: they belong to the game, not to the emulator, and a card
// that gets a newer Gearboy must find them where the user left them (gbdb.c,
// DIR_SAVES).

// Moves the ROM index this addon used to keep in <card>/.local/gearboy-cache
// into its own cache folder, once. The file is renamed, never copied, so a
// failed attempt (a read-only card, a card pulled mid-way) leaves the old one
// exactly as it was and the next start tries again; addon_migrate_path() logs
// what it could not do. The directory it leaves goes only when it is empty: a
// .local/ that something else writes to is not ours to tidy.
static void migrate_cache(const char *card_root, const char *cache_dir) {
	if (!card_root || !card_root[0]) {
		return;
	}

	char old_dir[ADDON_PATH_MAX];
	char old_file[ADDON_PATH_MAX + 16];
	char new_file[ADDON_PATH_MAX + 16];
	snprintf(old_dir, sizeof(old_dir), "%s/.local/gearboy-cache", card_root);
	snprintf(old_file, sizeof(old_file), "%s/index.tsv", old_dir);
	snprintf(new_file, sizeof(new_file), "%s/index.tsv", cache_dir);

	if (addon_migrate_path(old_file, new_file)) {
		(void)rmdir(old_dir);
	}
}

static lv_obj_t *gearboy_addon_init(const sonix_addon_host_t *host) {
	// The name the loader knows this addon by, for the screens it is told about
	// (sonix_addon_own_screen): the host structure is the player's and is filled
	// in again for the next addon, so it is copied rather than remembered.
	gearboyscreens_init(host->id);

	// Where the emulator looks for the games, and where it keeps what belongs to
	// the game rather than to the device: its own settings in the addon's folder
	// on the card, and the cartridges' saves in Games/Saves beside the ROMs.
	gbdb_set_root(host->sd_root);
	gbconf_init(host->dir);

	// The ROM index is the addon's own file, so it lives in the addon's own
	// folder: <addon>/cache, the place every addon's caches go (addonfs.h).
	// Without the directory -- a read-only card, or one that has just gone --
	// there is simply no index: the list still scans, it just reads each ROM
	// again on every start instead of remembering its CRC.
	char cache_dir[ADDON_PATH_MAX];
	if (addon_cache_dir(host, cache_dir, sizeof(cache_dir))) {
		migrate_cache(host->sd_root, cache_dir);
		gbdb_set_cache_dir(cache_dir);
	} else {
		fprintf(stderr, "gearboy: no cache folder on this card; the ROMs are read again on every start\n");
	}

	// The emulator's own screens first: the list leads to them, and both are
	// built by their init() before anything can be tapped.
	gearboyplay_screen = lv_obj_create(NULL);
	gearboysettings_screen = lv_obj_create(NULL);
	gearboypage_screen = lv_obj_create(NULL);

	gearboyplay_init(host->cfg);
	gearboysettings_init(host->cfg);
	gearboypage_init(host->cfg);

	// The page the tile opens is a browsing page like the ones in the shell: it
	// takes the swipe back (gearboypage_init() attaches that itself) and the
	// player can be pulled in from it.
	player_sheet_attach_drag(gearboypage_screen, true);

	// The screen the tile opens. The shell also knows it by return value; said
	// from here as well so that every screen this addon puts up is on the
	// loader's list (addon.h, sonix_addon_own_screen). The other two are
	// reported where they are opened -- gearboyplay_open() and
	// gearboysettings_open() -- because a list the loader writes from init()'s
	// return value does not keep what init() reported before it returned.
	gearboyscreens_own(gearboypage_screen);

	return gearboypage_screen;
}

// Deletes one of the screens this addon made. The loader deletes every screen
// registered with it after stop() and checks gui_obj_alive() before each, so
// this is both the fallback for one it was never told about and the reason the
// addon's own pointers are gone by the time the next init() runs.
//
// False while the screen is on the display: lv_obj_delete() of the active screen
// leaves the display without one at all (lv_obj_tree.c), and a page whose
// callbacks are about to be unmapped must not be left up either -- so the caller
// keeps the library mapped instead.
static bool drop_own_screen(lv_obj_t **screen, const char *what) {
	lv_obj_t *s = *screen;
	*screen = NULL;
	if (!s) {
		return true;
	}

	if (lv_screen_active() == s) {
		fprintf(stderr, "gearboy: %s is still on the display; not safe to unload\n", what);
		return false;
	}

	switcher_forget_screen(s);
	player_forget_screen(s);
	lv_obj_delete(s);
	return true;
}

// The library is going away (`docs/ADDONS.md`, addon.h). In the order the parts
// depend on each other: the game first (its thread writes the framebuffer, and
// the touch reader feeds it), then the two pages that are only state, then the
// screens.
//
// Nothing on the card is open between calls -- gbconf and gbdb read their files
// and close them (addons/common/store.c) -- so there is no descriptor to close
// here; the .sav of a running game is written by gearboy_stop(), inside the
// emulator thread, before it returns.
//
// True when every thread is out of the library. The one wait that can give up is
// the scan's (gearboypage_stop(), bounded); the rest are joins, and a page left
// on the display is the other case the loader must not unmap under.
static bool gearboy_addon_stop(void) {
	bool safe = true;

	// The emulator thread joined, the touch reader joined, the page's two timers
	// deleted and its theme hook withdrawn.
	if (!gearboyplay_stop()) {
		safe = false;
	}

	// The settings page's hook and pointers, then the list's scan thread (waited
	// for, bounded), its catalogue and its hook.
	gearboysettings_stop();
	if (!gearboypage_stop()) {
		safe = false;
	}

	// The two screens that are this addon's. gearboypage_screen is not one of
	// them: init() returned it and the loader deletes it after this returns.
	if (!drop_own_screen(&gearboyplay_screen, "the game screen")) {
		safe = false;
	}
	if (!drop_own_screen(&gearboysettings_screen, "the settings screen")) {
		safe = false;
	}

	return safe;
}

static const sonix_addon_t gearboy = {
	.api = SONIX_ADDON_API,
	.title = "Gearboy",
	.init = gearboy_addon_init,
};

static const sonix_addon_stop_t gearboy_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = gearboy_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &gearboy : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &gearboy_lifecycle : NULL;
}
