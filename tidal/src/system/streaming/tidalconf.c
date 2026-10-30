#include "tidalconf.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/store.h"

#include "src/system/core/json.h"
#include "src/system/streaming/streamkeys.h"

// See tidalconf.h.

#define SETTINGS_FILE "settings.ini"
#define SESSION_FILE "session.ini"

// The old place, as the player read it: <card>/sonix/streaming.json, written by
// the user, and <card>/sonix/streaming-session.json, written by the player.
#define LEGACY_DIR "sonix"
#define LEGACY_USER_FILE "streaming.json"
#define LEGACY_SESSION_FILE "streaming-session.json"
#define LEGACY_JSON_MAX 16384

// One line of session.ini: a key, " = ", a value of up to TIDALCONF_TOKEN_MAX.
#define SESSION_LINE_MAX (TIDALCONF_TOKEN_MAX + 64)

static pthread_mutex_t conf_lock = PTHREAD_MUTEX_INITIALIZER;
static store_t settings; // settings.ini
static char conf_dir[STORE_PATH_MAX];
static char card_root[STORE_PATH_MAX];
static bool loaded;

// One of the old JSON files, as far as Tidal is concerned.
typedef struct {
	char access_token[TIDALCONF_TOKEN_MAX];
	char refresh_token[TIDALCONF_TOKEN_MAX];
	char client_id[TIDALCONF_ID_MAX];
	char client_secret[TIDALCONF_SECRET_MAX];
	long expires_at;
} legacy_t;

// Not a plain memset: one on a buffer about to be freed may be dropped by the
// compiler.
static void wipe(void *p, size_t n) {
	volatile unsigned char *v = p;
	while (n--) {
		*v++ = 0;
	}
}

static void trim(char *s) {
	char *start = s;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != s) {
		memmove(s, start, strlen(start) + 1);
	}
	size_t len = strlen(s);
	while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\n' || s[len - 1] == '\r')) {
		s[--len] = '\0';
	}
}

static void take(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	char value[512];
	value[0] = '\0';
	if (obj < 0 || !json_obj_str(doc, obj, key, value, sizeof(value))) {
		return;
	}
	snprintf(out, size, "%s", value);
	wipe(value, sizeof(value));
}

// Reads one of the old files into `out`. A missing or broken file is not an
// error: it is a card that never had an account, and the caller then has
// nothing to migrate.
static void read_legacy(const char *path, legacy_t *out) {
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char *text = malloc(LEGACY_JSON_MAX + 1);
	if (!text) {
		fclose(f);
		return;
	}
	size_t len = fread(text, 1, LEGACY_JSON_MAX + 1, f);
	fclose(f);
	if (len > LEGACY_JSON_MAX) {
		printf("tidal: %s is over %d bytes, ignored\n", path, LEGACY_JSON_MAX);
		free(text);
		return;
	}
	text[len] = '\0';

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		printf("tidal: %s is not valid JSON, ignored\n", path);
		wipe(text, len);
		free(text);
		return;
	}

	int obj = json_get(&doc, json_root(&doc), "tidal");
	take(&doc, obj, "access_token", out->access_token, sizeof(out->access_token));
	take(&doc, obj, "refresh_token", out->refresh_token, sizeof(out->refresh_token));
	take(&doc, obj, "client_id", out->client_id, sizeof(out->client_id));
	take(&doc, obj, "client_secret", out->client_secret, sizeof(out->client_secret));
	long at = json_obj_long(&doc, obj, "expires_at", -1);
	if (at >= 0) {
		out->expires_at = at;
	}

	json_free(&doc);
	wipe(text, len);
	free(text);
	printf("tidal: %s read\n", path);
}

// Call with conf_lock held. The session file is not the store's: see
// tidalconf.h. Written whole under another name, flushed to the card and
// renamed over the old one, so a card pulled mid-write leaves the previous
// session rather than half of a new one.
static bool save_session_locked(const char *access_token, const char *refresh_token, long expires_at) {
	if (!conf_dir[0]) {
		return false;
	}
	char path[STORE_PATH_MAX];
	char tmp[STORE_PATH_MAX + 8];
	if (snprintf(path, sizeof(path), "%s/" SESSION_FILE, conf_dir) >= (int)sizeof(path)) {
		return false;
	}
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
		return false;
	}

	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "tidal: %s: %s\n", tmp, strerror(errno));
		return false;
	}
	fprintf(f, "# The Tidal addon's session, on the card next to it.\n");
	fprintf(f, "# Written by the addon; a sign-out removes the keys rather than emptying them.\n\n");
	if (access_token && access_token[0]) {
		fprintf(f, "access_token = %s\n", access_token);
	}
	if (refresh_token && refresh_token[0]) {
		fprintf(f, "refresh_token = %s\n", refresh_token);
	}
	if (expires_at > 0) {
		fprintf(f, "expires_at = %ld\n", expires_at);
	}

	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = (fclose(f) == 0) && ok;
	if (!ok || rename(tmp, path) != 0) {
		fprintf(stderr, "tidal: %s: %s\n", path, strerror(errno));
		unlink(tmp);
		return false;
	}
	return true;
}

// Call with conf_lock held.
static void load_session_locked(char *access_token, size_t access_size, char *refresh_token,
							   size_t refresh_size, long *expires_at) {
	if (access_token && access_size) {
		access_token[0] = '\0';
	}
	if (refresh_token && refresh_size) {
		refresh_token[0] = '\0';
	}
	if (expires_at) {
		*expires_at = 0;
	}
	if (!conf_dir[0]) {
		return;
	}
	char path[STORE_PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/" SESSION_FILE, conf_dir) >= (int)sizeof(path)) {
		return;
	}
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}

	char line[SESSION_LINE_MAX];
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#' || *p == ';' || *p == '[') {
			continue;
		}
		char *eq = strchr(p, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		char *key = p;
		char *value = eq + 1;
		trim(key);
		trim(value);
		if (strcmp(key, "access_token") == 0 && access_token) {
			snprintf(access_token, access_size, "%s", value);
		} else if (strcmp(key, "refresh_token") == 0 && refresh_token) {
			snprintf(refresh_token, refresh_size, "%s", value);
		} else if (strcmp(key, "expires_at") == 0 && expires_at) {
			*expires_at = strtol(value, NULL, 10);
		}
		wipe(line, sizeof(line));
	}
	fclose(f);
}

// The first run, and only that one: what the player used to hold, into the
// addon's files. The marker is written even when nothing was found, so that a
// sign-out is not undone by the old files still sitting on the card.
static void migrate_locked(void) {
	if (!loaded || store_get(&settings, "migrated")[0]) {
		return;
	}

	legacy_t user = {0};
	legacy_t sess = {0};
	char path[STORE_PATH_MAX];
	if (card_root[0] &&
		snprintf(path, sizeof(path), "%s/" LEGACY_DIR "/" LEGACY_USER_FILE, card_root) < (int)sizeof(path)) {
		read_legacy(path, &user);
	}
	if (card_root[0] &&
		snprintf(path, sizeof(path), "%s/" LEGACY_DIR "/" LEGACY_SESSION_FILE, card_root) < (int)sizeof(path)) {
		read_legacy(path, &sess);
	}

	// The session: the player's own file is the newer of the two, as it was
	// when streamkeys read both.
	const char *access = sess.access_token[0] ? sess.access_token : user.access_token;
	const char *refresh = sess.refresh_token[0] ? sess.refresh_token : user.refresh_token;
	long expires = sess.expires_at > 0 ? sess.expires_at : user.expires_at;
	bool moved_session = false;
	if (access[0] || refresh[0]) {
		moved_session = save_session_locked(access, refresh, expires);
		if (!moved_session) {
			fprintf(stderr, "tidal: cannot write the session to the card\n");
		}
	}

	// The pair: a pair named on the card first, then the old keys file. Without
	// either, the built-in one is used and nothing is written here.
	char id[TIDALCONF_ID_MAX];
	char secret[TIDALCONF_SECRET_MAX];
	id[0] = '\0';
	secret[0] = '\0';
	if (user.client_id[0] && user.client_secret[0]) {
		snprintf(id, sizeof(id), "%s", user.client_id);
		snprintf(secret, sizeof(secret), "%s", user.client_secret);
	} else {
		char legacy_id[TIDALCONF_ID_MAX];
		char legacy_secret[TIDALCONF_SECRET_MAX];
		legacy_id[0] = '\0';
		legacy_secret[0] = '\0';
		if (streamkeys_legacy_get("tidal", "client_id", legacy_id, sizeof(legacy_id)) &&
			streamkeys_legacy_get("tidal", "client_secret", legacy_secret, sizeof(legacy_secret))) {
			snprintf(id, sizeof(id), "%s", legacy_id);
			snprintf(secret, sizeof(secret), "%s", legacy_secret);
		}
		wipe(legacy_id, sizeof(legacy_id));
		wipe(legacy_secret, sizeof(legacy_secret));
	}

	if (id[0] && secret[0]) {
		store_set(&settings, "client_id", id);
		store_set(&settings, "client_secret", secret);
	}

	store_set(&settings, "migrated", "1");
	if (!store_save(&settings)) {
		fprintf(stderr, "tidal: cannot write %s\n", settings.path);
	}

	printf("tidal: first run -- %s, %s\n",
		   moved_session ? "the session moved into the addon's folder" : "no session to move",
		   (id[0] && secret[0]) ? "the client pair is kept here" : "no client pair, the built-in one is used");

	wipe(&user, sizeof(user));
	wipe(&sess, sizeof(sess));
	wipe(id, sizeof(id));
	wipe(secret, sizeof(secret));
}

static void load_locked(void) {
	settings = (store_t){0};
	loaded = false;
	if (!conf_dir[0]) {
		return;
	}
	char path[STORE_PATH_MAX];
	if (snprintf(path, sizeof(path), "%s/" SETTINGS_FILE, conf_dir) >= (int)sizeof(path)) {
		return;
	}
	store_init(&settings, path);
	loaded = true;
	printf("tidal: settings %s (%s)\n", path, store_load(&settings) ? "read" : "new");
	migrate_locked();
}

void tidalconf_init(const char *dir, const char *sd_root) {
	pthread_mutex_lock(&conf_lock);
	conf_dir[0] = '\0';
	card_root[0] = '\0';
	if (dir && dir[0]) {
		snprintf(conf_dir, sizeof(conf_dir), "%s", dir);
	}
	if (sd_root && sd_root[0]) {
		snprintf(card_root, sizeof(card_root), "%s", sd_root);
	}
	load_locked();
	pthread_mutex_unlock(&conf_lock);
}

void tidalconf_reload(void) {
	pthread_mutex_lock(&conf_lock);
	load_locked();
	pthread_mutex_unlock(&conf_lock);
}

bool tidalconf_keys(char *client_id, size_t id_size, char *client_secret, size_t secret_size) {
	if (!client_id || !client_secret) {
		return false;
	}
	pthread_mutex_lock(&conf_lock);
	snprintf(client_id, id_size, "%s", loaded ? store_get(&settings, "client_id") : "");
	snprintf(client_secret, secret_size, "%s", loaded ? store_get(&settings, "client_secret") : "");
	bool have = client_id[0] && client_secret[0];
	pthread_mutex_unlock(&conf_lock);
	return have;
}

void tidalconf_session(char *access_token, size_t access_size, char *refresh_token, size_t refresh_size,
					   long *expires_at) {
	pthread_mutex_lock(&conf_lock);
	load_session_locked(access_token, access_size, refresh_token, refresh_size, expires_at);
	pthread_mutex_unlock(&conf_lock);
}

void tidalconf_save_session(const char *access_token, const char *refresh_token, long expires_at) {
	pthread_mutex_lock(&conf_lock);
	if (conf_dir[0] && !save_session_locked(access_token, refresh_token, expires_at)) {
		fprintf(stderr, "tidal: the session was not saved\n");
	}
	pthread_mutex_unlock(&conf_lock);
}

const char *tidalconf_dir(void) { return conf_dir; }
