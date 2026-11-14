#include "ttt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/icons_select.h"
#include "src/gui/shell/scale.h"
#include "src/system/core/lang.h"

#include "games.h"
#include "gstate.h"
#include "tttsettings.h"

// Tic Tac Toe on the games frame.
//
// Three difficulties for O (picked on tttsettings, opened from the header gear):
//   Easy   — random empty cell
//   Medium — win / block / fork / block-fork, then centre / corner / side
//   Hard   — full minimax (perfect play: win or draw only)

lv_obj_t *ttt_screen;

enum { CELL_EMPTY = 0, CELL_X = 1, CELL_O = 2 };

static game_frame_t frame;

static lv_obj_t *cell_box[9];
static lv_obj_t *cell_mark[9];
static int board[9];
static bool playing;
static int difficulty = TTT_DIFF_MEDIUM;
static bool seeded;

static const int WIN_LINES[8][3] = {
	{0, 1, 2}, {3, 4, 5}, {6, 7, 8},
	{0, 3, 6}, {1, 4, 7}, {2, 5, 8},
	{0, 4, 8}, {2, 4, 6},
};

// Preference order after win/block on Medium.
static const int MEDIUM_PREF[9] = {4, 0, 2, 6, 8, 1, 3, 5, 7};

static bool ru(void) {
	const char *name = lang_current();
	return name && strcmp(name, "Russian") == 0;
}

static const char *diff_name(int d) {
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

static void ensure_seed(void) {
	if (seeded) {
		return;
	}
	srand((unsigned)time(NULL));
	seeded = true;
}

static void load_difficulty(void) {
	int d = gstate_get_int("difficulty", TTT_DIFF_MEDIUM);
	if (d < TTT_DIFF_EASY || d >= TTT_DIFF_COUNT) {
		d = TTT_DIFF_MEDIUM;
	}
	difficulty = d;
}

static void save_difficulty(void) {
	gstate_set_int("difficulty", difficulty);
}

int ttt_get_difficulty(void) {
	return difficulty;
}

static void paint_level(void) {
	game_frame_set_level(&frame, diff_name(difficulty));
}

void ttt_set_difficulty(int d) {
	if (d < TTT_DIFF_EASY || d >= TTT_DIFF_COUNT) {
		return;
	}
	difficulty = d;
	save_difficulty();
	paint_level();
}

static int winner(const int b[9]) {
	for (int i = 0; i < 8; i++) {
		int a = b[WIN_LINES[i][0]];
		if (a != CELL_EMPTY && a == b[WIN_LINES[i][1]] && a == b[WIN_LINES[i][2]]) {
			return a;
		}
	}
	return CELL_EMPTY;
}

static bool board_full(const int b[9]) {
	for (int i = 0; i < 9; i++) {
		if (b[i] == CELL_EMPTY) {
			return false;
		}
	}
	return true;
}

static int count_empty(const int b[9]) {
	int n = 0;
	for (int i = 0; i < 9; i++) {
		if (b[i] == CELL_EMPTY) {
			n++;
		}
	}
	return n;
}

static int random_empty(void) {
	ensure_seed();
	int n = count_empty(board);
	if (n <= 0) {
		return -1;
	}
	int pick = rand() % n;
	for (int i = 0; i < 9; i++) {
		if (board[i] != CELL_EMPTY) {
			continue;
		}
		if (pick-- == 0) {
			return i;
		}
	}
	return -1;
}

// Cell that completes a line for `who`, or -1.
static int find_winning_move(int who) {
	for (int i = 0; i < 9; i++) {
		if (board[i] != CELL_EMPTY) {
			continue;
		}
		board[i] = who;
		int w = winner(board);
		board[i] = CELL_EMPTY;
		if (w == who) {
			return i;
		}
	}
	return -1;
}

// How many immediate winning replies `who` would have from the current board.
static int threat_count(int who) {
	int n = 0;
	for (int i = 0; i < 9; i++) {
		if (board[i] != CELL_EMPTY) {
			continue;
		}
		board[i] = who;
		if (winner(board) == who) {
			n++;
		}
		board[i] = CELL_EMPTY;
	}
	return n;
}

// Empty cell that gives `who` two or more winning threats at once (a fork).
static int find_fork_move(int who) {
	for (int i = 0; i < 9; i++) {
		if (board[i] != CELL_EMPTY) {
			continue;
		}
		board[i] = who;
		int threats = 0;
		if (winner(board) != who) {
			threats = threat_count(who);
		}
		board[i] = CELL_EMPTY;
		if (threats >= 2) {
			return i;
		}
	}
	return -1;
}

static int medium_o_move(void) {
	int win = find_winning_move(CELL_O);
	if (win >= 0) {
		return win;
	}
	int block = find_winning_move(CELL_X);
	if (block >= 0) {
		return block;
	}
	int fork = find_fork_move(CELL_O);
	if (fork >= 0) {
		return fork;
	}
	int stop_fork = find_fork_move(CELL_X);
	if (stop_fork >= 0) {
		return stop_fork;
	}
	// Opposite corner if X sits in a corner.
	static const int opp[9] = {8, -1, 6, -1, -1, -1, 2, -1, 0};
	for (int c = 0; c < 9; c++) {
		if (opp[c] < 0) {
			continue;
		}
		if (board[c] == CELL_X && board[opp[c]] == CELL_EMPTY) {
			return opp[c];
		}
	}
	for (int p = 0; p < 9; p++) {
		int i = MEDIUM_PREF[p];
		if (board[i] == CELL_EMPTY) {
			return i;
		}
	}
	return -1;
}

// Maximizing for O, minimizing for X. Returns -1 / 0 / +1 from O's view.
static int minimax(int b[9], bool o_turn) {
	int w = winner(b);
	if (w == CELL_O) {
		return 1;
	}
	if (w == CELL_X) {
		return -1;
	}
	if (board_full(b)) {
		return 0;
	}

	if (o_turn) {
		int best = -2;
		for (int i = 0; i < 9; i++) {
			if (b[i] != CELL_EMPTY) {
				continue;
			}
			b[i] = CELL_O;
			int v = minimax(b, false);
			b[i] = CELL_EMPTY;
			if (v > best) {
				best = v;
			}
		}
		return best;
	}

	int best = 2;
	for (int i = 0; i < 9; i++) {
		if (b[i] != CELL_EMPTY) {
			continue;
		}
		b[i] = CELL_X;
		int v = minimax(b, true);
		b[i] = CELL_EMPTY;
		if (v < best) {
			best = v;
		}
	}
	return best;
}

static int hard_o_move(void) {
	int best_score = -2;
	int best_i = -1;
	for (int i = 0; i < 9; i++) {
		if (board[i] != CELL_EMPTY) {
			continue;
		}
		board[i] = CELL_O;
		int v = minimax(board, false);
		board[i] = CELL_EMPTY;
		if (v > best_score) {
			best_score = v;
			best_i = i;
		}
	}
	return best_i;
}

static int choose_o_move(void) {
	switch (difficulty) {
	case TTT_DIFF_EASY:
		return random_empty();
	case TTT_DIFF_HARD:
		return hard_o_move();
	default:
		return medium_o_move();
	}
}

static lv_color_t board_paper(void) { return game_frame_field_bg(false); }

// X cool teal, O warm coral — swapped so the player mark reads cooler on paper.
static lv_color_t mark_x(void) { return lv_color_make(0x1e, 0x7a, 0x8c); }
static lv_color_t mark_o(void) { return lv_color_make(0xc4, 0x3a, 0x2e); }

static lv_color_t cell_face_empty(void) { return lv_color_make(0xe4, 0xe0, 0xd8); }
static lv_color_t cell_face_x(void) { return lv_color_make(0xd4, 0xe6, 0xe8); }
static lv_color_t cell_face_o(void) { return lv_color_make(0xf3, 0xdc, 0xd6); }

static void paint_cell(int i) {
	const char *mark = "";
	lv_color_t face = cell_face_empty();
	lv_color_t ink = game_frame_ink(false);

	if (board[i] == CELL_X) {
		mark = "X";
		face = cell_face_x();
		ink = mark_x();
	} else if (board[i] == CELL_O) {
		mark = "O";
		face = cell_face_o();
		ink = mark_o();
	}

	lv_label_set_text(cell_mark[i], mark);
	lv_obj_set_style_bg_color(cell_box[i], face, 0);
	lv_obj_set_style_text_color(cell_mark[i], ink, 0);
}

static void paint_board(void) {
	for (int i = 0; i < 9; i++) {
		paint_cell(i);
	}
}

static void layout_board(void) {
	if (!frame.field || frame.field_w <= 0 || frame.field_h <= 0) {
		return;
	}

	lv_obj_set_style_bg_color(frame.field, board_paper(), 0);
	lv_obj_set_style_bg_opa(frame.field, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(frame.field, SC(12), 0);

	int gap = frame.u * 2;
	if (gap < 4) {
		gap = 4;
	}
	int pad = gap * 2;
	int side = frame.field_w < frame.field_h ? frame.field_w : frame.field_h;
	side -= pad * 2;
	if (side < SC(180)) {
		side = frame.field_w < frame.field_h ? frame.field_w - pad : frame.field_h - pad;
	}
	if (side < 3) {
		return;
	}
	int cell = (side - gap * 2) / 3;
	if (cell < SC(48)) {
		cell = SC(48);
	}
	int board_w = cell * 3 + gap * 2;
	int ox = (frame.field_w - board_w) / 2;
	int oy = (frame.field_h - board_w) / 2;
	if (ox < 0) {
		ox = pad / 2;
	}
	if (oy < 0) {
		oy = pad / 2;
	}

	for (int i = 0; i < 9; i++) {
		int r = i / 3;
		int c = i % 3;
		lv_obj_set_size(cell_box[i], cell, cell);
		lv_obj_set_pos(cell_box[i], ox + c * (cell + gap), oy + r * (cell + gap));
		lv_obj_set_style_radius(cell_box[i], SC(10), 0);
		lv_obj_set_style_bg_opa(cell_box[i], LV_OPA_COVER, 0);
		lv_obj_center(cell_mark[i]);
		lv_obj_set_style_text_font(cell_mark[i], &font_ui_36_bold, 0);
		paint_cell(i);
	}
}

static void set_hint_your_turn(void) {
	game_frame_set_hint(&frame, ru() ? "Ваш ход (X)" : "Your turn (X)");
}

static void clear_board(void) {
	memset(board, 0, sizeof(board));
	paint_board();
}

static void fill_ready_overlay(void);
static void fill_end_overlay(int w);

static void end_round(int w) {
	playing = false;
	if (w == CELL_X) {
		// Win streak: consecutive wins without a loss or a draw.
		game_frame_add_score(&frame, 1);
		game_frame_note_best(&frame, frame.score);
	} else {
		// Loss or draw breaks the streak.
		game_frame_set_score(&frame, 0);
	}
	fill_end_overlay(w);
}

static void place(int i, int who) {
	board[i] = who;
	paint_cell(i);
}

static void after_move(void) {
	int w = winner(board);
	if (w != CELL_EMPTY) {
		end_round(w);
		return;
	}
	if (board_full(board)) {
		end_round(CELL_EMPTY);
		return;
	}
	set_hint_your_turn();
}

static void ai_move(void) {
	int i = choose_o_move();
	if (i < 0) {
		end_round(CELL_EMPTY);
		return;
	}
	place(i, CELL_O);
	after_move();
}

static void start_round(void) {
	game_frame_hide_overlay(&frame);
	clear_board();
	playing = true;
	set_hint_your_turn();
}

static void fill_ready_overlay(void) {
	playing = false;
	clear_board();
	const char *title = ru() ? "Крестики-нолики" : "Tic Tac Toe";
	char body[96];
	if (ru()) {
		snprintf(body, sizeof(body), "Вы — X. Сложность: %s.\nШестерёнка сверху — сменить.",
			 diff_name(difficulty));
	} else {
		snprintf(body, sizeof(body), "You are X. Level: %s.\nGear up top changes it.",
			 diff_name(difficulty));
	}
	const char *play = ru() ? "Играть" : "Play";
	game_frame_set_hint(&frame, ru() ? "Коснитесь, чтобы начать" : "Tap to start");
	game_frame_show_overlay(&frame, title, body, play, NULL);
}

static void fill_end_overlay(int w) {
	const char *title;
	char body[96];
	const char *again = ru() ? "Ещё раз" : "Play again";

	if (w == CELL_X) {
		title = ru() ? "Победа" : "You win";
		if (ru()) {
			snprintf(body, sizeof(body), "Три в ряд. Уровень: %s.", diff_name(difficulty));
		} else {
			snprintf(body, sizeof(body), "Three in a row. Level: %s.", diff_name(difficulty));
		}
	} else if (w == CELL_O) {
		title = ru() ? "Поражение" : "You lose";
		if (ru()) {
			snprintf(body, sizeof(body), "Устройство сыграло лучше (%s).", diff_name(difficulty));
		} else {
			snprintf(body, sizeof(body), "The device played better (%s).", diff_name(difficulty));
		}
	} else {
		title = ru() ? "Ничья" : "Draw";
		if (ru()) {
			snprintf(body, sizeof(body), "Доска заполнена. Уровень: %s.", diff_name(difficulty));
		} else {
			snprintf(body, sizeof(body), "The board is full. Level: %s.", diff_name(difficulty));
		}
	}
	game_frame_set_hint(&frame, title);
	game_frame_show_overlay(&frame, title, body, again, NULL);
}

static void show_ready_card(void) {
	fill_ready_overlay();
}

static int cell_at(const lv_point_t *p) {
	for (int i = 0; i < 9; i++) {
		lv_area_t a;
		lv_obj_get_coords(cell_box[i], &a);
		if (p->x >= a.x1 && p->x <= a.x2 && p->y >= a.y1 && p->y <= a.y2) {
			return i;
		}
	}
	return -1;
}

static void tap_cb(const lv_point_t *p) {
	if (!playing) {
		return;
	}
	int i = cell_at(p);
	if (i < 0 || board[i] != CELL_EMPTY) {
		return;
	}
	place(i, CELL_X);
	int w = winner(board);
	if (w != CELL_EMPTY) {
		end_round(w);
		return;
	}
	if (board_full(board)) {
		end_round(CELL_EMPTY);
		return;
	}
	game_frame_set_hint(&frame, ru() ? "Ход устройства…" : "Device's turn…");
	ai_move();
}

static void show_cb(void) {
	layout_board();
	paint_board();
	game_frame_load_best(&frame);
	load_difficulty();
	paint_level();
	if (!playing) {
		int w = winner(board);
		if (frame.overlay_up && (w != CELL_EMPTY || board_full(board))) {
			fill_end_overlay(w);
		} else {
			show_ready_card();
		}
	}
}

static void hide_cb(void) {
	game_frame_save_best(&frame);
	save_difficulty();
}

static void theme_cb(void) {
	layout_board();
	paint_board();
}

static void card_primary(void) {
	start_round();
}

static void settings_pressed(void) {
	tttsettings_open();
}

void ttt_init(gui_config_t *cfg) {
	static const game_hooks_t hooks = {
		.on_show = show_cb,
		.on_hide = hide_cb,
		.on_tap = tap_cb,
		.on_theme = theme_cb,
	};

	ensure_seed();
	load_difficulty();

	frame.hooks = &hooks;
	frame.section = "tictactoe";
	frame.title = ru() ? "Крестики-нолики" : "Tic Tac Toe";
	frame.screen = ttt_screen;
	frame.on_overlay_primary = card_primary;
	frame.on_overlay_secondary = NULL;
	game_frame_build(&frame, cfg);
	// Small monochrome glyph (icon_menu_settings is an 85 px menu tile and
	// vanishes inside the header button).
	game_frame_add_header_button(&frame, &icon_music_settings, settings_pressed);
	paint_level();

	for (int i = 0; i < 9; i++) {
		cell_box[i] = lv_obj_create(frame.field);
		lv_obj_remove_style_all(cell_box[i]);
		lv_obj_set_scrollable(cell_box[i], false);
		lv_obj_set_clickable(cell_box[i], false);
		lv_obj_set_style_bg_color(cell_box[i], cell_face_empty(), 0);
		lv_obj_set_style_bg_opa(cell_box[i], LV_OPA_COVER, 0);

		cell_mark[i] = lv_label_create(cell_box[i]);
		lv_label_set_text(cell_mark[i], "");
		lv_obj_set_style_text_align(cell_mark[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_font(cell_mark[i], &font_ui_36_bold, 0);
	}

	lv_obj_move_foreground(frame.overlay);
	layout_board();
	paint_board();
	show_ready_card();
}

void ttt_open(void) {
	game_frame_open(&frame);
}

void ttt_stop(void) {
	save_difficulty();
	game_frame_release(&frame);
	for (int i = 0; i < 9; i++) {
		cell_box[i] = NULL;
		cell_mark[i] = NULL;
	}
	playing = false;
}
