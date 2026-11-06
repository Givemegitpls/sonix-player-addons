#ifndef YPAGE_H
#define YPAGE_H

#include <stdbool.h>

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Piped pages: the main one (search entry, recent queries, the instance
// in use), the results list, and the settings page that picks the instance.
//
// `addon_id` is sonix_addon_host_t::id, kept for sonix_addon_own_screen().
void ppage_init(gui_config_t *cfg, const char *addon_id);

// Takes the page apart for the addon's stop(): its workers, its timers, its
// screens beyond the one init() returned, and every screen pointer. False when
// a bounded wait found a worker still in the library.
bool ppage_stop(void);

// The screen init() returns, for the shell to switch to.
extern lv_obj_t *p_screen;

#endif /* YPAGE_H */
