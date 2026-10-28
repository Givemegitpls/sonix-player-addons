#ifndef ADDON_STORE_H
#define ADDON_STORE_H

#include <stdbool.h>
#include <stddef.h>

// An addon's own settings, in a file on the card next to the addon.
//
// An addon keeps nothing in the player's configuration. That file belongs to
// the player: it is rewritten whole on every save, its sections carry the
// device's settings, and an addon writing to it from a worker thread races the
// interface for it. An addon's settings are also its own business -- a server,
// a token, a quality choice -- and pairing them with the card the addon arrived
// on is the plain reading of "the addon is a package on a card".
//
// The format is flat, one key per line, because the file is the addon's and
// nothing else reads it:
//
//   # comment, and blank lines
//   server = http://192.168.1.10:4533
//   user = alice
//
// store_set() with an empty value removes the key, so what is signed out is
// gone from the card rather than left as an empty line. store_save() writes a
// temporary file and renames it over the old one: the card can be pulled or the
// player killed at any moment, and a half-written settings file loses the
// session (and, for a server that needs the password, the password with it).

#define STORE_KEYS 16
#define STORE_KEY_MAX 32
#define STORE_VALUE_MAX 256
#define STORE_PATH_MAX 256

typedef struct {
	char path[STORE_PATH_MAX];
	int count;
	struct {
		char key[STORE_KEY_MAX];
		char value[STORE_VALUE_MAX];
	} item[STORE_KEYS];
} store_t;

// Points the store at a file and empties it. Nothing is read yet: store_load().
void store_init(store_t *s, const char *path);

// Reads the file into memory. False when there is no file yet (a fresh card),
// which is not an error: the store is then empty.
bool store_load(store_t *s);

// The value, or "" when the key is not there.
const char *store_get(const store_t *s, const char *key);
int store_get_int(const store_t *s, const char *key, int fallback);

// "" removes the key.
void store_set(store_t *s, const char *key, const char *value);
void store_set_int(store_t *s, const char *key, int value);

// Writes the whole file back, atomically. False and a line on stderr when the
// card refuses the write (full, pulled, read-only).
bool store_save(const store_t *s);

#endif /* ADDON_STORE_H */
