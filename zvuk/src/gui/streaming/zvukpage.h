#ifndef ZVUKPAGE_H
#define ZVUKPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Zvuk page: the account, the collection, search, the audiobooks and the
// spoken shelf, and a list of rows that plays through zvukplay.
//
// A port of rekkod's zvukpage.c (Rekkod-Tree/sonix-player-rekkod-mod, GPLv3, as
// this tree), with its Graphite 2 markup (sx_row, sx_card, sx_pill, sx_sheet)
// replaced by this player's widgets: settingsrow for the page's own rows,
// `listrow` for the covers of a list, and a popover for the per-row menu. See
// docs/ADDONS.md, "Porting a service page".
//
// The screen the shell switches to when the tile is tapped. Built in init(),
// which runs once: everything that has to be redone per visit hangs off the
// screen's LV_EVENT_SCREEN_LOADED.
extern lv_obj_t *zvukpage_screen;

// Builds the page. `id` is the addon's directory name, which every screen it
// builds is registered with (sonix_addon_own_screen); `sd_root` is the card, for
// the token file, the covers and the saved tracks; `cfg` is the shell's
// geometry.
void zvukpage_init(gui_config_t *cfg, const char *id, const char *sd_root);

// The other end of init(). Withdraws the theme hooks and the save listener,
// closes an open row menu, confirm and busy toast, gives back listart's slots
// and deletes listrow's poll timer, stops the worker and any sign-in, and drops
// every widget and state pointer, so that the library may be dlclose()d and the
// page opened again.
//
// The screens are not touched: every one of them was registered as it was made,
// the shell took them off the display and out of its records before this call
// and deletes them after it. False is a thread still inside the library at the
// end of the wait, which tells the shell not to unmap. src/addon/addon.h, "the
// stop half".
bool zvukpage_stop(void);

// "Show album" in the player, for a Zvuk track: opens the release's track list.
// Filled into the service table by zvuk_service_attach_gui(); returns the
// screen the player goes back to, or NULL when it could not open.
void *zvukpage_open_album(const char *album_id, const char *title);

#endif /* ZVUKPAGE_H */
