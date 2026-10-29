#ifndef GEARBOY_GBCONF_H
#define GEARBOY_GBCONF_H

// Gearboy's settings: palette, shader, colour correction and the boot ROM
// switch -- four numbers, kept in `<addon>/settings.ini` on the card.
//
// They used to be `[gearboy]` keys in the player's `device_config.ini`, which
// was right while the emulator was part of the binary and wrong the moment it
// became a package on a card: the player's file is rewritten whole on every
// save, it carries the device's own settings, and it stays behind when the card
// (with the player's settings for this game) goes into another player. An
// addon's settings travel with the addon -- see `addons/common/store.h`.
//
// A key the file does not have yet is read from the old `[gearboy]` section
// once, so a card written before this change keeps the palette its owner chose;
// everything written from then on goes to the card.

void gbconf_init(const char *addon_dir); // "<addon_dir>/settings.ini"

int gbconf_get_int(const char *key, int fallback);

// Writes the file immediately: the player has no shutdown path, and a card can
// be pulled at any moment.
void gbconf_set_int(const char *key, int value);

#endif /* GEARBOY_GBCONF_H */
