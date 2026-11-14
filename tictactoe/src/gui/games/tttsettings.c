#include "tttsettings.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "src/addon/addon.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#include "ttt.h"

lv_obj_t *tttsettings_screen;

static char owner_id[ADDON_ID_MAX];
static lv_obj_t *diff_pills[TTT_DIFF_COUNT];
static bool theme_hook;

static bool ru(void) {
	const char *name = lang_current();
	return name && strcmp(name, "Russian") == 0;
}

static const char *pill_label(int d) {
	if (ru()) {
		switch (d) {
		case TTT_DIFF_EASY:
			return "Лёгкий";
		case TTT_DIFF_HARD:
			return "Сложный";
		default:
			return "Средний";
		}
	}
	switch (d) {
	case TTT_DIFF_EASY:
		return "Easy";
	case TTT_DIFF_HARD:
		return "Hard";
	default:
		return "Medium";
	}
}

static void refresh_pills(void) {
	int cur = ttt_get_difficulty();
	for (int i = 0; i < TTT_DIFF_COUNT; i++) {
		if (diff_pills[i]) {
			settingsrow_pill_active(diff_pills[i], i == cur);
		}
	}
}

static void pick_cb(lv_event_t *e) {
	int d = (int)(intptr_t)lv_event_get_user_data(e);
	ttt_set_difficulty(d);
	refresh_pills();
}

void tttsettings_init(gui_config_t *cfg, const char *addon_id) {
	if (addon_id && addon_id[0]) {
		snprintf(owner_id, sizeof(owner_id), "%s", addon_id);
	} else {
		owner_id[0] = '\0';
	}

	const char *title = ru() ? "Сложность" : "Difficulty";
	lv_obj_t *container = settingsrow_page(tttsettings_screen, cfg, title);

	lv_obj_t *pills = NULL;
	const char *card = ru() ? "Уровень бота" : "Bot level";
	settingsrow_pills(container, card, &pills);

	for (int i = 0; i < TTT_DIFF_COUNT; i++) {
		// Literal labels: not in the player's language files.
		diff_pills[i] = settingsrow_pill_text(pills, pill_label(i), i, pick_cb);
	}

	refresh_pills();
	if (!theme_hook) {
		theme_register_refresh(refresh_pills);
		theme_hook = true;
	}
	switcher_attach_back_gesture(tttsettings_screen);
}

void tttsettings_open(void) {
	if (owner_id[0] && tttsettings_screen) {
		sonix_addon_own_screen(owner_id, tttsettings_screen);
	}
	refresh_pills();
	switch_screen(tttsettings_screen);
}

void tttsettings_stop(void) {
	if (theme_hook) {
		theme_unregister_refresh(refresh_pills);
		theme_hook = false;
	}
	for (int i = 0; i < TTT_DIFF_COUNT; i++) {
		diff_pills[i] = NULL;
	}
}
