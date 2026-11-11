#include "gstate.h"

#include <stdio.h>

#include "common/store.h"

// One number, written when a run ends and read when the page comes up: both on
// the UI thread, so no lock.
static store_t state;
static bool ready;

void gstate_init(const char *addon_dir) {
	char path[STORE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/settings.ini", addon_dir && addon_dir[0] ? addon_dir : "");
	store_init(&state, path);
	bool loaded = store_load(&state); // false on a card that has never played
	ready = true;

	printf("2048: settings %s (%s)\n", path, loaded ? "read" : "new");
}

int gstate_get_int(const char *key, int fallback) {
	if (!ready) {
		return fallback;
	}
	return store_get_int(&state, key, fallback);
}

void gstate_set_int(const char *key, int value) {
	if (!ready) {
		return;
	}
	store_set_int(&state, key, value);
	store_save(&state);
}
