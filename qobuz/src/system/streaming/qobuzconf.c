#include "qobuzconf.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/store.h"

#include "src/system/core/json.h"
#include "src/system/streaming/streamkeys.h"

// See qobuzconf.h.

#define SETTINGS_FILE "settings.ini"
#define SESSION_FILE "session.ini"

// The old place, as the player read it: <card>/sonix/streaming.json, written by
// the user, and <card>/sonix/streaming-session.json, written by the player.
#define LEGACY_DIR "sonix"
#define LEGACY_USER_FILE "streaming.json"
#define LEGACY_SESSION_FILE "streaming-session.json"
#define LEGACY_JSON_MAX 16384

static pthread_mutex_t conf_lock = PTHREAD_MUTEX_INITIALIZER;
static store_t settings; // settings.ini
static store_t session;  // session.ini
static char conf_dir[STORE_PATH_MAX];
static char card_root[STORE_PATH_MAX];
static bool loaded;

// One of the old JSON files, as far as Qobuz is concerned. Every field is
// optional and a file that does not carry one leaves what the other one said.
typedef struct {
	char token[QOBUZCONF_TOKEN_MAX];
	char user[QOBUZCONF_USER_MAX];
	char app_id[QOBUZCONF_ID_MAX];
	char app_secret[QOBUZCONF_SECRET_MAX];
	long creds_at;
} legacy_t;

// Not a plain memset: one on a buffer about to be freed may be dropped by the
// compiler.
static void wipe(void *p, size_t n) {
	volatile unsigned char *v = p;
	while (n--) {
		*v++ = 0;
	}
}

static void take(const json_doc_t *doc, int obj, const char *key, char *out, size_t size) {
	char value[512];
	value[0] = '\0';
	if (obj < 0 || !json_obj_str(doc, obj, key, value, sizeof(value))) {
		return;
	}
	// A value longer than `out` is cut here rather than by snprintf's own
	// truncation: what is read is a token, and a half of one is no better than
	// none, so the cut is deliberate and visible.
	size_t n = strlen(value);
	if (n >= size) {
		n = size - 1;
	}
	memcpy(out, value, n);
	out[n] = '\0';
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
		printf("qobuz: %s is over %d bytes, ignored\n", path, LEGACY_JSON_MAX);
		free(text);
		return;
	}
	text[len] = '\0';

	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		printf("qobuz: %s is not valid JSON, ignored\n", path);
		wipe(text, len);
		free(text);
		return;
	}

	int obj = json_get(&doc, json_root(&doc), "qobuz");
	take(&doc, obj, "token", out->token, sizeof(out->token));
	take(&doc, obj, "user", out->user, sizeof(out->user));
	take(&doc, obj, "app_id", out->app_id, sizeof(out->app_id));
	take(&doc, obj, "app_secret", out->app_secret, sizeof(out->app_secret));
	long at = json_obj_long(&doc, obj, "creds_at", -1);
	if (at >= 0) {
		out->creds_at = at;
	}

	json_free(&doc);
	wipe(text, len);
	free(text);
	printf("qobuz: %s read\n", path);
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

	// The account: the player's own file is the newer of the two, as it was
	// when streamkeys read both.
	const char *token = sess.token[0] ? sess.token : user.token;
	const char *name = sess.user[0] ? sess.user : user.user;
	bool moved_session = false;
	if (token[0]) {
		store_set(&session, "token", token);
		store_set(&session, "user", name);
		moved_session = store_save(&session);
		if (!moved_session) {
			fprintf(stderr, "qobuz: cannot write %s\n", session.path);
		}
	}

	// The pair, in the order the old code resolved it: a pair named on the card,
	// then the old keys file, then the pair derived from the web player the last
	// time.
	char id[QOBUZCONF_ID_MAX];
	char secret[QOBUZCONF_SECRET_MAX];
	id[0] = '\0';
	secret[0] = '\0';
	bool by_user = false;
	long at = 0;
	if (user.app_id[0] && user.app_secret[0]) {
		snprintf(id, sizeof(id), "%s", user.app_id);
		snprintf(secret, sizeof(secret), "%s", user.app_secret);
		by_user = true;
	} else {
		char legacy_id[QOBUZCONF_ID_MAX];
		char legacy_secret[QOBUZCONF_SECRET_MAX];
		legacy_id[0] = '\0';
		legacy_secret[0] = '\0';
		bool in_file = streamkeys_legacy_get("qobuz", "app_id", legacy_id, sizeof(legacy_id)) &&
					   streamkeys_legacy_get("qobuz", "app_secret", legacy_secret, sizeof(legacy_secret));
		if (in_file) {
			snprintf(id, sizeof(id), "%s", legacy_id);
			snprintf(secret, sizeof(secret), "%s", legacy_secret);
			by_user = true;
		} else if (sess.app_id[0] && sess.app_secret[0]) {
			snprintf(id, sizeof(id), "%s", sess.app_id);
			snprintf(secret, sizeof(secret), "%s", sess.app_secret);
			at = sess.creds_at;
		}
		wipe(legacy_id, sizeof(legacy_id));
		wipe(legacy_secret, sizeof(legacy_secret));
	}

	if (id[0] && secret[0]) {
		char text[24];
		store_set(&settings, "app_id", id);
		store_set(&settings, "app_secret", secret);
		store_set(&settings, "by_user", by_user ? "1" : "");
		snprintf(text, sizeof(text), "%ld", at);
		store_set(&settings, "creds_at", at > 0 ? text : "");
	}

	store_set(&settings, "migrated", "1");
	bool saved = store_save(&settings);
	if (!saved) {
		fprintf(stderr, "qobuz: cannot write %s\n", settings.path);
	}

	printf("qobuz: first run -- %s, %s\n",
		   moved_session ? "the account moved into the addon's folder" : "no account to move",
		   (id[0] && secret[0]) ? "the application pair is kept here" : "no application pair, it is derived when needed");

	wipe(&user, sizeof(user));
	wipe(&sess, sizeof(sess));
	wipe(id, sizeof(id));
	wipe(secret, sizeof(secret));
}

static void load_locked(void) {
	settings = (store_t){0};
	session = (store_t){0};
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
	printf("qobuz: settings %s (%s)\n", path, store_load(&settings) ? "read" : "new");
	if (snprintf(path, sizeof(path), "%s/" SESSION_FILE, conf_dir) < (int)sizeof(path)) {
		store_init(&session, path);
		store_load(&session);
	}
	migrate_locked();
}

void qobuzconf_init(const char *dir, const char *sd_root) {
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

void qobuzconf_reload(void) {
	pthread_mutex_lock(&conf_lock);
	load_locked();
	pthread_mutex_unlock(&conf_lock);
}

void qobuzconf_keys(char *app_id, size_t id_size, char *app_secret, size_t secret_size, bool *by_user,
					long *creds_at) {
	if (!app_id || !app_secret) {
		return;
	}
	pthread_mutex_lock(&conf_lock);
	snprintf(app_id, id_size, "%s", loaded ? store_get(&settings, "app_id") : "");
	snprintf(app_secret, secret_size, "%s", loaded ? store_get(&settings, "app_secret") : "");
	if (by_user) {
		*by_user = loaded && strcmp(store_get(&settings, "by_user"), "1") == 0;
	}
	if (creds_at) {
		*creds_at = loaded ? strtol(store_get(&settings, "creds_at"), NULL, 10) : 0;
	}
	pthread_mutex_unlock(&conf_lock);
}

void qobuzconf_save_creds(const char *app_id, const char *app_secret, long at, bool by_user) {
	pthread_mutex_lock(&conf_lock);
	if (loaded) {
		char text[24];
		store_set(&settings, "app_id", app_id ? app_id : "");
		store_set(&settings, "app_secret", app_secret ? app_secret : "");
		store_set(&settings, "by_user", by_user ? "1" : "");
		snprintf(text, sizeof(text), "%ld", at);
		store_set(&settings, "creds_at", at > 0 ? text : "");
		if (!store_save(&settings)) {
			fprintf(stderr, "qobuz: cannot write %s\n", settings.path);
		}
	}
	pthread_mutex_unlock(&conf_lock);
}

void qobuzconf_session(char *token, size_t token_size, char *user, size_t user_size) {
	if (!token || !user) {
		return;
	}
	pthread_mutex_lock(&conf_lock);
	snprintf(token, token_size, "%s", loaded ? store_get(&session, "token") : "");
	snprintf(user, user_size, "%s", loaded ? store_get(&session, "user") : "");
	pthread_mutex_unlock(&conf_lock);
}

bool qobuzconf_save_session(const char *token, const char *user) {
	bool saved = false;
	pthread_mutex_lock(&conf_lock);
	if (loaded) {
		store_set(&session, "token", token ? token : "");
		store_set(&session, "user", user ? user : "");
		// A sign-out says so in the file: the token on the card would otherwise
		// sign the account straight back in at the next start (qobuzconf.h).
		store_set(&session, "signed_out", (token && token[0]) ? "" : "1");
		saved = store_save(&session);
		if (!saved) {
			fprintf(stderr, "qobuz: cannot write %s\n", session.path);
		}
	}
	pthread_mutex_unlock(&conf_lock);
	return saved;
}

bool qobuzconf_signed_out(void) {
	pthread_mutex_lock(&conf_lock);
	bool out = loaded && strcmp(store_get(&session, "signed_out"), "1") == 0;
	pthread_mutex_unlock(&conf_lock);
	return out;
}

const char *qobuzconf_dir(void) { return conf_dir; }
