// Best (career wins) next to the addon: <card>/Addons/tictactoe/settings.ini

#ifndef TICTACTOE_GSTATE_H
#define TICTACTOE_GSTATE_H

void gstate_init(const char *addon_dir);
int gstate_get_int(const char *key, int fallback);
void gstate_set_int(const char *key, int value);

#endif
