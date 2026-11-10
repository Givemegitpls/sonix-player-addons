#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/system/image/stb_image_decl.h"

#include "cpage.h"

// The owner's drawing, as sprites on the card (tools/addon_assets.py writes
// them already turned to the page). They are decoded once, at init, into ARGB
// images the page keeps for as long as it is open: the reel that turns, the one
// overlay everything else is baked into, and the light -- a shadow, whose dark
// pixels multiply, and a light, whose white pixels add. The two light textures
// are optional: without them the page is the flat shell and the overlay.
static lv_image_dsc_t spr_hub, spr_overlay, spr_shadow, spr_light;
static void *spr_pixels[4];

static bool sprite_load(const char *dir, const char *name, lv_image_dsc_t *dsc, void **keep) {
	char path[512];
	snprintf(path, sizeof(path), "%s/assets/%s", dir, name);
	FILE *f = fopen(path, "rb");
	if (f == NULL) {
		return false;
	}
	fseek(f, 0, SEEK_END);
	long len = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (len <= 0) {
		fclose(f);
		return false;
	}
	uint8_t *buf = malloc((size_t)len);
	if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return false;
	}
	fclose(f);

	int w = 0, h = 0, n = 0;
	uint8_t *px = stbi_load_from_memory(buf, (int)len, &w, &h, &n, 4);
	free(buf);
	if (px == NULL || w <= 0 || h <= 0) {
		stbi_image_free(px);
		return false;
	}
	// stb gives R,G,B,A; LVGL's ARGB8888 is B,G,R,A on this machine.
	for (int i = 0; i < w * h; i++) {
		uint8_t r = px[i * 4 + 0], g = px[i * 4 + 1], b = px[i * 4 + 2];
		px[i * 4 + 0] = b;
		px[i * 4 + 1] = g;
		px[i * 4 + 2] = r;
	}

	memset(dsc, 0, sizeof(*dsc));
	dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
	dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
	dsc->header.w = (uint32_t)w;
	dsc->header.h = (uint32_t)h;
	dsc->header.stride = (uint32_t)w * 4;
	dsc->data = px;
	dsc->data_size = (uint32_t)(w * h * 4);
	*keep = px;
	return true;
}

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cpal.h"

#include "src/gui/nowplaying/player.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/scale.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/topbar.h"
#include "src/system/library/metadata.h"
#include "src/system/playback/device_state.h"
#include "src/system/device/power.h"

#include "lvgl/lvgl.h"

// ---------------------------------------------------------------------------
// The page
// ---------------------------------------------------------------------------
//
// One cassette, filling the panel, and nothing else on it: no title row, no
// status bar, no chevron, no time readout. The tape itself is the interface --
// the hubs turn while something is playing and stand still when it is paused,
// and the wound tape moves from the supply hub to the take-up hub as the track
// runs. A tap is play/pause, up and down are the next and the previous track,
// and a rightward swipe goes back (see the gestures below).
//
// The cassette lies on its side: turned ninety degrees, so its long axis is the
// panel's, which is the only way it fills a portrait screen. The label then runs
// down one edge with its writing turned with it, exactly as it would be on a
// cassette standing on its short end -- the reference the owner picked.
//
// Three layers, bottom to top: one flat rectangle of the shell's colour, the
// two hubs turned every frame, and one 320x480 overlay with everything else
// baked into it -- the shell's shape, its shadows and bevel, the strip the
// reels sit in, the window, the screws, the label and the reflections. Its
// alpha is where the colour and the hubs show through, and it is drawn over
// them, so it frames the turning reels too. The label's writing is the only
// thing drawn above it: two LVGL labels, no background of their own.
//
// That is the whole page. Everything that does not move is the owner's picture
// in one file (tools/addon_assets.py writes it), so a frame costs one rotation
// of a 60 pixel image and the blend of the overlay over the rectangle it lands
// on -- which is what keeps this affordable on a MIPS without a GPU.

// Font faces, in panel pixels. The label is written on by hand, so it is
// Caveat, brought in the addon's assets/; the player's own face stands in when
// the file is not there (a card with the library only).
#define FONT_FILE "assets/Caveat.ttf"

// One step per LVGL refresh pass (LV_DEF_REFR_PERIOD is 16 ms here): the hubs
// are moved by how much time has passed, not by a fixed angle, so a late timer
// is a slightly longer step rather than a stutter.
#define ANIM_PERIOD_MS 16
#define ANIM_DEG_PER_S 120.0f
#define POLL_PERIOD_MS 500

// The gesture thresholds, in panel pixels and milliseconds.
#define SWIPE_MIN (SC(42))
#define SWIPE_RATIO 1.25f
#define TAP_SLOP (SC(20))
#define TAP_MS 600

#define DEG2RAD 0.017453292f

typedef struct {
	lv_obj_t *image; // the hub, from the owner's drawing (assets/hub.png)
} reel_t;

static void reel_turn(reel_t *r, float deg);

static const gui_config_t *cfg;
static lv_obj_t *screen;
static lv_obj_t *body;
static lv_obj_t *shell; // only when the overlay is missing: the plain colour
static lv_obj_t *label_block;
static lv_obj_t *artist_label;
static lv_obj_t *note_label;
static reel_t reels[2];

static lv_font_t *font_artist;
static lv_font_t *font_note;
static bool font_artist_owned;
static bool font_note_owned;

static lv_timer_t *anim_timer;
static lv_timer_t *poll_timer;

static cpal_t pal;

static float angle_deg;
static uint32_t anim_last_ms;

// What the page is showing, so a poll can tell whether anything changed. The
// palette is not keyed on the track: the player keeps the previous sleeve on
// screen until the new one is decoded (up to two seconds), so a palette built
// when the track changes is the last record's, and nothing rebuilds it when the
// real sleeve lands. The picture itself is what moves the palette.
static uint32_t shown_picture; // fingerprint of the sleeve on screen, 0 for none
static bool shown_playing;

static void apply_palette(void);

// ---------------------------------------------------------------------------
// the cassette
// ---------------------------------------------------------------------------

// The sheet the owner drew on is 480x320 and the page is that sheet turned
// ninety degrees clockwise, so a point of the drawing comes out on the page as
// x = 320 - y_drawing, y = x_drawing. The two reels are at (143,139) and
// (336,139) in the drawing; these are their centres on the page, and they are
// the pivots the hubs turn about. They do not depend on the hub's picture: swap
// in one drawn at another size and it is still placed, and still turns, about
// these points -- the overlay frames it.
static const int32_t reel_cx[2] = {181, 181};
static const int32_t reel_cy[2] = {143, 336};

// One step of the tape: the hub picture turns about its own centre, which the
// placement below puts on the reel's. It is a 60 pixel picture, so the
// transform LVGL spends on it every frame is a fraction of a full-screen one.
static void reel_turn(reel_t *r, float deg) {
	if (r->image) {
		lv_image_set_rotation(r->image, (int32_t)(deg * 10.0f));
	}
}

static lv_obj_t *blank(lv_obj_t *parent) {
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_clickable(o, false);
	lv_obj_set_scrollable(o, false);
	return o;
}

// A panel-sized picture laid over everything, blended its own way: the shadow
// multiplies (its dark pixels darken what is under them), the light adds (its
// white pixels brighten it). Both are one object over the whole panel, so the
// alpha in the file is the strength and the colour decides the sign.
static lv_obj_t *light_layer(lv_obj_t *parent, lv_image_dsc_t *dsc, lv_blend_mode_t mode) {
	if (dsc->data == NULL) {
		return NULL;
	}
	lv_obj_t *o = lv_image_create(parent);
	lv_image_set_src(o, dsc);
	lv_image_set_blend_mode(o, mode);
	lv_obj_set_pos(o, 0, 0);
	return o;
}

static void build_cassette(void) {
	int32_t W = (int32_t)cfg->screen_width;
	int32_t H = (int32_t)cfg->screen_height;

	// The first layer: the shell's colour and nothing else, the whole panel
	// tall. The overlay covers what is not the shell, so its own shape is not
	// this object's business. Without the overlay there is nothing to cover it,
	// and the page falls back to the plain rounded rectangle it used to be.
	body = blank(screen);
	lv_obj_set_pos(body, 0, 0);
	lv_obj_set_size(body, W, H);
	if (spr_overlay.data == NULL) {
		lv_obj_set_style_bg_color(body, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
		shell = blank(body);
		lv_obj_set_pos(shell, 21, 10); // the drawing's 10,20 -> 469,299
		lv_obj_set_size(shell, 279, 459);
		lv_obj_set_style_radius(shell, SC(20), 0);
		lv_obj_set_style_bg_opa(shell, LV_OPA_COVER, 0);
	} else {
		lv_obj_set_style_bg_color(body, pal.body, 0);
		lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
	}

	// The second layer: the two hubs, under the overlay. They are placed by
	// their centre, so a picture of another size keeps the pivot it had.
	for (int s = 0; s < 2; s++) {
		lv_obj_t *hub = lv_image_create(body);
		lv_image_set_src(hub, &spr_hub);
		lv_obj_set_pos(hub, reel_cx[s] - (int32_t)spr_hub.header.w / 2,
					   reel_cy[s] - (int32_t)spr_hub.header.h / 2);
		reels[s].image = hub;
	}

	// The third layer: everything that does not move.
	if (spr_overlay.data != NULL) {
		lv_obj_t *overlay = lv_image_create(body);
		lv_image_set_src(overlay, &spr_overlay);
		lv_obj_set_pos(overlay, 0, 0);
	}

	// The writing goes on the paper the overlay has already drawn: the band
	// down the middle of the label, turned with the rest of it. The container
	// has no colour of its own -- it is only here to rotate the two labels.
	int32_t bw = 300; // along the label; a wider rotated layer is dropped
	int32_t bh = 54;  // across it: the paper column between the strips
	label_block = blank(body);
	lv_obj_set_size(label_block, bw, bh);
	lv_obj_set_pos(label_block, 245 - bw / 2, 239 - bh / 2);
	lv_obj_set_style_transform_pivot_x(label_block, lv_pct(50), 0);
	lv_obj_set_style_transform_pivot_y(label_block, lv_pct(50), 0);
	lv_obj_set_style_transform_rotation(label_block, 900, 0);
	lv_obj_set_flex_flow(label_block, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(label_block, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(label_block, 0, 0);

	artist_label = lv_label_create(label_block);
	lv_obj_set_width(artist_label, lv_pct(100));
	lv_label_set_long_mode(artist_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(artist_label, font_artist, 0);
	lv_obj_set_style_text_color(artist_label, pal.ink, 0);
	lv_obj_set_style_text_align(artist_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(artist_label, "");

	note_label = lv_label_create(label_block);
	lv_obj_set_width(note_label, lv_pct(100));
	lv_label_set_long_mode(note_label, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(note_label, font_note, 0);
	lv_obj_set_style_text_color(note_label, pal.body_dark, 0);
	lv_obj_set_style_text_align(note_label, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(note_label, "");

	// The light goes over the lot of it, the writing included: it is the room
	// the cassette stands in, not a part of the drawing. Each file is optional
	// and each is a panel-sized picture, so a missing one costs nothing.
	light_layer(body, &spr_shadow, LV_BLEND_MODE_MULTIPLY);
	light_layer(body, &spr_light, LV_BLEND_MODE_ADDITIVE);
}

static void apply_palette(void) {
	if (body == NULL) {
		return;
	}
	// The colour layer is the whole page when the overlay is there to cover the
	// rest of it; without the overlay it is only the shell's rectangle.
	lv_obj_set_style_bg_color(screen, pal.bg, 0);
	if (spr_overlay.data != NULL) {
		lv_obj_set_style_bg_color(body, pal.body, 0);
	} else if (shell) {
		lv_obj_set_style_bg_color(shell, pal.body, 0);
	}
	lv_obj_set_style_text_color(artist_label, pal.ink, 0);
	lv_obj_set_style_text_color(note_label, pal.body_dark, 0);
}

// ---------------------------------------------------------------------------
// what is playing
// ---------------------------------------------------------------------------

static void refresh_labels(const device_state_t *st) {
	const char *artist = "";
	if (st->live) {
		artist = st->metadata.artist[0] ? st->metadata.artist : st->metadata.title;
	} else if (st->metadata.has_tags) {
		artist = metadata_shown_artist(&st->metadata);
	}
	if (artist == NULL || artist[0] == '\0') {
		artist = st->current_file[0] ? "Unknown artist" : "--";
	}
	if (strcmp(lv_label_get_text(artist_label), artist) != 0) {
		lv_label_set_text(artist_label, artist);
	}

	// Under the artist: the record it came from. A stream has no record, and a
	// file with no album tag has nothing to say either -- then it is the side.
	const char *album = st->metadata.album;
	char note[288];
	if (st->live) {
		snprintf(note, sizeof(note), "LIVE  ·  SIDE A");
	} else if (album[0] != '\0') {
		snprintf(note, sizeof(note), "%s", album);
	} else {
		snprintf(note, sizeof(note), "SIDE A");
	}
	if (strcmp(lv_label_get_text(note_label), note) != 0) {
		lv_label_set_text(note_label, note);
	}
}

// A cheap answer to "is this the sleeve the palette was made from": the size
// and every sixty-fourth byte of the picture, FNV-1a. The palette has to follow
// the picture and not the track (see shown_picture), and comparing the buffer
// itself is what notices the new sleeve arrive without asking the player to
// decode or copy it. On a 320-wide cover that is ten positions in every row --
// three thousand samples, a few microseconds, alongside the 6.4 thousand the
// palette itself already reads when it is rebuilt.
static uint32_t picture_fingerprint(const lv_image_dsc_t *cover) {
	if (cover == NULL || cover->data == NULL || cover->header.cf != LV_COLOR_FORMAT_RGB565) {
		return 0;
	}
	uint32_t h = 2166136261u;
	h = (h ^ cover->header.w) * 16777619u;
	h = (h ^ cover->header.h) * 16777619u;
	h = (h ^ cover->header.stride) * 16777619u;
	size_t bytes = (size_t)cover->header.stride * cover->header.h;
	const uint8_t *px = cover->data;
	for (size_t i = 0; i < bytes; i += 64) {
		h = (h ^ px[i]) * 16777619u;
	}
	return h ? h : 1; // 0 says "no picture"
}

static void poll_cb(lv_timer_t *timer) {
	(void)timer;

	device_state_t st;
	device_state_get(&st);

	// The hubs turn while the transport is playing, and also when there is
	// nothing on the tape at all -- an empty cassette standing still reads as a
	// broken picture, and the simulator (no sound device) never gets past
	// STOPPED with a file. Pause, and a stopped tape that still holds a track,
	// are the states that stop it.
	bool nothing_loaded = !st.live && st.current_file[0] == '\0';
	bool playing = st.status == AUDIO_STATUS_PLAYING || (st.status == AUDIO_STATUS_STOPPED && nothing_loaded);

	const lv_image_dsc_t *cover = player_cover_image();
	uint32_t picture = picture_fingerprint(cover);
	if (picture != shown_picture) {
		shown_picture = picture;
		if (picture != 0 && cpal_from_cover(cover, &pal)) {
			// the palette is ready
		} else {
			cpal_default(&pal);
		}
		apply_palette();
	}

	shown_playing = playing;
	refresh_labels(&st);

	double total = st.progress_total_secs;
	double cur = st.progress_current_secs;
	float left = 1.0f;
	if (total > 0.5) {
		left = (float)(1.0 - cur / total);
		if (left < 0.0f) {
			left = 0.0f;
		}
		if (left > 1.0f) {
			left = 1.0f;
		}
	}
	(void)left;
}

static void anim_cb(lv_timer_t *timer) {
	(void)timer;

	uint32_t now = lv_tick_get();
	float dt = (float)(now - anim_last_ms) / 1000.0f;
	anim_last_ms = now;
	if (!shown_playing) {
		return;
	}
	// A stall (a card read, a decode) is not a jump: the tape does not teleport.
	if (dt > 0.2f) {
		dt = 0.2f;
	}
	angle_deg = fmodf(angle_deg + ANIM_DEG_PER_S * dt, 360.0f);
	reel_turn(&reels[0], angle_deg);
	reel_turn(&reels[1], angle_deg);
}

// ---------------------------------------------------------------------------
// gestures
// ---------------------------------------------------------------------------

static struct {
	int32_t x, y;
	uint32_t started;
	bool swiped;
} gesture;

static void pressed_cb(lv_event_t *e) {
	(void)e;
	lv_indev_t *indev = lv_indev_active();
	if (indev == NULL) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	gesture.x = p.x;
	gesture.y = p.y;
	gesture.started = lv_tick_get();
	gesture.swiped = false;
}

static void released_cb(lv_event_t *e) {
	(void)e;
	lv_indev_t *indev = lv_indev_active();
	if (indev == NULL) {
		return;
	}
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	int32_t dx = p.x - gesture.x;
	int32_t dy = p.y - gesture.y;
	int32_t adx = dx < 0 ? -dx : dx;
	int32_t ady = dy < 0 ? -dy : dy;

	if (adx >= SWIPE_MIN && (float)adx > (float)ady * SWIPE_RATIO) {
		if (dx > 0) {
			gesture.swiped = true;
			back_btn_cb(NULL);
		}
		return;
	}
	if (ady >= SWIPE_MIN && (float)ady > (float)adx * SWIPE_RATIO) {
		gesture.swiped = true;
		if (dy < 0) {
			player_key_next();
		} else {
			player_key_prev();
		}
		return;
	}

	// A press that went nowhere: the cassette is the button.
	if (adx <= TAP_SLOP && ady <= TAP_SLOP &&
		lv_tick_elaps(gesture.started) <= TAP_MS && !gesture.swiped) {
		player_key_play_pause();
	}
}

static void clicked_cb(lv_event_t *e) {
	(void)e;
	gesture.swiped = false; // the release above has already dealt with it
}

// ---------------------------------------------------------------------------
// the fonts
// ---------------------------------------------------------------------------

static void fonts_load(const char *dir) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, FONT_FILE);

	int32_t w = (int32_t)cfg->screen_width;
	int32_t artist_px = (int32_t)((float)w * 0.070f);
	int32_t note_px = (int32_t)((float)w * 0.053f);

	font_artist = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
										  (uint32_t)artist_px, LV_FREETYPE_FONT_STYLE_NORMAL);
	font_artist_owned = font_artist != NULL;
	if (font_artist == NULL) {
		font_artist = &font_ui_24;
	}

	font_note = lv_freetype_font_create(path, LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
										(uint32_t)note_px, LV_FREETYPE_FONT_STYLE_NORMAL);
	font_note_owned = font_note != NULL;
	if (font_note == NULL) {
		font_note = &font_ui_14;
	}
}

// ---------------------------------------------------------------------------
// the page's life
// ---------------------------------------------------------------------------

static void loaded_cb(lv_event_t *e) {
	(void)e;
	// The panel belongs to the cassette: no status bar over it, no chevron in
	// its corner (the rightward swipe is the way out).
	topbar_set_hidden(true);
	back_btn_force_hidden(true);
	if (anim_timer) {
		lv_timer_resume(anim_timer);
	}
	if (poll_timer) {
		lv_timer_resume(poll_timer);
	}
	anim_last_ms = lv_tick_get();
	poll_cb(NULL);
}

static void unloaded_cb(lv_event_t *e) {
	(void)e;
	back_btn_force_hidden(false);
	topbar_set_hidden(false);
	if (anim_timer) {
		lv_timer_pause(anim_timer);
	}
	if (poll_timer) {
		lv_timer_pause(poll_timer);
	}
}

lv_obj_t *cpage_build(const sonix_addon_host_t *host) {
	cfg = host->cfg;
	cpal_default(&pal);
	shown_picture = 0;
	shown_playing = false;
	angle_deg = 0.0f;
	anim_last_ms = lv_tick_get();

	fonts_load(host->dir);
	sprite_load(host->dir, "hub.png", &spr_hub, &spr_pixels[0]);
	sprite_load(host->dir, "overlay.png", &spr_overlay, &spr_pixels[1]);
	sprite_load(host->dir, "shadow.png", &spr_shadow, &spr_pixels[2]);
	sprite_load(host->dir, "light.png", &spr_light, &spr_pixels[3]);

	screen = lv_obj_create(NULL);
	lv_obj_remove_style_all(screen);
	lv_obj_set_style_bg_color(screen, pal.bg, 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(screen, false);
	lv_obj_set_clickable(screen, true);
	lv_obj_set_scroll_dir(screen, LV_DIR_NONE);

	build_cassette();

	lv_obj_add_event_cb(screen, loaded_cb, LV_EVENT_SCREEN_LOADED, NULL);
	lv_obj_add_event_cb(screen, unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
	lv_obj_add_event_cb(screen, pressed_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(screen, released_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(screen, clicked_cb, LV_EVENT_CLICKED, NULL);

	anim_timer = lv_timer_create(anim_cb, ANIM_PERIOD_MS, NULL);
	poll_timer = lv_timer_create(poll_cb, POLL_PERIOD_MS, NULL);
	lv_timer_pause(anim_timer);
	lv_timer_pause(poll_timer);
	power_pause_in_standby(anim_timer);
	power_pause_in_standby(poll_timer);

	reel_turn(&reels[0], 0.0f);
	reel_turn(&reels[1], 60.0f);

	return screen;
}

void cpage_stop(void) {
	// The player can take the page away without the screen being unloaded
	// first (the card left): the chrome has to come back either way.
	back_btn_force_hidden(false);
	topbar_set_hidden(false);

	if (anim_timer) {
		power_forget_timer(anim_timer);
		lv_timer_delete(anim_timer);
		anim_timer = NULL;
	}
	if (poll_timer) {
		power_forget_timer(poll_timer);
		lv_timer_delete(poll_timer);
		poll_timer = NULL;
	}
	if (artist_label) {
		lv_obj_set_style_text_font(artist_label, &font_ui_24, 0);
	}
	if (note_label) {
		lv_obj_set_style_text_font(note_label, &font_ui_14, 0);
	}

	if (font_artist_owned) {
		lv_freetype_font_delete(font_artist);
	}
	if (font_note_owned) {
		lv_freetype_font_delete(font_note);
	}
	font_artist = NULL;
	font_note = NULL;
	font_artist_owned = false;
	font_note_owned = false;

	screen = NULL;
	body = NULL;
	shell = NULL;
	label_block = NULL;
	artist_label = NULL;
	note_label = NULL;
	memset(reels, 0, sizeof(reels));
	for (int i = 0; i < 4; i++) {
		if (spr_pixels[i]) {
			stbi_image_free(spr_pixels[i]);
			spr_pixels[i] = NULL;
		}
	}
	memset(&spr_hub, 0, sizeof(spr_hub));
	memset(&spr_overlay, 0, sizeof(spr_overlay));
	memset(&spr_shadow, 0, sizeof(spr_shadow));
	memset(&spr_light, 0, sizeof(spr_light));
	cfg = NULL;
	shown_picture = 0;
}
