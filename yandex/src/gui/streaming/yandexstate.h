#ifndef YANDEXSTATE_H
#define YANDEXSTATE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Yandex Music addon's screen, for now: the account and the keys.
//
// The service's own page (rekkod's yandexpage.c, 2693 lines of Graphite 2) is
// the next stage and is not here. What is here is what makes this stage testable
// without it: one settings page that says whether the addon found its keys,
// whether a token is on the card, who is signed in and what the quality ceiling
// is, and the four actions that change any of it -- sign in from the card,
// reload the settings, change the quality, sign out.
//
// Without keys, and without a token, the page still opens and says so: those are
// states, not failures, and nothing here talks to the network until an action
// row is tapped (the sign-in runs on a worker and posts back).

// Builds the screen. Called once, from the addon's init(); `sd_root` is the
// card, where yandex_token.txt is looked for.
void yandexstate_init(gui_config_t *cfg, const char *sd_root);
// The screen the tile opens.
extern lv_obj_t *yandexstate_screen;

#endif /* YANDEXSTATE_H */
