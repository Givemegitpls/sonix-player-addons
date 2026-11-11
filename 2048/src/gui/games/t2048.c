// t2048.c -- 2048, from the More page.
//
// Classic 4x4: swipe merges equal neighbours, a 2 or 4 appears after every
// move that changed the board, and the best score is what survives a visit.
// The board itself is not saved -- a new visit is a new game.
//
// Tiles slide with lv_anim (position), then a new one pops in (scale). Input is
// ignored while either phase is running. There is no on_tick: nothing moves on
// its own.

#include "t2048.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"

#include "src/gui/fonts/fonts.h"
#include "src/gui/games/games.h"
#include "src/gui/shell/scale.h"
#include "src/system/core/lang.h"

#define SECTION "2048"
#define N 4
#define TILE_POOL 20
#define MOVE_MS 130
#define SPAWN_MS 110
#define MERGE_POP_MS 90

#define GAP SC(8)
#define BOARD_PAD SC(8)

typedef struct {
	lv_obj_t *box;
	lv_obj_t *label;
	int r, c;
	int value;
	bool live;
	bool dying;
} tile_t;

typedef struct {
	tile_t *tile;
	int to_r, to_c;
	int end_value; // >0: set this value after the slide (merged tile)
	bool merge_kill;
} slide_t;

static game_frame_t frame;
lv_obj_t *t2048_screen;

static lv_obj_t *board;
static lv_obj_t *slots[N][N];
static tile_t tiles[TILE_POOL];
static int grid[N][N];

static int cell_px;
static int board_x, board_y;
static int pending_anims;
static bool animating;
static bool won_once;
static bool keep_going;
static bool over;

static slide_t slides[TILE_POOL];
static int slide_count;
static int spawn_r, spawn_c, spawn_v;

// ---------------------------------------------------------------------------
// locale
// ---------------------------------------------------------------------------

static bool lang_ru(void) {
	const char *name = lang_current();
	return name && strcmp(name, "Russian") == 0;
}

static const char *txt_hint(void) {
	return lang_ru() ? "Свайп для хода" : "Swipe to move";
}

static const char *txt_ready_body(void) {
	return lang_ru() ? "Собери плитку 2048" : "Build the 2048 tile";
}

static const char *txt_start(void) {
	return lang_ru() ? "Начать" : "Start";
}

static const char *txt_over_title(void) {
	return lang_ru() ? "Конец игры" : "Game over";
}

static const char *txt_over_body(void) {
	return lang_ru() ? "Ходов больше нет" : "No moves left";
}

static const char *txt_again(void) {
	return lang_ru() ? "Ещё раз" : "Play again";
}

static const char *txt_win_title(void) {
	return lang_ru() ? "Победа" : "You win";
}

static const char *txt_win_body(void) {
	return lang_ru() ? "Плитка 2048 собрана" : "You reached 2048";
}

static const char *txt_keep(void) {
	return lang_ru() ? "Продолжить" : "Keep going";
}

static const char *txt_new(void) {
	return lang_ru() ? "Новая игра" : "New game";
}

// ---------------------------------------------------------------------------
// tile look
// ---------------------------------------------------------------------------

static lv_color_t tile_bg(int value) {
	switch (value) {
	case 2:
		return lv_color_hex(0xeee4da);
	case 4:
		return lv_color_hex(0xede0c8);
	case 8:
		return lv_color_hex(0xf2b179);
	case 16:
		return lv_color_hex(0xf59563);
	case 32:
		return lv_color_hex(0xf67c5f);
	case 64:
		return lv_color_hex(0xf65e3b);
	case 128:
		return lv_color_hex(0xedcf72);
	case 256:
		return lv_color_hex(0xedcc61);
	case 512:
		return lv_color_hex(0xedc850);
	case 1024:
		return lv_color_hex(0xedc53f);
	case 2048:
		return lv_color_hex(0xedc22e);
	default:
		return lv_color_hex(0x3c3a32);
	}
}

static lv_color_t tile_fg(int value) {
	return value <= 4 ? lv_color_hex(0x776e65) : lv_color_hex(0xf9f6f2);
}

static const lv_font_t *tile_font(int value) {
	if (value >= 1000) {
		return &font_ui_22;
	}
	if (value >= 100) {
		return &font_ui_26;
	}
	return &font_ui_36_bold;
}

static void set_scale_cb(void *obj, int32_t v) {
	lv_obj_set_style_transform_scale((lv_obj_t *)obj, v, 0);
}

static void cell_xy(int r, int c, int *x, int *y) {
	*x = BOARD_PAD + c * (cell_px + GAP);
	*y = BOARD_PAD + r * (cell_px + GAP);
}

static void style_tile(tile_t *t) {
	char buf[8];
	snprintf(buf, sizeof(buf), "%d", t->value);
	lv_label_set_text(t->label, buf);
	lv_obj_set_style_text_font(t->label, tile_font(t->value), 0);
	lv_obj_set_style_text_color(t->label, tile_fg(t->value), 0);
	lv_obj_set_style_bg_color(t->box, tile_bg(t->value), 0);
}

static tile_t *tile_alloc(void) {
	for (int i = 0; i < TILE_POOL; i++) {
		if (!tiles[i].live) {
			return &tiles[i];
		}
	}
	return NULL;
}

static tile_t *tile_at(int r, int c) {
	for (int i = 0; i < TILE_POOL; i++) {
		if (tiles[i].live && !tiles[i].dying && tiles[i].r == r && tiles[i].c == c) {
			return &tiles[i];
		}
	}
	return NULL;
}

static void tile_place(tile_t *t, int r, int c) {
	int x, y;
	cell_xy(r, c, &x, &y);
	t->r = r;
	t->c = c;
	lv_obj_set_pos(t->box, x, y);
}

static void tile_show(tile_t *t, bool show) {
	lv_obj_set_hidden(t->box, !show);
}

static void tile_reset_xform(tile_t *t) {
	lv_obj_set_style_transform_scale(t->box, LV_SCALE_NONE, 0);
	lv_obj_set_style_opa(t->box, LV_OPA_COVER, 0);
}

static void tile_kill(tile_t *t) {
	if (!t) {
		return;
	}
	lv_anim_delete(t->box, NULL);
	tile_show(t, false);
	tile_reset_xform(t);
	t->live = false;
	t->dying = false;
	t->value = 0;
	t->r = t->c = -1;
}

static tile_t *tile_spawn_at(int r, int c, int value, bool with_pop) {
	tile_t *t = tile_alloc();
	if (!t) {
		return NULL;
	}
	t->live = true;
	t->dying = false;
	t->value = value;
	tile_place(t, r, c);
	style_tile(t);
	tile_reset_xform(t);
	tile_show(t, true);
	lv_obj_move_foreground(t->box);

	if (with_pop) {
		lv_obj_set_style_transform_pivot_x(t->box, cell_px / 2, 0);
		lv_obj_set_style_transform_pivot_y(t->box, cell_px / 2, 0);
		lv_obj_set_style_transform_scale(t->box, 1, 0);

		lv_anim_t a;
		lv_anim_init(&a);
		lv_anim_set_var(&a, t->box);
		lv_anim_set_exec_cb(&a, set_scale_cb);
		lv_anim_set_values(&a, 1, LV_SCALE_NONE);
		lv_anim_set_duration(&a, SPAWN_MS);
		lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
		lv_anim_start(&a);
	}
	return t;
}

// ---------------------------------------------------------------------------
// board rules
// ---------------------------------------------------------------------------

static void grid_clear(void) {
	memset(grid, 0, sizeof(grid));
}

static int empty_count(void) {
	int n = 0;
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			if (!grid[r][c]) {
				n++;
			}
		}
	}
	return n;
}

static bool spawn_random(bool with_pop) {
	int empties = empty_count();
	if (empties <= 0) {
		return false;
	}
	int pick = rand() % empties;
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			if (grid[r][c]) {
				continue;
			}
			if (pick-- == 0) {
				int v = (rand() % 10 == 0) ? 4 : 2;
				grid[r][c] = v;
				tile_spawn_at(r, c, v, with_pop);
				return true;
			}
		}
	}
	return false;
}

static bool can_move(void) {
	if (empty_count() > 0) {
		return true;
	}
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			int v = grid[r][c];
			if (c + 1 < N && grid[r][c + 1] == v) {
				return true;
			}
			if (r + 1 < N && grid[r + 1][c] == v) {
				return true;
			}
		}
	}
	return false;
}

static bool board_has_2048(void) {
	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			if (grid[r][c] >= 2048) {
				return true;
			}
		}
	}
	return false;
}

// Map a packed line index back to a board cell for the current swipe axis.
static void line_cell(int line_i, int pos, int dr, int dc, int *r, int *c) {
	if (dr != 0) {
		*c = line_i;
		*r = (dr < 0) ? pos : (N - 1 - pos);
	} else {
		*r = line_i;
		*c = (dc < 0) ? pos : (N - 1 - pos);
	}
}

static void push_slide(tile_t *t, int to_r, int to_c, int end_value, bool kill) {
	if (!t || slide_count >= TILE_POOL) {
		return;
	}
	slides[slide_count].tile = t;
	slides[slide_count].to_r = to_r;
	slides[slide_count].to_c = to_c;
	slides[slide_count].end_value = end_value;
	slides[slide_count].merge_kill = kill;
	slide_count++;
}

// Slide one line toward index 0. `vals` is the line in traversal order
// (index 0 = the side we slide into). Records slides for every live tile.
static int slide_line(int line_i, int dr, int dc, int vals[N], tile_t *srcs[N]) {
	int gained = 0;

	typedef struct {
		int value;
		tile_t *a;
		tile_t *b; // merge partner, or NULL
	} packed_t;

	packed_t packed[N];
	int n = 0;
	for (int i = 0; i < N; i++) {
		if (!vals[i]) {
			continue;
		}
		if (n > 0 && packed[n - 1].value == vals[i] && !packed[n - 1].b) {
			packed[n - 1].value *= 2;
			packed[n - 1].b = srcs[i];
			gained += packed[n - 1].value;
		} else {
			packed[n].value = vals[i];
			packed[n].a = srcs[i];
			packed[n].b = NULL;
			n++;
		}
	}

	int out[N] = {0};
	for (int i = 0; i < n; i++) {
		out[i] = packed[i].value;
		int tr, tc;
		line_cell(line_i, i, dr, dc, &tr, &tc);
		if (packed[i].a) {
			push_slide(packed[i].a, tr, tc, packed[i].b ? packed[i].value : 0, false);
		}
		if (packed[i].b) {
			packed[i].b->dying = true;
			push_slide(packed[i].b, tr, tc, 0, true);
		}
	}
	memcpy(vals, out, sizeof(out));
	return gained;
}

// ---------------------------------------------------------------------------
// layout
// ---------------------------------------------------------------------------

static void layout_board(void) {
	int side = frame.field_w < frame.field_h ? frame.field_w : frame.field_h;
	if (side > frame.field_h - SC(4)) {
		side = frame.field_h - SC(4);
	}
	if (side < SC(160)) {
		side = SC(160);
	}
	cell_px = (side - 2 * BOARD_PAD - (N - 1) * GAP) / N;
	int board_side = 2 * BOARD_PAD + N * cell_px + (N - 1) * GAP;
	board_x = (frame.field_w - board_side) / 2;
	board_y = (frame.field_h - board_side) / 2;
	if (board_x < 0) {
		board_x = 0;
	}
	if (board_y < 0) {
		board_y = 0;
	}

	lv_obj_set_pos(board, board_x, board_y);
	lv_obj_set_size(board, board_side, board_side);
	lv_obj_set_style_bg_color(board, lv_color_hex(0xbbada0), 0);
	lv_obj_set_style_radius(board, SC(12), 0);

	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			int x, y;
			cell_xy(r, c, &x, &y);
			lv_obj_set_pos(slots[r][c], x, y);
			lv_obj_set_size(slots[r][c], cell_px, cell_px);
			lv_obj_set_style_bg_color(slots[r][c], lv_color_hex(0xcdc1b4), 0);
			lv_obj_set_style_radius(slots[r][c], SC(8), 0);
		}
	}

	for (int i = 0; i < TILE_POOL; i++) {
		lv_obj_set_size(tiles[i].box, cell_px, cell_px);
		lv_obj_set_style_radius(tiles[i].box, SC(8), 0);
		lv_obj_set_style_transform_pivot_x(tiles[i].box, cell_px / 2, 0);
		lv_obj_set_style_transform_pivot_y(tiles[i].box, cell_px / 2, 0);
		if (tiles[i].live) {
			tile_place(&tiles[i], tiles[i].r, tiles[i].c);
			style_tile(&tiles[i]);
		}
	}
}

static lv_obj_t *make_slot(void) {
	lv_obj_t *o = lv_obj_create(board);
	lv_obj_remove_style_all(o);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_clickable(o, false);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	return o;
}

static void make_tile_widgets(tile_t *t) {
	t->box = lv_obj_create(board);
	lv_obj_remove_style_all(t->box);
	lv_obj_set_scrollable(t->box, false);
	lv_obj_set_clickable(t->box, false);
	lv_obj_set_style_bg_opa(t->box, LV_OPA_COVER, 0);
	t->label = lv_label_create(t->box);
	lv_obj_set_style_text_align(t->label, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_center(t->label);
	tile_show(t, false);
	t->live = false;
	t->dying = false;
	t->r = t->c = -1;
	t->value = 0;
}

// ---------------------------------------------------------------------------
// game flow
// ---------------------------------------------------------------------------

static void clear_tiles(void) {
	for (int i = 0; i < TILE_POOL; i++) {
		tile_kill(&tiles[i]);
	}
}

static void show_over(void) {
	over = true;
	game_frame_save_best(&frame);
	game_frame_show_overlay(&frame, txt_over_title(), txt_over_body(), txt_again(), NULL);
}

static void show_win(void) {
	won_once = true;
	game_frame_save_best(&frame);
	game_frame_show_overlay(&frame, txt_win_title(), txt_win_body(), txt_keep(), txt_new());
}

static void after_move_settle(void);

static void pop_merged(tile_t *t) {
	if (!t || !t->live) {
		return;
	}
	lv_obj_set_style_transform_pivot_x(t->box, cell_px / 2, 0);
	lv_obj_set_style_transform_pivot_y(t->box, cell_px / 2, 0);

	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, t->box);
	lv_anim_set_exec_cb(&a, set_scale_cb);
	lv_anim_set_values(&a, (int32_t)(LV_SCALE_NONE * 11 / 10), LV_SCALE_NONE);
	lv_anim_set_duration(&a, MERGE_POP_MS);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_start(&a);
}

static void slide_done(lv_anim_t *a) {
	(void)a;
	if (--pending_anims > 0) {
		return;
	}
	after_move_settle();
}

static void after_move_settle(void) {
	for (int i = 0; i < slide_count; i++) {
		slide_t *s = &slides[i];
		tile_t *t = s->tile;
		if (!t) {
			continue;
		}
		if (s->merge_kill) {
			tile_kill(t);
			continue;
		}
		t->r = s->to_r;
		t->c = s->to_c;
		tile_place(t, t->r, t->c);
		if (s->end_value > 0) {
			t->value = s->end_value;
			style_tile(t);
			pop_merged(t); // leaves its own scale anim running
		} else {
			tile_reset_xform(t);
		}
	}
	slide_count = 0;

	if (spawn_v > 0) {
		grid[spawn_r][spawn_c] = spawn_v;
		tile_spawn_at(spawn_r, spawn_c, spawn_v, true);
		spawn_v = 0;
	}

	animating = false;

	if (!won_once && board_has_2048()) {
		show_win();
		return;
	}
	if (!can_move()) {
		show_over();
	}
}

static void start_slides(void) {
	pending_anims = 0;
	for (int i = 0; i < slide_count; i++) {
		slide_t *s = &slides[i];
		tile_t *t = s->tile;
		int x, y;
		cell_xy(s->to_r, s->to_c, &x, &y);
		int from_x = lv_obj_get_x(t->box);
		int from_y = lv_obj_get_y(t->box);
		if (from_x == x && from_y == y) {
			continue;
		}
		pending_anims += 2;

		lv_anim_t ax;
		lv_anim_init(&ax);
		lv_anim_set_var(&ax, t->box);
		lv_anim_set_exec_cb(&ax, (lv_anim_exec_xcb_t)lv_obj_set_x);
		lv_anim_set_values(&ax, from_x, x);
		lv_anim_set_duration(&ax, MOVE_MS);
		lv_anim_set_path_cb(&ax, lv_anim_path_ease_out);
		lv_anim_set_completed_cb(&ax, slide_done);
		lv_anim_start(&ax);

		lv_anim_t ay;
		lv_anim_init(&ay);
		lv_anim_set_var(&ay, t->box);
		lv_anim_set_exec_cb(&ay, (lv_anim_exec_xcb_t)lv_obj_set_y);
		lv_anim_set_values(&ay, from_y, y);
		lv_anim_set_duration(&ay, MOVE_MS);
		lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
		lv_anim_set_completed_cb(&ay, slide_done);
		lv_anim_start(&ay);
	}
	if (pending_anims == 0) {
		after_move_settle();
	}
}

static bool try_move(lv_dir_t dir) {
	int dr = 0, dc = 0;
	if (dir == LV_DIR_LEFT) {
		dc = -1;
	} else if (dir == LV_DIR_RIGHT) {
		dc = 1;
	} else if (dir == LV_DIR_TOP) {
		dr = -1;
	} else if (dir == LV_DIR_BOTTOM) {
		dr = 1;
	} else {
		return false;
	}

	int next[N][N];
	memcpy(next, grid, sizeof(grid));
	slide_count = 0;
	int gained = 0;
	bool changed = false;

	for (int i = 0; i < N; i++) {
		int vals[N];
		tile_t *srcs[N];
		int before[N];
		for (int j = 0; j < N; j++) {
			int r, c;
			line_cell(i, j, dr, dc, &r, &c);
			vals[j] = next[r][c];
			before[j] = vals[j];
			srcs[j] = vals[j] ? tile_at(r, c) : NULL;
		}
		gained += slide_line(i, dr, dc, vals, srcs);
		for (int j = 0; j < N; j++) {
			int r, c;
			line_cell(i, j, dr, dc, &r, &c);
			next[r][c] = vals[j];
			if (before[j] != vals[j]) {
				changed = true;
			}
		}
	}

	if (!changed) {
		slide_count = 0;
		// Undo dying marks from a dry run that did not actually change the board.
		for (int i = 0; i < TILE_POOL; i++) {
			tiles[i].dying = false;
		}
		return false;
	}

	memcpy(grid, next, sizeof(grid));
	if (gained) {
		game_frame_add_score(&frame, gained);
	}

	spawn_v = 0;
	int empties = empty_count();
	if (empties > 0) {
		int pick = rand() % empties;
		for (int r = 0; r < N; r++) {
			for (int c = 0; c < N; c++) {
				if (grid[r][c]) {
					continue;
				}
				if (pick-- == 0) {
					spawn_r = r;
					spawn_c = c;
					spawn_v = (rand() % 10 == 0) ? 4 : 2;
					goto spawn_picked;
				}
			}
		}
	}
spawn_picked:

	animating = true;
	start_slides();
	return true;
}

static void new_game(void) {
	for (int i = 0; i < TILE_POOL; i++) {
		lv_anim_delete(tiles[i].box, NULL);
	}
	animating = false;
	pending_anims = 0;
	slide_count = 0;
	spawn_v = 0;
	over = false;
	won_once = false;
	keep_going = false;
	clear_tiles();
	grid_clear();
	game_frame_set_score(&frame, 0);
	spawn_random(false);
	spawn_random(false);
	game_frame_set_hint(&frame, txt_hint());
}

static void card_primary(void) {
	if (won_once && !over && !keep_going) {
		keep_going = true;
		game_frame_hide_overlay(&frame);
		game_frame_set_hint(&frame, txt_hint());
		return;
	}
	game_frame_hide_overlay(&frame);
	new_game();
}

static void card_secondary(void) {
	game_frame_hide_overlay(&frame);
	new_game();
}

static void swipe_cb(lv_dir_t dir, const lv_point_t *p) {
	(void)p;
	if (animating || over || frame.overlay_up) {
		return;
	}
	try_move(dir);
}

static void show_cb(void) {
	srand((unsigned)time(NULL) ^ (unsigned)lv_tick_get());
	game_frame_load_best(&frame);
	layout_board();
	new_game();
	game_frame_show_overlay(&frame, "2048", txt_ready_body(), txt_start(), NULL);
}

static void hide_cb(void) {
	game_frame_save_best(&frame);
	for (int i = 0; i < TILE_POOL; i++) {
		lv_anim_delete(tiles[i].box, NULL);
	}
	animating = false;
	pending_anims = 0;
}

static void theme_cb(void) {
	layout_board();
	lv_obj_move_foreground(frame.overlay);
}

void t2048_init(gui_config_t *cfg) {
	static const game_hooks_t hooks = {
		.on_show = show_cb,
		.on_hide = hide_cb,
		.on_swipe = swipe_cb,
		.on_theme = theme_cb,
	};

	frame.hooks = &hooks;
	frame.section = SECTION;
	frame.title = "2048";
	frame.screen = t2048_screen;
	frame.on_overlay_primary = card_primary;
	frame.on_overlay_secondary = card_secondary;
	game_frame_build(&frame, cfg);

	board = lv_obj_create(frame.field);
	lv_obj_remove_style_all(board);
	lv_obj_set_scrollable(board, false);
	lv_obj_set_clickable(board, false);
	lv_obj_set_style_bg_opa(board, LV_OPA_COVER, 0);

	for (int r = 0; r < N; r++) {
		for (int c = 0; c < N; c++) {
			slots[r][c] = make_slot();
		}
	}
	for (int i = 0; i < TILE_POOL; i++) {
		make_tile_widgets(&tiles[i]);
	}

	layout_board();
	lv_obj_move_foreground(frame.overlay);
	game_frame_set_hint(&frame, txt_hint());
}

void t2048_stop(void) {
	for (int i = 0; i < TILE_POOL; i++) {
		if (tiles[i].box) {
			lv_anim_delete(tiles[i].box, NULL);
		}
	}
	game_frame_release(&frame);

	board = NULL;
	memset(slots, 0, sizeof(slots));
	memset(tiles, 0, sizeof(tiles));
	grid_clear();
	t2048_screen = NULL;
	animating = false;
	pending_anims = 0;
	slide_count = 0;
	spawn_v = 0;
	over = false;
	won_once = false;
	keep_going = false;
}
