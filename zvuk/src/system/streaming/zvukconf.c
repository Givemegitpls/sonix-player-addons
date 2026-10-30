#include "zvukconf.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common/store.h"

#include "src/system/core/config.h"

// See zvukconf.h.

#define ZVUKCONF_LEGACY_SECTION "zvuk"

static pthread_mutex_t conf_lock = PTHREAD_MUTEX_INITIALIZER;
static store_t settings;
static char conf_dir[STORE_PATH_MAX];
static bool loaded;

void zvukconf_init(const char *dir) {
	char path[STORE_PATH_MAX];
	pthread_mutex_lock(&conf_lock);
	settings = (store_t){0};
	conf_dir[0] = '\0';
	loaded = false;
	if (dir && dir[0] && snprintf(path, sizeof(path), "%s/settings.ini", dir) < (int)sizeof(path)) {
		store_init(&settings, path);
		loaded = true;
		snprintf(conf_dir, sizeof(conf_dir), "%s", dir);
		printf("zvuk: settings %s (%s)\n", path, store_load(&settings) ? "read" : "new");
	}
	pthread_mutex_unlock(&conf_lock);
}

// Call with conf_lock held. The key's value from the card's file, or the old
// [zvuk] section read once and written here. `out` gets a copy either way.
static void get_locked(const char *key, const char *fallback, char *out, size_t size) {
	const char *v = loaded ? store_get(&settings, key) : "";
	if (v[0]) {
		snprintf(out, size, "%s", v);
		return;
	}
	// Not in the addon's file: what the player kept before the move. Read once,
	// then it belongs to the card.
	const char *legacy = config_get(ZVUKCONF_LEGACY_SECTION, key, "");
	if (legacy && legacy[0]) {
		snprintf(out, size, "%s", legacy);
		if (loaded) {
			store_set(&settings, key, legacy);
			store_save(&settings);
			printf("zvuk: [%s] %s moved to the addon's settings\n", ZVUKCONF_LEGACY_SECTION, key);
		}
		return;
	}
	snprintf(out, size, "%s", fallback ? fallback : "");
}

void zvukconf_copy(const char *key, const char *fallback, char *out, size_t size) {
	if (!key || !out || size == 0) {
		return;
	}
	pthread_mutex_lock(&conf_lock);
	get_locked(key, fallback, out, size);
	pthread_mutex_unlock(&conf_lock);
}

void zvukconf_set(const char *key, const char *value) {
	if (!key) {
		return;
	}
	pthread_mutex_lock(&conf_lock);
	if (loaded) {
		store_set(&settings, key, value ? value : "");
		if (!store_save(&settings)) {
			fprintf(stderr, "zvuk: cannot write %s\n", settings.path);
		}
	}
	pthread_mutex_unlock(&conf_lock);
}

const char *zvukconf_dir(void) { return conf_dir; }

bool zvukconf_path(const char *name, char *out, size_t size) {
	if (!name || !out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!conf_dir[0]) {
		return false;
	}
	return snprintf(out, size, "%s/%s", conf_dir, name) < (int)size;
}
