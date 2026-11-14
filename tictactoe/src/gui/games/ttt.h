// Tic Tac Toe — 3×3, you are X, the device plays O.

#ifndef GUI_TTT_H
#define GUI_TTT_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

enum {
	TTT_DIFF_EASY = 0,
	TTT_DIFF_MEDIUM = 1,
	TTT_DIFF_HARD = 2,
	TTT_DIFF_COUNT = 3,
};

extern lv_obj_t *ttt_screen;

void ttt_init(gui_config_t *cfg);
void ttt_open(void);
void ttt_stop(void);

int ttt_get_difficulty(void);
void ttt_set_difficulty(int d);

#endif /* GUI_TTT_H */
