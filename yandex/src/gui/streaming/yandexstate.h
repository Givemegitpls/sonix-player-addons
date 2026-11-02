#ifndef YANDEXSTATE_H
#define YANDEXSTATE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Yandex Music addon's page: the home of sections, the list behind them and
// the search page.
//
// The name of the file is the stage before it (a settings screen with two
// buttons): what is here now is the page those buttons stood in for -- My Wave,
// the liked tracks, liked artists, liked albums, playlists, the new releases,
// the chart, what is saved on the card and a search, each opening one list
// screen that pages its own way; an artist, an album and a playlist open their
// tracks on the same screen. The account half of the settings screen is kept on
// the home: the keys, the token on the card, the OAuth code flow, the quality
// ceiling, signing out, and reading the card again.
//
// Nothing that talks to the network runs on the UI thread. Opening the page
// touches nothing: a section is read when it is opened, on the page's worker,
// and its answer arrives through gui_post(). A card with no keys and no account
// still opens it and says why the service is off; a section opened in that
// state shows the sentence yandex.c gives it rather than waiting.

// Builds the screens. Called once, from the addon's init(); `sd_root` is the
// card (the covers' shared cache, listart), `dir` is the addon's own folder
// (<card>/Addons/yandex, where token.txt is looked for), and `id` is the id the
// host gave the library, which every screen is registered under
// (sonix_addon_own_screen).
void yandexstate_init(gui_config_t *cfg, const char *sd_root, const char *dir, const char *id);
// Undoes init(): the timers deleted, the page's workers waited for, the theme
// hook taken back, the covers released and every pointer into the screens
// dropped. Called from the addon's stop hook, on the GUI thread, before the
// service modules are stopped. False when one of the page's workers did not
// come back in time.
bool yandexstate_stop(void);
// The screen the tile opens.
extern lv_obj_t *yandexstate_screen;

#endif /* YANDEXSTATE_H */
