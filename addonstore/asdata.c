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

bool as_repos_load(as_model_t *m) {
	char path[600];
	snprintf(path, sizeof(path), "%s/repos.txt", m->addon_dir);
	FILE *f = fopen(path, "r");
	if (!f) {
		static const char *defaults[] = {"Givemegitpls/sonix-player-addons"};
		for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]) && m->repo_count < AS_REPOS_MAX; i++) {
			copy_str(m->repo[m->repo_count++], AS_REPO_MAX, defaults[i]);
		}
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "# Addons are read from the latest GitHub release of each repository\n");
			fprintf(f, "# here, one owner/name per line. Edit on a computer, then reopen.\n");
			for (int i = 0; i < m->repo_count; i++) {
				fprintf(f, "%s\n", m->repo[i]);
			}
			fclose(f);
			m->repos_created = true;
		}
		return true;
	}

	char line[256];
	while (fgets(line, sizeof(line), f) && m->repo_count < AS_REPOS_MAX) {
		trim(line);
		if (!line[0] || line[0] == '#' || !strchr(line, '/')) {
			continue;
		}
		bool duplicate = false;
		for (int i = 0; i < m->repo_count; i++) {
			if (strcmp(m->repo[i], line) == 0) {
				duplicate = true;
			}
		}
		if (!duplicate) {
			copy_str(m->repo[m->repo_count++], AS_REPO_MAX, line);
		}
	}
	fclose(f);
	return true;
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

// The installed library's own sha256, read in pieces: a few hundred kilobytes
// per addon, and only for the ones a release can be compared with.
static bool file_sha256(const char *path, char out[65]) {
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

bool as_installed_matches(const as_model_t *m, const char *id) {
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
		char path[1000];
		as_addon_path(m, id, path, sizeof(path));
		snprintf(path + strlen(path), sizeof(path) - strlen(path), "/addon.so");
		char hex[65];
		if (!file_sha256(path, hex)) {
			return false;
		}
		return strcasecmp(hex, rel->so_sha256) == 0;
	}
	if (rec) {
		return rec->size == rel->size && (rec->digest[0] == 0 || rel->digest[0] == 0);
	}
	return false;
}

bool as_update_available(const as_model_t *m, const char *id) {
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
	char line[AS_URL_MAX + 384];
	while (fgets(line, sizeof(line), f) && m->release_count < AS_MAX) {
		trim(line);
		if (!line[0]) {
			continue;
		}
		char *field[10] = {line, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
		int n = 1;
		for (char *p = line; *p && n < 10; p++) {
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
		as_id_from_asset(r->asset, r->id, sizeof(r->id));
		if (r->id[0]) {
			m->release_count++;
		}
	}
	fclose(f);
}

bool as_cache_save(const as_model_t *m) {
	char text[AS_MAX * (AS_URL_MAX + 256)];
	size_t used = 0;
	for (int i = 0; i < m->release_count; i++) {
		const as_release_t *r = &m->release[i];
		int n = snprintf(text + used, sizeof(text) - used, "%s\t%s\t%s\t%ld\t%s\t%s\t%s\t%s\t%s\t%ld\n", r->repo,
						 r->tag, r->asset, r->size, r->digest, r->url, r->title, r->section, r->so_sha256,
						 r->so_size);
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
	char text[AS_MAX * 512];
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
