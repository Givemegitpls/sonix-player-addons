#include "fstate.h"

#include <stdio.h>

#include "common/store.h"
#include "src/system/core/config.h"

// Two keys, one file, both touched on the UI thread (the tick timer and the tap
// handler are LVGL timers), so no lock is needed here -- unlike Gearboy's, whose
// settings are read from the emulator's own thread.
static store_t state;
static bool ready;

void fstate_init(const char *addon_dir) {
	char path[STORE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/state.ini", addon_dir && addon_dir[0] ? addon_dir : "");
	store_init(&state, path);
	bool loaded = store_load(&state); // false on a card that has never played: not an error
	ready = true;

	printf("flappybird: state %s (%s)\n", path, loaded ? "read" : "new");
}

int fstate_get_int(const char *key, int fallback) {
	if (!ready) {
		return fallback;
	}
	if (store_get(&state, key)[0]) {
		return store_get_int(&state, key, fallback);
	}
	// Nothing on the card yet: the score the player kept before the game moved
	// out of the binary.
	return (int)config_get_int("flappybird", key, fallback);
}

void fstate_set_int(const char *key, int value) {
	if (!ready) {
		return;
	}
	store_set_int(&state, key, value);
	store_save(&state);
}
