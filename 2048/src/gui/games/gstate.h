#ifndef DINO_GSTATE_H
#define DINO_GSTATE_H

// What the game remembers between runs -- the best score -- in a file on the
// card beside the addon (`<card>/Addons/2048/settings.ini`), via common/store.

void gstate_init(const char *addon_dir); // "<addon_dir>/settings.ini"

int gstate_get_int(const char *key, int fallback);

// Writes the file immediately: the player has no shutdown path and a card can be
// pulled at any moment.
void gstate_set_int(const char *key, int value);

#endif /* DINO_GSTATE_H */
