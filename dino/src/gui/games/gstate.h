#ifndef DINO_GSTATE_H
#define DINO_GSTATE_H

// What the game remembers between runs -- the best score -- in a file on the
// card beside the addon (`<card>/Addons/dino/settings.ini`).
//
// It came from the TempoTec V1 port, where it was `[dino] best` in the player's
// `device_config.ini`. That file belongs to the player (rewritten whole on every
// save, and it stays behind when the card goes into another player), so an addon
// keeps its own next to itself -- `addons/common/store.h`.
//
// A card written before this change has no file: the key is then read from the
// old `[dino]` section once, so a high score survives the move.

void gstate_init(const char *addon_dir); // "<addon_dir>/settings.ini"

int gstate_get_int(const char *key, int fallback);

// Writes the file immediately: the player has no shutdown path and a card can be
// pulled at any moment.
void gstate_set_int(const char *key, int value);

#endif /* DINO_GSTATE_H */
