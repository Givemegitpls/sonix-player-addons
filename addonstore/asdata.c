#include "asdata.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "src/system/core/json.h"
#include "src/system/core/sha256.h"
#include "src/system/core/lang.h"

#define MANIFEST_MAX 8192

static bool write_atomic(const char *path, const char *text, size_t len);

static void copy_str(char *dst, size_t size, const char *src) {
	if (!dst || size == 0) {
		return;
	}
	if (!src) {
		src = "";
	}
	size_t len = strlen(src);
	if (len >= size) {
		len = size - 1;
	}
	memcpy(dst, src, len);
	dst[len] = '\0';
}

// The directory part of a path, into `out`. Used when the host gave no sd_root.
static void parent_dir(const char *path, char *out, size_t size) {
	copy_str(out, size, path);
	char *slash = strrchr(out, '/');
	if (slash) {
		*slash = '\0';
	}
}

void as_model_init(as_model_t *m, const char *addon_dir, const char *sd_root, const char *cache_dir) {
	memset(m, 0, sizeof(*m));
	copy_str(m->addon_dir, sizeof(m->addon_dir), addon_dir);
	copy_str(m->cache_dir, sizeof(m->cache_dir), cache_dir);
	if (sd_root && sd_root[0]) {
		snprintf(m->addons_dir, sizeof(m->addons_dir), "%s/Addons", sd_root);
	} else {
		parent_dir(addon_dir, m->addons_dir, sizeof(m->addons_dir));
	}
}

void as_addon_path(const as_model_t *m, const char *id, char *out, size_t size) {
	snprintf(out, size, "%s/%s", m->addons_dir, id);
}

void as_id_from_asset(const char *asset, char *out, size_t size) {
	out[0] = '\0';
	if (!asset || !asset[0]) {
		return;
	}
	const char *name = asset;
	const char *slash = strrchr(asset, '/');
	if (slash) {
		name = slash + 1;
	}
	size_t len = strlen(name);
	if (len < 5 || strcasecmp(name + len - 4, ".zip") != 0) {
		return;
	}
	len -= 4;
	if (len == 0 || len >= size || len >= AS_ID_MAX) {
		return;
	}
	for (size_t i = 0; i < len; i++) {
		char c = (char)tolower((unsigned char)name[i]);
		if (c == ' ') {
			c = '_';
		}
		if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && c != '-' && c != '.') {
			out[0] = '\0';
			return;
		}
		if (c == '.') { // a dot in an id is a hidden folder; not ours to create
			out[0] = '\0';
			return;
		}
		out[i] = c;
	}
	out[len] = '\0';
}

static void trim(char *s) {
	char *p = s;
	while (*p && isspace((unsigned char)*p)) {
		p++;
	}
	if (p != s) {
		memmove(s, p, strlen(p) + 1);
	}
	size_t len = strlen(s);
	while (len > 0 && isspace((unsigned char)s[len - 1])) {
		s[--len] = '\0';
	}
}

bool as_repo_valid(const char *repo) {
	if (!repo || !repo[0] || strlen(repo) >= AS_REPO_MAX) {
		return false;
	}
	const char *slash = strchr(repo, '/');
	if (!slash || slash == repo || !slash[1] || strchr(slash + 1, '/')) {
		return false;
	}
	if (strstr(repo, "..")) {
		return false;
	}
	for (const char *p = repo; *p; p++) {
		char c = *p;
		if (c == '/') {
			continue;
		}
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
				  c == '_' || c == '.';
		if (!ok) {
			return false;
		}
	}
	return true;
}

// "https://github.com/owner/name.git" and "git@github.com:owner/name" are what
// people paste; the file only ever holds owner/name.
static bool repo_normalise(const char *input, char *out, size_t size) {
	char buf[256];
	copy_str(buf, sizeof(buf), input);
	trim(buf);
	const char *rest = buf;
	static const char *prefixes[] = {"https://github.com/", "http://github.com/", "https://www.github.com/",
									 "github.com/", "git@github.com:"};
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		size_t len = strlen(prefixes[i]);
		if (strncmp(rest, prefixes[i], len) == 0) {
			rest += len;
			break;
		}
	}
	copy_str(out, size, rest);
	size_t len = strlen(out);
	while (len > 0 && out[len - 1] == '/') {
		out[--len] = '\0';
	}
	if (len > 4 && strcmp(out + len - 4, ".git") == 0) {
		out[len - 4] = '\0';
	}
	return as_repo_valid(out);
}

static bool repos_save(const as_model_t *m) {
	char path[600];
	snprintf(path, sizeof(path), "%s/repos.txt", m->addon_dir);
	char text[AS_REPOS_MAX * (AS_REPO_MAX + 2) + 256];
	size_t used = (size_t)snprintf(
		text, sizeof(text),
		"# Addons are read from the latest GitHub release of each repository\n"
		"# here, one owner/name per line. The Store adds and removes them too.\n");
	for (int i = 0; i < m->repo_count; i++) {
		int n = snprintf(text + used, sizeof(text) - used, "%s\n", m->repo[i]);
		if (n <= 0 || (size_t)n >= sizeof(text) - used) {
			break;
		}
		used += (size_t)n;
	}
	return write_atomic(path, text, used);
}

bool as_repos_load(as_model_t *m) {
	char path[600];
	snprintf(path, sizeof(path), "%s/repos.txt", m->addon_dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		copy_str(m->repo[m->repo_count++], AS_REPO_MAX, "Givemegitpls/sonix-player-addons");
		m->repos_created = repos_save(m);
		return true;
	}

	char line[256];
	while (fgets(line, sizeof(line), f) && m->repo_count < AS_REPOS_MAX) {
		trim(line);
		if (!line[0] || line[0] == '#' || !strchr(line, '/')) {
			continue;
		}
		char repo[AS_REPO_MAX];
		if (!repo_normalise(line, repo, sizeof(repo))) {
			continue;
		}
		bool duplicate = false;
		for (int i = 0; i < m->repo_count; i++) {
			if (strcmp(m->repo[i], repo) == 0) {
				duplicate = true;
			}
		}
		if (!duplicate) {
			copy_str(m->repo[m->repo_count++], AS_REPO_MAX, repo);
		}
	}
	fclose(f);
	return true;
}

bool as_repos_add(as_model_t *m, const char *input) {
	char repo[AS_REPO_MAX];
	if (!repo_normalise(input, repo, sizeof(repo))) {
		return false;
	}
	for (int i = 0; i < m->repo_count; i++) {
		if (strcmp(m->repo[i], repo) == 0) {
			return true; // already there: not an error, nothing to write
		}
	}
	if (m->repo_count >= AS_REPOS_MAX) {
		return false;
	}
	copy_str(m->repo[m->repo_count++], AS_REPO_MAX, repo);
	return repos_save(m);
}

bool as_repos_remove(as_model_t *m, int index) {
	if (index < 0 || index >= m->repo_count) {
		return false;
	}
	memmove(&m->repo[index], &m->repo[index + 1], (size_t)(m->repo_count - index - 1) * sizeof(m->repo[0]));
	m->repo_count--;
	return repos_save(m);
}

// "titles": {"Russian": "Динозаврик"} -- what the tile is called in the
// interface's language, else the plain title.
static void manifest_title(const json_doc_t *doc, int root, const char *lang, char *out, size_t size) {
	if (!json_obj_str(doc, root, "title", out, size)) {
		out[0] = '\0';
	}
	if (!lang || !lang[0]) {
		return;
	}
	int titles = json_get(doc, root, "titles");
	if (titles < 0) {
		return;
	}
	char localized[AS_TITLE_MAX];
	if (json_obj_str(doc, titles, lang, localized, sizeof(localized)) && localized[0]) {
		copy_str(out, size, localized);
	}
}

void as_manifest_fields(const char *text, char *title, size_t title_size, char *section, size_t section_size,
						char *version, size_t version_size) {
	if (title && title_size) {
		title[0] = '\0';
	}
	if (section && section_size) {
		section[0] = '\0';
	}
	if (version && version_size) {
		version[0] = '\0';
	}
	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		return;
	}
	int root = json_root(&doc);
	if (title && title_size) {
		manifest_title(&doc, root, lang_current(), title, title_size);
	}
	if (section && section_size) {
		char word[16] = "";
		if (json_obj_str(&doc, root, "section", word, sizeof(word))) {
			if (strcmp(word, "music") == 0 || strcmp(word, "streaming") == 0 || strcmp(word, "wireless") == 0 ||
				strcmp(word, "more") == 0) {
				copy_str(section, section_size, word);
			}
		}
	}
	if (version && version_size) {
		json_obj_str(&doc, root, "version", version, version_size);
	}
	json_free(&doc);
}

void as_scan_installed(as_model_t *m) {
	m->installed_count = 0;
	DIR *dir = opendir(m->addons_dir);
	if (!dir) {
		return;
	}
	struct dirent *de;
	while ((de = readdir(dir)) != NULL && m->installed_count < AS_MAX) {
		if (de->d_name[0] == '.') {
			continue;
		}
		char path[1000];
		snprintf(path, sizeof(path), "%s/%s/addon.json", m->addons_dir, de->d_name);
		FILE *f = fopen(path, "r");
		if (!f) {
			continue;
		}
		char text[MANIFEST_MAX];
		size_t len = fread(text, 1, sizeof(text) - 1, f);
		fclose(f);
		text[len] = '\0';

		json_doc_t doc;
		if (!json_parse(text, &doc)) {
			continue;
		}
		json_free(&doc);
		as_installed_t *it = &m->installed[m->installed_count];
		copy_str(it->id, sizeof(it->id), de->d_name);
		as_manifest_fields(text, it->title, sizeof(it->title), it->section, sizeof(it->section), it->version,
						   sizeof(it->version));
		if (!it->title[0]) {
			copy_str(it->title, sizeof(it->title), de->d_name);
		}
		m->installed_count++;
	}
	closedir(dir);
}

const as_installed_t *as_installed_find(const as_model_t *m, const char *id) {
	for (int i = 0; i < m->installed_count; i++) {
		if (strcmp(m->installed[i].id, id) == 0) {
			return &m->installed[i];
		}
	}
	return NULL;
}

const as_release_t *as_release_find(const as_model_t *m, const char *id) {
	for (int i = 0; i < m->release_count; i++) {
		if (strcmp(m->release[i].id, id) == 0) {
			return &m->release[i];
		}
	}
	return NULL;
}

const as_record_t *as_record_find(const as_model_t *m, const char *id) {
	for (int i = 0; i < m->record_count; i++) {
		if (strcmp(m->record[i].id, id) == 0) {
			return &m->record[i];
		}
	}
	return NULL;
}

void as_installed_version(const as_model_t *m, const char *id, char *out, size_t size) {
	if (!out || size == 0) {
		return;
	}
	out[0] = '\0';
	const as_record_t *rec = as_record_find(m, id);
	if (rec && rec->version[0]) {
		copy_str(out, size, rec->version);
		return;
	}
	const as_installed_t *inst = as_installed_find(m, id);
	if (inst) {
		copy_str(out, size, inst->version);
	}
}

// A file's own sha256, read in pieces: a few hundred kilobytes per addon.
static bool hash_file(const char *path, char out[65]) {
	out[0] = '\0';
	FILE *f = fopen(path, "rb");
	if (!f) {
		return false;
	}
	sha256_ctx sha;
	sha256_init(&sha);
	uint8_t buf[8192];
	size_t n;
	bool ok = true;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
		sha256_update(&sha, buf, n);
	}
	if (ferror(f)) {
		ok = false;
	}
	fclose(f);
	if (ok) {
		sha256_final_hex(&sha, out);
	}
	return ok;
}

void as_local_load(as_model_t *m) {
	m->local_count = 0;
	char path[700];
	snprintf(path, sizeof(path), "%s/local.tsv", m->cache_dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[256];
	while (fgets(line, sizeof(line), f) && m->local_count < AS_MAX) {
		trim(line);
		if (!line[0]) {
			continue;
		}
		char *field[4] = {line, NULL, NULL, NULL};
		int n = 1;
		for (char *p = line; *p && n < 4; p++) {
			if (*p == '\t') {
				*p = '\0';
				field[n++] = p + 1;
			}
		}
		if (n < 4) {
			continue;
		}
		as_local_t *l = &m->local[m->local_count];
		copy_str(l->id, sizeof(l->id), field[0]);
		l->size = strtol(field[1], NULL, 10);
		l->mtime = strtol(field[2], NULL, 10);
		copy_str(l->sha256, sizeof(l->sha256), field[3]);
		if (l->id[0] && l->sha256[0]) {
			m->local_count++;
		}
	}
	fclose(f);
}

static bool local_save(const as_model_t *m) {
	char text[AS_MAX * 160];
	size_t used = 0;
	for (int i = 0; i < m->local_count; i++) {
		const as_local_t *l = &m->local[i];
		int n = snprintf(text + used, sizeof(text) - used, "%s\t%ld\t%ld\t%s\n", l->id, l->size, l->mtime,
						 l->sha256);
		if (n <= 0 || (size_t)n >= sizeof(text) - used) {
			break;
		}
		used += (size_t)n;
	}
	char path[700];
	snprintf(path, sizeof(path), "%s/local.tsv", m->cache_dir);
	return write_atomic(path, text, used);
}

bool as_local_sha(as_model_t *m, const char *id, char out[65]) {
	out[0] = '\0';
	char path[1000];
	as_addon_path(m, id, path, sizeof(path));
	size_t len = strlen(path);
	snprintf(path + len, sizeof(path) - len, "/addon.so");

	struct stat st;
	if (stat(path, &st) != 0) {
		return false;
	}
	as_local_t *slot = NULL;
	for (int i = 0; i < m->local_count; i++) {
		if (strcmp(m->local[i].id, id) == 0) {
			slot = &m->local[i];
		}
	}
	if (slot && slot->size == (long)st.st_size && slot->mtime == (long)st.st_mtime && slot->sha256[0]) {
		copy_str(out, 65, slot->sha256);
		return true;
	}
	char hex[65];
	if (!hash_file(path, hex)) {
		return false;
	}
	copy_str(out, 65, hex);
	if (slot || m->local_count < AS_MAX) {
		if (!slot) {
			slot = &m->local[m->local_count++];
			copy_str(slot->id, sizeof(slot->id), id);
		}
		slot->size = (long)st.st_size;
		slot->mtime = (long)st.st_mtime;
		copy_str(slot->sha256, sizeof(slot->sha256), hex);
		local_save(m);
	}
	return true;
}

bool as_installed_matches(as_model_t *m, const char *id) {
	const as_release_t *rel = as_release_find(m, id);
	if (!rel || !as_installed_find(m, id)) {
		return false;
	}
	// Installed by the store: the asset's digest covers every file in the zip
	// -- code, assets, and a shipped settings.ini included -- so that is the
	// exact answer, and it costs no read of the card.
	const as_record_t *rec = as_record_find(m, id);
	if (rec && rec->digest[0] && rel->digest[0]) {
		return rec->size == rel->size && strcasecmp(rec->digest, rel->digest) == 0;
	}

	// Copied by hand, or a record from a cache that had no digest: the library
	// itself is the evidence. It catches a code update; an assets-only release
	// is invisible to this, and the addon's own files are the user's anyway.
	if (rel->so_sha256[0]) {
		char hex[65];
		if (!as_local_sha(m, id, hex)) {
			return false;
		}
		return strcasecmp(hex, rel->so_sha256) == 0;
	}
	if (rec) {
		return rec->size == rel->size && (rec->digest[0] == 0 || rel->digest[0] == 0);
	}
	return false;
}

bool as_update_available(as_model_t *m, const char *id) {
	return as_installed_find(m, id) && as_release_find(m, id) && !as_installed_matches(m, id);
}

// ---------------------------------------------------------------------------
// The two files: tabs, one record a line. They are the store's own, nothing else
// reads them, and a line that does not parse is skipped rather than fatal.
// ---------------------------------------------------------------------------

static bool write_atomic(const char *path, const char *text, size_t len) {
	char tmp[620];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f) {
		return false;
	}
	bool ok = fwrite(text, 1, len, f) == len;
	if (ok && fflush(f) != 0) {
		ok = false;
	}
	if (ok && fsync(fileno(f)) != 0) {
		ok = false;
	}
	if (fclose(f) != 0) {
		ok = false;
	}
	if (!ok) {
		unlink(tmp);
		return false;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

void as_cache_load(as_model_t *m) {
	m->release_count = 0;
	char path[700];
	snprintf(path, sizeof(path), "%s/releases.tsv", m->cache_dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[AS_URL_MAX + 512];
	while (fgets(line, sizeof(line), f) && m->release_count < AS_MAX) {
		trim(line);
		if (!line[0]) {
			continue;
		}
		char *field[12] = {line, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
		int n = 1;
		for (char *p = line; *p && n < 12; p++) {
			if (*p == '\t') {
				*p = '\0';
				field[n++] = p + 1;
			}
		}
		if (n < 6) {
			continue;
		}
		as_release_t *r = &m->release[m->release_count];
		copy_str(r->repo, sizeof(r->repo), field[0]);
		copy_str(r->tag, sizeof(r->tag), field[1]);
		copy_str(r->asset, sizeof(r->asset), field[2]);
		r->size = strtol(field[3], NULL, 10);
		copy_str(r->digest, sizeof(r->digest), field[4]);
		copy_str(r->url, sizeof(r->url), field[5]);
		if (n >= 7) {
			copy_str(r->title, sizeof(r->title), field[6]);
		}
		if (n >= 8) {
			copy_str(r->section, sizeof(r->section), field[7]);
		}
		if (n >= 9) {
			copy_str(r->so_sha256, sizeof(r->so_sha256), field[8]);
		}
		if (n >= 10) {
			r->so_size = strtol(field[9], NULL, 10);
		}
		if (n >= 11) {
			copy_str(r->version, sizeof(r->version), field[10]);
		}
		if (n >= 12) {
			r->listed = strcmp(field[11], "1") == 0;
		}
		as_id_from_asset(r->asset, r->id, sizeof(r->id));
		if (r->id[0]) {
			m->release_count++;
		}
	}
	fclose(f);
}

bool as_cache_save(const as_model_t *m) {
	// Static, not on the stack: a full line is about a kilobyte and this is
	// called from the interface thread, where the stack is not the place for
	// thirty kilobytes of text.
	static char text[AS_MAX * (AS_URL_MAX + 512)];
	size_t used = 0;
	for (int i = 0; i < m->release_count; i++) {
		const as_release_t *r = &m->release[i];
		int n = snprintf(text + used, sizeof(text) - used, "%s\t%s\t%s\t%ld\t%s\t%s\t%s\t%s\t%s\t%ld\t%s\t%d\n",
						 r->repo, r->tag, r->asset, r->size, r->digest, r->url, r->title, r->section,
						 r->so_sha256, r->so_size, r->version, r->listed ? 1 : 0);
		if (n <= 0 || (size_t)n >= sizeof(text) - used) {
			break;
		}
		used += (size_t)n;
	}
	char path[700];
	snprintf(path, sizeof(path), "%s/releases.tsv", m->cache_dir);
	return write_atomic(path, text, used);
}

void as_records_load(as_model_t *m) {
	m->record_count = 0;
	char path[600];
	snprintf(path, sizeof(path), "%s/installed.tsv", m->addon_dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		return;
	}
	char line[512];
	while (fgets(line, sizeof(line), f) && m->record_count < AS_MAX) {
		trim(line);
		if (!line[0]) {
			continue;
		}
		char *field[7] = {line, NULL, NULL, NULL, NULL, NULL, NULL};
		int n = 1;
		for (char *p = line; *p && n < 7; p++) {
			if (*p == '\t') {
				*p = '\0';
				field[n++] = p + 1;
			}
		}
		if (n < 7) {
			continue;
		}
		as_record_t *r = &m->record[m->record_count];
		copy_str(r->id, sizeof(r->id), field[0]);
		copy_str(r->repo, sizeof(r->repo), field[1]);
		copy_str(r->tag, sizeof(r->tag), field[2]);
		copy_str(r->asset, sizeof(r->asset), field[3]);
		r->size = strtol(field[4], NULL, 10);
		copy_str(r->digest, sizeof(r->digest), field[5]);
		copy_str(r->version, sizeof(r->version), field[6]);
		if (r->id[0]) {
			m->record_count++;
		}
	}
	fclose(f);
}

static bool records_save(const as_model_t *m) {
	static char text[AS_MAX * 512];
	size_t used = 0;
	for (int i = 0; i < m->record_count; i++) {
		const as_record_t *r = &m->record[i];
		int n = snprintf(text + used, sizeof(text) - used, "%s\t%s\t%s\t%s\t%ld\t%s\t%s\n", r->id, r->repo, r->tag,
						 r->asset, r->size, r->digest, r->version);
		if (n <= 0 || (size_t)n >= sizeof(text) - used) {
			break;
		}
		used += (size_t)n;
	}
	char path[600];
	snprintf(path, sizeof(path), "%s/installed.tsv", m->addon_dir);
	return write_atomic(path, text, used);
}

bool as_record_put(as_model_t *m, const as_record_t *r) {
	int slot = -1;
	for (int i = 0; i < m->record_count; i++) {
		if (strcmp(m->record[i].id, r->id) == 0) {
			slot = i;
		}
	}
	if (slot < 0) {
		if (m->record_count >= AS_MAX) {
			return false;
		}
		slot = m->record_count++;
	}
	m->record[slot] = *r;
	return records_save(m);
}

bool as_record_drop(as_model_t *m, const char *id) {
	for (int i = 0; i < m->record_count; i++) {
		if (strcmp(m->record[i].id, id) == 0) {
			memmove(&m->record[i], &m->record[i + 1], (size_t)(m->record_count - i - 1) * sizeof(m->record[0]));
			m->record_count--;
			return records_save(m);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// Search
//
// The keyboard types lower case, and this panel's Russian is two-byte UTF-8
// where the upper case letters are D0 90..D0 AF. Folding both to one form is
// what makes "дин" find "Динозаврик" without a locale.
// ---------------------------------------------------------------------------

static void fold_ci(const char *src, char *dst, size_t size) {
	size_t out = 0;
	for (size_t i = 0; src[i] && out + 3 < size; i++) {
		unsigned char c = (unsigned char)src[i];
		unsigned char next = (unsigned char)src[i + 1];
		if (c >= 'A' && c <= 'Z') {
			dst[out++] = (char)(c + ('a' - 'A'));
			continue;
		}
		if (c == 0xD0 && next == 0x81) { // Ё -> ё
			dst[out++] = (char)0xD1;
			dst[out++] = (char)0x91;
			i++;
			continue;
		}
		if (c == 0xD0 && next >= 0x90 && next <= 0xAF) { // А..Я -> а..я
			dst[out++] = (char)0xD0;
			dst[out++] = (char)(next + 0x20);
			i++;
			continue;
		}
		dst[out++] = (char)c;
	}
	dst[out] = '\0';
}

bool as_matches(const char *id, const char *title, const char *query) {
	if (!query || !query[0]) {
		return true;
	}
	char needle[AS_QUERY_MAX * 2];
	char hay[AS_TITLE_MAX * 2];
	fold_ci(query, needle, sizeof(needle));
	if (!needle[0]) {
		return true;
	}
	if (title && title[0]) {
		fold_ci(title, hay, sizeof(hay));
		if (strstr(hay, needle)) {
			return true;
		}
	}
	if (id && id[0]) {
		fold_ci(id, hay, sizeof(hay));
		if (strstr(hay, needle)) {
			return true;
		}
	}
	return false;
}

// ---------------------------------------------------------------------------
// The repository's index.json
// ---------------------------------------------------------------------------

static void index_title(const json_doc_t *doc, int item, char *out, size_t size) {
	if (!json_obj_str(doc, item, "title", out, size)) {
		out[0] = '\0';
	}
	const char *lang = lang_current();
	if (!lang || !lang[0]) {
		return;
	}
	int titles = json_get(doc, item, "titles");
	if (titles < 0) {
		return;
	}
	char localized[AS_TITLE_MAX];
	if (json_obj_str(doc, titles, lang, localized, sizeof(localized)) && localized[0]) {
		copy_str(out, size, localized);
	}
}

int as_index_parse(const char *text, as_release_t *out, int max, const char *repo) {
	json_doc_t doc;
	if (!json_parse(text, &doc)) {
		return -1;
	}
	int root = json_root(&doc);
	if (json_obj_long(&doc, root, "schema", 0) != 1) {
		json_free(&doc);
		return -1;
	}
	int list = json_get(&doc, root, "addons");
	if (list < 0) {
		json_free(&doc);
		return -1;
	}

	int count = 0;
	int n = json_len(&doc, list);
	for (int i = 0; i < n && count < max; i++) {
		int item = json_at(&doc, list, i);
		if (item < 0) {
			continue;
		}
		as_release_t r;
		memset(&r, 0, sizeof(r));
		char asset[AS_ASSET_MAX];
		if (!json_obj_str(&doc, item, "asset", asset, sizeof(asset))) {
			continue;
		}
		as_id_from_asset(asset, r.id, sizeof(r.id));
		if (!r.id[0]) {
			continue;
		}
		copy_str(r.asset, sizeof(r.asset), asset);
		r.size = json_obj_long(&doc, item, "size", 0);
		json_obj_str(&doc, item, "url", r.url, sizeof(r.url));
		json_obj_str(&doc, item, "tag", r.tag, sizeof(r.tag));
		json_obj_str(&doc, item, "version", r.version, sizeof(r.version));
		json_obj_str(&doc, item, "so_sha256", r.so_sha256, sizeof(r.so_sha256));
		r.so_size = json_obj_long(&doc, item, "so_size", 0);
		if (!r.url[0] || strncmp(r.url, "https://", 8) != 0 || r.size <= 0) {
			continue;
		}
		char sha[65] = "";
		if (json_obj_str(&doc, item, "sha256", sha, sizeof(sha)) && strlen(sha) == 64) {
			snprintf(r.digest, sizeof(r.digest), "sha256:%s", sha);
		}
		char section[16] = "";
		if (json_obj_str(&doc, item, "section", section, sizeof(section))) {
			if (strcmp(section, "music") != 0 && strcmp(section, "streaming") != 0 &&
				strcmp(section, "wireless") != 0 && strcmp(section, "more") != 0) {
				section[0] = '\0';
			}
		}
		copy_str(r.section, sizeof(r.section), section);
		index_title(&doc, item, r.title, sizeof(r.title));
		if (repo) {
			copy_str(r.repo, sizeof(r.repo), repo);
		}
		r.listed = true;
		out[count++] = r;
	}
	json_free(&doc);
	return count;
}
