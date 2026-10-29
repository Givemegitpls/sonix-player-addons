#include "gbconf.h"

#include <pthread.h>
#include <stdio.h>

#include "common/store.h"
#include "src/system/core/config.h"

// The settings page writes these from the UI thread and the emulator's own
// thread reads them when a game starts, so the store is behind a lock. It is
// held for the length of a string compare -- the file write happens with it
// taken too, because store_save() rewrites the whole file from the struct.
static store_t settings;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool ready;

void gbconf_init(const char *addon_dir) {
	char path[STORE_PATH_MAX];
	snprintf(path, sizeof(path), "%s/settings.ini", addon_dir && addon_dir[0] ? addon_dir : "");

	pthread_mutex_lock(&lock);
	store_init(&settings, path);
	bool loaded = store_load(&settings); // false on a fresh card, which is not an error
	ready = true;
	pthread_mutex_unlock(&lock);

	printf("gearboy: settings %s (%s)\n", path, loaded ? "read" : "new");

}

int gbconf_get_int(const char *key, int fallback) {
	pthread_mutex_lock(&lock);
	int value = fallback;
	if (ready && store_get(&settings, key)[0]) {
		value = store_get_int(&settings, key, fallback);
	} else if (ready) {
		// Nothing on the card yet: what the player remembered before the
		// emulator moved out of it. config_get_int() answers with the same
		// fallback when the section is gone.
		value = (int)config_get_int("gearboy", key, fallback);
	}
	pthread_mutex_unlock(&lock);
	return value;
}

void gbconf_set_int(const char *key, int value) {
	pthread_mutex_lock(&lock);
	if (ready) {
		store_set_int(&settings, key, value);
		store_save(&settings);
	}
	pthread_mutex_unlock(&lock);
}
