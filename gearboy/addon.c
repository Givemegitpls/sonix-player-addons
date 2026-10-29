#include "src/addon/addon.h"

#include "src/gui/gearboy/gearboypage.h"
#include "src/gui/gearboy/gearboyplay.h"
#include "src/gui/gearboy/gearboysettings.h"
#include "src/gui/nowplaying/player.h"
#include "src/system/gearboy/gbconf.h"
#include "src/system/gearboy/gbdb.h"

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
//   panel_touch_*()   the touch panel, which the emulator takes over for
//                     multitouch (it reads the evdev node itself) and gives
//                     back on the way out. Those live in the player's main.c
//                     and are exported to the addons.
//
// The ROMs, the saves and the BIOS files are the user's own, on the card, and
// were never part of the binary.

static lv_obj_t *gearboy_addon_init(const sonix_addon_host_t *host) {
	// Where the emulator looks for the games, and where it keeps what belongs to
	// the game rather than to the device: its own settings in the addon's folder
	// on the card, and the cartridges' saves in Games/Saves beside the ROMs.
	gbdb_set_root(host->sd_root);
	gbconf_init(host->dir);

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

	return gearboypage_screen;
}

static const sonix_addon_t gearboy = {
	.api = SONIX_ADDON_API,
	.title = "Gearboy",
	.init = gearboy_addon_init,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &gearboy : NULL;
}
