#include "gstate.h"

#include <stdio.h>

#include "common/store.h"
#include "src/system/core/config.h"

// One number, written from the page's own timer when a run ends and read when
// the page comes up: both on the UI thread, so no lock.
static store_t state;
static bool ready;

void gstate_init(const char *addon_dir) {
	char path[STORE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/settings.ini", addon_dir && addon_dir[0] ? addon_dir : "");
	store_init(&state, path);
	bool loaded = store_load(&state); // false on a card that has never played
	ready = true;

	printf("dino: settings %s (%s)\n", path, loaded ? "read" : "new");
}

int gstate_get_int(const char *key, int fallback) {
	if (!ready) {
		return fallback;
	}
	if (store_get(&state, key)[0]) {
		return store_get_int(&state, key, fallback);
	}
	// Nothing here yet: the score the player's own configuration remembered
	// from before the game was an addon.
	return (int)config_get_int("dino", key, fallback);
}

void gstate_set_int(const char *key, int value) {
	if (!ready) {
		return;
	}
	store_set_int(&state, key, value);
	store_save(&state);
}
