#include "common/artisttag.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "src/system/library/library.h"

// See common/artisttag.h: this is the library's own splitting (library.c,
// split_names), kept here because the function itself is static to the player
// and no addon can call it. The separators come from the player's settings, so
// the two halves agree on what "A; B" is.

// Punctuation the fold maps to a space: comparison, not splitting.
static bool fold_sep_char(unsigned char c) {
	return c == ';' || c == '/' || c == '|' || c == '&' || c == ',' || c == '+';
}

// ASCII alphanumerics and every UTF-8 byte (≥0x80). Dropping high bytes folded
// Cyrillic labels to empty strings, which then conflicted with everything.
static bool label_char(unsigned char c) {
	return isalnum(c) != 0 || c >= 0x80;
}

artisttag_rules_t artisttag_rules_from_player(void) {
	artisttag_rules_t rules;
	rules.split = library_split_artists();
	rules.seps = library_artist_separators();
	return rules;
}

void artisttag_fold(const char *in, char *out, size_t out_sz) {
	if (!out || out_sz == 0) {
		return;
	}
	size_t o = 0;
	bool sp = true; // leading spaces dropped
	for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 1 < out_sz; p++) {
		unsigned char c = *p;
		if (fold_sep_char(c)) {
			c = ' ';
		} else if (c < 0x80) {
			c = (unsigned char)tolower(c);
		}
		if (!(label_char(c) || c == ' ' || c == '\'')) {
			continue;
		}
		if (c == ' ') {
			if (sp) {
				continue;
			}
			sp = true;
			out[o++] = ' ';
			continue;
		}
		sp = false;
		out[o++] = (char)c;
	}
	while (o > 0 && out[o - 1] == ' ') {
		o--;
	}
	out[o] = '\0';
}

typedef struct {
	unsigned bit;
	const char *symbol;
} split_symbol_t;

static const split_symbol_t SPLIT_SYMBOLS[] = {
	{LIBRARY_SPLIT_SEMICOLON, ";"},
	{LIBRARY_SPLIT_SEMICOLON, "\xef\xbc\x9b"}, // U+FF1B
	{LIBRARY_SPLIT_SLASH, "/"},
	{LIBRARY_SPLIT_SLASH, "\xef\xbc\x8f"}, // U+FF0F
	{LIBRARY_SPLIT_AMPERSAND, "&"},
	{LIBRARY_SPLIT_AMPERSAND, "\xef\xbc\x86"}, // U+FF06
	{LIBRARY_SPLIT_COMMA, ","},
	{LIBRARY_SPLIT_COMMA, "\xef\xbc\x8c"}, // U+FF0C
	{LIBRARY_SPLIT_COMMA, "\xe3\x80\x81"}, // U+3001, the ideographic comma
	{LIBRARY_SPLIT_PLUS, "+"},
	{LIBRARY_SPLIT_PLUS, "\xef\xbc\x8b"}, // U+FF0B
	{LIBRARY_SPLIT_X, "\xc3\x97"},		  // U+00D7, the multiplication sign
};

// A symbol separator at `p`: its length in bytes, or 0.
static int split_symbol_at(unsigned separators, const char *p) {
	for (size_t i = 0; i < sizeof(SPLIT_SYMBOLS) / sizeof(SPLIT_SYMBOLS[0]); i++) {
		if ((separators & SPLIT_SYMBOLS[i].bit) == 0) {
			continue;
		}
		size_t n = strlen(SPLIT_SYMBOLS[i].symbol);
		if (strncmp(p, SPLIT_SYMBOLS[i].symbol, n) == 0) {
			return (int)n;
		}
	}
	return 0;
}

// A word separator ("feat", "vs") at `p`: the library's conditions -- preceded
// by a space or an opening bracket, followed by a space or, for an abbreviation,
// a full stop. "x" counts in lower case only, so "Malcolm X" stays one name;
// "x", "with" and "and" are ordinary words inside brackets.
static int split_word_at(unsigned separators, const char *text, const char *p, bool in_brackets) {
	static const struct {
		unsigned bit;
		const char *word;
		bool stop;	  // may end in a full stop: "feat.", "vs."
		bool exact;	  // the case as written
		bool outside; // outside brackets only
	} WORDS[] = {
		{LIBRARY_SPLIT_FEAT, "featuring", true, false, false},
		{LIBRARY_SPLIT_FEAT, "feat", true, false, false},
		{LIBRARY_SPLIT_FEAT, "ft", true, false, false},
		{LIBRARY_SPLIT_VS, "versus", true, false, false},
		{LIBRARY_SPLIT_VS, "vs", true, false, false},
		{LIBRARY_SPLIT_X, "x", false, true, true},
		{LIBRARY_SPLIT_WITH, "with", false, false, true},
		{LIBRARY_SPLIT_AND, "and", false, false, true},
	};
	if (p == text || (p[-1] != ' ' && p[-1] != '(' && p[-1] != '[')) {
		return 0;
	}
	for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
		if ((separators & WORDS[i].bit) == 0 || (WORDS[i].outside && in_brackets)) {
			continue;
		}
		size_t n = strlen(WORDS[i].word);
		if ((WORDS[i].exact ? strncmp(p, WORDS[i].word, n) : strncasecmp(p, WORDS[i].word, n)) != 0) {
			continue;
		}
		if (WORDS[i].stop && p[n] == '.') {
			return (int)n + 1;
		}
		if (p[n] == ' ') {
			return (int)n;
		}
	}
	return 0;
}

// Spaces off both ends; a bracket left open at the end of a name ("A (" from
// "A (feat. B)"); and, on a name that began inside brackets, the bracket that
// closed them ("B)").
typedef struct {
	char text[ARTISTTAG_NAME_MAX];
	int len;
	bool in_brackets;
} tag_part_t;

static void tag_part_trim(tag_part_t *part) {
	char *s = part->text;
	int start = 0;
	int end = part->len;
	for (;;) {
		while (start < end && (s[start] == ' ' || s[start] == '\t')) {
			start++;
		}
		while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
			end--;
		}
		if (end > start && (s[end - 1] == '(' || s[end - 1] == '[' || s[end - 1] == '-')) {
			end--;
			continue;
		}
		if (part->in_brackets && end > start && (s[end - 1] == ')' || s[end - 1] == ']')) {
			end--;
			part->in_brackets = false;
			continue;
		}
		break;
	}
	memmove(s, s + start, (size_t)(end - start));
	s[end - start] = '\0';
	part->len = end - start;
}

// The names as written: trimmed, in order, repeats kept. Returns how many.
static int split_parts(const artisttag_rules_t *rules, const char *tag, tag_part_t *parts, int max_parts) {
	if (!tag || !tag[0] || !parts || max_parts <= 0) {
		return 0;
	}
	unsigned separators = (rules && rules->split) ? rules->seps : 0;
	if (separators == 0) {
		snprintf(parts[0].text, sizeof(parts[0].text), "%s", tag);
		parts[0].len = (int)strlen(parts[0].text);
		tag_part_trim(&parts[0]);
		return parts[0].text[0] ? 1 : 0;
	}

	memset(parts, 0, (size_t)max_parts * sizeof(*parts));
	int count = 1;
	int depth = 0;
	bool word_in_brackets = false;
	const char *p = tag;
	while (*p) {
		tag_part_t *cur = &parts[count - 1];
		int cut = 0;
		int symbol = split_symbol_at(separators, p);
		if (symbol > 0) {
			cut = (depth == 0 || word_in_brackets) ? symbol : 0;
		} else {
			cut = split_word_at(separators, tag, p, depth > 0);
			word_in_brackets = word_in_brackets || (cut > 0 && depth > 0);
		}
		if (cut > 0) {
			p += cut;
			if (count < max_parts) {
				count++;
				parts[count - 1].in_brackets = depth > 0;
			}
			continue;
		}
		if (*p == '(' || *p == '[') {
			depth++;
		} else if ((*p == ')' || *p == ']') && depth > 0) {
			depth--;
			word_in_brackets = word_in_brackets && depth > 0;
		}
		if (cur->len + 1 < (int)sizeof(cur->text)) {
			cur->text[cur->len++] = *p;
		}
		p++;
	}

	int written = 0;
	for (int i = 0; i < count && written < max_parts; i++) {
		parts[i].text[parts[i].len] = '\0';
		tag_part_trim(&parts[i]);
		if (!parts[i].text[0]) {
			continue;
		}
		if (written != i) {
			parts[written] = parts[i];
		}
		written++;
	}
	return written;
}

int artisttag_split(const artisttag_rules_t *rules, const char *tag, char out[][ARTISTTAG_NAME_MAX], int max_names) {
	if (!out || max_names <= 0) {
		return 0;
	}
	tag_part_t parts[ARTISTTAG_MAX];
	int parts_n = split_parts(rules, tag, parts, ARTISTTAG_MAX);
	int written = 0;
	for (int i = 0; i < parts_n && written < max_names; i++) {
		char fold[ARTISTTAG_NAME_MAX];
		artisttag_fold(parts[i].text, fold, sizeof(fold));
		if (!fold[0]) {
			continue;
		}
		bool repeat = false;
		for (int j = 0; j < written && !repeat; j++) {
			repeat = strcmp(out[j], fold) == 0;
		}
		if (!repeat) {
			snprintf(out[written++], ARTISTTAG_NAME_MAX, "%s", fold);
		}
	}
	if (written == 0 && tag && tag[0]) {
		artisttag_fold(tag, out[0], ARTISTTAG_NAME_MAX);
		return out[0][0] ? 1 : 0;
	}
	return written;
}

bool artisttag_first(const artisttag_rules_t *rules, const char *tag, char *out, size_t out_sz) {
	if (!out || out_sz == 0) {
		return false;
	}
	out[0] = '\0';
	tag_part_t parts[ARTISTTAG_MAX];
	int n = split_parts(rules, tag, parts, ARTISTTAG_MAX);
	if (n < 1) {
		return false;
	}
	snprintf(out, out_sz, "%s", parts[0].text);
	return out[0] != '\0';
}

artisttag_credit_t artisttag_credit(const artisttag_rules_t *rules, const char *tag, const char *name) {
	char whole_tag[ARTISTTAG_NAME_MAX];
	char whole_name[ARTISTTAG_NAME_MAX];
	artisttag_fold(tag ? tag : "", whole_tag, sizeof(whole_tag));
	artisttag_fold(name ? name : "", whole_name, sizeof(whole_name));
	if (!whole_name[0]) {
		return ARTISTTAG_NONE;
	}
	// The tag is this one artist and nothing else -- including a name that
	// carries a separator itself ("AC/DC", "M|O|O|N"), which is what the
	// library's exceptions list keeps whole and the split below would saw in
	// half.
	if (strcmp(whole_tag, whole_name) == 0) {
		return ARTISTTAG_FIRST;
	}

	char names[ARTISTTAG_MAX][ARTISTTAG_NAME_MAX];
	int n = artisttag_split(rules, tag ? tag : "", names, ARTISTTAG_MAX);
	for (int i = 0; i < n; i++) {
		if (strcmp(names[i], whole_name) == 0) {
			return i == 0 ? ARTISTTAG_FIRST : ARTISTTAG_GUEST;
		}
	}
	return ARTISTTAG_NONE;
}
