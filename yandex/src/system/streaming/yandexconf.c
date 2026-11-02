#include "yandexconf.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "common/store.h"

#include "src/system/core/config.h"

// See yandexconf.h.

#define YANDEXCONF_LEGACY_SECTION "yandex"

// The four the service cannot work without. The order is the one the screen
// shows them in.
static const char *const KEYS[] = {"client_id", "client_secret", "sign_key", "client_header"};
#define KEY_COUNT ((int)(sizeof(KEYS) / sizeof(KEYS[0])))

static pthread_mutex_t conf_lock = PTHREAD_MUTEX_INITIALIZER;
static store_t settings;
static char conf_dir[STORE_PATH_MAX];
static bool loaded;

static void load_locked(void) {
	settings = (store_t){0};
	loaded = false;
	if (!conf_dir[0]) {
		return;
	}
	char path[STORE_PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/settings.ini", conf_dir) >= (int)sizeof(path)) {
		return;
	}
	store_init(&settings, path);
	loaded = true;
	printf("yandex: settings %s (%s)\n", path, store_load(&settings) ? "read" : "new");
}

void yandexconf_init(const char *dir) {
	pthread_mutex_lock(&conf_lock);
	conf_dir[0] = '\0';
	if (dir && dir[0]) {
		snprintf(conf_dir, sizeof(conf_dir), "%s", dir);
	}
	load_locked();
	pthread_mutex_unlock(&conf_lock);
}

void yandexconf_reload(void) {
	pthread_mutex_lock(&conf_lock);
	load_locked();
	pthread_mutex_unlock(&conf_lock);
}

// Call with conf_lock held. The key's value from the card's file, or the old
// [yandex] section read once and written here. `out` gets a copy either way.
static void get_locked(const char *key, const char *fallback, char *out, size_t size) {
	const char *v = loaded ? store_get(&settings, key) : "";
	if (v[0]) {
		snprintf(out, size, "%s", v);
		return;
	}
	// Not in the addon's file: what the player kept before the move. Read once,
	// then it belongs to the card. The keys were sealed in streaming-keys.bin
	// rather than kept here, so for them this finds nothing and the fallback --
	// empty -- stands.
	const char *legacy = config_get(YANDEXCONF_LEGACY_SECTION, key, "");
	if (legacy && legacy[0]) {
		snprintf(out, size, "%s", legacy);
		if (loaded) {
			store_set(&settings, key, legacy);
			store_save(&settings);
			printf("yandex: [%s] %s moved to the addon's settings\n", YANDEXCONF_LEGACY_SECTION, key);
		}
		return;
	}
	snprintf(out, size, "%s", fallback ? fallback : "");
}

void yandexconf_copy(const char *key, const char *fallback, char *out, size_t size) {
	if (!key || !out || size == 0) {
		return;
	}
	pthread_mutex_lock(&conf_lock);
	get_locked(key, fallback, out, size);
	pthread_mutex_unlock(&conf_lock);
}

bool yandexconf_key(const char *key, char *out, size_t size) {
	if (!key || !out || size == 0) {
		return false;
	}
	yandexconf_copy(key, "", out, size);
	return out[0] != '\0';
}

bool yandexconf_have_keys(void) {
	pthread_mutex_lock(&conf_lock);
	bool ok = true;
	for (int i = 0; i < KEY_COUNT && ok; i++) {
		// Never the old section for these: a card that has no keys is a card
		// the owner has not filled in yet, and saying "keys present" because a
		// stale line is somewhere else would be a lie.
		const char *v = loaded ? store_get(&settings, KEYS[i]) : "";
		ok = v[0] != '\0';
	}
	pthread_mutex_unlock(&conf_lock);
	return ok;
}

void yandexconf_set(const char *key, const char *value) {
	if (!key) {
		return;
	}
	pthread_mutex_lock(&conf_lock);
	if (loaded) {
		store_set(&settings, key, value ? value : "");
		if (!store_save(&settings)) {
			fprintf(stderr, "yandex: cannot write %s\n", settings.path);
		}
	}
	pthread_mutex_unlock(&conf_lock);
}

const char *yandexconf_dir(void) { return conf_dir; }

// The addon being unloaded: the folder and the settings read out of it are
// forgotten, and nothing is written. The file itself stays on the card -- it is
// the user's, and the next init() reads it again -- so this is not a "reset".
void yandexconf_stop(void) {
	pthread_mutex_lock(&conf_lock);
	conf_dir[0] = '\0';
	settings = (store_t){0};
	loaded = false;
	pthread_mutex_unlock(&conf_lock);
}

bool yandexconf_path(const char *name, char *out, size_t size) {
	if (!name || !out || size == 0) {
		return false;
	}
	out[0] = '\0';
	if (!conf_dir[0]) {
		return false;
	}
	return snprintf(out, size, "%s/%s", conf_dir, name) < (int)size;
}
