#ifndef FLAPPYBIRD_STATE_H
#define FLAPPYBIRD_STATE_H

// The game's own two numbers -- the best score and whether it makes sounds --
// in a file on the card, beside the addon.
//
// They used to be `[flappybird]` keys in the player's `device_config.ini`. That
// is the player's file: it is rewritten whole on every save, it belongs to the
// device rather than to the game, and a best score kept there does not travel
// with the card the game came on. An addon keeps its state next to itself
// (`addons/common/store.h`).
//
// A key the file does not have yet is read from the old `[flappybird]` section,
// so a card written before this change keeps its high score; everything written
// from then on goes to the card.

void fstate_init(const char *addon_dir); // "<addon_dir>/state.ini"

int fstate_get_int(const char *key, int fallback);

// Writes the file immediately. Called from the UI thread and from the game's
// own timer, never from a worker.
void fstate_set_int(const char *key, int value);

#endif /* FLAPPYBIRD_STATE_H */
