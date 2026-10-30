#include "yandexstate.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/gui/shell/gui.h"
#include "src/gui/shell/settingsrow.h"

#include "src/system/streaming/yandex.h"
#include "src/system/streaming/yandexconf.h"

#include "lvgl/lvgl.h"

// The Yandex Music addon's screen for this stage: the keys, the account, and
// the actions that change them. See yandexstate.h.
//
// Nothing that talks to the network runs on the UI thread: the sign-in is a
// detached worker that posts its answer back with gui_post(), as
// docs/ADDONS.md asks. Opening the page itself never touches the network, so a
// card with no keys and no token still opens it and says why the service is off.

lv_obj_t *yandexstate_screen;

static char card_root[400];

static lv_obj_t *keys_value;
static lv_obj_t *account_value;
static lv_obj_t *token_value;
static lv_obj_t *quality_value;

// The strings here are English, not tr(): an addon's strings are its own and
// are not in the player's language files (docs/ADDONS.md, "Languages"). The
// keys the service modules themselves pass through tr() (an error from the
// downloader, for instance) come back as the tag when there is no line for
// them, which is why the page does not show those.

static void token_path(char *out, size_t size) {
	if (!card_root[0]) {
		out[0] = '\0';
		return;
	}
	snprintf(out, size, "%s/%s", card_root, YANDEX_TOKEN_FILE);
}

// The first line of the card's token file that has anything on it. A whole
// "Authorization: OAuth ..." line pasted in is one line too, and
// yandex_token_clean() takes the token out of it.
static bool read_token_file(char *out, size_t size) {
	char path[520];
	out[0] = '\0';
	token_path(path, sizeof(path));
	FILE *f = path[0] ? fopen(path, "r") : NULL;
	if (!f) {
		return false;
	}
	char line[1024];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		const char *start = line;
		// A UTF-8 BOM, as an editor on Windows leaves it.
		if ((unsigned char)start[0] == 0xef && (unsigned char)start[1] == 0xbb && (unsigned char)start[2] == 0xbf) {
			start += 3;
		}
		while (*start == ' ' || *start == '\t') {
			start++;
		}
		if (*start) {
			// The line may be longer than a token: yandex_token_clean() keeps
			// what is a token and stops at the first character that is not.
			snprintf(out, size, "%s", start);
			break;
		}
	}
	fclose(f);
	return out[0] != '\0';
}

static void refresh(void) {
	if (!keys_value) {
		return;
	}
	lv_label_set_text(keys_value, yandex_configured() ? "present" : "no keys");
	if (yandex_logged_in()) {
		const char *name = yandex_display_name();
		lv_label_set_text(account_value, name && name[0] ? name : "signed in");
	} else {
		lv_label_set_text(account_value, "not signed in");
	}
	char path[520];
	token_path(path, sizeof(path));
	bool token = false;
	if (path[0]) {
		FILE *f = fopen(path, "r");
		if (f) {
			token = true;
			fclose(f);
		}
	}
	lv_label_set_text(token_value, token ? "found on the card" : "not on the card");
	static const char *const QUALITY[] = {"Lossless", "High", "Normal"};
	yandex_quality_t q = yandex_get_quality();
	lv_label_set_text(quality_value, QUALITY[(unsigned)q < YANDEX_QUALITY_COUNT ? (unsigned)q : 0]);
}

// ---------------------------------------------------------------------------
// the worker
// ---------------------------------------------------------------------------

typedef struct {
	char message[256];
	bool ok;
} answer_t;

static void answer_async(void *user) {
	answer_t *a = user;
	if (a->message[0]) {
		gui_notify_popup(a->message);
	}
	refresh();
	free(a);
}

static void *sign_in_main(void *arg) {
	(void)arg;
	answer_t *a = calloc(1, sizeof(*a));
	if (!a) {
		return NULL;
	}
	char token[1024];
	if (!read_token_file(token, sizeof(token))) {
		snprintf(a->message, sizeof(a->message), "no %s at the card's root", YANDEX_TOKEN_FILE);
		gui_post(answer_async, a);
		return NULL;
	}
	a->ok = yandex_import_token(token, a->message, sizeof(a->message));
	memset(token, 0, sizeof(token));
	if (!a->message[0]) {
		snprintf(a->message, sizeof(a->message), "%s", a->ok ? "signed in" : "the token was not accepted");
	}
	if (!gui_post(answer_async, a)) {
		free(a);
	}
	return NULL;
}

static void start_worker(void *(*main_fn)(void *)) {
	pthread_t th;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, 256 * 1024); // TLS: as the other workers
	if (pthread_create(&th, &attr, main_fn, NULL) != 0) {
		gui_notify_popup("could not start a worker");
	}
	pthread_attr_destroy(&attr);
}

// ---------------------------------------------------------------------------
// the rows
// ---------------------------------------------------------------------------

static void sign_in_cb(lv_event_t *e) {
	(void)e;
	if (!yandex_configured()) {
		gui_notify_popup("no keys: the addon's settings.ini has none");
		return;
	}
	start_worker(sign_in_main);
}

static void reload_cb(lv_event_t *e) {
	(void)e;
	// The card may have been written while the player ran: the keys, the token,
	// a hand-edited settings.ini. Nothing here talks to the network.
	yandex_reload();
	refresh();
	gui_notify_popup("read again from the card");
}

static void sign_out_cb(lv_event_t *e) {
	(void)e;
	yandex_logout();
	refresh();
	gui_notify_popup("signed out");
}

static void quality_cb(lv_event_t *e) {
	(void)e;
	yandex_quality_t q = yandex_get_quality();
	q = (yandex_quality_t)(((unsigned)q + 1) % YANDEX_QUALITY_COUNT);
	yandex_set_quality(q);
	refresh();
}

static void loaded_cb(lv_event_t *e) {
	(void)e;
	refresh();
}

void yandexstate_init(gui_config_t *cfg, const char *sd_root) {
	snprintf(card_root, sizeof(card_root), "%s", sd_root ? sd_root : "");
	yandexstate_screen = lv_obj_create(NULL);
	lv_obj_t *container = settingsrow_page(yandexstate_screen, cfg, "Yandex Music");

	settingsrow_add(container, "Keys", &keys_value, NULL, NULL);
	settingsrow_add(container, "Account", &account_value, NULL, NULL);
	settingsrow_add(container, "Token file", &token_value, NULL, NULL);
	settingsrow_add(container, "Quality", &quality_value, quality_cb, NULL);

	settingsrow_action(container, "Sign in from the card", sign_in_cb, NULL);
	settingsrow_action(container, "Read again from the card", reload_cb, NULL);
	settingsrow_action(container, "Sign out", sign_out_cb, NULL);

	refresh();

	lv_obj_add_event_cb(yandexstate_screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
}
