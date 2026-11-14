#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/gravity.h"
#include "platform/touch.h"

#include "src/addon/addon.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/topbar.h"
#include "src/system/audio/audio.h"

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

// Gravity Defied as an addon.
//
// The game is upstream's (rgimad/gravity_defied_cpp, GPL-2.0 -- see LICENSE and
// NOTICE.md in this directory), running on a platform layer of ours instead of
// SDL: platform/ holds the frame, the blitter, the baked font and the thread the
// game's own loop lives on. This file is the join between that and the shell: one
// screen, one picture of the game's frame, and the tile's name to hang them on.
//
// The picture is the game's 480x320 frame -- the panel's own pixels with the
// player held sideways -- shown turned a quarter turn inside the shell's
// portrait screen. Holding the player on its side is the whole interface: see
// "Controls" in README.txt.

static lv_obj_t *gravity_screen;
static lv_obj_t *view;		 // the picture of the frame
static lv_image_dsc_t frame_dsc; // points at frame
static uint16_t *frame;		 // 480x320 RGB565, in our own memory
static char gravity_id[ADDON_ID_MAX];
static lv_obj_t *return_screen; // the page the tile was tapped on
static char gravity_dir[ADDON_PATH_MAX]; // <card>/Addons/gravity, for a restart

// A finished frame, on the interface thread (posted by the game's thread).
//
// The copy is the point: the game never stops drawing, so the descriptor LVGL is
// reading has to be memory nothing else writes until the next hand-over. 300 KB
// at the game's 30 steps a second is 9 MB/s, a fifth of what the panel's own
// copy of a frame costs.
// A tap anywhere on the glass is the game's FIRE key: its menus choose a row with
// it (MenuManager::processKeyCode, case 8 -> processGameActionUpd(1)), and the ride
// itself has no use for it. It is also the only thing the touchscreen does now --
// the ride is on the buttons (platform/touch.c), and the chevron the shell draws on
// top of the picture stays live, because LVGL's indev is no longer parked.
static void tap_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	gtouch_fire(code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING);
}

// The game's own menu has an "Exit Game" item, and it ends the game's loop. While
// the game runs the panel belongs to it (platform/touch.c parks LVGL's indev), so
// the chevron cannot be tapped: this is the way back, and it runs on the interface
// thread with the game thread already gone.
static void game_finished(void *user)
{
	(void)user;
	if (return_screen) {
		switch_screen(return_screen);
	}
}

static void frame_ready(void *user);
static void game_finished(void *user);

// The screen's own lifetime is what the panel and the game thread hang on.
//
// The game has the touch panel while it runs (platform/touch.c parks LVGL's
// indev, because a bike needs gas and a lean at once and one pointer cannot say
// that). Anything that takes the screen away without going through the game's
// own "Exit Game" -- a back gesture, another page -- must hand the panel back, or
// the shell is left with a dead touchscreen until the player is restarted: that
// is exactly what happened on the device the first time. So: loaded starts the
// game if it is not already running, unloaded stops it and gives the panel back.
static void screen_loaded_cb(lv_event_t *e)
{
	(void)e;
	topbar_set_hidden(true);
	// The game paints white, and the shell's chevron is white on a dark theme: it
	// was invisible on this page. Tinting it for the screen the addon owns is what
	// the shell offers a page with colours of its own.
	back_btn_tint_on_screen(gravity_screen, true, lv_color_black());
	if (!gravity_running() && gravity_dir[0]) {
		gravity_frame_cb_set(frame_ready, NULL);
		gravity_exit_cb_set(game_finished, NULL);
		if (!gravity_run(gravity_dir)) {
			printf("gravity: the game did not start again\n");
		}
	}
}

static void screen_unloaded_cb(lv_event_t *e)
{
	(void)e;
	topbar_set_hidden(false);
	back_btn_tint_on_screen(gravity_screen, false, lv_color_black());
	back_btn_force_hidden(false);
	gravity_frame_cb_set(NULL, NULL);
	gravity_exit_cb_set(NULL, NULL);
	gravity_quit();
}

static void frame_ready(void *user)
{
	(void)user;

	// The chevron is the way out, and the way out is wanted when the game is
	// listening rather than while a race is being ridden: it is shown in the
	// game's menus only. The shell hides it per page, so it is put back on the way
	// out of the addon.
	// -1 until the first frame: the chevron has to be hidden while the logo and the
	// race are up, and "not known yet" is not the same as "shown".
	static int chevron_state = -1;
	int menu = gravity_menu_open() ? 1 : 0;
	if (menu != chevron_state) {
		chevron_state = menu;
		back_btn_force_hidden(!menu);
	}

	gravity_frame_copy(frame);
	if (view) {
		lv_obj_invalidate(view);
	}
}

static lv_obj_t *gravity_addon_init(const sonix_addon_host_t *host)
{
	int w = gravity_width();
	int h = gravity_height();
	if (w <= 0 || h <= 0) {
		return NULL;
	}

	snprintf(gravity_id, sizeof(gravity_id), "%s", host->id && host->id[0] ? host->id : "gravity");
	snprintf(gravity_dir, sizeof(gravity_dir), "%s", host->dir ? host->dir : "");

	// Where the game goes back to: the section page the tile is on. init() runs
	// before the shell switches to the screen this function returns.
	return_screen = lv_screen_active();

	size_t bytes = (size_t)w * (size_t)h * sizeof(uint16_t);
	frame = malloc(bytes);
	if (!frame) {
		printf("gravity: no frame (%zu bytes)\n", bytes);
		return NULL;
	}
	memset(frame, 0, bytes);

	gravity_screen = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(gravity_screen, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(gravity_screen, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(gravity_screen, 0, 0);
	lv_obj_set_scrollable(gravity_screen, false);

	frame_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	frame_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	frame_dsc.header.w = (uint32_t)w;
	frame_dsc.header.h = (uint32_t)h;
	frame_dsc.header.stride = (uint32_t)w * 2;
	frame_dsc.data_size = (uint32_t)bytes;
	frame_dsc.data = (const uint8_t *)frame;

	view = lv_image_create(gravity_screen);
	lv_obj_set_pos(view, 0, 0);
	lv_obj_set_size(view, host->cfg->screen_width, host->cfg->screen_height);

	// A quarter turn, and whatever scaling makes the turned frame fill the
	// screen: 1:1 on the V3 Analog, whose layout is the 320x480 glass itself,
	// and 1.5 on a 480x720 panel (the simulator is one). The source is 480x320
	// and turned it is 320x480, so the factor falls out of the screen's width.
	lv_image_set_src(view, &frame_dsc);
	lv_image_set_rotation(view, 900);
	lv_image_set_scale(view, (uint32_t)(((int32_t)host->cfg->screen_width * LV_SCALE_NONE + h / 2) / h));
	lv_image_set_antialias(view, false);
	lv_image_set_inner_align(view, LV_IMAGE_ALIGN_CENTER);
	lv_obj_set_clickable(view, false);

	lv_obj_add_event_cb(gravity_screen, tap_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(gravity_screen, tap_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(gravity_screen, tap_cb, LV_EVENT_PRESS_LOST, NULL);

	lv_obj_add_event_cb(gravity_screen, screen_loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(gravity_screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

	// A game is not a soundtrack: whatever the player was playing stops. Paused
	// rather than stopped, so leaving the addon puts it back where it was.
	audio_pause();

	gravity_frame_cb_set(frame_ready, NULL);
	gravity_exit_cb_set(game_finished, NULL);
	if (!gravity_run(host->dir)) {
		printf("gravity: the game did not start\n");
		gravity_frame_cb_set(NULL, NULL);
		lv_image_set_src(view, NULL);
		lv_image_cache_drop(&frame_dsc);
		free(frame);
		frame = NULL;
		return NULL;
	}

	// The one screen this addon makes, reported so the loader has it in its
	// list and deletes it after stop() returns (addon.h,
	// sonix_addon_own_screen).
	sonix_addon_own_screen(gravity_id, gravity_screen);
	return gravity_screen;
}

// The library is going away. The game thread first -- it is the only thing still
// writing pixels -- then the picture, then the buffer it pointed at.
static bool gravity_addon_stop(void)
{
	// The game thread and the panel were let go when the screen was unloaded
	// (screen_unloaded_cb); this only drops the picture and its buffer.
	if (view) {
		lv_image_set_src(view, NULL);
		lv_image_cache_drop(&frame_dsc);
		view = NULL;
	}
	free(frame);
	frame = NULL;
	memset(&frame_dsc, 0, sizeof(frame_dsc));
	gravity_screen = NULL;
	return true;
}

static const sonix_addon_t gravity = {
	.api = SONIX_ADDON_API,
	.title = "Gravity Defied",
	.init = gravity_addon_init,
};

static const sonix_addon_stop_t gravity_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = gravity_addon_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api)
{
	return api == SONIX_ADDON_API ? &gravity : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api)
{
	return api == SONIX_ADDON_API ? &gravity_lifecycle : NULL;
}
