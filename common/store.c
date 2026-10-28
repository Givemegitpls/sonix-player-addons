#include "common/store.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void trim(char *text) {
	char *start = text;
	while (*start == ' ' || *start == '\t') {
		start++;
	}
	if (start != text) {
		memmove(text, start, strlen(start) + 1);
	}
	size_t len = strlen(text);
	while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\n' || text[len - 1] == '\r')) {
		text[--len] = '\0';
	}
}

static int find(const store_t *s, const char *key) {
	for (int i = 0; i < s->count; i++) {
		if (strcmp(s->item[i].key, key) == 0) {
			return i;
		}
	}
	return -1;
}

void store_init(store_t *s, const char *path) {
	memset(s, 0, sizeof(*s));
	if (path) {
		snprintf(s->path, sizeof(s->path), "%s", path);
	}
}

bool store_load(store_t *s) {
	if (!s->path[0]) {
		return false;
	}
	FILE *f = fopen(s->path, "r");
	if (!f) {
		return false;
	}
	char line[STORE_KEY_MAX + STORE_VALUE_MAX + 8];
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t') {
			p++;
		}
		// Comments, a section header (a hand-edited file may have one) and
		// blank lines.
		if (*p == '\0' || *p == '\n' || *p == '\r' || *p == '#' || *p == '[' || *p == ';') {
			continue;
		}
		char *eq = strchr(p, '=');
		if (!eq) {
			continue;
		}
		*eq = '\0';
		trim(p);
		char *value = eq + 1;
		trim(value);
		if (p[0]) {
			store_set(s, p, value);
		}
	}
	fclose(f);
	return true;
}

const char *store_get(const store_t *s, const char *key) {
	int i = find(s, key);
	return i < 0 ? "" : s->item[i].value;
}

int store_get_int(const store_t *s, const char *key, int fallback) {
	int i = find(s, key);
	if (i < 0) {
		return fallback;
	}
	char *end = NULL;
	long v = strtol(s->item[i].value, &end, 10);
	return end != s->item[i].value ? (int)v : fallback;
}

void store_set(store_t *s, const char *key, const char *value) {
	if (!key || !key[0] || strlen(key) >= STORE_KEY_MAX) {
		return;
	}
	int i = find(s, key);
	if (!value || !value[0]) {
		if (i >= 0) {
			for (int j = i; j + 1 < s->count; j++) {
				s->item[j] = s->item[j + 1];
			}
			s->count--;
		}
		return;
	}
	if (i < 0) {
		if (s->count == STORE_KEYS) {
			fprintf(stderr, "addon store: %s: no room for %s\n", s->path, key);
			return;
		}
		i = s->count++;
		snprintf(s->item[i].key, sizeof(s->item[i].key), "%s", key);
	}
	snprintf(s->item[i].value, sizeof(s->item[i].value), "%s", value);
}

void store_set_int(store_t *s, const char *key, int value) {
	char text[16];
	snprintf(text, sizeof(text), "%d", value);
	store_set(s, key, text);
}

bool store_save(const store_t *s) {
	if (!s->path[0]) {
		return false;
	}
	char tmp[STORE_PATH_MAX + 8];
	if (snprintf(tmp, sizeof(tmp), "%s.tmp", s->path) >= (int)sizeof(tmp)) {
		return false;
	}
	FILE *f = fopen(tmp, "w");
	if (!f) {
		fprintf(stderr, "addon store: %s: %s\n", tmp, strerror(errno));
		return false;
	}
	fprintf(f, "# The addon's own settings, on the card next to %s.\n", "the addon");
	fprintf(f, "# Written by the addon; one \"key = value\" per line, hand edits are read back.\n\n");
	for (int i = 0; i < s->count; i++) {
		fprintf(f, "%s = %s\n", s->item[i].key, s->item[i].value);
	}
	// The rename makes the new file visible in one step, but only what reached
	// the card survives a power cut: the battery can go at any moment.
	bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = (fclose(f) == 0) && ok;
	if (!ok || rename(tmp, s->path) != 0) {
		fprintf(stderr, "addon store: %s: %s\n", s->path, strerror(errno));
		unlink(tmp);
		return false;
	}
	return true;
}
