#include <stdio.h>
#include <string.h>

#include "src/addon/addon.h"
#include "src/gui/shell/settingsrow.h"
#include "src/system/core/lang.h"

#include "lvgl/lvgl.h"

// The smallest addon there is, and the one to copy when starting another: a
// manifest beside it, one screen built in init(), three settings rows saying
// where it thinks it is, and a stop hook that has almost nothing to undo.
//
// Built into build/target/hello.so by `make addons`, and copied to
// <card>/Addons/hello/ by `make addons pack`.

static lv_obj_t *hello_screen;

static lv_obj_t *hello_init(const sonix_addon_host_t *host) {
	hello_screen = lv_obj_create(NULL);
	lv_obj_t *page = settingsrow_page(hello_screen, host->cfg, "Hello");

	char line[256];
	lv_obj_t *value = NULL;

	settingsrow_add(page, "id", &value, NULL, NULL);
	lv_label_set_text(value, host->id);

	settingsrow_add(page, "dir", &value, NULL, NULL);
	lv_label_set_text(value, host->dir);

	// The screen the addon was laid out for: an addon that draws its own
	// pixels reads these rather than the panel it saw at build time.
	lv_snprintf(line, sizeof(line), "%s", "see host->cfg");
	settingsrow_add(page, "screen", &value, NULL, NULL);
	lv_snprintf(line, sizeof(line), "%ux%u", host->cfg->screen_width, host->cfg->screen_height);
	lv_label_set_text(value, line);

	return hello_screen;
}

// Nothing runs, nothing is registered, no file is open: the shell deletes the
// screen and dlclose()s the library. The only thing to do is forget the pointer,
// which the next init() would otherwise write over while a deleted object still
// looks alive to a stale read -- and answer true, because nothing of this
// library is executing by the time this returns.
static bool hello_stop(void) {
	hello_screen = NULL;
	return true;
}

// No state beyond the screen: see addon.h. Everything the structure does not
// name stays zero, which is what .api and .init are not.
static const sonix_addon_t hello = {
	.api = SONIX_ADDON_API,
	.title = "Hello",
	.init = hello_init,
};

static const sonix_addon_stop_t hello_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = hello_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &hello : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &hello_lifecycle : NULL;
}
