#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/addon/addon.h"
#include "src/gui/fonts/fonts.h"
#include "src/gui/shell/scale.h"
#include "src/gui/shell/settingsrow.h"
#include "src/gui/shell/switcher.h"
#include "src/gui/shell/theme.h"
#include "src/system/core/lang.h"

#include "lvgl/lvgl.h"

// Four-function calculator for More: AC/C, backspace, phone-style %,
// repeat '=', locale decimal, digit cap / scientific display, op highlight.
// Title / error / clear labels follow lang_current(); glyphs are neutral.

#define BUF_MAX 32
#define EXPR_MAX 96
#define MAX_DIGITS 12
#define KEY_GAP SC(10)
#define KEY_RADIUS SC(16)
#define DISPLAY_H SC(130)

enum {
	KEY_CLEAR = 100,
	KEY_BACKSPACE,
	KEY_SIGN,
	KEY_PERCENT,
	KEY_DIV,
	KEY_MUL,
	KEY_SUB,
	KEY_ADD,
	KEY_DOT,
	KEY_EQ,
	KEY_0 = 0,
	KEY_1 = 1,
	KEY_2 = 2,
	KEY_3 = 3,
	KEY_4 = 4,
	KEY_5 = 5,
	KEY_6 = 6,
	KEY_7 = 7,
	KEY_8 = 8,
	KEY_9 = 9,
};

static lv_obj_t *calc_screen;
static lv_obj_t *expr_label;
static lv_obj_t *display_label;
static lv_obj_t *clear_label;
static lv_obj_t *op_btns[4]; // ÷ × − +, in that order
static gui_config_t *calc_cfg;

static char buf[BUF_MAX];
static char lhs[BUF_MAX];
static char eq_expr[EXPR_MAX];
static double acc;
static int pending;
static int last_op;
static double last_rhs;
static bool have_repeat;
static bool fresh;
static bool errored;
static bool showing_eq;

static const char *key_label(int key);
static void refresh_display(void);
static void refresh_op_highlight(void);

static bool lang_ru(void) {
	const char *name = lang_current();
	return name && strcmp(name, "Russian") == 0;
}

static const char *t_title(void) {
	return lang_ru() ? "Калькулятор" : "Calculator";
}

static const char *t_error(void) {
	return lang_ru() ? "Ошибка" : "Error";
}

static char decimal_char(void) {
	return lang_ru() ? ',' : '.';
}

static int digit_count(const char *s) {
	int n = 0;
	for (; s && *s; s++) {
		if (*s >= '0' && *s <= '9') {
			n++;
		}
	}
	return n;
}

// Internal text uses '.'; the labels show ',' when the UI is Russian.
static void localise_number(const char *src, char *dst, size_t size) {
	if (!src || !dst || size == 0) {
		return;
	}
	char dec = decimal_char();
	size_t i = 0;
	for (; src[i] && i + 1 < size; i++) {
		dst[i] = (src[i] == '.' && dec == ',') ? ',' : src[i];
	}
	dst[i] = '\0';
}

static void format_number(double v, char *out, size_t size) {
	if (!isfinite(v)) {
		snprintf(out, size, "%s", t_error());
		return;
	}

	double a = fabs(v);
	char tmp[64];

	// Phone-calc range: beyond ~12 digits or tiny fractions, use scientific.
	if (a != 0.0 && (a >= 1e12 || a < 1e-9)) {
		snprintf(tmp, sizeof(tmp), "%.6g", v);
		snprintf(out, size, "%s", tmp);
		return;
	}

	snprintf(tmp, sizeof(tmp), "%.10f", v);
	char *dot = strchr(tmp, '.');
	if (dot) {
		char *end = tmp + strlen(tmp) - 1;
		while (end > dot && *end == '0') {
			*end-- = '\0';
		}
		if (end == dot) {
			*end = '\0';
		}
	}
	if (tmp[0] == '-' && strcmp(tmp + 1, "0") == 0) {
		snprintf(out, size, "0");
		return;
	}
	snprintf(out, size, "%s", tmp);
}

static bool has_entry(void) {
	if (errored || showing_eq) {
		return true;
	}
	if (pending) {
		return true;
	}
	if (!fresh && buf[0] && strcmp(buf, "0") != 0) {
		return true;
	}
	if (buf[0] == '-' || (buf[0] && strchr(buf, '.'))) {
		return true;
	}
	return false;
}

static void refresh_clear_label(void) {
	if (!clear_label) {
		return;
	}
	lv_label_set_text(clear_label, has_entry() ? "C" : "AC");
}

static void refresh_op_highlight(void) {
	static const int OPS[4] = {KEY_DIV, KEY_MUL, KEY_SUB, KEY_ADD};
	for (int i = 0; i < 4; i++) {
		lv_obj_t *btn = op_btns[i];
		if (!btn) {
			continue;
		}
		bool on = pending == OPS[i] && fresh;
		lv_obj_t *lab = lv_obj_get_child(btn, 0);
		if (on) {
			lv_obj_set_style_bg_color(btn, theme()->surface_pressed, 0);
			lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
			if (lab) {
				lv_obj_set_style_text_color(lab, theme()->accent, 0);
			}
		} else {
			lv_obj_set_style_bg_color(btn, theme()->accent, 0);
			lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
			if (lab) {
				lv_obj_set_style_text_color(lab, lv_color_white(), 0);
			}
		}
	}
}

static void refresh_display(void) {
	if (!display_label) {
		return;
	}

	char expr[EXPR_MAX];
	char shown[BUF_MAX];
	char lhs_l[BUF_MAX];
	char buf_l[BUF_MAX];
	expr[0] = '\0';

	if (errored) {
		if (expr_label) {
			lv_label_set_text(expr_label, "");
		}
		lv_label_set_text(display_label, t_error());
		refresh_clear_label();
		refresh_op_highlight();
		return;
	}

	localise_number(lhs, lhs_l, sizeof(lhs_l));
	localise_number(buf[0] ? buf : "0", buf_l, sizeof(buf_l));

	if (pending) {
		const char *op = key_label(pending);
		if (!fresh && buf[0]) {
			snprintf(expr, sizeof(expr), "%s %s %s", lhs_l, op, buf_l);
		} else {
			snprintf(expr, sizeof(expr), "%s %s", lhs_l, op);
		}
	} else if (showing_eq && eq_expr[0]) {
		snprintf(expr, sizeof(expr), "%s", eq_expr);
	}

	if (expr_label) {
		lv_label_set_text(expr_label, expr);
	}
	localise_number(buf[0] ? buf : "0", shown, sizeof(shown));
	lv_label_set_text(display_label, shown);
	refresh_clear_label();
	refresh_op_highlight();
}

static void set_error(void) {
	errored = true;
	acc = 0;
	pending = 0;
	fresh = true;
	showing_eq = false;
	have_repeat = false;
	buf[0] = '\0';
	lhs[0] = '\0';
	eq_expr[0] = '\0';
	refresh_display();
}

static void clear_all(void) {
	acc = 0;
	pending = 0;
	fresh = true;
	errored = false;
	showing_eq = false;
	have_repeat = false;
	last_op = 0;
	last_rhs = 0;
	buf[0] = '\0';
	lhs[0] = '\0';
	eq_expr[0] = '\0';
	refresh_display();
}

static void clear_entry(void) {
	buf[0] = '\0';
	fresh = true;
	errored = false;
	if (showing_eq) {
		showing_eq = false;
		eq_expr[0] = '\0';
	}
	refresh_display();
}

static void input_clear(void) {
	if (errored || showing_eq) {
		clear_all();
		return;
	}
	if (has_entry()) {
		// C: drop the current entry; a second tap (now AC) clears the rest.
		if (!fresh && buf[0] && (pending || strcmp(buf, "0") != 0 || strchr(buf, '.') || buf[0] == '-')) {
			clear_entry();
			return;
		}
		if (pending) {
			pending = 0;
			lhs[0] = '\0';
			have_repeat = false;
			refresh_display();
			return;
		}
	}
	clear_all();
}

static double current_value(void) {
	if (!buf[0]) {
		return 0;
	}
	return strtod(buf, NULL);
}

static bool apply_op(int op, double left, double right, double *out) {
	switch (op) {
	case KEY_ADD:
		*out = left + right;
		break;
	case KEY_SUB:
		*out = left - right;
		break;
	case KEY_MUL:
		*out = left * right;
		break;
	case KEY_DIV:
		if (right == 0.0) {
			set_error();
			return false;
		}
		*out = left / right;
		break;
	default:
		*out = right;
		break;
	}
	if (!isfinite(*out)) {
		set_error();
		return false;
	}
	return true;
}

static bool apply_pending(void) {
	double rhs = current_value();
	double result = rhs;

	if (pending) {
		if (!apply_op(pending, acc, rhs, &result)) {
			return false;
		}
	}

	acc = result;
	format_number(result, buf, sizeof(buf));
	fresh = true;
	return true;
}

static void input_digit(int d) {
	if (errored) {
		clear_all();
	}
	if (showing_eq && !pending) {
		showing_eq = false;
		eq_expr[0] = '\0';
		have_repeat = false;
	}
	if (fresh) {
		buf[0] = '\0';
		fresh = false;
	}
	if (digit_count(buf) >= MAX_DIGITS) {
		return;
	}
	if (strcmp(buf, "0") == 0) {
		buf[0] = '\0';
	}
	size_t n = strlen(buf);
	if (n + 1 >= sizeof(buf)) {
		return;
	}
	buf[n] = (char)('0' + d);
	buf[n + 1] = '\0';
	refresh_display();
}

static void input_dot(void) {
	if (errored) {
		clear_all();
	}
	if (showing_eq && !pending) {
		showing_eq = false;
		eq_expr[0] = '\0';
		have_repeat = false;
	}
	if (fresh) {
		snprintf(buf, sizeof(buf), "0");
		fresh = false;
	}
	if (strchr(buf, '.')) {
		return;
	}
	size_t n = strlen(buf);
	if (n + 1 >= sizeof(buf)) {
		return;
	}
	if (n == 0) {
		snprintf(buf, sizeof(buf), "0.");
	} else {
		buf[n] = '.';
		buf[n + 1] = '\0';
	}
	refresh_display();
}

static void input_backspace(void) {
	if (errored) {
		clear_all();
		return;
	}
	if (fresh || showing_eq) {
		return;
	}
	size_t n = strlen(buf);
	if (n == 0) {
		return;
	}
	buf[n - 1] = '\0';
	if (!buf[0] || strcmp(buf, "-") == 0 || strcmp(buf, "-0") == 0) {
		buf[0] = '\0';
		fresh = true;
	}
	refresh_display();
}

static void input_op(int op) {
	if (errored) {
		clear_all();
		return;
	}
	showing_eq = false;
	eq_expr[0] = '\0';
	if (!fresh || !pending) {
		if (!apply_pending()) {
			return;
		}
	}
	snprintf(lhs, sizeof(lhs), "%s", buf[0] ? buf : "0");
	pending = op;
	acc = current_value();
	fresh = true;
	have_repeat = false;
	refresh_display();
}

static void input_eq(void) {
	if (errored) {
		clear_all();
		return;
	}

	if (pending) {
		char rhs_txt[BUF_MAX];
		char lhs_l[BUF_MAX];
		char rhs_l[BUF_MAX];
		snprintf(rhs_txt, sizeof(rhs_txt), "%s", buf[0] ? buf : "0");
		last_op = pending;
		last_rhs = current_value();
		have_repeat = true;
		localise_number(lhs, lhs_l, sizeof(lhs_l));
		localise_number(rhs_txt, rhs_l, sizeof(rhs_l));
		snprintf(eq_expr, sizeof(eq_expr), "%s %s %s", lhs_l, key_label(pending), rhs_l);
		if (!apply_pending()) {
			return;
		}
		pending = 0;
		lhs[0] = '\0';
		showing_eq = true;
		refresh_display();
		return;
	}

	if (have_repeat && last_op) {
		char left_l[BUF_MAX];
		char rhs_txt[BUF_MAX];
		char rhs_l[BUF_MAX];
		double left = current_value();
		double result;
		format_number(last_rhs, rhs_txt, sizeof(rhs_txt));
		localise_number(buf[0] ? buf : "0", left_l, sizeof(left_l));
		localise_number(rhs_txt, rhs_l, sizeof(rhs_l));
		snprintf(eq_expr, sizeof(eq_expr), "%s %s %s", left_l, key_label(last_op), rhs_l);
		if (!apply_op(last_op, left, last_rhs, &result)) {
			return;
		}
		acc = result;
		format_number(result, buf, sizeof(buf));
		fresh = true;
		showing_eq = true;
		refresh_display();
		return;
	}

	showing_eq = false;
	eq_expr[0] = '\0';
	refresh_display();
}

static void input_sign(void) {
	if (errored) {
		clear_all();
		return;
	}
	if (fresh && !buf[0]) {
		format_number(acc, buf, sizeof(buf));
		fresh = false;
	}
	if (!buf[0] || strcmp(buf, "0") == 0) {
		return;
	}
	if (buf[0] == '-') {
		memmove(buf, buf + 1, strlen(buf));
	} else if ((int)strlen(buf) < BUF_MAX - 1) {
		memmove(buf + 1, buf, strlen(buf) + 1);
		buf[0] = '-';
	}
	if (pending && fresh) {
		snprintf(lhs, sizeof(lhs), "%s", buf);
		acc = current_value();
	}
	refresh_display();
}

static void input_percent(void) {
	if (errored) {
		clear_all();
		return;
	}
	double cur = current_value();
	double v;
	// Phone-style: with +/− pending, n% is (acc * n / 100); otherwise /100.
	if (pending == KEY_ADD || pending == KEY_SUB) {
		v = acc * (cur / 100.0);
	} else {
		v = cur / 100.0;
	}
	if (!isfinite(v)) {
		set_error();
		return;
	}
	format_number(v, buf, sizeof(buf));
	fresh = true;
	showing_eq = false;
	refresh_display();
}

static void on_key(int key) {
	switch (key) {
	case KEY_CLEAR:
		input_clear();
		break;
	case KEY_BACKSPACE:
		input_backspace();
		break;
	case KEY_SIGN:
		input_sign();
		break;
	case KEY_PERCENT:
		input_percent();
		break;
	case KEY_DIV:
	case KEY_MUL:
	case KEY_SUB:
	case KEY_ADD:
		input_op(key);
		break;
	case KEY_DOT:
		input_dot();
		break;
	case KEY_EQ:
		input_eq();
		break;
	case KEY_0:
	case KEY_1:
	case KEY_2:
	case KEY_3:
	case KEY_4:
	case KEY_5:
	case KEY_6:
	case KEY_7:
	case KEY_8:
	case KEY_9:
		input_digit(key);
		break;
	default:
		break;
	}
}

static void key_cb(lv_event_t *e) {
	on_key((int)(intptr_t)lv_event_get_user_data(e));
}

static const char *key_label(int key) {
	switch (key) {
	case KEY_CLEAR:
		return has_entry() ? "C" : "AC";
	case KEY_BACKSPACE:
		// MiSans has no U+232B (⌫); a plain ASCII label always draws.
		return "<-";
	case KEY_SIGN:
		return "±";
	case KEY_PERCENT:
		return "%";
	case KEY_DIV:
		return "÷";
	case KEY_MUL:
		return "×";
	case KEY_SUB:
		return "−";
	case KEY_ADD:
		return "+";
	case KEY_DOT: {
		static char dot[2];
		dot[0] = decimal_char();
		dot[1] = '\0';
		return dot;
	}
	case KEY_EQ:
		return "=";
	case KEY_0:
		return "0";
	case KEY_1:
		return "1";
	case KEY_2:
		return "2";
	case KEY_3:
		return "3";
	case KEY_4:
		return "4";
	case KEY_5:
		return "5";
	case KEY_6:
		return "6";
	case KEY_7:
		return "7";
	case KEY_8:
		return "8";
	case KEY_9:
		return "9";
	default:
		return "";
	}
}

static bool key_is_arith(int key) {
	return key == KEY_DIV || key == KEY_MUL || key == KEY_SUB || key == KEY_ADD;
}

static bool key_is_op(int key) {
	return key_is_arith(key) || key == KEY_EQ;
}

static bool key_is_fn(int key) {
	return key == KEY_CLEAR || key == KEY_BACKSPACE || key == KEY_SIGN || key == KEY_PERCENT;
}

static lv_obj_t *make_key(lv_obj_t *parent, int key, int width, int height) {
	lv_obj_t *btn = lv_btn_create(parent);
	lv_obj_set_size(btn, width, height);
	lv_obj_set_style_radius(btn, KEY_RADIUS, 0);
	lv_obj_set_style_shadow_width(btn, 0, 0);
	lv_obj_set_style_border_width(btn, 0, 0);
	lv_obj_set_scrollable(btn, false);
	lv_obj_add_event_cb(btn, key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)key);

	if (key_is_op(key)) {
		lv_obj_add_style(btn, &theme_style_accent_bg, 0);
		// Pressed: darken so the finger gets the same feedback digits do.
		lv_obj_set_style_bg_color(btn, theme()->surface_pressed, LV_STATE_PRESSED);
		lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
	} else {
		lv_obj_add_style(btn, &theme_style_card, 0);
		lv_obj_add_style(btn, &theme_style_card_pressed, LV_STATE_PRESSED);
	}

	lv_obj_t *label = lv_label_create(btn);
	lv_label_set_text(label, key_label(key));
	lv_obj_set_style_text_font(label, &font_ui_26, 0);
	if (key_is_op(key)) {
		lv_obj_set_style_text_color(label, lv_color_white(), 0);
	} else if (key_is_fn(key)) {
		lv_obj_add_style(label, &theme_style_text_dim, 0);
	} else {
		lv_obj_add_style(label, &theme_style_text, 0);
	}
	lv_obj_center(label);

	if (key == KEY_CLEAR) {
		clear_label = label;
	}
	if (key == KEY_DIV) {
		op_btns[0] = btn;
	} else if (key == KEY_MUL) {
		op_btns[1] = btn;
	} else if (key == KEY_SUB) {
		op_btns[2] = btn;
	} else if (key == KEY_ADD) {
		op_btns[3] = btn;
	}

	return btn;
}

static void paint_display_card(lv_obj_t *card) {
	lv_obj_add_style(card, &theme_style_card, 0);
	lv_obj_set_style_radius(card, KEY_RADIUS, 0);
	lv_obj_set_style_border_width(card, 0, 0);
	lv_obj_set_style_shadow_width(card, 0, 0);
}

static lv_obj_t *make_row(lv_obj_t *body, int body_w, int key_h) {
	lv_obj_t *row = lv_obj_create(body);
	lv_obj_set_size(row, body_w, key_h);
	lv_obj_set_style_bg_opa(row, 0, 0);
	lv_obj_set_style_border_width(row, 0, 0);
	lv_obj_set_style_pad_all(row, 0, 0);
	lv_obj_set_style_pad_column(row, KEY_GAP, 0);
	lv_obj_set_scrollable(row, false);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return row;
}

static lv_obj_t *calc_init(const sonix_addon_host_t *host) {
	calc_cfg = host->cfg;
	clear_label = NULL;
	memset(op_btns, 0, sizeof(op_btns));
	clear_all();

	calc_screen = lv_obj_create(NULL);
	lv_obj_add_style(calc_screen, &theme_style_screen, 0);
	settingsrow_title(calc_screen, calc_cfg, t_title());

	int top = settingsrow_content_top(calc_cfg);
	int pad = calc_cfg->padding;
	int body_h = calc_cfg->screen_height - top - pad;
	int body_w = calc_cfg->screen_width - 2 * pad;

	lv_obj_t *body = lv_obj_create(calc_screen);
	lv_obj_set_size(body, body_w, body_h);
	lv_obj_align(body, LV_ALIGN_TOP_MID, 0, top);
	lv_obj_set_style_bg_opa(body, 0, 0);
	lv_obj_set_style_border_width(body, 0, 0);
	lv_obj_set_style_pad_all(body, 0, 0);
	lv_obj_set_style_pad_row(body, KEY_GAP, 0);
	lv_obj_set_scrollable(body, false);
	lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	switcher_attach_back_gesture(body);

	lv_obj_t *display_card = lv_obj_create(body);
	lv_obj_set_size(display_card, body_w, DISPLAY_H);
	paint_display_card(display_card);
	lv_obj_set_style_pad_hor(display_card, SC(16), 0);
	lv_obj_set_style_pad_ver(display_card, SC(10), 0);
	lv_obj_set_style_pad_row(display_card, SC(2), 0);
	lv_obj_set_scrollable(display_card, false);
	lv_obj_set_flex_flow(display_card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(display_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);

	int line_w = body_w - SC(32);

	expr_label = lv_label_create(display_card);
	lv_obj_add_style(expr_label, &theme_style_text_dim, 0);
	lv_obj_set_style_text_font(expr_label, &font_ui_22, 0);
	lv_label_set_long_mode(expr_label, LV_LABEL_LONG_CLIP);
	lv_obj_set_width(expr_label, line_w);
	lv_obj_set_style_text_align(expr_label, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_text(expr_label, "");

	display_label = lv_label_create(display_card);
	lv_obj_add_style(display_label, &theme_style_text, 0);
	lv_obj_set_style_text_font(display_label, &font_ui_36_bold, 0);
	lv_label_set_long_mode(display_label, LV_LABEL_LONG_CLIP);
	lv_obj_set_width(display_label, line_w);
	lv_obj_set_style_text_align(display_label, LV_TEXT_ALIGN_RIGHT, 0);

	int rows = 5;
	int grid_h = body_h - DISPLAY_H - KEY_GAP;
	int key_h = (grid_h - (rows - 1) * KEY_GAP) / rows;
	int key_w = (body_w - 3 * KEY_GAP) / 4;

	static const int ROWS[5][4] = {
		{KEY_CLEAR, KEY_BACKSPACE, KEY_PERCENT, KEY_DIV},
		{KEY_7, KEY_8, KEY_9, KEY_MUL},
		{KEY_4, KEY_5, KEY_6, KEY_SUB},
		{KEY_1, KEY_2, KEY_3, KEY_ADD},
		{KEY_0, KEY_SIGN, KEY_DOT, KEY_EQ},
	};

	for (int r = 0; r < 5; r++) {
		lv_obj_t *row = make_row(body, body_w, key_h);
		for (int c = 0; c < 4; c++) {
			make_key(row, ROWS[r][c], key_w, key_h);
		}
	}

	refresh_display();
	return calc_screen;
}

static bool calc_stop(void) {
	calc_screen = NULL;
	expr_label = NULL;
	display_label = NULL;
	clear_label = NULL;
	memset(op_btns, 0, sizeof(op_btns));
	calc_cfg = NULL;
	acc = 0;
	pending = 0;
	last_op = 0;
	last_rhs = 0;
	have_repeat = false;
	fresh = true;
	errored = false;
	showing_eq = false;
	buf[0] = '\0';
	lhs[0] = '\0';
	eq_expr[0] = '\0';
	return true;
}

static const sonix_addon_t calc = {
	.api = SONIX_ADDON_API,
	.title = "Calculator",
	.init = calc_init,
};

static const sonix_addon_stop_t calc_lifecycle = {
	.api = SONIX_ADDON_API,
	.stop = calc_stop,
};

const sonix_addon_t *sonix_addon_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &calc : NULL;
}

const sonix_addon_stop_t *sonix_addon_stop_entry(uint32_t api) {
	return api == SONIX_ADDON_API ? &calc_lifecycle : NULL;
}
