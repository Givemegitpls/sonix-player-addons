#ifndef ADDON_ARTISTTAG_H
#define ADDON_ARTISTTAG_H

#include <stdbool.h>
#include <stddef.h>

// An artist tag, read the way the player's library read it.
//
// A tag like "A; B" is two artists in ARTIST_TABLE and one string everywhere
// else, and which one it is depends on the player's own settings: [library]
// split_artists and artist_separators, each separator behind a LIBRARY_SPLIT_*
// bit. An addon that offers artist names as answers, or compares two of them to
// keep near-duplicates out of a list, has to read the tag the same way -- a name
// the player does not show is a question nobody can answer.
//
// The rules are the library's (src/system/library/library.c, split_names): the
// symbol separators with their full-width forms, the word separators ("feat.",
// "with", "x") with their stop/case/bracket rules, brackets that split only
// after a word separator inside them, and the same trimming. What is not
// replicated is the exceptions list ("AC/DC" kept whole): a name carrying a
// separator is compared whole first instead, which is what artisttag_credit()
// does before it looks at the split names.
//
// The functions are pure and take their rules explicitly, so a test can pass
// any ruleset; artisttag_rules_from_player() is the one call that reads the
// player's configuration.
//
//   artisttag_rules_t rules = artisttag_rules_from_player();
//   ...
//   char names[ARTISTTAG_MAX][ARTISTTAG_NAME_MAX];
//   int n = artisttag_split(&rules, tag, names, ARTISTTAG_MAX);
//
// config.h has no lock: read the rules on the UI thread and hand the struct to
// a worker, rather than calling artisttag_rules_from_player() from one.

#define ARTISTTAG_MAX 8		  // names taken out of one tag
#define ARTISTTAG_NAME_MAX 192 // one name, folded

typedef struct {
	bool split;	  // [library] split_artists
	unsigned seps; // [library] artist_separators, LIBRARY_SPLIT_*
} artisttag_rules_t;

// The player's current rules. UI thread only (config.h is not locked).
artisttag_rules_t artisttag_rules_from_player(void);

// Lowercase, separators to spaces, letters/digits/spaces only, runs collapsed.
// What two names are compared as, not what they are split into -- "A;B" and
// "A B" fold alike whatever the separator settings say.
void artisttag_fold(const char *in, char *out, size_t out_sz);

// The names in `tag`, split by the rules, folded, trimmed and without repeats.
// Returns how many were written, 0 for an empty tag; a tags the library would
// not have split comes back as one name, folded whole.
int artisttag_split(const artisttag_rules_t *rules, const char *tag, char out[][ARTISTTAG_NAME_MAX], int max_names);

// The first name in the tag, as written (case kept, trimmed): what a one-line
// label shows. The whole tag when the library would not have split it.
bool artisttag_first(const artisttag_rules_t *rules, const char *tag, char *out, size_t out_sz);

// Where `name` stands in `tag`: nowhere, first -- the name a listener gives when
// the track plays -- or a guest credit after it.
typedef enum { ARTISTTAG_NONE = 0, ARTISTTAG_FIRST, ARTISTTAG_GUEST } artisttag_credit_t;

artisttag_credit_t artisttag_credit(const artisttag_rules_t *rules, const char *tag, const char *name);

#endif /* ADDON_ARTISTTAG_H */
