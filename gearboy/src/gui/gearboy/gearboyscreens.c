#include "gearboyscreens.h"

#include <stdio.h>

#include "src/addon/addon.h"

// The id the host gave this addon, kept: the host structure belongs to the
// player and is filled in again for every addon it opens, so a pointer to it is
// not something to read a minute later -- which is when an unload happens.
static char addon_id[ADDON_ID_MAX];

void gearboyscreens_init(const char *id) {
	snprintf(addon_id, sizeof(addon_id), "%s", id && id[0] ? id : "");
}

void gearboyscreens_own(lv_obj_t *screen) {
	if (!screen || !addon_id[0]) {
		return;
	}
	sonix_addon_own_screen(addon_id, screen);
}
